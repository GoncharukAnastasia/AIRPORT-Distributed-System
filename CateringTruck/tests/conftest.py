import pytest
from httpx import ASGITransport, AsyncClient

from app.dependencies import get_catering_truck_service
from app.main import app
from app.services.catering_truck_service import CateringTruckService
from app.storage.repository import CateringTruckRepository
from tests.mocks import MockBoardClient, MockGroundControlClient, MockHandlingSupervisorClient


@pytest.fixture()
def isolated_service():
    """
    Сервис с чистым in-memory репозиторием и моками всех внешних клиентов.
    Каждый тест получает изолированное состояние.
    """
    return CateringTruckService(
        repository=CateringTruckRepository(),
        ground_control=MockGroundControlClient(),
        handling_supervisor=MockHandlingSupervisorClient(),
        board=MockBoardClient(),
    )


@pytest.fixture()
async def client(isolated_service):
    """
    Асинхронный HTTPX-клиент, смонтированный прямо в ASGI-приложение.
    Зависимость get_catering_truck_service переопределена на изолированный сервис.
    """
    app.dependency_overrides[get_catering_truck_service] = lambda: isolated_service
    async with AsyncClient(
        transport=ASGITransport(app=app), base_url="http://test"
    ) as ac:
        yield ac
    app.dependency_overrides.clear()
