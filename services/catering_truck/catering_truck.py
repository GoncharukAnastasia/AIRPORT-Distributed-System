import asyncio
import json
import os
import httpx
import aio_pika
from fastapi import FastAPI, HTTPException, Body
from pydantic import BaseModel
from typing import Dict, List, Optional
import logging
from contextlib import asynccontextmanager

# Конфигурация и URL-ы
GROUND_CONTROL_URL = os.getenv(
    "GROUND_CONTROL_URL", "http://localhost:8081/v1")
HS_URL = os.getenv("HS_URL", "http://localhost:8085")
HS_HOST = os.getenv("HS_HOST", "handling-supervisor")
HS_PORT = int(os.getenv("HS_PORT", "8085"))
PORT = int(os.getenv("CATERING_PORT", "8089"))
RABBITMQ_URL = os.getenv("RABBITMQ_URL", "amqp://guest:guest@rabbitmq:5672/")

# Логирование
logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s - %(levelname)s - %(message)s")
logger = logging.getLogger("CateringTruckAPI")


# Модели данных для Catering Truck
class CateringTruckData(BaseModel):
    id: str
    capacity: int
    status: str
    currentLocation: str
    menu: Dict[str, int]


# Модель запроса на движение (с алиасом для поля "from")
class MovementRequest(BaseModel):
    from_: str = Body(..., alias="from")
    to: str


# Модель запроса на загрузку еды
class LoadFoodRequest(BaseModel):
    menu: Dict[str, int]


# Модель запроса на доставку еды
class DeliverFoodRequest(BaseModel):
    planeId: str


# Класс CateringTruck, который соответствует структуре API
class CateringTruck:
    def __init__(self, id: str, capacity: int, status: str, current_location: str, menu: Dict[str, int],
                 base_location: str = "CR-1"):
        self.id = id
        self.capacity = capacity
        self.status = status
        self.current_location = current_location
        self.menu = menu
        self.base_location = base_location

    def __repr__(self):
        return (f"CateringTruck(id={self.id}, capacity={self.capacity}, status={self.status}, "
                f"current_location={self.current_location}, menu={self.menu})")

    def to_dict(self):
        return {
            "id": self.id,
            "capacity": self.capacity,
            "status": self.status,
            "currentLocation": self.current_location,
            "menu": self.menu
        }


# In-memory хранилище машин
catering_trucks: Dict[str, CateringTruck] = {}

# RabbitMQ connection (catering consumer)
_rmq_conn_c: Optional[aio_pika.RobustConnection] = None

# ---------------------------------------------------------------------------
# Single truck config
# ---------------------------------------------------------------------------
_TRUCK_ID = "CT-1"
_TRUCK_HOME = "CS-1"  # cateringService node

# Parking → nearest service point
_PARKING_TO_CP: Dict[str, str] = {
    "P-1": "CP-11", "P-2": "CP-21", "P-3": "CP-31",
    "P-4": "CP-41", "P-5": "CP-51",
}


# ---------------------------------------------------------------------------
# RabbitMQ consumer for tasks.catering
# ---------------------------------------------------------------------------
async def _consume_catering_tasks() -> None:
    """Listens on tasks.catering queue and runs _catering_serve for each task."""
    global _rmq_conn_c
    logger.info("Connecting catering consumer to RabbitMQ: %s", RABBITMQ_URL)
    for attempt in range(12):
        try:
            _rmq_conn_c = await aio_pika.connect_robust(RABBITMQ_URL)
            channel = await _rmq_conn_c.channel()
            await channel.set_qos(prefetch_count=1)
            queue = await channel.declare_queue("tasks.catering", durable=True)
            logger.info("Catering consumer ready on tasks.catering")

            async with queue.iterator() as q_iter:
                async for message in q_iter:
                    async with message.process():
                        try:
                            payload = json.loads(message.body.decode())
                            task_id = payload.get("taskId", 0)
                            flight_id = payload.get("flightId", "")
                            parking_node = payload.get("parkingNode", "P-1")
                            logger.info(
                                "[%s] received deliverFood task #%d", flight_id, task_id)
                            await _catering_serve(flight_id, parking_node, task_id)
                        except Exception as exc:
                            logger.error(
                                "Error processing catering task: %s", exc)
            return
        except Exception as e:
            logger.warning(
                "Catering RMQ connect attempt %d: %s", attempt + 1, e)
            await asyncio.sleep(3)
    logger.error("Catering consumer could not connect to RabbitMQ")


