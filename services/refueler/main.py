import asyncio
import json
import os
import httpx
import aio_pika
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from typing import Dict, List, Optional
import logging
from contextlib import asynccontextmanager

# ---------------------------------------------------------------------------
# Config (env)
# ---------------------------------------------------------------------------
GROUND_CONTROL_URL = os.getenv(
    "GROUND_CONTROL_URL", "http://localhost:8081/v1")
HS_HOST = os.getenv("HS_HOST", "handling-supervisor")
HS_PORT = int(os.getenv("HS_PORT", "8085"))
PORT = int(os.getenv("REFUELER_PORT", "8088"))
RABBITMQ_URL = os.getenv("RABBITMQ_URL", "amqp://guest:guest@rabbitmq:5672/")

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s - %(levelname)s - %(message)s")
logger = logging.getLogger("RefuelerAPI")


# ---------------------------------------------------------------------------
# Data models
# ---------------------------------------------------------------------------
class RefuelTruckData(BaseModel):
    id: str
    status: str
    currentLocation: str
    fuelLevel: int


class RefuelTruck:
    def __init__(self, id: str, status: str, current_location: str,
                 base_location: str, fuel_level: int = 100):
        self.id = id
        self.status = status
        self.current_location = current_location
        self.base_location = base_location
        self.fuel_level = fuel_level

    def to_dict(self):
        return {
            "id": self.id,
            "status": self.status,
            "currentLocation": self.current_location,
            "fuelLevel": self.fuel_level,
        }


# ---------------------------------------------------------------------------
# In-memory state
# ---------------------------------------------------------------------------
refuel_trucks: Dict[str, RefuelTruck] = {}

# RabbitMQ connection
_rmq_conn: Optional[aio_pika.RobustConnection] = None

# ---------------------------------------------------------------------------
# Single truck config
# ---------------------------------------------------------------------------
_TRUCK_ID = "RT-1"
_TRUCK_HOME = "RS-1"   # refuelService node, connected to CR-5 via E-34

# Parking → nearest contact point
_PARKING_TO_CP: Dict[str, str] = {
    "P-1": "CP-11", "P-2": "CP-21", "P-3": "CP-31",
    "P-4": "CP-41", "P-5": "CP-51",
}


# ---------------------------------------------------------------------------
# Path helper  (identical logic as catering_truck, fixed slice)
# ---------------------------------------------------------------------------
async def gc_get_path(from_node: str, to_node: str) -> list:
    """Request route from GC; fall back to manual CR-chain walk."""
    if from_node == to_node:
        return [from_node]
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.get(
                f"{GROUND_CONTROL_URL}/map/car/path",
                params={"from": from_node, "to": to_node},
            )
            if r.status_code < 300:
                route = r.json().get("route", [])
                if route:
                    return route
    except Exception as e:
        logger.warning("GC path request failed: %s", e)

    # ---- Fallback: manual CR-chain walk ----
    cr_map = {
        "G-11": "CR-1", "G-12": "CR-1", "BR-11": "CR-1",
        "CP-11": "CR-1", "CP-12": "CR-1",
        "CS-1": "CR-1", "BW-1": "CR-1", "CG-1": "CR-1", "XT-1": "CR-1",
        "G-21": "CR-2", "G-22": "CR-2", "BR-21": "CR-2",
        "CP-21": "CR-2", "CP-22": "CR-2",
        "G-31": "CR-3", "G-32": "CR-3", "BR-31": "CR-3",
        "CP-31": "CR-3", "CP-32": "CR-3",
        "G-41": "CR-4", "G-42": "CR-4", "BR-41": "CR-4",
        "CP-41": "CR-4", "CP-42": "CR-4",
        "G-51": "CR-5", "G-52": "CR-5", "BR-51": "CR-5",
        "CP-51": "CR-5", "CP-52": "CR-5",
        "RS-1": "CR-5", "FS-1": "CR-5",
    }
    for i in range(1, 6):
        cr_map[f"CR-{i}"] = f"CR-{i}"

    # CR chain order (highest index first, matching E-14..E-11 edges)
    cr_chain = ["CR-5", "CR-4", "CR-3", "CR-2", "CR-1"]

    cr_from = cr_map.get(from_node, "")
    cr_to = cr_map.get(to_node, "")
    if not cr_from or not cr_to:
        logger.error("No fallback route from %s to %s", from_node, to_node)
        return []

    if cr_from == cr_to:
        if from_node == cr_from and to_node == cr_to:
            return [cr_from]
        elif from_node == cr_from:
            return [cr_from, to_node]
        elif to_node == cr_to:
            return [from_node, cr_from]
        else:
            return [from_node, cr_from, to_node]

    try:
        idx_from = cr_chain.index(cr_from)
        idx_to = cr_chain.index(cr_to)
    except ValueError:
        logger.error("CR not in chain: %s or %s", cr_from, cr_to)
        return []

    # Safe slice — never use negative stop index
    if idx_from <= idx_to:
        cr_segment = cr_chain[idx_from:idx_to + 1]
    else:
        cr_segment = cr_chain[idx_to:idx_from + 1][::-1]

    full = ([] if from_node == cr_from else [from_node]) + \
        cr_segment + ([] if to_node == cr_to else [to_node])
    return full


