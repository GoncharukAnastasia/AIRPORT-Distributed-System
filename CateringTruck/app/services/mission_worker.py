"""
Mission worker — логика движения и обслуживания машины катеринга.
Полный аналог run_mission_worker() из FollowMe.cpp, но на Python / asyncio.

Флоу:
  landing  (посадка):
      база  ──carRoad──▶  parkingNode  (обслуживание, пустая машина)
      parkingNode ──carRoad──▶  база
      → доклад HS

  takeoff  (взлёт):
      база  ──carRoad──▶  HUB  (загружаемся едой)
      HUB   ──carRoad──▶  parkingNode (доставляем)
      parkingNode ──carRoad──▶  база
      → доклад HS
"""
import asyncio
import logging
from typing import Optional

from app import config
from app.clients.ground_control import GroundControlClient
from app.clients.handling_supervisor import HandlingSupervisorClient
from app.models.catering_truck import CateringTruck, TruckStatus
from app.storage.repository import CateringTruckRepository
from app.utils.map_router import airport_map

logger = logging.getLogger(__name__)


class MissionWorker:
    def __init__(
        self,
        repo: CateringTruckRepository,
        gc: GroundControlClient,
        hs: HandlingSupervisorClient,
    ) -> None:
        self._repo = repo
        self._gc = gc
        self._hs = hs

    # ------------------------------------------------------------------
    # Публичный метод — запускается как фоновая asyncio-задача
    # ------------------------------------------------------------------
    async def run(
        self,
        truck: CateringTruck,
        flight_id: str,
        mission_type: str,   # "landing" | "takeoff"
        parking_node: str,
    ) -> None:
        vehicle_id = truck.id
        base_node = config.CT_BASE_NODE
        hub_node = config.CT_HUB_NODE

        logger.info(
            "[Mission] START vehicle=%s flight=%s type=%s parking=%s",
            vehicle_id, flight_id, mission_type, parking_node,
        )

        try:
            if mission_type == "landing":
                await self._mission_landing(vehicle_id, flight_id, parking_node, base_node)
            else:
                await self._mission_takeoff(vehicle_id, flight_id, parking_node, base_node, hub_node)
        except Exception as e:
            logger.error("[Mission] CRASH vehicle=%s flight=%s: %s",
                         vehicle_id, flight_id, e)
        finally:
            # В любом случае освобождаем машину
            await self._set_status(vehicle_id, TruckStatus.EMPTY, None)
            logger.info("[Mission] DONE vehicle=%s flight=%s",
                        vehicle_id, flight_id)

    # ------------------------------------------------------------------
    # Посадка: едем пустым к самолёту, обслуживаем, возвращаемся
    # ------------------------------------------------------------------
    async def _mission_landing(
        self,
        vehicle_id: str,
        flight_id: str,
        parking_node: str,
        base_node: str,
    ) -> None:
        # 1. база → парковка самолёта
        route_to_plane = airport_map.shortest_path(base_node, parking_node)
        if not route_to_plane:
            logger.error("[Mission] no route %s -> %s",
                         base_node, parking_node)
            return

        await self._set_status(vehicle_id, TruckStatus.MOVE_TO_PLANE, flight_id)
        ok = await self._gc.drive_route(
            vehicle_id,
            route_to_plane,
            on_node_arrived=lambda node: self._on_node(vehicle_id, node),
        )
        if not ok:
            logger.error(
                "[Mission] landing: route to plane failed for %s", vehicle_id)
            await self._notify_hs(flight_id, vehicle_id, mission_type="landing")
            return

        # 2. Обслуживаем самолёт (имитация)
        await self._set_status(vehicle_id, TruckStatus.SERVICING, flight_id)
        logger.info("[Mission] servicing plane at %s for %ds",
                    parking_node, config.CT_SERVICE_SEC)
        await asyncio.sleep(config.CT_SERVICE_SEC)

        # 3. парковка → база
        route_return = airport_map.shortest_path(parking_node, base_node)
        if not route_return:
            logger.error("[Mission] no return route %s -> %s",
                         parking_node, base_node)
            await self._notify_hs(flight_id, vehicle_id, mission_type="landing")
            return

        await self._set_status(vehicle_id, TruckStatus.RETURNING, flight_id)
        await self._gc.drive_route(
            vehicle_id,
            route_return,
            on_node_arrived=lambda node: self._on_node(vehicle_id, node),
        )

        # 4. Доклад HS
        await self._notify_hs(flight_id, vehicle_id, mission_type="landing")

    # ------------------------------------------------------------------
    # Взлёт: едем на хаб (грузимся), затем к самолёту, затем назад
    # ------------------------------------------------------------------
    async def _mission_takeoff(
        self,
        vehicle_id: str,
        flight_id: str,
        parking_node: str,
        base_node: str,
        hub_node: str,
    ) -> None:
        # 1. база → хаб загрузки (если хаб != база)
        if hub_node != base_node:
            route_to_hub = airport_map.shortest_path(base_node, hub_node)
            if not route_to_hub:
                logger.error("[Mission] no route to hub %s -> %s",
                             base_node, hub_node)
                await self._notify_hs(flight_id, vehicle_id, mission_type="takeoff")
                return

            await self._set_status(vehicle_id, TruckStatus.MOVE_TO_HUB, flight_id)
            ok = await self._gc.drive_route(
                vehicle_id,
                route_to_hub,
                on_node_arrived=lambda node: self._on_node(vehicle_id, node),
            )
            if not ok:
                logger.error(
                    "[Mission] takeoff: route to hub failed for %s", vehicle_id)
                await self._notify_hs(flight_id, vehicle_id, mission_type="takeoff")
                return

        # 2. Загружаем еду (имитация)
        logger.info("[Mission] loading food at hub=%s for %ds",
                    hub_node, config.CT_SERVICE_SEC)
        await asyncio.sleep(config.CT_SERVICE_SEC)
        effective_start = hub_node

        # 3. хаб → парковка самолёта
        route_to_plane = airport_map.shortest_path(
            effective_start, parking_node)
        if not route_to_plane:
            logger.error("[Mission] no route hub -> plane %s -> %s",
                         effective_start, parking_node)
            await self._notify_hs(flight_id, vehicle_id, mission_type="takeoff")
            return

        await self._set_status(vehicle_id, TruckStatus.MOVE_TO_PLANE, flight_id)
        ok = await self._gc.drive_route(
            vehicle_id,
            route_to_plane,
            on_node_arrived=lambda node: self._on_node(vehicle_id, node),
        )
        if not ok:
            logger.error(
                "[Mission] takeoff: route to plane failed for %s", vehicle_id)
            await self._notify_hs(flight_id, vehicle_id, mission_type="takeoff")
            return

        # 4. Доставляем еду (имитация обслуживания у самолёта)
        await self._set_status(vehicle_id, TruckStatus.SERVICING, flight_id)
        logger.info("[Mission] delivering food at %s for %ds",
                    parking_node, config.CT_SERVICE_SEC)
        await asyncio.sleep(config.CT_SERVICE_SEC)

        # 5. парковка → база
        route_return = airport_map.shortest_path(parking_node, base_node)
        if not route_return:
            logger.error("[Mission] no return route %s -> %s",
                         parking_node, base_node)
            await self._notify_hs(flight_id, vehicle_id, mission_type="takeoff")
            return

        await self._set_status(vehicle_id, TruckStatus.RETURNING, flight_id)
        await self._gc.drive_route(
            vehicle_id,
            route_return,
            on_node_arrived=lambda node: self._on_node(vehicle_id, node),
        )

        # 6. Доклад HS
        await self._notify_hs(flight_id, vehicle_id, mission_type="takeoff")

    # ------------------------------------------------------------------
    # Вспомогательные методы
    # ------------------------------------------------------------------
    async def _set_status(
        self,
        vehicle_id: str,
        status: TruckStatus,
        flight_id: Optional[str],
    ) -> None:
        truck = self._repo.get_by_id(vehicle_id)
        if truck is None:
            return
        updated = truck.model_copy(update={"status": status})
        if status == TruckStatus.EMPTY:
            # Освобождаем машину — очищаем flight_id в БД
            self._repo.save(updated, clear_flight=True)
        elif flight_id is not None:
            # Устанавливаем flight_id явно
            self._repo.save(updated, flight_id=flight_id)
        else:
            # Только меняем статус/позицию, flight_id не трогаем
            self._repo.save(updated)
        logger.debug("[Mission] %s status → %s", vehicle_id, status.value)

    async def _on_node(self, vehicle_id: str, node: str) -> None:
        """Callback: вызывается после каждого leave-edge — обновляем currentLocation."""
        truck = self._repo.get_by_id(vehicle_id)
        if truck is None:
            return
        updated = truck.model_copy(update={"currentLocation": node})
        # Сохраняем без изменения flight_id (None — не затрёт)
        self._repo.save(updated)

    async def _notify_hs(
        self, flight_id: str, vehicle_id: str, mission_type: str
    ) -> None:
        logger.info(
            "[Mission] notifying HS: flight=%s vehicle=%s type=%s done",
            flight_id, vehicle_id, mission_type,
        )
        await self._hs.notify_stage_complete(flight_id, vehicle_id, mission_type)
