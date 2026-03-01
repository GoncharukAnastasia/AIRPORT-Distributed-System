import httpx

from app import config


class HandlingSupervisorClient:
    """HTTP-клиент для взаимодействия с Handling Supervisor."""

    def __init__(self, base_url: str = config.HANDLING_SUPERVISOR_URL) -> None:
        self._base_url = base_url

    async def notify_delivery_complete(
        self, truck_id: str, plane_id: str
    ) -> dict:
        """
        Уведомляет Handling Supervisor о завершении доставки еды к самолёту.
        Возвращает {success: bool, message: str}.
        """
        async with httpx.AsyncClient() as client:
            response = await client.post(
                f"{self._base_url}/v1/catering-delivery-complete",
                json={"truckId": truck_id, "planeId": plane_id},
            )
            response.raise_for_status()
            return response.json()


handling_supervisor_client = HandlingSupervisorClient()
