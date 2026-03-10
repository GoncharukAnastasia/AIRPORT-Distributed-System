import os
import asyncpg
import uvicorn
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from datetime import datetime

app = FastAPI()

DB_HOST = os.getenv("DB_HOST", "postgres")
DB_PORT = os.getenv("DB_PORT", "5432")
DB_NAME = os.getenv("DB_NAME", "airport")
DB_USER = os.getenv("DB_USER", "airport_user")
DB_PASS = os.getenv("DB_PASS", "airport_pass")

print(DB_HOST, DB_PORT, DB_NAME, DB_USER, DB_PASS)

pool = None

class Event(BaseModel):
    eventId: str
    eventType: str
    flightId: str
    passengerId: str | None = None
    count: int | None = None
    timestamp: datetime | None = None


@app.on_event("startup")
async def startup():
    global pool

    pool = await asyncpg.create_pool(
        user=DB_USER,
        password=DB_PASS,
        database=DB_NAME,
        host=DB_HOST,
        port=DB_PORT
    )


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.post("/v1/events")
async def receive_event(event: Event):

    async with pool.acquire() as conn:
        exists = await conn.fetchrow(
            "SELECT event_id FROM reporting_events WHERE event_id=$1",
            event.eventId
        )
        if exists:
            return {"status": "duplicate"}

        await conn.execute(
            """
            INSERT INTO reporting_events(event_id,event_type,flight_id,passenger_id,count,timestamp)
            VALUES($1,$2,$3,$4,$5,$6)
            """,
            event.eventId,
            event.eventType,
            event.flightId,
            event.passengerId,
            event.count,
            event.timestamp or datetime.utcnow()
        )
        if event.eventType == "ticket_sold":
            await conn.execute(
            """
            INSERT INTO flight_statistics (flight_id, tickets_sold)
            VALUES ($1, 1)
            ON CONFLICT (flight_id)
            DO UPDATE SET tickets_sold = flight_statistics.tickets_sold + 1
            """,
            event.flightId
            )
        elif event.eventType == "ticket_refunded":
            await conn.execute(
                """
                INSERT INTO flight_statistics (flight_id, tickets_refunded)
                VALUES ($1, 1)
                ON CONFLICT (flight_id)
                DO UPDATE SET tickets_refunded = flight_statistics.tickets_refunded + 1
                """,
                event.flightId
            )
        elif event.eventType == "passenger_checked_in":
            await conn.execute(
                """
                INSERT INTO flight_statistics (flight_id, passengers_checked_in)
                VALUES ($1, 1)
                ON CONFLICT (flight_id)
                DO UPDATE SET passengers_checked_in =
                flight_statistics.passengers_checked_in + 1
                """,
                event.flightId
            )
        elif event.eventType == "passengers_boarded":
            await conn.execute(
                """
                INSERT INTO flight_statistics (flight_id, passengers_boarded)
                VALUES ($1, $2)
                ON CONFLICT (flight_id)
                DO UPDATE SET passengers_boarded =
                flight_statistics.passengers_boarded + $2
                """,
                event.flightId,
                event.count or 0
    )
    return {"status": "accepted"}


@app.get("/v1/reports/flights/{flight_id}")
async def report_flight(flight_id: str):
    async with pool.acquire() as conn:

        row = await conn.fetchrow(
            """
            SELECT *
            FROM flight_statistics
            WHERE flight_id=$1
            """,
            flight_id
        )
        if not row:
            raise HTTPException(status_code=404)
        
        return dict(row)


@app.get("/v1/reports/summary")
async def report_summary():
    async with pool.acquire() as conn:

        row = await conn.fetchrow(
            """
            SELECT
            SUM(tickets_sold) as tickets_sold,
            SUM(tickets_refunded) as tickets_refunded,
            SUM(passengers_checked_in) as passengers_checked_in,
            SUM(passengers_boarded) as passengers_boarded
            FROM flight_statistics
            """
        )

        return dict(row)


@app.get("/v1/reports/flights")
async def report_all_flights():
    async with pool.acquire() as conn:

        rows = await conn.fetch(
            """
            SELECT
                flight_id,
                tickets_sold,
                tickets_refunded,
                passengers_checked_in,
                passengers_boarded
            FROM flight_statistics
            ORDER BY flight_id
            """
        )

        return [dict(r) for r in rows]


@app.get("/v1/reports/flights/{flight_id}/metrics")
async def flight_metrics(flight_id: str):
    async with pool.acquire() as conn:

        row = await conn.fetchrow(
            """
            SELECT
                tickets_sold,
                passengers_boarded
            FROM flight_statistics
            WHERE flight_id=$1
            """,
            flight_id
        )

        if not row:
            raise HTTPException(status_code=404)

        sold = row["tickets_sold"] or 0
        boarded = row["passengers_boarded"] or 0

        load_factor = boarded / sold if sold else 0

        return {
            "flightId": flight_id,
            "ticketsSold": sold,
            "passengersBoarded": boarded,
            "loadFactor": load_factor
        }


@app.get("/v1/reports/events")
async def events_statistics():
    async with pool.acquire() as conn:

        rows = await conn.fetch(
            """
            SELECT
                event_type,
                COUNT(*) as count
            FROM reporting_events
            GROUP BY event_type
            ORDER BY count DESC
            """
        )

        return [dict(r) for r in rows]
    

@app.get("/v1/reports/timeline")
async def report_timeline():
    async with pool.acquire() as conn:

        rows = await conn.fetch(
            """
            SELECT
                date_trunc('hour', timestamp) as hour,
                COUNT(*) as events
            FROM reporting_events
            GROUP BY hour
            ORDER BY hour
            """
        )

        return [dict(r) for r in rows]


@app.get("/v1/reports/metrics")
async def system_metrics():
    async with pool.acquire() as conn:

        row = await conn.fetchrow(
            """
            SELECT
                AVG(tickets_sold) as avg_tickets,
                AVG(passengers_boarded) as avg_boarded,
                SUM(tickets_refunded)::float /
                NULLIF(SUM(tickets_sold),0) as refund_rate
            FROM flight_statistics
            """
        )

        return dict(row)
    

@app.get("/v1/events/recent")
async def recent_events(limit: int = 20):
    async with pool.acquire() as conn:

        rows = await conn.fetch(
            """
            SELECT *
            FROM reporting_events
            ORDER BY timestamp DESC
            LIMIT $1
            """,
            limit
        )

        return [dict(r) for r in rows]


@app.post("/v1/reports/reset")
async def reset_reports():
    async with pool.acquire() as conn:

        await conn.execute("DELETE FROM reporting_events")
        await conn.execute("DELETE FROM flight_statistics")

    return {"status": "reset"}


if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=8092)