# ---------------------------------------------------------------------------
# Movement helper
# ---------------------------------------------------------------------------
async def move_truck(truck_id: str, from_location: str, to_location: str) -> bool:
    """Drive truck along GC traffic API (enter-edge / leave-edge)."""
    logger.info("move_truck %s: %s → %s", truck_id, from_location, to_location)
    truck = refuel_trucks.get(truck_id)
    if not truck:
        logger.error("Truck %s not found", truck_id)
        return False
    if from_location == to_location:
        return True

    route = await gc_get_path(from_location, to_location)
    if len(route) < 2:
        logger.error("No valid route: %s → %s", from_location, to_location)
        return False

    logger.info("Route for %s: %s", truck_id, route)

    for i in range(len(route) - 1):
        seg_from = route[i]
        seg_to = route[i + 1]

        # Enter edge — retry until granted
        for attempt in range(30):
            try:
                async with httpx.AsyncClient(timeout=10.0) as cli:
                    r = await cli.post(
                        f"{GROUND_CONTROL_URL}/map/traffic/enter-edge",
                        json={"vehicleId": truck_id,
                              "from": seg_from, "to": seg_to},
                    )
                    data = r.json()
                    if r.status_code < 300 and data.get("granted"):
                        break
                    logger.debug("%s enter-edge %s→%s: %s",
                                 truck_id, seg_from, seg_to,
                                 data.get("reason", r.status_code))
            except Exception as exc:
                logger.warning("%s enter-edge error: %s", truck_id, exc)
            await asyncio.sleep(1.0)
        else:
            logger.error("%s FAILED enter-edge %s→%s",
                         truck_id, seg_from, seg_to)
            return False

        await asyncio.sleep(2.0)   # travel time

        # Leave edge
        for attempt in range(10):
            try:
                async with httpx.AsyncClient(timeout=10.0) as cli:
                    r = await cli.post(
                        f"{GROUND_CONTROL_URL}/map/traffic/leave-edge",
                        json={"vehicleId": truck_id, "to": seg_to},
                    )
                    data = r.json()
                    if r.status_code < 300 and data.get("granted"):
                        break
                    logger.debug("%s leave-edge →%s: %s",
                                 truck_id, seg_to,
                                 data.get("reason", r.status_code))
            except Exception as exc:
                logger.warning("%s leave-edge error: %s", truck_id, exc)
            await asyncio.sleep(1.0)
        else:
            logger.error("%s FAILED leave-edge →%s", truck_id, seg_to)
            return False

        truck.current_location = seg_to
        logger.info("%s arrived at %s", truck_id, seg_to)

    return True


# ---------------------------------------------------------------------------
# Core refueling logic
# ---------------------------------------------------------------------------
async def _refuel_serve(flight_id: str, parking_node: str,
                        task_id: int = 0) -> None:
    """Drive RT-1 to CP-xx, refuel plane, return to RS-1, notify HS."""
    cp = _PARKING_TO_CP.get(parking_node, "CP-11")
    truck = refuel_trucks.get(_TRUCK_ID)
    if not truck:
        logger.error("[%s] truck %s not found", flight_id, _TRUCK_ID)
        return

    logger.info("[%s] refuel task: %s %s → %s",
                flight_id, _TRUCK_ID, truck.current_location, cp)

    if truck.status == "busy":
        logger.warning("[%s] %s is busy, aborting refuel",
                       flight_id, _TRUCK_ID)
        return

    truck.status = "busy"
    truck.fuel_level = 100
    logger.info("[%s] %s loaded with fuel", flight_id, _TRUCK_ID)

    success = await move_truck(_TRUCK_ID, truck.current_location, cp)
    if not success:
        logger.warning("[%s] could not move to %s, aborting", flight_id, cp)
        truck.status = "free"
        return

    logger.info("[%s] %s arrived at %s, refueling plane",
                flight_id, _TRUCK_ID, cp)
    await asyncio.sleep(5)   # refueling duration

    truck.fuel_level = 0
    logger.info("[%s] refueling done at %s", flight_id, cp)

    await move_truck(_TRUCK_ID, cp, _TRUCK_HOME)
    truck.status = "free"
    truck.fuel_level = 100   # tank filled back at base
    logger.info("[%s] refuel complete, %s back at %s",
                flight_id, _TRUCK_ID, _TRUCK_HOME)

    # Notify Handling Supervisor
    if task_id:
        try:
            async with httpx.AsyncClient(timeout=5.0) as cli:
                await cli.put(
                    f"http://{HS_HOST}:{HS_PORT}/v1/tasks/{task_id}/complete"
                )
                logger.info("[%s] PUT /v1/tasks/%d/complete sent to HS",
                            flight_id, task_id)
        except Exception as exc:
            logger.warning("[%s] HS callback failed: %s", flight_id, exc)


