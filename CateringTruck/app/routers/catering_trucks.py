from typing import List

from fastapi import APIRouter, Depends, Query

from app import config
from app.dependencies import get_catering_truck_service
from app.models.catering_truck import CateringTruck
from app.schemas.catering_truck import (
    DeliverFoodRequest,
    InitCateringTruckRequest,
    MenuLoadRequest,
    MovePermissionResponse,
    MoveRequest,
    StartHandlingRequest,
    SuccessResponse,
)
from app.services.catering_truck_service import CateringTruckService

router = APIRouter(tags=["Catering Trucks"])


# -----------------------------------------------------------------------
# Health check (обязательно для Docker healthcheck)
# -----------------------------------------------------------------------
@router.get("/health")
def health():
    """Health-check endpoint."""
    return {"service": "CateringTruck", "status": "ok"}


# -----------------------------------------------------------------------
# Endpoint для HandlingSuperviser: запустить обслуживание
# -----------------------------------------------------------------------
@router.post("/v1/catering/handling/start")
async def start_handling(
    body: StartHandlingRequest,
    service: CateringTruckService = Depends(get_catering_truck_service),
):
    """
    HandlingSuperviser вызывает этот endpoint когда нужно обслужить рейс.
    Тело: {flightId, missionType: landing|takeoff, parkingNode}
    """
    return await service.start_handling(body)


# -----------------------------------------------------------------------
# CRUD для машин
# -----------------------------------------------------------------------
@router.get("/v1/catering-trucks", response_model=List[CateringTruck])
def list_trucks(
    service: CateringTruckService = Depends(get_catering_truck_service),
):
    """Возвращает список всех зарегистрированных машин катеринга."""
    return service.get_all()


@router.post("/v1/catering-trucks/init", response_model=SuccessResponse)
def init_truck(
    body: InitCateringTruckRequest,
    service: CateringTruckService = Depends(get_catering_truck_service),
):
    """Инициализирует новую машину катеринга в заданной точке карты."""
    return service.init_truck(body)


@router.get("/v1/catering-trucks/{truck_id}", response_model=CateringTruck)
def get_truck(
    truck_id: str,
    service: CateringTruckService = Depends(get_catering_truck_service),
):
    """Возвращает информацию о конкретной машине по её ID."""
    return service.get_by_id(truck_id)


@router.get("/v1/catering-trucks/{truck_id}/move-permission", response_model=MovePermissionResponse)
async def move_permission(
    truck_id: str,
    from_location: str = Query(alias="from"),
    to: str = Query(),
    service: CateringTruckService = Depends(get_catering_truck_service),
):
    """Запрашивает у Ground Control разрешение на движение машины."""
    return await service.request_move_permission(truck_id, from_location, to)


@router.post("/v1/catering-trucks/{truck_id}/move", response_model=SuccessResponse)
async def move(
    truck_id: str,
    body: MoveRequest,
    service: CateringTruckService = Depends(get_catering_truck_service),
):
    """Регистрирует начало движения машины в Ground Control."""
    return await service.move(truck_id, body)


@router.post("/v1/catering-trucks/{truck_id}/arrived", response_model=SuccessResponse)
async def arrived(
    truck_id: str,
    body: MoveRequest,
    service: CateringTruckService = Depends(get_catering_truck_service),
):
    """Подтверждает прибытие машины в конечную точку."""
    return await service.arrived(truck_id, body)


@router.post("/v1/catering-trucks/{truck_id}/load-food", response_model=SuccessResponse)
def load_food(
    truck_id: str,
    body: MenuLoadRequest,
    service: CateringTruckService = Depends(get_catering_truck_service),
):
    """Загружает указанный набор блюд в машину катеринга."""
    return service.load_food(truck_id, body)


@router.post("/v1/catering-trucks/{truck_id}/deliver-food", response_model=SuccessResponse)
async def deliver_food(
    truck_id: str,
    body: DeliverFoodRequest,
    service: CateringTruckService = Depends(get_catering_truck_service),
):
    """Доставляет еду к самолёту и уведомляет Handling Supervisor."""
    return await service.deliver_food(truck_id, body)
