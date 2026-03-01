import httpx

from app import config


class GroundControlClient:
    """HTTP-клиент для взаимодействия с Ground Control."""

    def __init__(self, base_url: str = config.GROUND_CONTROL_URL) -> None:
        self._base_url = base_url

    async def request_move_permission(
        self, truck_id: str, from_location: str, to: str
    ) -> dict:
        """
        Запрашивает разрешение на движение из точки A в точку B.
        Возвращает {allowed: bool, message: str}.
        """
        async with httpx.AsyncClient() as client:
            response = await client.get(
                f"{self._base_url}/v1/move-permission",
                params={"truckId": truck_id, "from": from_location, "to": to},
            )
            response.raise_for_status()
            return response.json()

    async def notify_move_started(
        self, truck_id: str, from_location: str, to: str
    ) -> dict:
        """
        Уведомляет Ground Control о начале движения.
        Возвращает {success: bool, message: str}.
        """
        async with httpx.AsyncClient() as client:
            response = await client.post(
                f"{self._base_url}/v1/move",
                json={"truckId": truck_id, "from": from_location, "to": to},
            )
            response.raise_for_status()
            return response.json()

    async def notify_arrived(
        self, truck_id: str, from_location: str, to: str
    ) -> dict:
        """
        Уведомляет Ground Control о прибытии в конечную точку.
        Возвращает {success: bool, message: str}.
        """
        async with httpx.AsyncClient() as client:
            response = await client.post(
                f"{self._base_url}/v1/arrived",
                json={"truckId": truck_id, "from": from_location, "to": to},
            )
            response.raise_for_status()
            return response.json()


ground_control_client = GroundControlClient()