# ---------------------------------------------------------------------------
# RabbitMQ consumer for tasks.refueler
# ---------------------------------------------------------------------------
async def _consume_refuel_tasks() -> None:
    """Listens on tasks.refueler queue and runs _refuel_serve for each task."""
    global _rmq_conn
    logger.info("Connecting refueler consumer to RabbitMQ: %s", RABBITMQ_URL)
    for attempt in range(12):
        try:
            _rmq_conn = await aio_pika.connect_robust(RABBITMQ_URL)
            channel = await _rmq_conn.channel()
            await channel.set_qos(prefetch_count=1)
            queue = await channel.declare_queue("tasks.refueler", durable=True)
            logger.info("Refueler consumer ready on tasks.refueler")

            async with queue.iterator() as q_iter:
                async for message in q_iter:
                    async with message.process():
                        try:
                            payload = json.loads(message.body.decode())
                            task_id = payload.get("taskId", 0)
                            flight_id = payload.get("flightId", "")
                            parking_node = payload.get("parkingNode", "P-1")
                            logger.info(
                                "[%s] received refuel task #%d", flight_id, task_id)
                            await _refuel_serve(flight_id, parking_node, task_id)
                        except Exception as exc:
                            logger.error(
                                "Error processing refuel task: %s", exc)
            return
        except Exception as e:
            logger.warning(
                "Refueler RMQ connect attempt %d: %s", attempt + 1, e)
            await asyncio.sleep(3)
    logger.error("Refueler consumer could not connect to RabbitMQ")


# ---------------------------------------------------------------------------
# FastAPI lifespan — register RT-1, start RMQ consumer
# ---------------------------------------------------------------------------
@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("Starting Refueler module")
    refuel_trucks[_TRUCK_ID] = RefuelTruck(
        id=_TRUCK_ID, status="free",
        current_location=_TRUCK_HOME, base_location=_TRUCK_HOME,
        fuel_level=100,
    )
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.post(
                f"{GROUND_CONTROL_URL}/vehicles/init",
                json={"vehicles": [
                    {"vehicleId": _TRUCK_ID, "currentNode": _TRUCK_HOME}]},
            )
            logger.info("GC truck registration: %d %s", r.status_code, r.text)
    except Exception as e:
        logger.warning("GC registration at startup failed: %s", e)

    consumer_task = asyncio.create_task(_consume_refuel_tasks())
    yield
    consumer_task.cancel()
    if _rmq_conn and not _rmq_conn.is_closed:
        await _rmq_conn.close()
    logger.info("Stopping Refueler module")


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------
app = FastAPI(title="Refueler Service", version="1.0.0", lifespan=lifespan)


@app.get("/health")
def health():
    return {"service": "Refueler", "status": "ok"}


@app.get("/v1/refuelers", response_model=List[RefuelTruckData])
def get_all_trucks():
    return [t.to_dict() for t in refuel_trucks.values()]


@app.get("/v1/refuelers/{truck_id}", response_model=RefuelTruckData)
def get_truck_by_id(truck_id: str):
    truck = refuel_trucks.get(truck_id)
    if not truck:
        raise HTTPException(status_code=404, detail="Refueler not found")
    return truck.to_dict()


class RefuelRequestBody(BaseModel):
    flightId: str
    parkingNode: str


@app.post("/v1/refuel/request")
async def refuel_request(body: RefuelRequestBody):
    """HTTP entry point (fallback when RabbitMQ is unavailable)."""
    logger.info("[%s] HTTP refuel request for parking %s",
                body.flightId, body.parkingNode)
    asyncio.create_task(_refuel_serve(body.flightId, body.parkingNode))
    return {"ok": True, "message": f"Refueling started for {body.flightId}"}


@app.post("/v1/refuel/reset")
async def refuel_reset():
    """Reset RT-1 to free at RS-1."""
    truck = refuel_trucks.get(_TRUCK_ID)
    if truck:
        truck.status = "free"
        truck.current_location = _TRUCK_HOME
        truck.base_location = _TRUCK_HOME
        truck.fuel_level = 100
    else:
        refuel_trucks[_TRUCK_ID] = RefuelTruck(
            id=_TRUCK_ID, status="free",
            current_location=_TRUCK_HOME, base_location=_TRUCK_HOME,
            fuel_level=100,
        )
    logger.info("%s reset to %s", _TRUCK_ID, _TRUCK_HOME)

    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            await cli.post(
                f"{GROUND_CONTROL_URL}/vehicles/init",
                json={"vehicles": [
                    {"vehicleId": _TRUCK_ID, "currentNode": _TRUCK_HOME}]},
            )
        logger.info("%s re-registered in GC at %s", _TRUCK_ID, _TRUCK_HOME)
    except Exception as exc:
        logger.warning("GC re-registration during reset failed: %s", exc)

    return {"ok": True, "reset": 1}


# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=PORT, reload=False)
