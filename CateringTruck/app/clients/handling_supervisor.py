import logging

import httpx

from app import config

logger = logging.getLogger(__name__)


class HandlingSupervisorClient:
    """HTTP-клиент для взаимодействия с Handling Supervisor."""

    def __init__(self, base_url: str = config.HANDLING_SUPERVISOR_URL) -> None:
        self._base_url = base_url

    async def notify_stage_complete(
        self,
        flight_id: str,
        vehicle_id: str,
        mission_type: str,
    ) -> dict:
        """
        Уведомляет Handling Supervisor о завершении своего этапа обслуживания.
        POST /v1/handling/catering/complete
        {flightId, vehicleId, missionType, status: "completed"}
        """
        try:
            async with httpx.AsyncClient(timeout=10) as client:
                response = await client.post(
                    f"{self._base_url}/v1/handling/catering/complete",
                    json={
                        "flightId":    flight_id,
                        "vehicleId":   vehicle_id,
                        "missionType": mission_type,
                        "status":      "completed",
                    },
                )
                response.raise_for_status()
                return response.json()
        except Exception as e:
            logger.warning("[HSClient] notify_stage_complete failed: %s", e)
            return {"ok": False, "error": str(e)}

    # legacy — оставляем для совместимости (ни на что не влияет)
    async def notify_delivery_complete(self, truck_id: str, plane_id: str) -> dict:
        return await self.notify_stage_complete(plane_id, truck_id, "takeoff")


handling_supervisor_client = HandlingSupervisorClient()