# FastAPI Lifespan — register single truck CT-1 at CS-1
@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("Starting Catering Truck module")
    catering_trucks[_TRUCK_ID] = CateringTruck(
        id=_TRUCK_ID, capacity=100, status="free",
        current_location=_TRUCK_HOME, base_location=_TRUCK_HOME,
        menu={"chicken": 0, "pork": 0, "fish": 0, "vegetarian": 0}
    )
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.post(
                f"{GROUND_CONTROL_URL}/vehicles/init",
                json={"vehicles": [
                    {"vehicleId": _TRUCK_ID, "currentNode": _TRUCK_HOME}]})
            logger.info(f"GC truck registration: {r.status_code} {r.text}")
    except Exception as e:
        logger.warning(f"GC registration at startup failed: {e}")

    consumer_task = asyncio.create_task(_consume_catering_tasks())
    yield
    consumer_task.cancel()
    if _rmq_conn_c and not _rmq_conn_c.is_closed:
        await _rmq_conn_c.close()
    logger.info("Stopping Catering Truck module")


# Создание FastAPI приложения
app = FastAPI(title="API Catering Truck by Ramazanova Diana",
              lifespan=lifespan)


# ---------------------------------------------------------------------------
# Health check
# ---------------------------------------------------------------------------
@app.get("/health")
def health():
    return {"service": "CateringTruck", "status": "ok"}


# ---------------------------------------------------------------------------
# Эндпоинты для Catering Truck API
# ---------------------------------------------------------------------------

@app.get("/")
def read_root():
    logger.info("Root endpoint accessed")
    return {"message": "Welcome to API Catering Truck. Use /v1/catering-trucks for operations."}


@app.get("/v1/catering-trucks", response_model=List[CateringTruckData])
def get_all_trucks():
    logger.info("Fetching all catering trucks")
    return [truck.to_dict() for truck in catering_trucks.values()]


@app.get("/v1/catering-trucks/{truck_id}", response_model=CateringTruckData)
def get_truck_by_id(truck_id: str):
    logger.info(f"Fetching truck with id: {truck_id}")
    truck = catering_trucks.get(truck_id)
    if not truck:
        logger.warning(f"Truck {truck_id} not found")
        raise HTTPException(status_code=404, detail="Catering Truck not found")
    return truck.to_dict()


@app.post("/v1/catering-trucks/init", response_model=dict)
async def initialize_truck(data: dict):
    """
    Инициализация Catering Truck.
    Тело запроса: { "id": "CT-001", "location": "G11" }
    """
    logger.info(f"Received init request with data: {data}")
    truck_id = data.get("id")
    location = data.get("location")
    if not truck_id or not location:
        logger.error("Missing id or location in request")
        raise HTTPException(status_code=400, detail="Missing id or location")

    try:
        async with httpx.AsyncClient(timeout=10.0) as cli:
            r = await cli.post(
                f"{GROUND_CONTROL_URL}/vehicles/init",
                json={"vehicles": [
                    {"vehicleId": truck_id, "currentNode": location}]}
            )
            logger.info(
                f"Ground Control response: status={r.status_code}, body={r.text}")
            if r.status_code >= 300:
                raise HTTPException(
                    status_code=400, detail=f"Failed to initialize truck: {r.text}")
    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Failed to connect to Ground Control: {str(e)}")
        raise HTTPException(
            status_code=503, detail=f"Ground Control unavailable: {str(e)}")

    truck = CateringTruck(
        id=truck_id,
        capacity=100,
        status="free",
        current_location=location,
        menu={"chicken": 0, "pork": 0, "fish": 0, "vegetarian": 0}
    )
    catering_trucks[truck_id] = truck
    logger.info(f"Truck {truck_id} successfully initialized at {location}")
    return {"success": True, "message": f"Catering Truck {truck_id} initialized at {location}"}


