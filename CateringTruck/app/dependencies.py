from app.clients.board import board_client
from app.clients.ground_control import ground_control_client
from app.clients.handling_supervisor import handling_supervisor_client
from app.services.catering_truck_service import CateringTruckService
from app.storage.repository import repository

# Синглтон сервиса — не пересоздаём при каждом запросе,
# чтобы MissionWorker и репозиторий были одними и теми же объектами
_service = CateringTruckService(
    repository=repository,
    ground_control=ground_control_client,
    handling_supervisor=handling_supervisor_client,
    board=board_client,
)


def get_catering_truck_service() -> CateringTruckService:
    """FastAPI-зависимость: предоставляет готовый экземпляр сервиса."""
    return _service
