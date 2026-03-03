import logging
import threading
from typing import Dict, List, Optional

import psycopg2
import psycopg2.extras

from app import config
from app.models.catering_truck import CateringTruck, Menu, TruckStatus

logger = logging.getLogger(__name__)


class CateringTruckRepository:
    """
    PostgreSQL-хранилище машин катеринга.
    Кэширует данные в памяти для быстрого доступа; БД — источник истины при рестарте.
    """

    def __init__(self, dsn: str = config.PG_DSN) -> None:
        self._dsn = dsn
        self._lock = threading.Lock()
        self._cache: Dict[str, CateringTruck] = {}

    # ------------------------------------------------------------------
    # Инициализация схемы
    # ------------------------------------------------------------------
    def ensure_schema(self) -> None:
        """Создаёт таблицу если её ещё нет."""
        with psycopg2.connect(self._dsn) as conn:
            with conn.cursor() as cur:
                cur.execute("""
                    CREATE TABLE IF NOT EXISTS catering_trucks (
                        vehicle_id   TEXT PRIMARY KEY,
                        current_node TEXT NOT NULL DEFAULT 'FS-1',
                        status       TEXT NOT NULL DEFAULT 'empty',
                        flight_id    TEXT NULL,
                        mission_type TEXT NULL,
                        updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
                    )
                """)
            conn.commit()

    # ------------------------------------------------------------------
    # Загрузка из БД в кэш при старте
    # ------------------------------------------------------------------
    def load_all_from_db(self) -> List[CateringTruck]:
        """Загружает все машины из БД, наполняет кэш."""
        try:
            with psycopg2.connect(self._dsn) as conn:
                with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
                    cur.execute(
                        "SELECT vehicle_id, current_node, status, flight_id FROM catering_trucks"
                    )
                    rows = cur.fetchall()
        except Exception as e:
            logger.error("[Repository] load_all_from_db failed: %s", e)
            return []

        trucks = []
        with self._lock:
            self._cache.clear()
            for row in rows:
                truck = CateringTruck(
                    id=row["vehicle_id"],
                    capacity=100,
                    status=TruckStatus(row["status"]),
                    currentLocation=row["current_node"],
                    menu=Menu(),
                )
                self._cache[truck.id] = truck
                trucks.append(truck)
        logger.info("[Repository] loaded %d trucks from DB", len(trucks))
        return trucks

    def _upsert_db(self, truck: CateringTruck, flight_id: Optional[str] = None, clear_flight: bool = False) -> None:
        """
        Сохраняет или обновляет одну машину в БД.
        flight_id=None + clear_flight=False → flight_id в БД НЕ меняется (COALESCE).
        flight_id=<str>                    → устанавливается явно.
        clear_flight=True                  → flight_id обнуляется (NULL).
        """
        try:
            with psycopg2.connect(self._dsn) as conn:
                with conn.cursor() as cur:
                    if flight_id is not None or clear_flight:
                        # Явное обновление flight_id
                        cur.execute("""
                            INSERT INTO catering_trucks
                                (vehicle_id, current_node, status, flight_id, updated_at)
                            VALUES (%s, %s, %s, %s, NOW())
                            ON CONFLICT (vehicle_id) DO UPDATE SET
                                current_node = EXCLUDED.current_node,
                                status       = EXCLUDED.status,
                                flight_id    = EXCLUDED.flight_id,
                                updated_at   = NOW()
                        """, (
                            truck.id,
                            truck.currentLocation,
                            truck.status.value,
                            flight_id if not clear_flight else None,
                        ))
                    else:
                        # НЕ трогаем flight_id (только позиция и статус)
                        cur.execute("""
                            INSERT INTO catering_trucks
                                (vehicle_id, current_node, status, updated_at)
                            VALUES (%s, %s, %s, NOW())
                            ON CONFLICT (vehicle_id) DO UPDATE SET
                                current_node = EXCLUDED.current_node,
                                status       = EXCLUDED.status,
                                updated_at   = NOW()
                        """, (
                            truck.id,
                            truck.currentLocation,
                            truck.status.value,
                        ))
                conn.commit()
        except Exception as e:
            logger.error("[Repository] upsert failed for %s: %s", truck.id, e)

    # ------------------------------------------------------------------
    # Публичный интерфейс (такой же, как был in-memory)
    # ------------------------------------------------------------------
    def get_all(self) -> List[CateringTruck]:
        with self._lock:
            return list(self._cache.values())

    def get_by_id(self, truck_id: str) -> Optional[CateringTruck]:
        with self._lock:
            return self._cache.get(truck_id)

    def save(self, truck: CateringTruck, flight_id: Optional[str] = None, clear_flight: bool = False) -> CateringTruck:
        """Сохраняет в кэш и в БД."""
        with self._lock:
            self._cache[truck.id] = truck
        self._upsert_db(truck, flight_id, clear_flight)
        return truck

    def exists(self, truck_id: str) -> bool:
        with self._lock:
            return truck_id in self._cache

    def location_occupied(self, location: str) -> bool:
        with self._lock:
            return any(t.currentLocation == location for t in self._cache.values())

    def pick_free(self) -> Optional[CateringTruck]:
        """Выбирает первую свободную машину. Возвращает None если нет."""
        with self._lock:
            for truck in self._cache.values():
                if truck.status == TruckStatus.EMPTY:
                    return truck
        return None


repository = CateringTruckRepository()