@app.post("/v1/catering-trucks/{truck_id}/load-food", response_model=dict)
def load_food(truck_id: str, load_req: LoadFoodRequest):
    truck = catering_trucks.get(truck_id)
    if not truck:
        raise HTTPException(status_code=404, detail="Catering Truck not found")
    if truck.status == "busy":
        raise HTTPException(status_code=400, detail="Truck is busy")
    total = sum(load_req.menu.values())
    if total > truck.capacity:
        raise HTTPException(
            status_code=400, detail="Menu exceeds truck capacity")
    truck.menu = load_req.menu
    logger.info(f"Food loaded into truck {truck_id}")
    return {"success": True, "message": f"Food loaded into Catering Truck {truck_id}"}


@app.post("/v1/catering-trucks/{truck_id}/deliver-food", response_model=dict)
async def deliver_food(truck_id: str, deliver_req: DeliverFoodRequest):
    truck = catering_trucks.get(truck_id)
    if not truck:
        raise HTTPException(status_code=404, detail="Catering Truck not found")
    if truck.status == "busy":
        raise HTTPException(status_code=400, detail="Truck is busy")
    # Сброс меню после доставки
    truck.menu = {"chicken": 0, "pork": 0, "fish": 0, "vegetarian": 0}
    truck.status = "free"
    logger.info(
        f"Food delivered to plane {deliver_req.planeId} by truck {truck_id}")
    # Уведомление Handling Supervisor (если необходимо)
    try:
        async with httpx.AsyncClient(timeout=10.0) as cli:
            hs_response = await cli.post(
                f"{HS_URL}/notify/deliver-food",
                json={"truckId": truck_id, "planeId": deliver_req.planeId}
            )
            logger.info(
                f"Handling Supervisor notified: {hs_response.status_code}")
    except Exception as e:
        logger.error(f"Failed to notify Handling Supervisor: {str(e)}")
    return {"success": True, "message": f"Food delivered to plane {deliver_req.planeId}"}


async def gc_get_path(from_node: str, to_node: str) -> list:
    """Получаем маршрут у GC через /v1/map/car/path."""
    if from_node == to_node:
        return [from_node]
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.get(
                f"{GROUND_CONTROL_URL}/map/car/path",
                params={"from": from_node, "to": to_node}
            )
            if r.status_code < 300:
                route = r.json().get("route", [])
                if route:
                    return route
    except Exception as e:
        logger.warning(f"GC path request failed: {e}")

    # Fallback: manual BFS using known graph structure
    cr_map = {
        "G-11": "CR-1", "G-12": "CR-1", "BR-11": "CR-1",
        "CP-11": "CR-1", "CP-12": "CR-1",
        "CS-1": "CR-1", "BW-1": "CR-1", "CG-1": "CR-1", "XT-1": "CR-1",
        "G-21": "CR-2", "G-22": "CR-2", "BR-21": "CR-2", "CP-21": "CR-2", "CP-22": "CR-2",
        "G-31": "CR-3", "G-32": "CR-3", "BR-31": "CR-3", "CP-31": "CR-3", "CP-32": "CR-3",
        "G-41": "CR-4", "G-42": "CR-4", "BR-41": "CR-4", "CP-41": "CR-4", "CP-42": "CR-4",
        "G-51": "CR-5", "G-52": "CR-5", "BR-51": "CR-5", "CP-51": "CR-5", "CP-52": "CR-5",
        "RS-1": "CR-5", "FS-1": "CR-5",
    }
    for i in range(1, 6):
        cr_map[f"CR-{i}"] = f"CR-{i}"

    # CR chain: CR-5 - CR-4 - CR-3 - CR-2 - CR-1  (E-14..E-11)
    cr_chain = ["CR-5", "CR-4", "CR-3", "CR-2", "CR-1"]

    cr_from = cr_map.get(from_node, "")
    cr_to = cr_map.get(to_node,   "")
    if not cr_from or not cr_to:
        logger.error(f"No fallback route from {from_node} to {to_node}")
        return []

    # Build segment from -> cr_from
    prefix = [] if from_node == cr_from else [from_node, cr_from]
    # Build segment cr_to -> to
    suffix = [] if to_node == cr_to else [cr_to, to_node]

    if cr_from == cr_to:
        if from_node == cr_from:
            route = [cr_from] if to_node == cr_to else [cr_from, to_node]
        elif to_node == cr_to:
            route = [from_node, cr_from]
        else:
            route = [from_node, cr_from, to_node]
        return route

    # Cross-cluster: walk CR chain between cr_from and cr_to
    try:
        idx_from = cr_chain.index(cr_from)
        idx_to = cr_chain.index(cr_to)
    except ValueError:
        logger.error(f"CR not in chain: {cr_from} or {cr_to}")
        return []

    # Safe slice: works correctly even when idx_to == 0 (avoids -1 end-index trap)
    if idx_from <= idx_to:
        cr_segment = cr_chain[idx_from:idx_to + 1]
    else:
        cr_segment = cr_chain[idx_to:idx_from + 1][::-1]

    full = ([] if from_node == cr_from else [from_node]) + \
        cr_segment + ([] if to_node == cr_to else [to_node])
    return full


