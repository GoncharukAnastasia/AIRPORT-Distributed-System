from typing import Dict, List, Optional

from app.models.catering_truck import CateringTruck


class CateringTruckRepository:
    """In-memory хранилище машин катеринга."""

    def __init__(self) -> None:
        self._store: Dict[str, CateringTruck] = {}

    def get_all(self) -> List[CateringTruck]:
        """Возвращает все машины."""
        return list(self._store.values())

    def get_by_id(self, truck_id: str) -> Optional[CateringTruck]:
        """Возвращает машину по идентификатору или None."""
        return self._store.get(truck_id)

    def save(self, truck: CateringTruck) -> CateringTruck:
        """Сохраняет или обновляет машину."""
        self._store[truck.id] = truck
        return truck

    def exists(self, truck_id: str) -> bool:
        """Проверяет, существует ли машина с данным ID."""
        return truck_id in self._store

    def location_occupied(self, location: str) -> bool:
        """Проверяет, занята ли точка другой машиной."""
        return any(t.currentLocation == location for t in self._store.values())


repository = CateringTruckRepository()
