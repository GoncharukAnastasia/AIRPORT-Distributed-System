import httpx

from app import config


class BoardClient:
    """HTTP-клиент для взаимодействия с Board (бортовой системой самолёта)."""

    def __init__(self, base_url: str = config.BOARD_URL) -> None:
        self._base_url = base_url

    async def get_delivery_details(self, plane_id: str) -> dict:
        """
        Получает детали доставки для конкретного самолёта.
        Возвращает объект с параметрами, необходимыми для загрузки.
        """
        async with httpx.AsyncClient() as client:
            response = await client.get(
                f"{self._base_url}/v1/delivery-details/{plane_id}",
            )
            response.raise_for_status()
            return response.json()


board_client = BoardClient()
