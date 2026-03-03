"""
Passenger Bus — сервис перевозки пассажиров и багажа автобусами.

Порт по умолчанию: 8086

Управляет флотом автобусов:
  - Инициализация автобусов (POST /v1/bus/init)
  - Диспетчеризация (POST /v1/bus/dispatch)
  - Перемещение по графу с разрешением Ground Control
  - Погрузка / выгрузка пассажиров

Каждый автобус перемещается по carRoad-рёбрам графа аэродрома,
запрашивая разрешения у Ground Control (enter-edge / leave-edge).
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import time
import uuid
from enum import Enum
from typing import Optional

import aio_pika
import httpx
import uvicorn
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] [PassengerBus] [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
log = logging.getLogger("passenger_bus")

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
GC_HOST = os.getenv("GC_HOST", "localhost")
GC_PORT = int(os.getenv("GC_PORT", "8081"))

HANDLING_HOST = os.getenv("HANDLING_HOST", "localhost")
HANDLING_PORT = int(os.getenv("HANDLING_PORT", "8085"))

PORT = int(os.getenv("BUS_PORT", "8086"))

BUS_CAPACITY = int(os.getenv("BUS_CAPACITY", "20"))
TRAVEL_DELAY = float(os.getenv("BUS_TRAVEL_DELAY", "1.0"))  # секунд на ребро
RABBITMQ_URL = os.getenv("RABBITMQ_URL", "amqp://guest:guest@rabbitmq:5672/")

# RabbitMQ connection (bus consumer)
_rmq_conn_b: Optional[aio_pika.RobustConnection] = None

# ---------------------------------------------------------------------------
# Cluster topology — each node belongs to an isolated car-road cluster
# ---------------------------------------------------------------------------
NODE_TO_CLUSTER: dict[str, int] = {}
for _i in range(1, 6):
    NODE_TO_CLUSTER[f"CR-{_i}"] = _i
    NODE_TO_CLUSTER[f"CP-{_i}1"] = _i
    NODE_TO_CLUSTER[f"CP-{_i}2"] = _i
    NODE_TO_CLUSTER[f"G-{_i}1"] = _i
    NODE_TO_CLUSTER[f"G-{_i}2"] = _i
    NODE_TO_CLUSTER[f"BR-{_i}1"] = _i
# Extra nodes in cluster 1
NODE_TO_CLUSTER.update({"CS-1": 1, "BW-1": 1, "CG-1": 1, "XT-1": 1})
# Extra nodes in cluster 5
NODE_TO_CLUSTER.update({"RS-1": 5, "FS-1": 5})

CLUSTER_DEFAULT_GATE: dict[int, str] = {
    1: "G-11", 2: "G-21", 3: "G-31", 4: "G-41", 5: "G-51",
}


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class BusStatus(str, Enum):
    IDLE = "idle"
    EN_ROUTE = "en_route"
    LOADING = "loading"
    UNLOADING = "unloading"
    RETURNING = "returning"
    OUT_OF_SERVICE = "out_of_service"


class Bus:
    def __init__(self, bus_id: str, capacity: int = 20, current_node: str = "CG-1"):
        self.bus_id = bus_id
        self.capacity = capacity
        self.current_node = current_node
        self.status = BusStatus.IDLE
        self.flight_id: str = ""
        self.passengers_on_board: int = 0
        self.mission_log: list[str] = []
        self.created_at = time.time()
        self.lock = asyncio.Lock()

    def to_dict(self) -> dict:
        return {
            "busId": self.bus_id,
            "capacity": self.capacity,
            "currentNode": self.current_node,
            "status": self.status.value,
            "flightId": self.flight_id,
            "passengersOnBoard": self.passengers_on_board,
            "createdAt": self.created_at,
        }


class DispatchRequest(BaseModel):
    flightId: str
    action: str = "unload"  # "unload" | "load"
    fromNode: Optional[str] = None  # пользовательский старт (gate)
    to: str = ""            # целевая точка (service point у самолёта)
    returnTo: str = ""      # куда вернуть автобус
    parkingNode: str = ""   # стоянка самолёта

    # Альтернативные имена (из handling supervisor)
    model_config = {"populate_by_name": True}

    def __init__(self, **data):
        # Поддержка "from" как альтернативного ключа
        if "from" in data and "fromNode" not in data:
            data["fromNode"] = data.pop("from")
        super().__init__(**data)


class InitRequest(BaseModel):
    buses: list[dict]


# ---------------------------------------------------------------------------
# In-memory fleet
# ---------------------------------------------------------------------------
fleet: dict[str, Bus] = {}


# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------
async def gc_post(path: str, payload: dict) -> Optional[dict]:
    """POST к Ground Control с ретраями."""
    url = f"http://{GC_HOST}:{GC_PORT}{path}"
    for attempt in range(30):
        try:
            async with httpx.AsyncClient(timeout=10.0) as cli:
                r = await cli.post(url, json=payload)
                data = r.json()
                if r.status_code < 300 and data.get("granted", False):
                    return data
                # Если не выдано разрешение — ждём и повторяем
                log.debug("GC %s → %s (attempt %d)", path,
                          data.get("reason", r.status_code), attempt)
        except Exception as exc:
            log.debug("GC %s → error: %s (attempt %d)", path, exc, attempt)
        await asyncio.sleep(1.0)
    return None


async def gc_get_path(from_node: str, to_node: str) -> list[str]:
    """
    Получаем маршрут у GC. Если GC не предоставляет маршрут для автобусов напрямую,
    используем простой fallback через crossroad.
    """
    # Пытаемся получить маршрут через GC
    url = f"http://{GC_HOST}:{GC_PORT}/v1/map/car/path?from={from_node}&to={to_node}"
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.get(url)
            if r.status_code < 300:
                data = r.json()
                route = data.get("route", [])
                if route:
                    return route
    except Exception:
        pass

    # Fallback: если from и to в одном кластере CR-x — маршрут через crossroad
    # В текущей карте узлы в кластере связаны через CR-x
    cr_map = {
        "G-11": "CR-1", "G-12": "CR-1", "BR-11": "CR-1", "CP-11": "CR-1", "CP-12": "CR-1",
        "CS-1": "CR-1", "BW-1": "CR-1", "CG-1": "CR-1", "XT-1": "CR-1",
        "G-21": "CR-2", "G-22": "CR-2", "BR-21": "CR-2", "CP-21": "CR-2", "CP-22": "CR-2",
        "G-31": "CR-3", "G-32": "CR-3", "BR-31": "CR-3", "CP-31": "CR-3", "CP-32": "CR-3",
        "G-41": "CR-4", "G-42": "CR-4", "BR-41": "CR-4", "CP-41": "CR-4", "CP-42": "CR-4",
        "G-51": "CR-5", "G-52": "CR-5", "BR-51": "CR-5", "CP-51": "CR-5", "CP-52": "CR-5",
        "RS-1": "CR-5", "FS-1": "CR-5",
    }
    # Crossroads themselves
    for i in range(1, 6):
        cr_map[f"CR-{i}"] = f"CR-{i}"

    cr_from = cr_map.get(from_node, "")
    cr_to = cr_map.get(to_node, "")

    if from_node == to_node:
        return [from_node]

    if cr_from and cr_to and cr_from == cr_to:
        cr = cr_from
        if from_node == cr:
            return [cr, to_node]
        elif to_node == cr:
            return [from_node, cr]
        else:
            return [from_node, cr, to_node]

    # Cross-cluster: walk CR chain
    if cr_from and cr_to and cr_from != cr_to:
        cr_chain = ["CR-5", "CR-4", "CR-3", "CR-2", "CR-1"]
        try:
            idx_from = cr_chain.index(cr_from)
            idx_to = cr_chain.index(cr_to)
        except ValueError:
            log.error("CR not in chain: %s or %s", cr_from, cr_to)
            return []
        if idx_from <= idx_to:
            cr_segment = cr_chain[idx_from:idx_to + 1]
        else:
            cr_segment = cr_chain[idx_to:idx_from + 1][::-1]
        prefix = [] if from_node == cr_from else [from_node]
        suffix = [] if to_node == cr_to else [to_node]
        return prefix + cr_segment + suffix

    # Unknown nodes — try direct (GC will reject if no edge)
    log.warning("No local route from %s to %s — attempting direct",
                from_node, to_node)
    return [from_node, to_node]


# ---------------------------------------------------------------------------
# Drive route (movement through graph via GC traffic API)
# ---------------------------------------------------------------------------
async def drive_route(bus: Bus, route: list[str], flight_id: str = "") -> bool:
    """
    Проводит автобус по маршруту, запрашивая разрешения у Ground Control.
    Возвращает True если успешно.
    """
    if len(route) < 2:
        return True

    vehicle_id = bus.bus_id

    for i in range(len(route) - 1):
        from_node = route[i]
        to_node = route[i + 1]

        log.info("[%s] [%s] entering edge %s → %s",
                 vehicle_id, flight_id, from_node, to_node)

        # Запрос на вход в ребро
        result = await gc_post("/v1/map/traffic/enter-edge", {
            "vehicleId": vehicle_id,
            "flightId": flight_id,
            "from": from_node,
            "to": to_node,
        })

        if not result:
            log.error("[%s] [%s] FAILED to enter edge %s → %s after retries",
                      vehicle_id, flight_id, from_node, to_node)
            return False

        # Имитация движения по ребру
        await asyncio.sleep(TRAVEL_DELAY)

        # Запрос на выход из ребра
        result = await gc_post("/v1/map/traffic/leave-edge", {
            "vehicleId": vehicle_id,
            "flightId": flight_id,
            "to": to_node,
        })

        if not result:
            log.error("[%s] [%s] FAILED to leave edge to %s after retries",
                      vehicle_id, flight_id, to_node)
            return False

        bus.current_node = to_node
        log.info("[%s] [%s] arrived at %s", vehicle_id, flight_id, to_node)

    return True


# ---------------------------------------------------------------------------
# Mission workers
# ---------------------------------------------------------------------------
async def run_unload_mission(bus: Bus, flight_id: str, gate: str, service_point: str, return_to: str, task_id: int = 0):
    """Миссия выгрузки пассажиров с самолёта."""
    async with bus.lock:
        try:
            bus.status = BusStatus.EN_ROUTE
            bus.flight_id = flight_id

            # 1. Едем от текущей позиции к service point (у самолёта)
            route_to_plane = await gc_get_path(bus.current_node, service_point)
            log.info("[%s] UNLOAD mission: route to plane = %s",
                     bus.bus_id, route_to_plane)

            if not route_to_plane or not await drive_route(bus, route_to_plane, flight_id):
                log.error(
                    "[%s] UNLOAD mission aborted — route to plane failed", bus.bus_id)
                bus.status = BusStatus.IDLE
                bus.flight_id = ""
                return

            # 2. Загружаем пассажиров в автобус (выгрузка с самолёта)
            bus.status = BusStatus.LOADING
            bus.passengers_on_board = BUS_CAPACITY
            log.info("[%s] loading %d passengers from plane %s",
                     bus.bus_id, bus.passengers_on_board, flight_id)
            await asyncio.sleep(2.0)  # имитация погрузки

            # 3. Едем к гейту (терминал)
            route_to_gate = await gc_get_path(service_point, gate)
            bus.status = BusStatus.EN_ROUTE
            log.info("[%s] UNLOAD mission: route to gate = %s",
                     bus.bus_id, route_to_gate)

            if not route_to_gate or not await drive_route(bus, route_to_gate, flight_id):
                log.error(
                    "[%s] UNLOAD mission aborted — route to gate failed", bus.bus_id)
                bus.status = BusStatus.IDLE
                bus.flight_id = ""
                return

            # 4. Выгружаем пассажиров в терминал
            bus.status = BusStatus.UNLOADING
            log.info("[%s] unloading %d passengers at gate %s",
                     bus.bus_id, bus.passengers_on_board, gate)
            await asyncio.sleep(2.0)
            bus.passengers_on_board = 0

            # 5. Возвращаемся
            if return_to and return_to != bus.current_node:
                route_return = await gc_get_path(bus.current_node, return_to)
                bus.status = BusStatus.RETURNING
                if await drive_route(bus, route_return, flight_id):
                    bus.current_node = return_to

            bus.status = BusStatus.IDLE
            bus.flight_id = ""
            log.info("[%s] UNLOAD mission COMPLETE for %s",
                     bus.bus_id, flight_id)

            # Notify HS task complete
            if task_id:
                try:
                    async with httpx.AsyncClient(timeout=5.0) as cli:
                        await cli.put(
                            f"http://{HANDLING_HOST}:{HANDLING_PORT}/v1/tasks/{task_id}/complete")
                except Exception:
                    pass

        except Exception as exc:
            log.exception("[%s] UNLOAD mission error: %s", bus.bus_id, exc)
            bus.status = BusStatus.IDLE
            bus.flight_id = ""


async def run_load_mission(bus: Bus, flight_id: str, gate: str, service_point: str, return_to: str, task_id: int = 0):
    """Миссия погрузки пассажиров на самолёт."""
    async with bus.lock:
        try:
            bus.status = BusStatus.EN_ROUTE
            bus.flight_id = flight_id

            # 1. Едем к гейту забрать пассажиров
            route_to_gate = await gc_get_path(bus.current_node, gate)
            log.info("[%s] LOAD mission: route to gate = %s",
                     bus.bus_id, route_to_gate)

            if not route_to_gate or not await drive_route(bus, route_to_gate, flight_id):
                log.error(
                    "[%s] LOAD mission aborted — route to gate failed", bus.bus_id)
                bus.status = BusStatus.IDLE
                bus.flight_id = ""
                return

            # 2. Забираем пассажиров из гейта
            bus.status = BusStatus.LOADING
            bus.passengers_on_board = BUS_CAPACITY
            log.info("[%s] picking up %d passengers at gate %s",
                     bus.bus_id, bus.passengers_on_board, gate)
            await asyncio.sleep(2.0)

            # 3. Едем к самолёту
            route_to_plane = await gc_get_path(gate, service_point)
            bus.status = BusStatus.EN_ROUTE
            log.info("[%s] LOAD mission: route to plane = %s",
                     bus.bus_id, route_to_plane)

            if not route_to_plane or not await drive_route(bus, route_to_plane, flight_id):
                log.error(
                    "[%s] LOAD mission aborted — route to plane failed", bus.bus_id)
                bus.status = BusStatus.IDLE
                bus.flight_id = ""
                return

            # 4. Выгружаем пассажиров на борт
            bus.status = BusStatus.UNLOADING
            log.info("[%s] boarding %d passengers onto plane %s",
                     bus.bus_id, bus.passengers_on_board, flight_id)
            await asyncio.sleep(2.0)
            bus.passengers_on_board = 0

            # 5. Возвращаемся
            if return_to and return_to != bus.current_node:
                route_return = await gc_get_path(bus.current_node, return_to)
                bus.status = BusStatus.RETURNING
                if await drive_route(bus, route_return, flight_id):
                    bus.current_node = return_to

            bus.status = BusStatus.IDLE
            bus.flight_id = ""
            log.info("[%s] LOAD mission COMPLETE for %s",
                     bus.bus_id, flight_id)

            # Notify HS task complete (via PUT or legacy endpoint)
            try:
                async with httpx.AsyncClient(timeout=5.0) as cli:
                    if task_id:
                        await cli.put(
                            f"http://{HANDLING_HOST}:{HANDLING_PORT}/v1/tasks/{task_id}/complete")
                    else:
                        await cli.post(
                            f"http://{HANDLING_HOST}:{HANDLING_PORT}/v1/handling/complete_checklist_item",
                            json={"flightId": flight_id, "service": "boarding",
                                  "status": "completed", "detail": f"bus {bus.bus_id} completed boarding"},
                        )
            except Exception:
                pass

        except Exception as exc:
            log.exception("[%s] LOAD mission error: %s", bus.bus_id, exc)
            bus.status = BusStatus.IDLE
            bus.flight_id = ""


# ---------------------------------------------------------------------------
# RabbitMQ consumer for tasks.passengerBus
# ---------------------------------------------------------------------------
async def _consume_bus_tasks() -> None:
    """Listens on tasks.passengerBus and dispatches bus missions."""
    global _rmq_conn_b
    log.info("Connecting bus consumer to RabbitMQ: %s", RABBITMQ_URL)
    for attempt in range(12):
        try:
            _rmq_conn_b = await aio_pika.connect_robust(RABBITMQ_URL)
            channel = await _rmq_conn_b.channel()
            await channel.set_qos(prefetch_count=1)
            queue = await channel.declare_queue("tasks.passengerBus", durable=True)
            log.info("Bus consumer ready on tasks.passengerBus")

            async with queue.iterator() as q_iter:
                async for message in q_iter:
                    async with message.process():
                        try:
                            payload = json.loads(message.body.decode())
                            task_id = payload.get("taskId", 0)
                            task_type = payload.get("taskType", "")
                            flight_id = payload.get("flightId", "")
                            parking_node = payload.get("parkingNode", "P-1")
                            gate = payload.get("gate", "G-11")
                            sp = payload.get("servicePoint", "CP-11")
                            return_to = payload.get("returnTo", gate)

                            log.info("[%s] received %s task #%d",
                                     flight_id, task_type, task_id)

                            target_cluster = NODE_TO_CLUSTER.get(
                                gate, 0) or NODE_TO_CLUSTER.get(sp, 0)
                            chosen: Optional[Bus] = None
                            for bus in fleet.values():
                                if bus.status == BusStatus.IDLE and NODE_TO_CLUSTER.get(bus.current_node, -1) == target_cluster:
                                    chosen = bus
                                    break

                            if not chosen:
                                bus_id = f"BUS-{uuid.uuid4().hex[:6].upper()}"
                                chosen = Bus(
                                    bus_id=bus_id, capacity=BUS_CAPACITY, current_node=gate)
                                fleet[bus_id] = chosen
                                try:
                                    async with httpx.AsyncClient(timeout=5.0) as cli:
                                        await cli.post(
                                            f"http://{GC_HOST}:{GC_PORT}/v1/vehicles/init",
                                            json={"vehicles": [
                                                {"vehicleId": bus_id, "currentNode": gate}]},
                                        )
                                except Exception:
                                    pass

                            if task_type == "pickUpPassengers":
                                asyncio.create_task(run_unload_mission(
                                    chosen, flight_id, gate, sp, return_to, task_id))
                            else:  # deliverPassengers
                                asyncio.create_task(run_load_mission(
                                    chosen, flight_id, gate, sp, return_to, task_id))

                        except Exception as exc:
                            log.error("Error processing bus task: %s", exc)
            return
        except Exception as e:
            log.warning("Bus RMQ connect attempt %d: %s", attempt + 1, e)
            await asyncio.sleep(3)
    log.error("Bus consumer could not connect to RabbitMQ")


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------
app = FastAPI(title="Passenger Bus", version="2.0.0")


@app.get("/health")
async def health():
    return {
        "service": "PassengerBus",
        "status": "ok",
        "time": int(time.time()),
        "busCount": len(fleet),
    }


@app.post("/v1/bus/init")
async def init_buses(req: InitRequest):
    """Инициализация флота автобусов."""
    created = []
    for b in req.buses:
        bus_id = b.get("busId", f"BUS-{uuid.uuid4().hex[:6].upper()}")
        capacity = b.get("capacity", BUS_CAPACITY)
        node = b.get("currentNode", "CG-1")

        bus = Bus(bus_id=bus_id, capacity=capacity, current_node=node)
        fleet[bus_id] = bus
        created.append(bus_id)
        log.info("Initialized bus %s at %s (capacity=%d)",
                 bus_id, node, capacity)

    # Регистрируем автобусы в Ground Control
    gc_vehicles = [
        {"vehicleId": bid, "currentNode": fleet[bid].current_node} for bid in created]
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            await cli.post(
                f"http://{GC_HOST}:{GC_PORT}/v1/vehicles/init",
                json={"vehicles": gc_vehicles},
            )
    except Exception as exc:
        log.warning("Failed to register buses in GC: %s", exc)

    return {"ok": True, "initialized": created}


@app.post("/v1/bus/reset")
async def reset_buses():
    """
    Forces all buses to IDLE and re-registers them in GC at their current positions.
    This clears any stale edge-occupancy from missions still in progress.
    Called by the visualizer demo reset.
    """
    for bus in fleet.values():
        bus.status = BusStatus.IDLE
        bus.flight_id = ""
        bus.passengers_on_board = 0

    gc_vehicles = [{"vehicleId": b.bus_id, "currentNode": b.current_node}
                   for b in fleet.values()]
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            await cli.post(
                f"http://{GC_HOST}:{GC_PORT}/v1/vehicles/init",
                json={"vehicles": gc_vehicles},
            )
        log.info("All buses reset to IDLE and re-registered in GC")
    except Exception as exc:
        log.warning("GC re-registration during reset failed: %s", exc)
    return {"ok": True, "reset": len(fleet)}


@app.post("/v1/bus/dispatch")
async def dispatch_bus(req: DispatchRequest):
    """
    Диспетчеризация автобуса для перевозки пассажиров.
    Вызывается Handling Supervisor.
    """
    log.info(
        "Dispatch request: flight=%s action=%s from=%s to=%s",
        req.flightId, req.action, req.fromNode, req.to,
    )

    # Determine target cluster from the gate / service-point
    target_cluster = NODE_TO_CLUSTER.get(
        req.fromNode or "", 0) or NODE_TO_CLUSTER.get(req.to or "", 0)

    # Pick an idle bus in the SAME cluster (or at the exact fromNode)
    chosen: Optional[Bus] = None
    for bus in fleet.values():
        if bus.status != BusStatus.IDLE:
            continue
        bus_cluster = NODE_TO_CLUSTER.get(bus.current_node, -1)
        if target_cluster and bus_cluster == target_cluster:
            chosen = bus
            break

    if not chosen:
        # No idle bus in the right cluster → create one at the gate
        bus_id = f"BUS-{uuid.uuid4().hex[:6].upper()}"
        gate = req.fromNode or CLUSTER_DEFAULT_GATE.get(target_cluster, "CG-1")
        chosen = Bus(bus_id=bus_id, capacity=BUS_CAPACITY, current_node=gate)
        fleet[bus_id] = chosen
        log.info("Auto-created bus %s at %s (cluster %d)",
                 bus_id, gate, target_cluster)

        # Регистрируем в GC
        try:
            async with httpx.AsyncClient(timeout=5.0) as cli:
                await cli.post(
                    f"http://{GC_HOST}:{GC_PORT}/v1/vehicles/init",
                    json={"vehicles": [
                        {"vehicleId": bus_id, "currentNode": gate}]},
                )
        except Exception:
            pass

    gate = req.fromNode or "G-11"
    target = req.to or "CP-11"
    return_to = req.returnTo or gate

    # Запускаем миссию в фоне
    if req.action == "unload":
        asyncio.create_task(run_unload_mission(
            chosen, req.flightId, gate, target, return_to))
    else:
        asyncio.create_task(run_load_mission(
            chosen, req.flightId, gate, target, return_to))

    return {
        "ok": True,
        "busId": chosen.bus_id,
        "flightId": req.flightId,
        "action": req.action,
        "status": "dispatched",
    }


@app.get("/v1/bus/vehicles")
async def list_buses():
    """Список всех автобусов и их статусов."""
    return {
        "buses": [bus.to_dict() for bus in fleet.values()],
        "total": len(fleet),
    }


@app.get("/v1/bus/status/{bus_id}")
async def get_bus_status(bus_id: str):
    bus = fleet.get(bus_id)
    if not bus:
        raise HTTPException(status_code=404, detail="bus not found")
    return bus.to_dict()


# ---------------------------------------------------------------------------
# Startup — create default buses + start RabbitMQ consumer
# ---------------------------------------------------------------------------
@app.on_event("startup")
async def on_startup():
    default_count = int(os.getenv("BUS_DEFAULT_COUNT", "5"))
    for i in range(1, default_count + 1):
        bus_id = f"BUS-{i}"
        gates = ["G-11", "G-21", "G-31", "G-41", "G-51"]
        node = gates[(i - 1) % len(gates)]
        bus = Bus(bus_id=bus_id, capacity=BUS_CAPACITY, current_node=node)
        fleet[bus_id] = bus
        log.info("Default bus created: %s at %s", bus_id, node)

    await asyncio.sleep(2)
    gc_vehicles = [{"vehicleId": b.bus_id, "currentNode": b.current_node}
                   for b in fleet.values()]
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            await cli.post(
                f"http://{GC_HOST}:{GC_PORT}/v1/vehicles/init",
                json={"vehicles": gc_vehicles},
            )
        log.info("Registered %d buses in Ground Control", len(gc_vehicles))
    except Exception as exc:
        log.warning("Failed to register default buses in GC: %s", exc)

    asyncio.create_task(_consume_bus_tasks())


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    log.info("Starting Passenger Bus service on port %d", PORT)
    log.info("  GC=%s:%d  HANDLING=%s:%d  RMQ=%s",
             GC_HOST, GC_PORT, HANDLING_HOST, HANDLING_PORT, RABBITMQ_URL)
    uvicorn.run(app, host="0.0.0.0", port=PORT, log_level="info")
