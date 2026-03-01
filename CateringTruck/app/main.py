import uvicorn
from fastapi import FastAPI

from app import config
from app.routers.catering_trucks import router

app = FastAPI(
    title="Catering Truck API",
    version="1.0.0",
    description=(
        "Модуль управления машинами катеринга в аэропорту. "
        "Взаимодействует с Ground Control, Handling Supervisor и Board."
    ),
)

app.include_router(router)


if __name__ == "__main__":
    uvicorn.run("app.main:app", host=config.APP_HOST,
                port=config.APP_PORT, reload=True)
