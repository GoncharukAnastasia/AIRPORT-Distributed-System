# CateringTruck

Микросервис управления машинами катеринга в аэропорту. Получает задание от HandlingSuperviser, самостоятельно прокладывает маршрут по карте аэропорта, движется к самолёту через GroundControl (enter-edge / leave-edge), выполняет обслуживание и возвращается на базу.

Порт: `8086`

---

## Содержание

- [Роль в системе](#роль-в-системе)
- [Архитектура](#архитектура)
- [Логика миссии](#логика-миссии)
- [API](#api)
- [Переменные окружения](#переменные-окружения)
- [База данных](#база-данных)
- [Запуск](#запуск)

---

## Роль в системе

```
Board  --(POST /v1/handling/request)-->  HandlingSuperviser
                                               |
                              (POST /v1/catering/handling/start)
                                               |
                                               v
                                         CateringTruck
                                               |
                           (enter-edge / leave-edge per каждому ребру)
                                               |
                                               v
                                        GroundControl
                                               |
                   (POST /v1/handling/catering/complete)
                                               |
                                               v
                                       HandlingSuperviser
```

CateringTruck не принимает решений о полётах. Он отвечает на входящий триггер от HandlingSuperviser и движется по carRoad-рёбрам карты аэропорта, запрашивая разрешение у GroundControl на каждое ребро.

---

## Архитектура

```
app/
├── main.py                        — точка входа, FastAPI lifespan
├── config.py                      — все настройки через env-переменные
├── dependencies.py                — DI-синглтон сервиса
├── models/
│   └── catering_truck.py          — доменная модель CateringTruck, TruckStatus
├── schemas/
│   └── catering_truck.py          — Pydantic-схемы запросов и ответов
├── storage/
│   └── repository.py              — PostgreSQL-репозиторий с in-memory кэшем
├── utils/
│   └── map_router.py              — BFS по carRoad-рёбрам airport_map.json
├── clients/
│   ├── ground_control.py          — enter-edge, leave-edge, init_vehicles
│   └── handling_supervisor.py     — notify_stage_complete
├── services/
│   ├── catering_truck_service.py  — start_handling, выбор свободной машины
│   └── mission_worker.py          — полная логика миссии (landing / takeoff)
└── routers/
    └── catering_trucks.py         — HTTP-эндпоинты
```

### Слои

**Router** — принимает HTTP-запрос, валидирует тело через Pydantic, вызывает сервис.

**Service** — выбирает свободную машину, резервирует её (статус `reserved`), запускает `MissionWorker` как фоновую asyncio-задачу и немедленно возвращает ответ клиенту.

**MissionWorker** — выполняет полный сценарий миссии в фоне: движение по маршруту с enter-edge/leave-edge, обслуживание самолёта, возврат на базу, доклад HandlingSuperviser.

**AirportMap** — загружает `airport_map.json` один раз при старте, строит граф из рёбер типа `carRoad` и `carRoad|planeRoad`, реализует BFS для поиска кратчайшего пути.

**Repository** — хранит состояние в PostgreSQL с потокобезопасным in-memory кэшем. Не затирает `flight_id` при промежуточных обновлениях локации, а обнуляет его только явно при освобождении машины.

**Clients** — изолированные HTTP-клиенты для GroundControl и HandlingSuperviser. Клиент GroundControl реализует цикл retry для enter-edge (ожидание разрешения) и вызывает `on_node_arrived` после каждого leave-edge.

---

## Логика миссии

### Статусы машины

| Статус        | Описание                                          |
|---------------|---------------------------------------------------|
| `empty`       | Свободна, стоит на базе                           |
| `reserved`    | Зарезервирована, миссия ещё не стартовала         |
| `moveToHub`   | Едет к хабу загрузки (только для takeoff)         |
| `moveToPlane` | Едет к самолёту                                   |
| `servicing`   | Обслуживает самолёт у parkingNode                 |
| `returning`   | Возвращается на базу                              |

### Сценарий landing (посадка)

Самолёт только приземлился, обслуживание — осмотр/уборка/разгрузка без загрузки еды.

```
empty --> reserved
база --(moveToPlane)--> parkingNode
parkingNode: servicing (CT_SERVICE_SEC секунд)
parkingNode --(returning)--> база
--> notify HandlingSuperviser (catering/complete)
--> empty
```

### Сценарий takeoff (вылет)

Самолёт готовится к вылету, нужно загрузить еду.

```
empty --> reserved
база --(moveToHub)--> HUB  (загрузка еды, CT_SERVICE_SEC секунд)
HUB  --(moveToPlane)--> parkingNode
parkingNode: servicing (CT_SERVICE_SEC секунд)
parkingNode --(returning)--> база
--> notify HandlingSuperviser (catering/complete)
--> empty
```

Если `CT_HUB_NODE == CT_BASE_NODE` (по умолчанию), шаг "база -> HUB" пропускается.

### Движение по рёбрам

На каждом ребре маршрута выполняется следующая последовательность:

1. `POST /v1/map/traffic/enter-edge` — запрос разрешения у GroundControl. При `granted: false` — retry с интервалом 1 секунда (до `CT_EDGE_WAIT_TIMEOUT_SEC`).
2. Ожидание `CT_EDGE_TRAVEL_SEC` секунд (время проезда ребра).
3. `POST /v1/map/traffic/leave-edge` — освобождение ребра.
4. Обновление `currentLocation` в репозитории.

---

## API

### GET /health

Проверка работоспособности. Используется Docker healthcheck.

**Ответ `200`**
```json
{"service": "CateringTruck", "status": "ok"}
```

---

### GET /v1/catering-trucks

Список всех зарегистрированных машин.

**Ответ `200`**
```json
[
  {
    "id": "CT-1",
    "capacity": 100,
    "status": "empty",
    "currentLocation": "FS-1",
    "menu": {
      "chicken": 0,
      "pork": 0,
      "fish": 0,
      "vegetarian": 0
    }
  }
]
```

---

### GET /v1/catering-trucks/{id}

Информация о конкретной машине.

| Код | Описание                     |
|-----|------------------------------|
| 200 | Машина найдена               |
| 404 | Машина с таким ID не найдена |

---

### POST /v1/catering-trucks/init

Инициализирует новую машину в заданном узле карты и регистрирует её в GroundControl.

**Тело запроса**
```json
{
  "id": "CT-1",
  "location": "FS-1"
}
```

| Код | Описание                                         |
|-----|--------------------------------------------------|
| 200 | Машина успешно инициализирована                  |
| 400 | Машина с таким ID уже существует, или узел занят |

---

### POST /v1/catering/handling/start

Основной триггер от HandlingSuperviser. Запускает миссию в фоне и сразу возвращает ответ.

**Тело запроса**
```json
{
  "flightId": "SU100",
  "missionType": "takeoff",
  "parkingNode": "P-3"
}
```

| Поле          | Тип                       | Описание                          |
|---------------|---------------------------|-----------------------------------|
| `flightId`    | string                    | Идентификатор рейса               |
| `missionType` | `"landing"` / `"takeoff"` | Тип миссии                        |
| `parkingNode` | string                    | Узел парковки самолёта (P-1..P-5) |

**Ответ `200`**
```json
{"ok": true, "vehicleId": "CT-1"}
```

| Код | Описание                    |
|-----|-----------------------------|
| 200 | Миссия поставлена в очередь |
| 503 | Нет свободных машин         |

---

## Переменные окружения

| Переменная                 | По умолчанию              | Описание                                      |
|----------------------------|---------------------------|-----------------------------------------------|
| `CT_PORT`                  | `8086`                    | Порт сервиса                                  |
| `GC_HOST`                  | `localhost`               | Хост GroundControl                            |
| `GC_PORT`                  | `8081`                    | Порт GroundControl                            |
| `HS_HOST`                  | `localhost`               | Хост HandlingSuperviser                       |
| `HS_PORT`                  | `8085`                    | Порт HandlingSuperviser                       |
| `CT_PG_HOST`               | `localhost`               | Хост PostgreSQL                               |
| `CT_PG_PORT`               | `5432`                    | Порт PostgreSQL                               |
| `CT_PG_DB`                 | `airport`                 | Имя базы данных                               |
| `CT_PG_USER`               | `airport_user`            | Пользователь БД                               |
| `CT_PG_PASSWORD`           | `airport_pass`            | Пароль БД                                     |
| `CT_MAP_PATH`              | `/app/data/airport_map.json` | Путь к карте аэропорта                     |
| `CT_BASE_NODE`             | `FS-1`                    | Базовый узел (стартовая позиция)              |
| `CT_HUB_NODE`              | `= CT_BASE_NODE`          | Узел загрузки еды (для takeoff)               |
| `CT_EDGE_TRAVEL_SEC`       | `1`                       | Время проезда одного ребра (сек)              |
| `CT_SERVICE_SEC`           | `5`                       | Время обслуживания у самолёта (сек)           |
| `CT_EDGE_WAIT_TIMEOUT_SEC` | `90`                      | Таймаут ожидания разрешения enter-edge (сек)  |

---

## База данных

Используется общая PostgreSQL-база аэропорта. Таблица создаётся автоматически при старте сервиса (скрипт `db/init/003_catering_truck.sql`).

```sql
CREATE TABLE IF NOT EXISTS catering_trucks (
    vehicle_id    VARCHAR(64) PRIMARY KEY,
    current_node  VARCHAR(64) NOT NULL,
    status        VARCHAR(32) NOT NULL
                    CHECK (status IN ('empty','reserved','moveToHub',
                                      'moveToPlane','servicing','returning')),
    flight_id     VARCHAR(64),
    mission_type  VARCHAR(32),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
```

---

## Запуск

### Локально

```bash
cd CateringTruck
python -m venv venv
source venv/bin/activate
pip install -r requirements.txt

export GC_HOST=localhost GC_PORT=8081
export HS_HOST=localhost HS_PORT=8085
export CT_PG_HOST=localhost CT_PG_DB=airport CT_PG_USER=airport_user CT_PG_PASSWORD=airport_pass

python -m app.main
```

Swagger-документация: `http://localhost:8086/docs`

### Docker Compose (рекомендуется)

```bash
# из корня репозитория
docker compose up --build

# инициализация машин
./scripts/init_catering_trucks.sh
```

Контейнер ожидает готовности `postgres` и `ground-control` перед стартом (`depends_on: condition: service_healthy`).
