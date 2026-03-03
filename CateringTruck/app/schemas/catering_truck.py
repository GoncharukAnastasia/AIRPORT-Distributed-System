from typing import Literal

from pydantic import BaseModel, Field


class MenuSchema(BaseModel):
    """Набор блюд."""

    chicken: float = 0
    pork: float = 0
    fish: float = 0
    vegetarian: float = 0


class InitCateringTruckRequest(BaseModel):
    """Тело запроса на инициализацию машины."""

    id: str
    location: str


class MoveRequest(BaseModel):
    """Тело запроса на движение / подтверждение прибытия."""

    from_location: str = Field(alias="from")
    to: str

    model_config = {"populate_by_name": True}


class MenuLoadRequest(BaseModel):
    """Тело запроса на загрузку еды."""

    menu: MenuSchema


class DeliverFoodRequest(BaseModel):
    """Тело запроса на доставку еды к самолёту."""

    planeId: str


class StartHandlingRequest(BaseModel):
    """
    Тело запроса от HandlingSuperviser: начать обслуживание самолёта.
    """
    flight_id:    str = Field(alias="flightId")
    mission_type: Literal["landing", "takeoff"] = Field(
        alias="missionType", default="landing")
    parking_node: str = Field(alias="parkingNode")

    model_config = {"populate_by_name": True}


class SuccessResponse(BaseModel):
    """Стандартный ответ об успехе операции."""

    success: bool
    message: str


class MovePermissionResponse(BaseModel):
    """Ответ на запрос разрешения движения."""

    allowed: bool
    message: str
