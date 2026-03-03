#!/usr/bin/env bash
set -euo pipefail

# =========================
# Настройки (если надо)
# =========================
COMPOSE_CMD="docker compose"

PG_SERVICE="postgres"
PG_DB="airport"
PG_USER="airport_user"

INFO_SERVICE="information-panel"
FM_SERVICE="followme"
GC_SERVICE="ground-control"
BOARD_SERVICE="board"

# Базовое демо-состояние рейсов
FLIGHT1_ID="SU100"
FLIGHT2_ID="SU200"
FLIGHT2_PARKING="P-3"

# Базовое демо-состояние FollowMe машин
FM1_ID="FM-1"
FM2_ID="FM-2"
FM_BASE_NODE="FS-1"

# =========================
# Функции
# =========================
wait_for_postgres() {
  echo "[reset] Жду готовности PostgreSQL..."
  for i in {1..30}; do
    if $COMPOSE_CMD exec -T "$PG_SERVICE" pg_isready -U "$PG_USER" -d "$PG_DB" >/dev/null 2>&1; then
      echo "[reset] PostgreSQL готов"
      return 0
    fi
    sleep 1
  done
  echo "[reset] PostgreSQL не поднялся вовремя" >&2
  exit 1
}

wait_http() {
  local url="$1"
  local name="$2"
  echo "[reset] Жду $name ($url)..."
  for i in {1..30}; do
    if curl -fsS "$url" >/dev/null 2>&1; then
      echo "[reset] $name готов"
      return 0
    fi
    sleep 1
  done
  echo "[reset] $name не ответил вовремя" >&2
  exit 1
}

# =========================
# 1) Остановить приложения
# =========================
echo "[reset] Останавливаю сервисы приложения..."
$COMPOSE_CMD stop "$GC_SERVICE" "$BOARD_SERVICE" "$FM_SERVICE" "$INFO_SERVICE" || true

# =========================
# 2) Убедиться, что PostgreSQL запущен
# =========================
echo "[reset] Поднимаю PostgreSQL (если не запущен)..."
$COMPOSE_CMD up -d "$PG_SERVICE"
wait_for_postgres

# =========================
# 3) Откат данных в БД
# =========================
echo "[reset] Откатываю данные в БД..."

NOW_EPOCH="$(date +%s)"

$COMPOSE_CMD exec -T "$PG_SERVICE" psql -U "$PG_USER" -d "$PG_DB" <<SQL
BEGIN;

-- =========================
-- InformationPanel: рейсы
-- Таблица ожидается: information_flights
-- Колонки ожидаются: flight_id, scheduled_at, status, phase, parking_node, updated_at
-- =========================

-- Если хочешь строго оставить только 2 рейса, раскомментируй:
-- DELETE FROM information_flights
-- WHERE flight_id NOT IN ('$FLIGHT1_ID', '$FLIGHT2_ID');

INSERT INTO information_flights (flight_id, scheduled_at, status, phase, parking_node, updated_at)
VALUES
  ('$FLIGHT1_ID', $NOW_EPOCH, 'Scheduled', 'airborne', NULL, $NOW_EPOCH),
  ('$FLIGHT2_ID', $NOW_EPOCH, 'Parked', 'grounded', '$FLIGHT2_PARKING', $NOW_EPOCH)
ON CONFLICT (flight_id) DO UPDATE SET
  scheduled_at = EXCLUDED.scheduled_at,
  status = EXCLUDED.status,
  phase = EXCLUDED.phase,
  parking_node = EXCLUDED.parking_node,
  updated_at = EXCLUDED.updated_at;

-- =========================
-- FollowMe: машинки
-- Таблица: followme_vehicles
-- Колонки: vehicle_id, current_node, status, flight_id, updated_at
-- =========================

-- Если хочешь строго оставить только 2 машинки, раскомментируй:
-- DELETE FROM followme_vehicles
-- WHERE vehicle_id NOT IN ('$FM1_ID', '$FM2_ID');

INSERT INTO followme_vehicles (vehicle_id, current_node, status, flight_id, updated_at)
VALUES
  ('$FM1_ID', '$FM_BASE_NODE', 'empty', NULL, NOW()),
  ('$FM2_ID', '$FM_BASE_NODE', 'empty', NULL, NOW())
ON CONFLICT (vehicle_id) DO UPDATE SET
  current_node = EXCLUDED.current_node,
  status = EXCLUDED.status,
  flight_id = EXCLUDED.flight_id,
  updated_at = NOW();

COMMIT;
SQL

echo "[reset] Данные в БД откатились"

# =========================
# 4) Поднять сервисы обратно
# =========================
echo "[reset] Поднимаю InformationPanel и FollowMe..."
$COMPOSE_CMD up -d "$INFO_SERVICE" "$FM_SERVICE"

wait_http "http://localhost:8082/health" "InformationPanel"
wait_http "http://localhost:8083/health" "FollowMe"

echo "[reset] Поднимаю GroundControl..."
$COMPOSE_CMD up -d "$GC_SERVICE"
wait_http "http://localhost:8081/health" "GroundControl"

echo "[reset] Поднимаю Board..."
$COMPOSE_CMD up -d "$BOARD_SERVICE"
wait_http "http://localhost:8084/health" "Board"

echo
echo "[reset] ✅ Готово. Система возвращена в демо-состояние:"
echo "  - $FLIGHT1_ID: Scheduled / airborne"
echo "  - $FLIGHT2_ID: Parked / grounded ($FLIGHT2_PARKING)"
echo "  - $FM1_ID, $FM2_ID: empty at $FM_BASE_NODE"