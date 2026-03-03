import asyncio
import logging

import httpx

from app import config

logger = logging.getLogger(__name__)


class GroundControlClient:
    """
    HTTP-клиент для взаимодействия с Ground Control.
    Использует реальный API GC:
      POST /v1/vehicles/init      — регистрация машин при старте
      POST /v1/map/traffic/enter-edge  — запрос въезда в ребро
      POST /v1/map/traffic/leave-edge  — подтверждение выезда из ребра
    """

    def __init__(self, base_url: str = config.GROUND_CONTROL_URL) -> None:
        self._base_url = base_url

    # ------------------------------------------------------------------
    # Регистрация машин при старте
    # ------------------------------------------------------------------
    async def init_vehicles(self, vehicles: list[dict]) -> dict:
        """
        Регистрирует машины в GC.
        vehicles: [{vehicleId, currentNode, status}, ...]
        """
        async with httpx.AsyncClient(timeout=10) as client:
            response = await client.post(
                f"{self._base_url}/v1/vehicles/init",
                json={"vehicles": vehicles},
            )
            response.raise_for_status()
            return response.json()

    # ------------------------------------------------------------------
    # Трафик: пошаговое движение по рёбрам
    # ------------------------------------------------------------------
    async def enter_edge(self, vehicle_id: str, frm: str, to: str) -> dict:
        """
        Запрашивает разрешение въехать в ребро frm->to.
        Возвращает {granted: bool, edge: str, ...}.
        """
        async with httpx.AsyncClient(timeout=5) as client:
            response = await client.post(
                f"{self._base_url}/v1/map/traffic/enter-edge",
                json={"vehicleId": vehicle_id, "from": frm, "to": to},
            )
            if response.status_code in (200, 409):
                return response.json()
            response.raise_for_status()
            return response.json()

    async def leave_edge(self, vehicle_id: str, to: str) -> dict:
        """
        Уведомляет GC о выезде из ребра в узел to.
        Возвращает {granted: bool, ...}.
        """
        async with httpx.AsyncClient(timeout=5) as client:
            response = await client.post(
                f"{self._base_url}/v1/map/traffic/leave-edge",
                json={"vehicleId": vehicle_id, "to": to},
            )
            if response.status_code in (200, 409):
                return response.json()
            response.raise_for_status()
            return response.json()

    # ------------------------------------------------------------------
    # Движение по маршруту: шаг за шагом с ожиданием granted
    # ------------------------------------------------------------------
    async def drive_route(
        self,
        vehicle_id: str,
        route_nodes: list[str],
        on_node_arrived=None,          # callback(node: str) -> Awaitable
        travel_sec: int = config.CT_EDGE_TRAVEL_SEC,
        wait_timeout_sec: int = config.CT_EDGE_WAIT_TIMEOUT_SEC,
    ) -> bool:
        """
        Ведёт машину по маршруту route_nodes пошагово через enter-edge / leave-edge.
        Возвращает True если дошли до конца, False при таймауте или ошибке.
        on_node_arrived(node) вызывается после прибытия в каждый узел (кроме начального).
        """
        if len(route_nodes) < 2:
            return True

        for i in range(len(route_nodes) - 1):
            frm = route_nodes[i]
            to = route_nodes[i + 1]

            # --- enter-edge: ждём granted ---
            waited = 0
            while True:
                try:
                    result = await self.enter_edge(vehicle_id, frm, to)
                    if result.get("granted", False):
                        break
                except Exception as e:
                    logger.warning(
                        "[GCClient] enter-edge error %s->%s: %s", frm, to, e)

                await asyncio.sleep(1)
                waited += 1
                if waited >= wait_timeout_sec:
                    logger.error(
                        "[GCClient] enter-edge timeout %s->%s", frm, to)
                    return False

            # --- имитация движения по ребру ---
            await asyncio.sleep(travel_sec)

            # --- leave-edge: ждём granted ---
            waited = 0
            while True:
                try:
                    result = await self.leave_edge(vehicle_id, to)
                    if result.get("granted", False):
                        break
                except Exception as e:
                    logger.warning(
                        "[GCClient] leave-edge error ->%s: %s", to, e)

                await asyncio.sleep(1)
                waited += 1
                if waited >= wait_timeout_sec:
                    logger.error("[GCClient] leave-edge timeout ->%s", to)
                    return False

            logger.info("[GCClient] %s arrived at %s", vehicle_id, to)
            if on_node_arrived:
                await on_node_arrived(to)

        return True


ground_control_client = GroundControlClient()
