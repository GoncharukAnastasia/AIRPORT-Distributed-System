"""
Заглушки внешних HTTP-клиентов для изолированного тестирования.
Каждый метод возвращает минимально корректный ответ,
не поднимая никаких реальных соединений.
"""


class MockGroundControlClient:
    async def request_move_permission(
        self, truck_id: str, from_location: str, to: str
    ) -> dict:
        return {"allowed": True, "message": "mock: permission granted"}

    async def notify_move_started(
        self, truck_id: str, from_location: str, to: str
    ) -> dict:
        return {"success": True, "message": "mock: move started"}

    async def notify_arrived(
        self, truck_id: str, from_location: str, to: str
    ) -> dict:
        return {"success": True, "message": "mock: arrived"}


class MockHandlingSupervisorClient:
    async def notify_delivery_complete(
        self, truck_id: str, plane_id: str
    ) -> dict:
        return {"success": True, "message": "mock: delivery complete"}


class MockBoardClient:
    async def get_delivery_details(self, plane_id: str) -> dict:
        return {"planeId": plane_id, "gate": "G11"}
