import asyncio
import logging

from fastapi import HTTPException

from app.clients.board import BoardClient
from app.clients.ground_control import GroundControlClient
from app.clients.handling_supervisor import HandlingSupervisorClient
from app.models.catering_truck import CateringTruck, Menu, TruckStatus
from app.schemas.catering_truck import (
    DeliverFoodRequest,
    InitCateringTruckRequest,
    MenuLoadRequest,
    MoveRequest,
    StartHandlingRequest,
)
from app.services.mission_worker import MissionWorker
from app.storage.repository import CateringTruckRepository

logger = logging.getLogger(__name__)


class CateringTruckService:
    """Бизнес-логика управления машинами катеринга."""

    def __init__(
        self,
        repository: CateringTruckRepository,
        ground_control: GroundControlClient,
        handling_supervisor: HandlingSupervisorClient,
        board: BoardClient,
    ) -> None:
        self._repo = repository
        self._gc = ground_control
        self._hs = handling_supervisor
        self._board = board
        self._worker = MissionWorker(
            repo=repository, gc=ground_control, hs=handling_supervisor)

    def get_all(self) -> list[CateringTruck]:
        """Возвращает список всех зарегистрированных машин."""
        return self._repo.get_all()

    def get_by_id(self, truck_id: str) -> CateringTruck:
        """Возвращает машину по ID или выбрасывает 404."""
        truck = self._repo.get_by_id(truck_id)
        if truck is None:
            raise HTTPException(
                status_code=404, detail=f"Catering truck '{truck_id}' not found")
        return truck

    def init_truck(self, data: InitCateringTruckRequest) -> dict:
        """
        Инициализирует новую машину в заданной точке карты.
        Возвращает {success, message}.
        """
        if self._repo.exists(data.id):
            raise HTTPException(
                status_code=400,
                detail=f"Catering truck '{data.id}' already exists",
            )
        if self._repo.location_occupied(data.location):
            raise HTTPException(
                status_code=400,
                detail=f"Location '{data.location}' is already occupied",
            )
        truck = CateringTruck(
            id=data.id,
            capacity=100,
            status=TruckStatus.EMPTY,
            currentLocation=data.location,
            menu=Menu(),
        )
        self._repo.save(truck)
        return {"success": True, "message": f"Truck '{data.id}' initialized at '{data.location}'"}

    # ------------------------------------------------------------------
    # Основной endpoint: запуск миссии от HandlingSuperviser
    # ------------------------------------------------------------------
    async def start_handling(self, data: StartHandlingRequest) -> dict:
        """
        Вызывается HandlingSuperviser когда нужно обслужить самолёт.
        Находит свободную машину, резервирует её, запускает фоновую asyncio-задачу.
        """
        truck = self._repo.pick_free()
        if truck is None:
            raise HTTPException(
                status_code=409,
                detail="No free catering truck available",
            )

        # Резервируем
        reserved = truck.model_copy(update={"status": TruckStatus.RESERVED})
        self._repo.save(reserved, flight_id=data.flight_id)

        logger.info(
            "[Service] reserved %s for flight=%s type=%s parking=%s",
            truck.id, data.flight_id, data.mission_type, data.parking_node,
        )

        # Запускаем фоновую задачу (не блокируемся)
        asyncio.create_task(
            self._worker.run(
                truck=reserved,
                flight_id=data.flight_id,
                mission_type=data.mission_type,
                parking_node=data.parking_node,
            )
        )

        return {
            "ok": True,
            "vehicleId": truck.id,
            "flightId": data.flight_id,
            "missionType": data.mission_type,
            "status": "reserved",
        }

    # ------------------------------------------------------------------
    # Старые методы (оставляем для совместимости)
    # ------------------------------------------------------------------
    async def request_move_permission(
        self, truck_id: str, from_location: str, to: str
    ) -> dict:
        self.get_by_id(truck_id)
        try:
            return await self._gc.enter_edge(truck_id, from_location, to)
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Ground Control unavailable: {exc}") from exc

    async def move(self, truck_id: str, data: MoveRequest) -> dict:
        truck = self.get_by_id(truck_id)
        try:
            result = await self._gc.enter_edge(truck_id, data.from_location, data.to)
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Ground Control unavailable: {exc}") from exc
        truck = truck.model_copy(update={"status": TruckStatus.MOVE_TO_PLANE})
        self._repo.save(truck)
        return result

    async def arrived(self, truck_id: str, data: MoveRequest) -> dict:
        truck = self.get_by_id(truck_id)
        try:
            result = await self._gc.leave_edge(truck_id, data.to)
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Ground Control unavailable: {exc}") from exc
        truck = truck.model_copy(
            update={"currentLocation": data.to, "status": TruckStatus.EMPTY})
        self._repo.save(truck)
        return result

    def load_food(self, truck_id: str, data: MenuLoadRequest) -> dict:
        truck = self.get_by_id(truck_id)
        incoming = data.menu
        new_menu = Menu(
            chicken=truck.menu.chicken + incoming.chicken,
            pork=truck.menu.pork + incoming.pork,
            fish=truck.menu.fish + incoming.fish,
            vegetarian=truck.menu.vegetarian + incoming.vegetarian,
        )
        if new_menu.total() > truck.capacity:
            raise HTTPException(
                status_code=400,
                detail=(
                    f"Capacity exceeded: max {truck.capacity}, "
                    f"requested total {new_menu.total()}"
                ),
            )
        truck = truck.model_copy(update={"menu": new_menu})
        self._repo.save(truck)
        return {"success": True, "message": "Food loaded successfully"}

    async def deliver_food(self, truck_id: str, data: DeliverFoodRequest) -> dict:
        truck = self.get_by_id(truck_id)
        try:
            await self._board.get_delivery_details(data.planeId)
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Board service unavailable: {exc}"
            ) from exc
        try:
            await self._hs.notify_stage_complete(data.planeId, truck_id, "takeoff")
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Handling Supervisor unavailable: {exc}"
            ) from exc
        cleared_truck = truck.model_copy(
            update={"menu": Menu(), "status": TruckStatus.EMPTY}
        )
        self._repo.save(cleared_truck)
        return {
            "success": True,
            "message": f"Food delivered to plane '{data.planeId}' by truck '{truck_id}'",
        }

    async def request_move_permission(
        self, truck_id: str, from_location: str, to: str
    ) -> dict:
        """
        Запрашивает у Ground Control разрешение на движение.
        Возвращает {allowed, message}.
        """
        self.get_by_id(truck_id)
        try:
            return await self._gc.request_move_permission(truck_id, from_location, to)
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Ground Control unavailable: {exc}") from exc

    async def move(self, truck_id: str, data: MoveRequest) -> dict:
        """
        Регистрирует начало движения в Ground Control, ставит статус BUSY.
        Возвращает {success, message}.
        """
        truck = self.get_by_id(truck_id)
        try:
            result = await self._gc.notify_move_started(
                truck_id, data.from_location, data.to
            )
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Ground Control unavailable: {exc}") from exc

        truck = truck.model_copy(update={"status": TruckStatus.BUSY})
        self._repo.save(truck)
        return result

    async def arrived(self, truck_id: str, data: MoveRequest) -> dict:
        """
        Подтверждает прибытие в Ground Control, обновляет местоположение машины.
        Возвращает {success, message}.
        """
        truck = self.get_by_id(truck_id)
        try:
            result = await self._gc.notify_arrived(truck_id, data.from_location, data.to)
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Ground Control unavailable: {exc}") from exc

        truck = truck.model_copy(
            update={"currentLocation": data.to, "status": TruckStatus.FREE})
        self._repo.save(truck)
        return result

    def load_food(self, truck_id: str, data: MenuLoadRequest) -> dict:
        """
        Загружает еду в машину.
        Проверяет, что суммарный объём не превышает вместимость.
        Возвращает {success, message}.
        """
        truck = self.get_by_id(truck_id)
        incoming = data.menu
        new_menu = Menu(
            chicken=truck.menu.chicken + incoming.chicken,
            pork=truck.menu.pork + incoming.pork,
            fish=truck.menu.fish + incoming.fish,
            vegetarian=truck.menu.vegetarian + incoming.vegetarian,
        )
        if new_menu.total() > truck.capacity:
            raise HTTPException(
                status_code=400,
                detail=(
                    f"Capacity exceeded: max {truck.capacity}, "
                    f"requested total {new_menu.total()}"
                ),
            )
        truck = truck.model_copy(update={"menu": new_menu})
        self._repo.save(truck)
        return {"success": True, "message": "Food loaded successfully"}

    async def deliver_food(self, truck_id: str, data: DeliverFoodRequest) -> dict:
        """
        Доставляет еду к самолёту:
        1. Уточняет детали у Board.
        2. Уведомляет Handling Supervisor о завершении доставки.
        3. Сбрасывает меню и статус машины.
        Возвращает {success, message}.
        """
        truck = self.get_by_id(truck_id)

        try:
            await self._board.get_delivery_details(data.planeId)
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Board service unavailable: {exc}"
            ) from exc

        try:
            await self._hs.notify_delivery_complete(truck_id, data.planeId)
        except Exception as exc:
            raise HTTPException(
                status_code=503, detail=f"Handling Supervisor unavailable: {exc}"
            ) from exc

        cleared_truck = truck.model_copy(
            update={"menu": Menu(), "status": TruckStatus.FREE}
        )
        self._repo.save(cleared_truck)
        return {
            "success": True,
            "message": f"Food delivered to plane '{data.planeId}' by truck '{truck_id}'",
        }
