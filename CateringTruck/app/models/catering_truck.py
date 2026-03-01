from enum import Enum

from pydantic import BaseModel


class TruckStatus(str, Enum):
    """Возможные статусы машины катеринга."""

    FREE = "free"
    BUSY = "busy"


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
