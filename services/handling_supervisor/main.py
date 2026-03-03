"""
Handling Supervisor — координирует наземное обслуживание борта.

Порт по умолчанию: 8085

Принимает запрос от Board (POST /v1/handling/request) и оркестрирует:
  - выгрузку / погрузку пассажиров (через Passenger Bus)
  - выгрузку / погрузку багажа   (через Passenger Bus)
  - заправку                     (через Refueler — stub)
  - питание                      (через Catering)

Каждый сервис может быть недоступен — обработчик повторяет попытки.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import time
from contextlib import asynccontextmanager
from enum import Enum
from typing import Optional

import aio_pika
import httpx
import uvicorn
from fastapi import Body, FastAPI, HTTPException
from pydantic import BaseModel

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] [HandlingSupervisor] [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
log = logging.getLogger("handling_supervisor")

# ---------------------------------------------------------------------------
# Config (env)
# ---------------------------------------------------------------------------
GC_HOST = os.getenv("GC_HOST", "localhost")
GC_PORT = int(os.getenv("GC_PORT", "8081"))

BUS_HOST = os.getenv("BUS_HOST", "localhost")
BUS_PORT = int(os.getenv("BUS_PORT", "8086"))

REFUELER_HOST = os.getenv("REFUELER_HOST", "localhost")
REFUELER_PORT = int(os.getenv("REFUELER_PORT", "8088"))

CATERING_HOST = os.getenv("CATERING_HOST", "localhost")
CATERING_PORT = int(os.getenv("CATERING_PORT", "8089"))

BOARD_HOST = os.getenv("BOARD_HOST", "localhost")
BOARD_PORT = int(os.getenv("BOARD_PORT", "8084"))

INFO_HOST = os.getenv("INFO_HOST", "localhost")
INFO_PORT = int(os.getenv("INFO_PORT", "8082"))

PORT = int(os.getenv("HANDLING_PORT", "8085"))

# Сколько пассажиров вмещает один автобус
BUS_CAPACITY = int(os.getenv("BUS_CAPACITY", "20"))

RABBITMQ_URL = os.getenv("RABBITMQ_URL", "amqp://guest:guest@rabbitmq:5672/")


# ---------------------------------------------------------------------------
# RabbitMQ state
# ---------------------------------------------------------------------------
_rmq_conn: Optional[aio_pika.RobustConnection] = None
_rmq_ch: Optional[aio_pika.RobustChannel] = None

# ---------------------------------------------------------------------------
# Task ID tracking
# ---------------------------------------------------------------------------
_task_counter: int = 0
_task_events: dict[int, asyncio.Event] = {}


def _next_task_id() -> int:
    global _task_counter
    _task_counter += 1
    return _task_counter


async def _rmq_publish(queue: str, payload: dict) -> bool:
    """Publish a message to a RabbitMQ queue. Returns False if unavailable."""
    if _rmq_ch is None:
        return False
    try:
        await _rmq_ch.default_exchange.publish(
            aio_pika.Message(
                body=json.dumps(payload).encode(),
                delivery_mode=aio_pika.DeliveryMode.PERSISTENT,
            ),
            routing_key=queue,
        )
        log.info("RMQ → %s  taskId=%s type=%s", queue,
                 payload.get("taskId"), payload.get("taskType"))
        return True
    except Exception as e:
        log.warning("RabbitMQ publish to %s failed: %s", queue, e)
        return False


async def _publish_and_wait(queue: str, payload: dict, timeout: float = 180.0) -> bool:
    """Publish task and block until PUT /v1/tasks/{id}/complete is called."""
    task_id = payload["taskId"]
    event = asyncio.Event()
    _task_events[task_id] = event
    ok = await _rmq_publish(queue, payload)
    if not ok:
        _task_events.pop(task_id, None)
        return False
    try:
        await asyncio.wait_for(event.wait(), timeout=timeout)
        log.info("Task %d completed (callback received)", task_id)
        return True
    except asyncio.TimeoutError:
        log.warning("Task %d timed out after %.0fs", task_id, timeout)
        return False
    finally:
        _task_events.pop(task_id, None)


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class TaskStatus(str, Enum):
    PENDING = "pending"
    IN_PROGRESS = "in_progress"
    COMPLETED = "completed"
    FAILED = "failed"


class ServiceTask(BaseModel):
    name: str
    status: TaskStatus = TaskStatus.PENDING
    detail: str = ""


class HandlingRequest(BaseModel):
    flightId: str
    parkingNode: str
    services: list[str] = ["fuel", "catering", "boarding"]


class FlightHandling:
    """Трекер обслуживания одного рейса."""

    def __init__(self, flight_id: str, parking_node: str, requested_services: list[str]):
        self.flight_id = flight_id
        self.parking_node = parking_node
        self.status = "in_progress"
        self.created_at = time.time()

        # Чеклист
        self.checklist: dict[str, ServiceTask] = {}
        for svc in requested_services:
            self.checklist[svc] = ServiceTask(name=svc)

    def all_done(self) -> bool:
        return all(t.status == TaskStatus.COMPLETED for t in self.checklist.values())

    def to_dict(self) -> dict:
        return {
            "flightId": self.flight_id,
            "parkingNode": self.parking_node,
            "status": self.status,
            "createdAt": self.created_at,
            "checklist": {k: v.model_dump() for k, v in self.checklist.items()},
        }


# ---------------------------------------------------------------------------
# In-memory storage
# ---------------------------------------------------------------------------
handlings: dict[str, FlightHandling] = {}


# ---------------------------------------------------------------------------
# Helper — HTTP с ретраями
# ---------------------------------------------------------------------------
async def safe_post(host: str, port: int, path: str, payload: dict, retries: int = 3) -> Optional[dict]:
    url = f"http://{host}:{port}{path}"
    for attempt in range(1, retries + 1):
        try:
            async with httpx.AsyncClient(timeout=10.0) as cli:
                r = await cli.post(url, json=payload)
                if r.status_code < 300:
                    return r.json()
                log.warning("POST %s → %s (attempt %d)",
                            url, r.status_code, attempt)
        except Exception as exc:
            log.warning("POST %s → error: %s (attempt %d)", url, exc, attempt)
        await asyncio.sleep(min(2 ** attempt, 10))
    return None


async def safe_get(host: str, port: int, path: str, retries: int = 3) -> Optional[dict]:
    url = f"http://{host}:{port}{path}"
    for attempt in range(1, retries + 1):
        try:
            async with httpx.AsyncClient(timeout=10.0) as cli:
                r = await cli.get(url)
                if r.status_code < 300:
                    return r.json()
                log.warning("GET %s → %s (attempt %d)",
                            url, r.status_code, attempt)
        except Exception as exc:
            log.warning("GET %s → error: %s (attempt %d)", url, exc, attempt)
        await asyncio.sleep(min(2 ** attempt, 10))
    return None


# ---------------------------------------------------------------------------
# Parking → Gate / BaggageRoom mapping (derived from airport_map.json layout)
# ---------------------------------------------------------------------------
PARKING_TO_GATE: dict[str, str] = {
    "P-1": "G-11",
    "P-2": "G-21",
    "P-3": "G-31",
    "P-4": "G-41",
    "P-5": "G-51",
}

PARKING_TO_BAGGAGE: dict[str, str] = {
    "P-1": "BR-11",
    "P-2": "BR-21",
    "P-3": "BR-31",
    "P-4": "BR-41",
    "P-5": "BR-51",
}

# Parking → ближайший planeServicing point (точка подъезда к самолёту)
PARKING_TO_SERVICE_POINT: dict[str, str] = {
    "P-1": "CP-11",
    "P-2": "CP-21",
    "P-3": "CP-31",
    "P-4": "CP-41",
    "P-5": "CP-51",
}

# Parking → crossroad  (для маршрутизации автобусов внутри кластера)
PARKING_TO_CROSSROAD: dict[str, str] = {
    "P-1": "CR-1",
    "P-2": "CR-2",
    "P-3": "CR-3",
    "P-4": "CR-4",
    "P-5": "CR-5",
}


# ---------------------------------------------------------------------------
# Service orchestration coroutines
# ---------------------------------------------------------------------------
async def handle_unload_passengers(h: FlightHandling):
    """Выгрузка пассажиров — публикует задачу pickUpPassengers в tasks.passengerBus."""
    task = h.checklist.get("boarding")
    if not task:
        return
    task.status = TaskStatus.IN_PROGRESS

    gate = PARKING_TO_GATE.get(h.parking_node, "G-11")
    sp = PARKING_TO_SERVICE_POINT.get(h.parking_node, "CP-11")
    task_id = _next_task_id()

    log.info("[%s] publishing pickUpPassengers task #%d", h.flight_id, task_id)
    success = await _publish_and_wait("tasks.passengerBus", {
        "taskId": task_id,
        "taskType": "pickUpPassengers",
        "flightId": h.flight_id,
        "parkingNode": h.parking_node,
        "gate": gate,
        "servicePoint": sp,
        "returnTo": gate,
    })

    if not success:
        log.warning(
            "[%s] RMQ unavailable, HTTP fallback for unload", h.flight_id)
        await safe_post(BUS_HOST, BUS_PORT, "/v1/bus/dispatch", {
            "flightId": h.flight_id, "action": "unload",
            "from": gate, "to": sp, "returnTo": gate, "parkingNode": h.parking_node,
        })
    log.info("[%s] unload passengers — done", h.flight_id)


async def handle_load_passengers(h: FlightHandling):
    """Погрузка пассажиров — публикует задачу deliverPassengers в tasks.passengerBus."""
    task = h.checklist.get("boarding")
    if not task:
        return

    gate = PARKING_TO_GATE.get(h.parking_node, "G-11")
    sp = PARKING_TO_SERVICE_POINT.get(h.parking_node, "CP-11")
    task_id = _next_task_id()

    log.info("[%s] publishing deliverPassengers task #%d",
             h.flight_id, task_id)
    success = await _publish_and_wait("tasks.passengerBus", {
        "taskId": task_id,
        "taskType": "deliverPassengers",
        "flightId": h.flight_id,
        "parkingNode": h.parking_node,
        "gate": gate,
        "servicePoint": sp,
        "returnTo": gate,
    })

    if not success:
        log.warning(
            "[%s] RMQ unavailable, HTTP fallback for load", h.flight_id)
        await safe_post(BUS_HOST, BUS_PORT, "/v1/bus/dispatch", {
            "flightId": h.flight_id, "action": "load",
            "from": gate, "to": sp, "returnTo": gate, "parkingNode": h.parking_node,
        })

    task.status = TaskStatus.COMPLETED
    task.detail = "boarding completed"
    log.info("[%s] load passengers — done", h.flight_id)


async def handle_fuel(h: FlightHandling):
    """Заправка — публикует задачу refuelPlane в tasks.refueler и ждёт завершения."""
    task = h.checklist.get("fuel")
    if not task:
        return
    task.status = TaskStatus.IN_PROGRESS
    task_id = _next_task_id()

    log.info("[%s] publishing refuelPlane task #%d", h.flight_id, task_id)
    success = await _publish_and_wait("tasks.refueler", {
        "taskId": task_id,
        "taskType": "refuelPlane",
        "flightId": h.flight_id,
        "parkingNode": h.parking_node,
    }, timeout=600.0)

    if not success:
        log.warning(
            "[%s] RMQ unavailable, HTTP fallback for refueler", h.flight_id)
        result = await safe_post(REFUELER_HOST, REFUELER_PORT, "/v1/refuel/request", {
            "flightId": h.flight_id,
            "parkingNode": h.parking_node,
        }, retries=2)
        task.detail = "refueled via HTTP fallback"
    else:
        task.detail = "refueled"

    task.status = TaskStatus.COMPLETED
    log.info("[%s] refueling — %s", h.flight_id, task.detail)


async def handle_catering(h: FlightHandling):
    """Питание — публикует задачу deliverFood в tasks.catering и ждёт завершения."""
    task = h.checklist.get("catering")
    if not task:
        return
    task.status = TaskStatus.IN_PROGRESS
    task_id = _next_task_id()

    log.info("[%s] publishing deliverFood task #%d", h.flight_id, task_id)
    success = await _publish_and_wait("tasks.catering", {
        "taskId": task_id,
        "taskType": "deliverFood",
        "flightId": h.flight_id,
        "parkingNode": h.parking_node,
    }, timeout=600.0)

    if not success:
        log.warning(
            "[%s] RMQ unavailable, HTTP fallback for catering", h.flight_id)
        result = await safe_post(CATERING_HOST, CATERING_PORT, "/v1/catering/request", {
            "flightId": h.flight_id,
            "parkingNode": h.parking_node,
        }, retries=2)
        task.detail = "catering via HTTP fallback"
    else:
        task.detail = "catering delivered"

    task.status = TaskStatus.COMPLETED
    log.info("[%s] catering — %s", h.flight_id, task.detail)


async def orchestrate(h: FlightHandling):
    """Главный оркестратор обслуживания рейса."""
    try:
        # Этап 1: разгрузка (пассажиры + багаж) параллельно с заправкой и питанием
        tasks = []
        if "boarding" in h.checklist:
            tasks.append(handle_unload_passengers(h))
        if "fuel" in h.checklist:
            tasks.append(handle_fuel(h))
        if "catering" in h.checklist:
            tasks.append(handle_catering(h))

        if tasks:
            await asyncio.gather(*tasks)

        # Этап 2: Если надо загрузить пассажиров (boarding для вылета)
        # В текущей реализации boarding включает и разгрузку и загрузку
        if "boarding" in h.checklist:
            await handle_load_passengers(h)

        # Проверяем, всё ли завершено
        if h.all_done():
            h.status = "completed"
            log.info("[%s] ALL SERVICING COMPLETED", h.flight_id)
        else:
            failed = [k for k, v in h.checklist.items() if v.status ==
                      TaskStatus.FAILED]
            h.status = "completed_with_issues"
            log.warning("[%s] servicing completed with issues: %s",
                        h.flight_id, failed)

    except Exception as exc:
        log.exception("[%s] orchestration error: %s", h.flight_id, exc)
        h.status = "failed"


# ---------------------------------------------------------------------------
# FastAPI lifespan — RabbitMQ connect / disconnect
# ---------------------------------------------------------------------------
@asynccontextmanager
async def lifespan(app: FastAPI):
    global _rmq_conn, _rmq_ch
    log.info("Connecting to RabbitMQ: %s", RABBITMQ_URL)
    for attempt in range(12):
        try:
            _rmq_conn = await aio_pika.connect_robust(RABBITMQ_URL)
            _rmq_ch = await _rmq_conn.channel()
            for q in ("tasks.catering", "tasks.passengerBus", "tasks.refueler"):
                await _rmq_ch.declare_queue(q, durable=True)
            log.info("RabbitMQ connected and queues declared")
            break
        except Exception as e:
            log.warning("RabbitMQ connect attempt %d failed: %s",
                        attempt + 1, e)
            await asyncio.sleep(3)
    else:
        log.error("Could not connect to RabbitMQ — HTTP fallback will be used")
    yield
    if _rmq_conn and not _rmq_conn.is_closed:
        await _rmq_conn.close()
        log.info("RabbitMQ connection closed")


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------
app = FastAPI(title="Handling Supervisor", version="2.0.0", lifespan=lifespan)


@app.get("/health")
async def health():
    return {
        "service": "HandlingSupervisor",
        "status": "ok",
        "time": int(time.time()),
        "rabbitmq": _rmq_conn is not None and not _rmq_conn.is_closed,
    }


@app.post("/v1/handling/request")
async def request_handling(req: HandlingRequest):
    """
    Board вызывает этот эндпоинт, когда самолёт припаркован.
    """
    log.info(
        "[%s] handling request received: parking=%s services=%s",
        req.flightId,
        req.parkingNode,
        req.services,
    )

    # Идемпотентность: если уже есть — возвращаем текущий статус
    if req.flightId in handlings:
        existing = handlings[req.flightId]
        return {"ok": True, "flightId": req.flightId, "status": existing.status, "idempotent": True}

    h = FlightHandling(req.flightId, req.parkingNode, req.services)
    handlings[req.flightId] = h

    # Запускаем оркестрацию в фоне
    asyncio.create_task(orchestrate(h))

    return {
        "ok": True,
        "flightId": req.flightId,
        "parkingNode": req.parkingNode,
        "status": h.status,
        "checklist": {k: v.model_dump() for k, v in h.checklist.items()},
    }


@app.get("/v1/handling/status/{flight_id}")
async def get_handling_status(flight_id: str):
    """Возвращает текущий статус обслуживания рейса."""
    h = handlings.get(flight_id)
    if not h:
        raise HTTPException(
            status_code=404, detail="handling session not found")
    return h.to_dict()


@app.get("/v1/handling/tasks")
async def list_tasks():
    """Список всех задач обслуживания."""
    return {
        "tasks": [h.to_dict() for h in handlings.values()],
        "total": len(handlings),
    }


@app.put("/v1/tasks/{task_id}/complete")
async def complete_task(task_id: int, payload: dict = Body(default={})):
    """
    Сервис (catering, bus) уведомляет HS о завершении задачи.
    PUT /v1/tasks/{taskId}/complete
    """
    event = _task_events.get(task_id)
    if event:
        event.set()
        log.info("Task %d marked complete via callback", task_id)
        return {"ok": True, "taskId": task_id}
    log.warning(
        "PUT /v1/tasks/%d/complete — unknown taskId (already timed out?)", task_id)
    return {"ok": False, "reason": "unknown taskId"}


@app.post("/v1/handling/complete_checklist_item")
async def complete_checklist_item(payload: dict):
    """
    Внешний сервис (Refueler, Passenger Bus) уведомляет о завершении.
    { "flightId": "SU100", "service": "fuel", "status": "completed" }
    """
    flight_id = payload.get("flightId", "")
    service = payload.get("service", "")
    status = payload.get("status", "completed")

    h = handlings.get(flight_id)
    if not h:
        raise HTTPException(
            status_code=404, detail="handling session not found")

    task = h.checklist.get(service)
    if not task:
        raise HTTPException(
            status_code=404, detail=f"service '{service}' not in checklist")

    task.status = TaskStatus.COMPLETED if status == "completed" else TaskStatus.FAILED
    task.detail = payload.get("detail", "")

    if h.all_done():
        h.status = "completed"
        log.info("[%s] all checklist items completed via callback", flight_id)

    return {"ok": True, "flightId": flight_id, "service": service, "taskStatus": task.status}


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    log.info("Starting Handling Supervisor on port %d", PORT)
    log.info("  GC=%s:%d  BUS=%s:%d  REFUELER=%s:%d  CATERING=%s:%d",
             GC_HOST, GC_PORT, BUS_HOST, BUS_PORT, REFUELER_HOST, REFUELER_PORT,
             CATERING_HOST, CATERING_PORT)
    uvicorn.run(app, host="0.0.0.0", port=PORT, log_level="info")
