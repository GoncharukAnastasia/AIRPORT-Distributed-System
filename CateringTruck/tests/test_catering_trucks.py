import pytest

BASE = "/v1/catering-trucks"
TRUCK_ID = "CT-001"
LOC_A = "G11"
LOC_B = "E15"


@pytest.mark.asyncio
async def test_list_empty(client):
    """Изначально список машин пуст."""
    r = await client.get(BASE)
    assert r.status_code == 200
    assert r.json() == []


@pytest.mark.asyncio
async def test_init_truck(client):
    """Успешная инициализация машины."""
    r = await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    assert r.status_code == 200
    assert r.json()["success"] is True


@pytest.mark.asyncio
async def test_init_truck_duplicate(client):
    """Повторная инициализация той же машины — 400."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    r = await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    assert r.status_code == 400


@pytest.mark.asyncio
async def test_init_truck_occupied_location(client):
    """Инициализация в уже занятой точке — 400."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    r = await client.post(f"{BASE}/init", json={"id": "CT-002", "location": LOC_A})
    assert r.status_code == 400


@pytest.mark.asyncio
async def test_get_truck(client):
    """Получение машины по ID — проверка полей."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    r = await client.get(f"{BASE}/{TRUCK_ID}")
    assert r.status_code == 200
    data = r.json()
    assert data["id"] == TRUCK_ID
    assert data["currentLocation"] == LOC_A
    assert data["status"] == "free"


@pytest.mark.asyncio
async def test_get_truck_not_found(client):
    """Запрос несуществующей машины — 404."""
    r = await client.get(f"{BASE}/GHOST")
    assert r.status_code == 404


@pytest.mark.asyncio
async def test_list_after_init(client):
    """После инициализации список содержит одну машину."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    r = await client.get(BASE)
    assert r.status_code == 200
    assert len(r.json()) == 1


@pytest.mark.asyncio
async def test_move_permission(client):
    """Запрос разрешения на движение — мок всегда разрешает."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    r = await client.get(
        f"{BASE}/{TRUCK_ID}/move-permission",
        params={"from": LOC_A, "to": LOC_B},
    )
    assert r.status_code == 200
    assert r.json()["allowed"] is True


@pytest.mark.asyncio
async def test_move(client):
    """После start-move статус машины становится busy."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    r = await client.post(
        f"{BASE}/{TRUCK_ID}/move",
        json={"from": LOC_A, "to": LOC_B},
    )
    assert r.status_code == 200
    assert r.json()["success"] is True

    truck = (await client.get(f"{BASE}/{TRUCK_ID}")).json()
    assert truck["status"] == "busy"


@pytest.mark.asyncio
async def test_arrived(client):
    """После arrived местоположение обновляется, статус — free."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    await client.post(f"{BASE}/{TRUCK_ID}/move", json={"from": LOC_A, "to": LOC_B})
    r = await client.post(
        f"{BASE}/{TRUCK_ID}/arrived",
        json={"from": LOC_A, "to": LOC_B},
    )
    assert r.status_code == 200

    truck = (await client.get(f"{BASE}/{TRUCK_ID}")).json()
    assert truck["currentLocation"] == LOC_B
    assert truck["status"] == "free"


@pytest.mark.asyncio
async def test_load_food(client):
    """Загрузка еды обновляет меню машины."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    r = await client.post(
        f"{BASE}/{TRUCK_ID}/load-food",
        json={"menu": {"chicken": 30, "pork": 20, "fish": 10, "vegetarian": 5}},
    )
    assert r.status_code == 200

    truck = (await client.get(f"{BASE}/{TRUCK_ID}")).json()
    assert truck["menu"]["chicken"] == 30
    assert truck["menu"]["pork"] == 20


@pytest.mark.asyncio
async def test_load_food_exceeds_capacity(client):
    """Загрузка сверх вместимости — 400."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    r = await client.post(
        f"{BASE}/{TRUCK_ID}/load-food",
        json={"menu": {"chicken": 200, "pork": 0, "fish": 0, "vegetarian": 0}},
    )
    assert r.status_code == 400


@pytest.mark.asyncio
async def test_deliver_food(client):
    """Доставка еды сбрасывает меню и возвращает статус free."""
    await client.post(f"{BASE}/init", json={"id": TRUCK_ID, "location": LOC_A})
    await client.post(
        f"{BASE}/{TRUCK_ID}/load-food",
        json={"menu": {"chicken": 30, "pork": 20, "fish": 10, "vegetarian": 5}},
    )
    r = await client.post(
        f"{BASE}/{TRUCK_ID}/deliver-food",
        json={"planeId": "PL-001"},
    )
    assert r.status_code == 200
    assert r.json()["success"] is True

    truck = (await client.get(f"{BASE}/{TRUCK_ID}")).json()
    assert truck["menu"]["chicken"] == 0
    assert truck["status"] == "free"
