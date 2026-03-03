import os

# Ground Control
GC_HOST = os.getenv("GC_HOST", "localhost")
GC_PORT = int(os.getenv("GC_PORT", "8081"))
GROUND_CONTROL_URL = f"http://{GC_HOST}:{GC_PORT}"

# Handling Supervisor
HS_HOST = os.getenv("HS_HOST", "localhost")
HS_PORT = int(os.getenv("HS_PORT", "8085"))
HANDLING_SUPERVISOR_URL = f"http://{HS_HOST}:{HS_PORT}"

# Board (оставляем для совместимости)
BOARD_URL = os.getenv("BOARD_URL", "http://localhost:8084")

# Этот сервис
APP_HOST = "0.0.0.0"
APP_PORT = int(os.getenv("CT_PORT", "8086"))

# PostgreSQL
PG_HOST = os.getenv("CT_PG_HOST", "localhost")
PG_PORT = os.getenv("CT_PG_PORT", "5432")
PG_DB = os.getenv("CT_PG_DB",   "airport")
PG_USER = os.getenv("CT_PG_USER", "airport_user")
PG_PASSWORD = os.getenv("CT_PG_PASSWORD", "airport_pass")
PG_DSN = os.getenv(
    "CT_PG_DSN",
    f"host={PG_HOST} port={PG_PORT} dbname={PG_DB} user={PG_USER} password={PG_PASSWORD}",
)

# Карта аэропорта
MAP_PATH = os.getenv("CT_MAP_PATH", "/app/data/airport_map.json")

# Базовый узел (стоянка) и хаб загрузки еды
CT_BASE_NODE = os.getenv("CT_BASE_NODE", "FS-1")
CT_HUB_NODE = os.getenv("CT_HUB_NODE", CT_BASE_NODE)

# Параметры движения
CT_EDGE_TRAVEL_SEC = int(os.getenv("CT_EDGE_TRAVEL_SEC",      "1"))
CT_SERVICE_SEC = int(os.getenv("CT_SERVICE_SEC",          "5"))
CT_EDGE_WAIT_TIMEOUT_SEC = int(os.getenv("CT_EDGE_WAIT_TIMEOUT_SEC", "90"))
