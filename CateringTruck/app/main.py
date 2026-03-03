import logging
from contextlib import asynccontextmanager

import uvicorn
from fastapi import FastAPI

from app import config
from app.clients.ground_control import ground_control_client
from app.routers.catering_trucks import router
from app.storage.repository import repository
from app.utils.map_router import airport_map

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    # ----------------------------------------------------------------
    # STARTUP
    # ----------------------------------------------------------------
    logger.info("[Startup] Loading airport map from %s", config.MAP_PATH)
    airport_map.load(config.MAP_PATH)

    logger.info("[Startup] Ensuring DB schema")
    try:
        repository.ensure_schema()
    except Exception as e:
        logger.error("[Startup] DB schema failed: %s", e)

    logger.info("[Startup] Loading trucks from DB")
    trucks = repository.load_all_from_db()
    logger.info("[Startup] Loaded %d trucks", len(trucks))

    # Регистрируем машины в GroundControl (так же как FollowMe)
    if trucks:
        vehicles = [
            {"vehicleId": t.id, "currentNode": t.currentLocation,
                "status": t.status.value}
            for t in trucks
        ]
        try:
            result = await ground_control_client.init_vehicles(vehicles)
            logger.info("[Startup] Registered %d trucks in GC: %s",
                        len(vehicles), result)
        except Exception as e:
            logger.warning(
                "[Startup] GC registration failed (will retry on first use): %s", e)

    yield
    # ----------------------------------------------------------------
    # SHUTDOWN (nothing special needed)
    # ----------------------------------------------------------------


app = FastAPI(
    title="Catering Truck API",
    version="2.0.0",
    description=(
        "Модуль управления машинами катеринга в аэропорту. "
        "Перемещается по карте аэропорта через Ground Control аналогично FollowMe."
    ),
    lifespan=lifespan,
)

app.include_router(router)


if __name__ == "__main__":
    uvicorn.run("app.main:app", host=config.APP_HOST,
                port=config.APP_PORT, reload=False)