async def move_truck(truck_id: str, from_location: str, to_location: str) -> bool:
    """
    Перемещает грузовой автомобиль через GC traffic API
    (enter-edge / leave-edge), аналогично passenger_bus.
    """
    logger.info(f"move_truck {truck_id}: {from_location} → {to_location}")
    truck = catering_trucks.get(truck_id)
    if not truck:
        logger.error(f"Truck {truck_id} not found")
        return False

    if from_location == to_location:
        return True

    route = await gc_get_path(from_location, to_location)
    if len(route) < 2:
        logger.error(f"No valid route: {from_location} → {to_location}")
        return False

    logger.info(f"Route for {truck_id}: {route}")

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
                              "from": seg_from, "to": seg_to}
                    )
                    data = r.json()
                    if r.status_code < 300 and data.get("granted"):
                        break
                    logger.debug(f"{truck_id} enter-edge {seg_from}→{seg_to}: "
                                 f"{data.get('reason', r.status_code)}")
            except Exception as exc:
                logger.warning(f"{truck_id} enter-edge error: {exc}")
            await asyncio.sleep(1.0)
        else:
            logger.error(f"{truck_id} FAILED enter-edge {seg_from}→{seg_to}")
            return False

        await asyncio.sleep(2.0)  # travel time

        # Leave edge
        for attempt in range(10):
            try:
                async with httpx.AsyncClient(timeout=10.0) as cli:
                    r = await cli.post(
                        f"{GROUND_CONTROL_URL}/map/traffic/leave-edge",
                        json={"vehicleId": truck_id, "to": seg_to}
                    )
                    data = r.json()
                    if r.status_code < 300 and data.get("granted"):
                        break
                    logger.debug(f"{truck_id} leave-edge →{seg_to}: "
                                 f"{data.get('reason', r.status_code)}")
            except Exception as exc:
                logger.warning(f"{truck_id} leave-edge error: {exc}")
            await asyncio.sleep(1.0)
        else:
            logger.error(f"{truck_id} FAILED leave-edge →{seg_to}")
            return False

        truck.current_location = seg_to
        logger.info(f"{truck_id} arrived at {seg_to}")

    return True


