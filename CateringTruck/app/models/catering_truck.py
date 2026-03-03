from enum import Enum

from pydantic import BaseModel


class TruckStatus(str, Enum):
    """Возможные статусы машины катеринга (аналогично FollowMe)."""

    EMPTY = "empty"           # свободна, стоит на базе
    RESERVED = "reserved"        # зарезервирована под рейс
    MOVE_TO_HUB = "moveToHub"       # едет на хаб загрузки еды (взлёт)
    MOVE_TO_PLANE = "moveToPlane"    # едет к самолёту
    SERVICING = "servicing"       # обслуживает самолёт у стойки
    RETURNING = "returning"       # возвращается на базу

    # legacy aliases (keep for backward compat)
    FREE = "empty"
    BUSY = "moveToPlane"


class Menu(BaseModel):
    """Набор блюд, загруженных в машину."""

    chicken: float = 0
    pork: float = 0
    fish: float = 0
    vegetarian: float = 0

    def total(self) -> float:
        """Суммарное количество всех блюд."""
        return self.chicken + self.pork + self.fish + self.vegetarian


class CateringTruck(BaseModel):
    """Доменная модель машины катеринга."""

    id: str
    capacity: float
    status: TruckStatus
    currentLocation: str
    menu: Menu