# ---------------------------------------------------------------------------
# Handling Supervisor integration — full catering cycle
# ---------------------------------------------------------------------------
async def _catering_serve(flight_id: str, parking_node: str, task_id: int = 0) -> None:
    """Background task: drive CT-1 from CS-1 to CP-xx, deliver food, return."""
    cp = _PARKING_TO_CP.get(parking_node, "CP-11")
    truck = catering_trucks.get(_TRUCK_ID)
    if not truck:
        logger.error(f"[{flight_id}] truck {_TRUCK_ID} not found")
        return

    logger.info(
        f"[{flight_id}] catering task: {_TRUCK_ID} {truck.current_location}\u2192{cp}")

    if truck.status == "busy":
        logger.warning(f"[{flight_id}] {_TRUCK_ID} is busy, aborting catering")
        return

    truck.status = "busy"
    truck.menu = {"chicken": 30, "pork": 20, "fish": 20, "vegetarian": 10}
    logger.info(f"[{flight_id}] food loaded into {_TRUCK_ID}")

    success = await move_truck(_TRUCK_ID, truck.current_location, cp)
    if not success:
        logger.warning(f"[{flight_id}] could not move to {cp}, aborting")
        truck.status = "free"
        truck.menu = {"chicken": 0, "pork": 0, "fish": 0, "vegetarian": 0}
        return

    logger.info(f"[{flight_id}] {_TRUCK_ID} arrived at {cp}, delivering food")
    await asyncio.sleep(5)

    truck.menu = {"chicken": 0, "pork": 0, "fish": 0, "vegetarian": 0}
    logger.info(f"[{flight_id}] catering delivered at {cp}")

    await move_truck(_TRUCK_ID, cp, _TRUCK_HOME)
    truck.status = "free"
    logger.info(
        f"[{flight_id}] catering complete, {_TRUCK_ID} back at {_TRUCK_HOME}")

    # Notify HS that task is complete
    if task_id:
        try:
            async with httpx.AsyncClient(timeout=5.0) as cli:
                await cli.put(f"http://{HS_HOST}:{HS_PORT}/v1/tasks/{task_id}/complete")
                logger.info(
                    f"[{flight_id}] PUT /v1/tasks/{task_id}/complete sent to HS")
        except Exception as exc:
            logger.warning(f"[{flight_id}] HS callback failed: {exc}")


class CateringRequestBody(BaseModel):
    flightId: str
    parkingNode: str


@app.post("/v1/catering/request")
async def catering_request(body: CateringRequestBody):
    """Called by Handling Supervisor when a plane needs food service."""
    logger.info(
        f"[{body.flightId}] catering request received for parking {body.parkingNode}")
    asyncio.create_task(_catering_serve(body.flightId, body.parkingNode))
    return {"ok": True, "message": f"Catering started for {body.flightId}"}


@app.post("/v1/catering/reset")
async def catering_reset():
    """Resets CT-1 to free at CS-1."""
    truck = catering_trucks.get(_TRUCK_ID)
    if truck:
        truck.status = "free"
        truck.current_location = _TRUCK_HOME
        truck.base_location = _TRUCK_HOME
        truck.menu = {"chicken": 0, "pork": 0, "fish": 0, "vegetarian": 0}
    else:
        catering_trucks[_TRUCK_ID] = CateringTruck(
            id=_TRUCK_ID, capacity=100, status="free",
            current_location=_TRUCK_HOME, base_location=_TRUCK_HOME,
            menu={"chicken": 0, "pork": 0, "fish": 0, "vegetarian": 0}
        )
    logger.info(f"{_TRUCK_ID} reset to {_TRUCK_HOME}")

    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            await cli.post(
                f"{GROUND_CONTROL_URL}/vehicles/init",
                json={"vehicles": [
                    {"vehicleId": _TRUCK_ID, "currentNode": _TRUCK_HOME}]},
            )
        logger.info(f"{_TRUCK_ID} re-registered in GC at {_TRUCK_HOME}")
    except Exception as exc:
        logger.warning(f"GC re-registration during reset failed: {exc}")

    return {"ok": True, "reset": 1}


@app.post("/v1/catering-trucks/{truck_id}/move", response_model=dict)
async def start_move(truck_id: str, move_req: MovementRequest):
    truck = catering_trucks.get(truck_id)
    if not truck:
        raise HTTPException(status_code=404, detail="Catering Truck not found")
    if truck.status == "busy":
        raise HTTPException(status_code=400, detail="Truck is busy")
    truck.status = "busy"
    success = await move_truck(truck_id, move_req.from_, move_req.to)
    truck.status = "free"
    if success:
        return {"success": True, "message": f"Catering Truck {truck_id} moved from {move_req.from_} to {move_req.to}"}
    else:
        raise HTTPException(
            status_code=400, detail=f"Failed to move Catering Truck {truck_id}")


# Запуск приложения
if __name__ == "__main__":
    import uvicorn
    uvicorn.run("catering_truck:app", host="0.0.0.0", port=PORT, reload=False)
