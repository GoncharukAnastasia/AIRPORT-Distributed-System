"""
Airport Visualizer — визуализатор аэродрома в реальном времени.

Порт по умолчанию: 8087

Отображает:
  - Граф аэродрома (узлы + рёбра)
  - Позиции самолётов (plane:*)
  - Позиции наземного транспорта (vehicle:*, BUS-*)
  - Статусы рейсов (сессии)
  - Автобусы и обслуживание

Данные берёт из:
  - Ground Control /v1/visualizer/snapshot
  - Information Panel /v1/flights
  - Handling Supervisor /v1/handling/tasks
  - Passenger Bus /v1/bus/vehicles
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import time
import uuid
from pathlib import Path

import httpx
import uvicorn
from fastapi import FastAPI
from fastapi.responses import HTMLResponse, StreamingResponse

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] [AirportVisualizer] [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
log = logging.getLogger("airport_visualizer")

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
GC_HOST = os.getenv("GC_HOST", "localhost")
GC_PORT = int(os.getenv("GC_PORT", "8081"))

INFO_HOST = os.getenv("INFO_HOST", "localhost")
INFO_PORT = int(os.getenv("INFO_PORT", "8082"))

HANDLING_HOST = os.getenv("HANDLING_HOST", "localhost")
HANDLING_PORT = int(os.getenv("HANDLING_PORT", "8085"))

BUS_HOST = os.getenv("BUS_HOST", "localhost")
BUS_PORT = int(os.getenv("BUS_PORT", "8086"))

FOLLOWME_HOST = os.getenv("FOLLOWME_HOST", "localhost")
FOLLOWME_PORT = int(os.getenv("FOLLOWME_PORT", "8083"))

BOARD_HOST = os.getenv("BOARD_HOST", "localhost")
BOARD_PORT = int(os.getenv("BOARD_PORT", "8084"))

CATERING_HOST = os.getenv("CATERING_HOST", "localhost")
CATERING_PORT = int(os.getenv("CATERING_PORT", "8089"))

PORT = int(os.getenv("VISUALIZER_PORT", "8087"))

MAP_PATH = os.getenv("GC_MAP_PATH", str(
    Path(__file__).resolve().parent.parent.parent / "data" / "airport_map.json"))

# ---------------------------------------------------------------------------
# Load airport map for coordinate generation
# ---------------------------------------------------------------------------
try:
    with open(MAP_PATH, "r", encoding="utf-8") as f:
        AIRPORT_MAP = json.load(f)
except Exception:
    AIRPORT_MAP = {"nodes": [], "edges": []}
    log.warning("Could not load airport_map.json from %s", MAP_PATH)


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------
app = FastAPI(title="Airport Visualizer", version="1.0.0")


@app.get("/health")
async def health():
    return {
        "service": "AirportVisualizer",
        "status": "ok",
        "time": int(time.time()),
    }


@app.get("/api/snapshot")
async def proxy_snapshot():
    """Проксирует snapshot от Ground Control и добавляет данные от других сервисов."""
    result = {}

    # Ground Control snapshot
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.get(f"http://{GC_HOST}:{GC_PORT}/v1/visualizer/snapshot")
            if r.status_code < 300:
                result["gc"] = r.json()
    except Exception as exc:
        result["gc"] = {"error": str(exc)}

    # Information Panel flights
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.get(f"http://{INFO_HOST}:{INFO_PORT}/v1/flights")
            if r.status_code < 300:
                result["flights"] = r.json()
    except Exception:
        result["flights"] = {"flights": []}

    # Handling Supervisor tasks
    try:
        async with httpx.AsyncClient(timeout=3.0) as cli:
            r = await cli.get(f"http://{HANDLING_HOST}:{HANDLING_PORT}/v1/handling/tasks")
            if r.status_code < 300:
                result["handling"] = r.json()
    except Exception:
        result["handling"] = {"tasks": []}

    # Passenger Bus vehicles
    try:
        async with httpx.AsyncClient(timeout=3.0) as cli:
            r = await cli.get(f"http://{BUS_HOST}:{BUS_PORT}/v1/bus/vehicles")
            if r.status_code < 300:
                result["buses"] = r.json()
    except Exception:
        result["buses"] = {"buses": []}

    # Catering trucks
    try:
        async with httpx.AsyncClient(timeout=3.0) as cli:
            r = await cli.get(f"http://{CATERING_HOST}:{CATERING_PORT}/v1/catering-trucks")
            if r.status_code < 300:
                result["catering_trucks"] = r.json()
    except Exception:
        result["catering_trucks"] = []

    return result


@app.get("/api/gc-events")
async def proxy_gc_events(since: int = 0):
    """Проксирует инкрементальные события Ground Control для live-рендеринга."""
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.get(
                f"http://{GC_HOST}:{GC_PORT}/v1/visualizer/events",
                params={"since": since},
            )
            if r.status_code < 300:
                return r.json()
    except Exception:
        pass
    return {"events": [], "latestSeq": since}


@app.get("/api/map")
async def get_map():
    """Возвращает структуру карты аэродрома."""
    return AIRPORT_MAP


# ---------------------------------------------------------------------------
# Docker log reader — reads container logs via /var/run/docker.sock
# ---------------------------------------------------------------------------
_DOCKER_SOCK = "/var/run/docker.sock"
_LOG_CONTAINERS = [
    "ground-control",
    "information-panel",
    "followme",
    "board",
    "handling-supervisor",
    "passenger-bus",
    "catering",
]


def _parse_docker_log_stream(data: bytes) -> list[str]:
    """
    Docker multiplexed log stream: each frame has an 8-byte header
      byte 0   : stream type (1=stdout, 2=stderr)
      bytes 1-3: zero (reserved)
      bytes 4-7: payload size (big-endian uint32)
    Falls back to raw UTF-8 text when TTY mode is detected (no headers).
    """
    if len(data) < 8 or data[0] not in (1, 2) or data[1:4] != b"\x00\x00\x00":
        return [ln for ln in data.decode("utf-8", errors="replace").splitlines() if ln]
    lines: list[str] = []
    i = 0
    total = len(data)
    while i + 8 <= total:
        stype = data[i]
        if stype not in (0, 1, 2):
            break
        size = int.from_bytes(data[i + 4: i + 8], "big")
        i += 8
        if size == 0:
            continue
        if i + size > total:
            break
        chunk = data[i: i + size].decode("utf-8", errors="replace")
        lines.extend(chunk.rstrip("\n").split("\n"))
        i += size
    return [ln for ln in lines if ln]


async def _docker_tail(container: str, tail: int = 150, since: int = 0) -> list[str]:
    """Fetch recent logs from a Docker container via the Unix socket."""
    import os
    if not os.path.exists(_DOCKER_SOCK):
        return [f"[Docker socket not available — add volume /var/run/docker.sock to docker-compose.yml]"]
    try:
        transport = httpx.AsyncHTTPTransport(uds=_DOCKER_SOCK)
        params: dict[str, str] = {"stdout": "1",
                                  "stderr": "1", "timestamps": "1"}
        if since:
            params["since"] = str(since)
        else:
            params["tail"] = str(tail)
        async with httpx.AsyncClient(
            transport=transport, timeout=8.0, base_url="http://docker"
        ) as cli:
            r = await cli.get(f"/containers/{container}/logs", params=params)
            if r.status_code != 200:
                return [f"[Docker API returned HTTP {r.status_code} for '{container}'"]
            return _parse_docker_log_stream(r.content)
    except Exception as e:
        return [f"[error reading logs for {container}: {e}]"]


@app.get("/api/logs/tail")
async def logs_tail(n: int = 150, since: int = 0):
    """
    Returns log lines from all service containers.
    ?since=<unix_ts>  — only lines after that timestamp
    ?n=<count>        — tail N lines (used when since=0)
    """
    results = await asyncio.gather(
        *[_docker_tail(c, tail=n, since=since) for c in _LOG_CONTAINERS],
        return_exceptions=True,
    )
    return {
        c: (r if isinstance(r, list) else [str(r)])
        for c, r in zip(_LOG_CONTAINERS, results)
    }


@app.post("/api/demo/reset")
async def demo_reset():
    """Сбрасывает состояние всех сервисов для чистой демки."""
    errors = []
    # Stop all Board plane agents
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.get(f"http://{BOARD_HOST}:{BOARD_PORT}/v1/planes")
            if r.status_code < 300:
                for p in r.json().get("planes", []):
                    await cli.post(
                        f"http://{BOARD_HOST}:{BOARD_PORT}/v1/planes/stop",
                        json={"flightId": p["flightId"]}
                    )
    except Exception as e:
        errors.append(f"board_stop: {e}")
    # Reset buses (force IDLE + clear GC edge state)
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            await cli.post(f"http://{BUS_HOST}:{BUS_PORT}/v1/bus/reset")
    except Exception as e:
        errors.append(f"bus_reset: {e}")
    # Reset catering trucks
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            await cli.post(f"http://{CATERING_HOST}:{CATERING_PORT}/v1/catering/reset")
    except Exception as e:
        errors.append(f"catering_reset: {e}")
    return {"ok": True, "errors": errors}


# ---------------------------------------------------------------------------
# Demo — background task + status polling
# ---------------------------------------------------------------------------
# session_id -> {logs: [], done: bool, error: bool}
demo_sessions: dict[str, dict] = {}


def _demo_log(session_id: str, msg: str, kind: str = "log"):
    if session_id in demo_sessions:
        demo_sessions[session_id]["logs"].append(
            {"msg": msg, "kind": kind, "ts": int(time.time())}
        )


async def _background_demo(session_id: str, flight_id: str):
    """Фоновая задача: ждёт mission.completed и создаёт grounded-агент."""
    log = lambda msg, kind="log": _demo_log(session_id, msg, kind)

    last_seq = 0
    parking_node = ""
    mission_done = False
    deadline = time.time() + 150  # max 2.5 min

    log("[5/6] Ожидание посадки и миссии FollowMe (фон)...")

    while not mission_done and time.time() < deadline:
        await asyncio.sleep(3)  # less frequent — reduce GC load
        try:
            async with httpx.AsyncClient(timeout=6.0) as cli:
                r = await cli.get(
                    f"http://{GC_HOST}:{GC_PORT}/v1/visualizer/events"
                )
                if r.status_code < 300:
                    for ev in r.json().get("events", []):
                        seq = ev.get("seq", 0)
                        if seq <= last_seq:
                            continue
                        last_seq = seq
                        etype = ev.get("type", "")
                        payload = ev.get("payload", {})
                        fid = payload.get("flightId", "")

                        if fid != flight_id:
                            continue

                        if etype == "flight.landing_approved":
                            parking_node = payload.get("parking", "")
                            log(
                                f"  ✈ Посадка одобрена → стоянка {parking_node}, сопровождает {payload.get('vehicleId', '')}")
                        elif etype == "flight.landed_re1":
                            log("  ✈ Самолёт приземлился на ВПП (RE-1)")
                        elif etype == "flight.arrived_parking":
                            parking_node = payload.get("parking", parking_node)
                            log(f"  ✈ Самолёт на стоянке {parking_node}")
                        elif etype == "mission.completed":
                            log(
                                f"  ✓ FollowMe завершил миссию, {payload.get('vehicleId', '')} вернулся на базу")
                            mission_done = True
                            break
        except Exception as e:
            log(f"  ⚠ Ошибка опроса событий: {e}")

    if not mission_done:
        log("  ✗ Таймаут ожидания миссии FollowMe", "error")
        demo_sessions[session_id]["done"] = True
        demo_sessions[session_id]["error"] = True
        return

    await asyncio.sleep(1)

    # Step 6: Create grounded agent
    log(f"[6/6] Создание наземного агента (стоянка {parking_node})...")
    if not parking_node:
        log("  ✗ Стоянка не определена", "error")
        demo_sessions[session_id]["done"] = True
        demo_sessions[session_id]["error"] = True
        return

    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.post(
                f"http://{BOARD_HOST}:{BOARD_PORT}/v1/planes/grounded",
                json={
                    "flightId": flight_id,
                    "parkingNode": parking_node,
                    "handlingHost": HANDLING_HOST,
                    "handlingPort": HANDLING_PORT,
                }
            )
            data = r.json()
            if data.get("ok"):
                log(f"  ✓ Наземный агент создан — запущено обслуживание и автобусы")
            else:
                log(f"  ✗ Board grounded ошибка: {data}", "error")
                demo_sessions[session_id]["done"] = True
                demo_sessions[session_id]["error"] = True
                return
    except Exception as e:
        log(f"  ✗ Board ошибка: {e}", "error")
        demo_sessions[session_id]["done"] = True
        demo_sessions[session_id]["error"] = True
        return

    # Wait for handling to finish and buses to complete their return journeys
    await asyncio.sleep(45)
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            r = await cli.get(
                f"http://{HANDLING_HOST}:{HANDLING_PORT}/v1/handling/status/{flight_id}"
            )
            if r.status_code < 300:
                h = r.json()
                for svc, task in h.get("checklist", {}).items():
                    log(f"  🔧 {svc}: {task.get('status', '?')} — {task.get('detail', '')}")
                log(f"  ✅ Обслуживание: {h.get('status', '?')}")
    except Exception:
        pass

    # Clean up: return buses to home so map is tidy
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            await cli.post(f"http://{BUS_HOST}:{BUS_PORT}/v1/bus/reset")
    except Exception:
        pass
    # Clean up: reset catering trucks
    try:
        async with httpx.AsyncClient(timeout=5.0) as cli:
            await cli.post(f"http://{CATERING_HOST}:{CATERING_PORT}/v1/catering/reset")
    except Exception:
        pass

    log(f"🏁 Демка завершена! Рейс {flight_id} полностью обслужен.", "done")
    demo_sessions[session_id]["done"] = True


@app.get("/api/demo/status/{session_id}")
async def demo_status(session_id: str):
    """Возвращает накопленные логи фоновой задачи."""
    s = demo_sessions.get(session_id)
    if not s:
        return {"sessionId": session_id, "logs": [], "done": True, "error": True}
    return {
        "sessionId": session_id,
        "logs": s["logs"],
        "done": s["done"],
        "error": s["error"],
    }


@app.get("/api/demo/landing")
async def demo_landing():
    """
    SSE-стрим только для шагов 1-4 (быстро, без блокировки).
    Шаги 5-6 (ожидание FollowMe + grounded агент) запускаются как
    фоновый asyncio.create_task и не блокируют event loop.
    """
    flight_id = f"DEMO-{uuid.uuid4().hex[:4].upper()}"
    session_id = flight_id
    demo_sessions[session_id] = {"logs": [], "done": False, "error": False}

    async def event_stream():
        def sse(msg: str, kind: str = "log") -> str:
            data = json.dumps({
                "kind": kind, "msg": msg,
                "ts": int(time.time()), "sessionId": session_id,
            })
            return f"data: {data}\n\n"

        yield sse(f"🚀 Запуск демки посадки рейса {flight_id}", "start")
        await asyncio.sleep(0.05)

        # ── Step 1: Cleanup ─────────────────────────────────────────────────
        yield sse("[1/4] Очистка парковок и старых агентов...")
        freed = 0
        try:
            async with httpx.AsyncClient(timeout=8.0) as cli:
                ri = await cli.get(f"http://{INFO_HOST}:{INFO_PORT}/v1/flights")
                if ri.status_code < 300:
                    for f in ri.json().get("flights", []):
                        if f.get("phase") == "grounded" and f.get("flightId"):
                            tr = await cli.post(
                                f"http://{GC_HOST}:{GC_PORT}/v1/flights/{f['flightId']}/tookoff"
                            )
                            if tr.status_code < 300:
                                freed += 1

                rb = await cli.get(f"http://{BOARD_HOST}:{BOARD_PORT}/v1/planes")
                stopped = 0
                if rb.status_code < 300:
                    for p in rb.json().get("planes", []):
                        sr = await cli.post(
                            f"http://{BOARD_HOST}:{BOARD_PORT}/v1/planes/stop",
                            json={"flightId": p["flightId"]}
                        )
                        if sr.status_code < 300:
                            stopped += 1

                # Re-register buses in GC
                rv = await cli.get(f"http://{BUS_HOST}:{BUS_PORT}/v1/bus/vehicles")
                if rv.status_code < 300:
                    buses = [b for b in rv.json().get("buses", [])
                             if b.get("status") == "idle"]
                    if buses:
                        await cli.post(
                            f"http://{GC_HOST}:{GC_PORT}/v1/vehicles/init",
                            json={"vehicles": [
                                {"vehicleId": b["busId"],
                                    "currentNode": b["currentNode"]}
                                for b in buses
                            ]}
                        )

                # Reset catering trucks to home + re-register in GC
                await cli.post(f"http://{CATERING_HOST}:{CATERING_PORT}/v1/catering/reset")

            yield sse(f"  ✓ Освобождено {freed} парковок, остановлено {stopped} агентов")
        except Exception as e:
            yield sse(f"  ⚠ Очистка: {e}")
        await asyncio.sleep(0.5)

        # ── Step 2: Init FollowMe ───────────────────────────────────────────
        yield sse("[2/4] Инициализация машин сопровождения FM-1, FM-2...")
        try:
            async with httpx.AsyncClient(timeout=5.0) as cli:
                r = await cli.post(
                    f"http://{FOLLOWME_HOST}:{FOLLOWME_PORT}/v1/vehicles/init",
                    json={"vehicles": [
                        {"vehicleId": "FM-1", "currentNode": "FS-1"},
                        {"vehicleId": "FM-2", "currentNode": "FS-1"},
                    ]}
                )
                yield sse(f"  ✓ FollowMe: {r.json().get('initialized', 0)} машин готово")
        except Exception as e:
            yield sse(f"  ✗ FollowMe: {e}", "error")
            return
        await asyncio.sleep(0.3)

        # ── Step 3: Register flight ─────────────────────────────────────────
        yield sse(f"[3/4] Регистрация рейса {flight_id}...")
        try:
            async with httpx.AsyncClient(timeout=5.0) as cli:
                r = await cli.post(
                    f"http://{INFO_HOST}:{INFO_PORT}/v1/flights/init",
                    json={"flights": [{
                        "flightId": flight_id,
                        "status": "Scheduled",
                        "scheduledAt": int(time.time()) - 60,
                    }]}
                )
                data = r.json()
                if not data.get("ok"):
                    yield sse(f"  ✗ InfoPanel: {data}", "error")
                    return
                yield sse(f"  ✓ Рейс зарегистрирован")
        except Exception as e:
            yield sse(f"  ✗ InfoPanel: {e}", "error")
            return
        await asyncio.sleep(0.3)

        # ── Step 4: Create airborne agent ───────────────────────────────────
        yield sse(f"[4/4] Создание воздушного агента (pollSec=5)...")
        try:
            async with httpx.AsyncClient(timeout=5.0) as cli:
                r = await cli.post(
                    f"http://{BOARD_HOST}:{BOARD_PORT}/v1/planes/airborne",
                    json={"flightId": flight_id, "pollSec": 5}
                )
                data = r.json()
                if not data.get("ok"):
                    yield sse(f"  ✗ Board: {data}", "error")
                    return
                yield sse(f"  ✓ {flight_id} в воздухе, ждёт разрешения на посадку...")
        except Exception as e:
            yield sse(f"  ✗ Board: {e}", "error")
            return

        # ── Background task handles steps 5-6 ──────────────────────────────
        asyncio.create_task(_background_demo(session_id, flight_id))
        yield sse(
            f"🟡 Демка запущена! Наблюдайте за картой. Шаги 5-6 идут в фоне (sessionId={session_id})",
            "launched"
        )

    return StreamingResponse(
        event_stream(),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.get("/", response_class=HTMLResponse)
async def index():
    """Главная страница — интерактивная карта аэропорта."""
    return HTML_PAGE


# ---------------------------------------------------------------------------
# HTML/CSS/JS — полная визуализация аэропорта
# ---------------------------------------------------------------------------
HTML_PAGE = r"""<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Airport Visualizer</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
    background: #0a0e17;
    color: #e0e0e0;
    font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
    overflow: hidden;
}
#header {
    position: fixed; top: 0; left: 0; right: 0;
    height: 48px;
    background: linear-gradient(135deg, #1a1f2e 0%, #2d3548 100%);
    display: flex; align-items: center; justify-content: space-between;
    padding: 0 20px;
    z-index: 100;
    border-bottom: 1px solid #3a4560;
    box-shadow: 0 2px 10px rgba(0,0,0,0.3);
}
#header h1 {
    font-size: 18px; font-weight: 600;
    background: linear-gradient(90deg, #00d4ff, #7b68ee);
    -webkit-background-clip: text; -webkit-text-fill-color: transparent;
}
#header .status { font-size: 12px; color: #8892a4; }
#header .status .dot {
    display: inline-block; width: 8px; height: 8px;
    border-radius: 50%; margin-right: 6px;
    background: #44ff44; animation: pulse 2s infinite;
}
@keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.4; } }

#canvas-container {
    position: fixed; top: 48px; left: 0; right: 320px; bottom: 0;
}
canvas {
    width: 100%; height: 100%;
    cursor: grab;
}
canvas:active { cursor: grabbing; }

#sidebar {
    position: fixed; top: 48px; right: 0; bottom: 0;
    width: 320px;
    background: #111827;
    border-left: 1px solid #2d3548;
    overflow-y: auto;
    padding: 12px;
    z-index: 50;
}
.panel {
    background: #1e2538;
    border-radius: 8px;
    margin-bottom: 10px;
    border: 1px solid #2d3548;
}
.panel-header {
    padding: 10px 14px;
    font-size: 13px;
    font-weight: 600;
    color: #7b8baa;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    border-bottom: 1px solid #2d3548;
    display: flex; justify-content: space-between; align-items: center;
}
.panel-header .badge {
    background: #3b82f6;
    color: white;
    font-size: 11px;
    padding: 2px 8px;
    border-radius: 10px;
    font-weight: 500;
}
.panel-body {
    padding: 8px 12px;
    font-size: 12px;
    max-height: 200px;
    overflow-y: auto;
}
.item {
    padding: 6px 0;
    border-bottom: 1px solid #1a2030;
    display: flex; justify-content: space-between; align-items: center;
}
.item:last-child { border-bottom: none; }
.item .id { font-weight: 600; color: #c8d6e5; }
.item .status-badge {
    font-size: 10px; padding: 2px 6px;
    border-radius: 4px; font-weight: 500;
}
.status-parked, .status-arrivedparking { background: #10b981; color: white; }
.status-idle { background: #6b7280; color: white; }
.status-en_route, .status-moving, .status-taxiing { background: #f59e0b; color: #1a1a1a; }
.status-loading, .status-unloading { background: #8b5cf6; color: white; }
.status-completed, .status-departed { background: #3b82f6; color: white; }
.status-scheduled, .status-airborne, .status-created { background: #06b6d4; color: white; }
.status-servicing, .status-in_progress { background: #ec4899; color: white; }
.status-landingapproved { background: #22c55e; color: white; }
.status-returning { background: #a855f7; color: white; }
.status-failed, .status-denied, .status-cancelled { background: #ef4444; color: white; }

.legend {
    display: flex; flex-wrap: wrap; gap: 8px; padding: 8px 12px;
}
.legend-item {
    display: flex; align-items: center; gap: 4px; font-size: 11px; color: #8892a4;
}
.legend-item .swatch {
    width: 12px; height: 12px; border-radius: 3px;
}
/* ── Log panel ─────────────────────────────────────────────────────────── */
#log-panel {
    position: fixed; bottom: 0; left: 0; right: 0; height: 260px;
    background: #080d14; border-top: 2px solid #3a4560;
    display: none; flex-direction: column; z-index: 120;
    font-family: 'Courier New', monospace;
}
body.logs-open #log-panel   { display: flex; }
body.logs-open #canvas-container { bottom: 260px; }
body.logs-open #sidebar      { bottom: 260px; }
#log-panel-header {
    display: flex; align-items: center; gap: 8px;
    padding: 4px 12px; background: #111827;
    border-bottom: 1px solid #2d3548; flex-shrink: 0; height: 34px;
}
#log-panel-header .lp-title {
    font-size: 12px; font-weight: 700; color: #7b8baa;
    letter-spacing: 0.8px; margin-right: 4px; flex-shrink: 0;
}
#log-panel-header select,
#log-panel-header button {
    font-size: 11px; padding: 2px 8px; border-radius: 4px; cursor: pointer;
    background: #1e2538; color: #c8d6e5; border: 1px solid #3a4560;
}
#log-panel-header button:hover { background: #2d3548; }
#log-panel-header label {
    font-size: 11px; color: #8892a4; display: flex; align-items: center; gap: 3px;
}
#log-panel-body {
    flex: 1; overflow-y: auto; padding: 4px 14px;
    font-size: 11.5px; line-height: 1.55;
}
.log-line { white-space: pre-wrap; word-break: break-all; }
</style>
</head>
<body>

<div id="header">
    <h1>✈ Airport Visualizer</h1>
    <div style="display:flex;align-items:center;gap:12px">
        <button onclick="toggleLogs()" style="
            background:#1e2538;border:1px solid #3a4560;color:#8892a4;
            padding:5px 12px;border-radius:20px;font-size:12px;cursor:pointer;
        ">📋 LOGS</button>
        <button id="demo-btn" onclick="startDemo()" style="
            background:linear-gradient(135deg,#7b68ee,#06b6d4);
            border:none;color:white;padding:6px 16px;border-radius:20px;
            font-size:13px;font-weight:600;cursor:pointer;letter-spacing:0.5px;
            box-shadow:0 2px 8px rgba(123,104,238,0.4);transition:opacity 0.2s;
        ">▶ DEMO</button>
        <span id="evt-count" style="font-size:11px;color:#06b6d4;background:#0e1628;border:1px solid #1e3a5f;padding:3px 8px;border-radius:12px;">⚡ —</span>
        <div class="status">
            <span class="dot"></span>
            <span id="update-time">Connecting...</span>
        </div>
    </div>
</div>

<!-- Demo modal -->
<div id="demo-modal" style="display:none;position:fixed;inset:0;z-index:200;background:rgba(0,0,0,0.7);align-items:center;justify-content:center;">
  <div style="background:#1e2538;border:1px solid #3a4560;border-radius:12px;width:520px;max-width:95vw;box-shadow:0 8px 40px rgba(0,0,0,0.6);">
    <div style="padding:16px 20px;border-bottom:1px solid #2d3548;display:flex;justify-content:space-between;align-items:center;">
      <span style="font-weight:700;font-size:15px;background:linear-gradient(90deg,#00d4ff,#7b68ee);-webkit-background-clip:text;-webkit-text-fill-color:transparent;">✈ Demo — полный цикл посадки</span>
      <button onclick="closeDemo()" style="background:none;border:none;color:#8892a4;font-size:20px;cursor:pointer;line-height:1;">×</button>
    </div>
    <div id="demo-log" style="padding:14px 18px;min-height:180px;max-height:380px;overflow-y:auto;font-family:monospace;font-size:12px;line-height:1.7;"></div>
    <div style="padding:10px 18px;border-top:1px solid #2d3548;display:flex;gap:8px;">
      <button id="demo-start-btn" onclick="startDemo()" style="
          background:#3b82f6;border:none;color:white;padding:6px 20px;
          border-radius:8px;font-size:13px;cursor:pointer;font-weight:600;
      ">Запустить</button>
      <button onclick="closeDemo()" style="
          background:#374151;border:none;color:#c8d6e5;padding:6px 16px;
          border-radius:8px;font-size:13px;cursor:pointer;
      ">Закрыть</button>
    </div>
  </div>
</div>

<!-- Log panel -->
<div id="log-panel">
  <div id="log-panel-header">
    <span class="lp-title">📋 LOGS</span>
    <select id="log-filter" onchange="clearLogPanel(); fetchLogTail(true);">
      <option value="all">All services</option>
      <option value="ground-control">ground-control</option>
      <option value="information-panel">information-panel</option>
      <option value="followme">followme</option>
      <option value="board">board</option>
      <option value="handling-supervisor">handling-supervisor</option>
      <option value="passenger-bus">passenger-bus</option>
      <option value="catering">catering</option>
    </select>
    <label><input id="log-autoscroll" type="checkbox" checked> auto-scroll</label>
    <button onclick="clearLogPanel()">Clear</button>
    <button onclick="toggleLogs()">✕ Close</button>
  </div>
  <div id="log-panel-body"></div>
</div>

<div id="canvas-container">
    <canvas id="airport-canvas"></canvas>
</div>

<div id="sidebar">
    <div class="panel">
        <div class="panel-header">Legend</div>
        <div class="legend">
            <div class="legend-item"><div class="swatch" style="background:#3b82f6"></div>Runway</div>
            <div class="legend-item"><div class="swatch" style="background:#22c55e"></div>Parking</div>
            <div class="legend-item"><div class="swatch" style="background:#a855f7"></div>Gate</div>
            <div class="legend-item"><div class="swatch" style="background:#f59e0b"></div>Service</div>
            <div class="legend-item"><div class="swatch" style="background:#ef4444"></div>Plane</div>
            <div class="legend-item"><div class="swatch" style="background:#06b6d4"></div>Vehicle</div>
            <div class="legend-item"><div class="swatch" style="background:#ec4899"></div>Bus</div>
            <div class="legend-item"><div class="swatch" style="background:#6b7280"></div>Road</div>
        </div>
    </div>

    <div class="panel">
        <div class="panel-header">
            Flights <span class="badge" id="flight-count">0</span>
        </div>
        <div class="panel-body" id="flights-list"></div>
    </div>

    <div class="panel">
        <div class="panel-header">
            Sessions <span class="badge" id="session-count">0</span>
        </div>
        <div class="panel-body" id="sessions-list"></div>
    </div>

    <div class="panel">
        <div class="panel-header">
            Buses <span class="badge" id="bus-count">0</span>
        </div>
        <div class="panel-body" id="buses-list"></div>
    </div>

    <div class="panel">
        <div class="panel-header">
            Handling <span class="badge" id="handling-count">0</span>
        </div>
        <div class="panel-body" id="handling-list"></div>
    </div>

    <div class="panel">
        <div class="panel-header">
            Catering <span class="badge" id="catering-count">0</span>
        </div>
        <div class="panel-body" id="catering-list"></div>
    </div>

</div>

<script>
// =========================================================================
// Node coordinates — realistic airport layout
// =========================================================================
const NODE_COORDS = {
    // Runway entrance (top-left)
    "RE-1": { x: 80, y: 180 },

    // Plane crossroads (taxiway — horizontal line)
    "PCR-5": { x: 220, y: 180 },
    "PCR-4": { x: 380, y: 180 },
    "PCR-3": { x: 540, y: 180 },
    "PCR-2": { x: 700, y: 180 },
    "PCR-1": { x: 860, y: 180 },

    // Plane parkings (just below taxiway)
    "P-5": { x: 220, y: 280 },
    "P-4": { x: 380, y: 280 },
    "P-3": { x: 540, y: 280 },
    "P-2": { x: 700, y: 280 },
    "P-1": { x: 860, y: 280 },

    // FollowMe station (near RE-1)
    "FS-1": { x: 140, y: 100 },

    // Car crossroads (below parkings, service area)
    "CR-5": { x: 220, y: 420 },
    "CR-4": { x: 380, y: 420 },
    "CR-3": { x: 540, y: 420 },
    "CR-2": { x: 700, y: 420 },
    "CR-1": { x: 860, y: 420 },

    // Plane Servicing points (between parking and car crossroads)
    "CP-51": { x: 190, y: 350 }, "CP-52": { x: 250, y: 350 },
    "CP-41": { x: 350, y: 350 }, "CP-42": { x: 410, y: 350 },
    "CP-31": { x: 510, y: 350 }, "CP-32": { x: 570, y: 350 },
    "CP-21": { x: 670, y: 350 }, "CP-22": { x: 730, y: 350 },
    "CP-11": { x: 830, y: 350 }, "CP-12": { x: 890, y: 350 },

    // Gates (below car crossroads)
    "G-51": { x: 190, y: 500 }, "G-52": { x: 250, y: 500 },
    "G-41": { x: 350, y: 500 }, "G-42": { x: 410, y: 500 },
    "G-31": { x: 510, y: 500 }, "G-32": { x: 570, y: 500 },
    "G-21": { x: 670, y: 500 }, "G-22": { x: 730, y: 500 },
    "G-11": { x: 830, y: 500 }, "G-12": { x: 890, y: 500 },

    // Baggage rooms
    "BR-51": { x: 220, y: 560 },
    "BR-41": { x: 380, y: 560 },
    "BR-31": { x: 540, y: 560 },
    "BR-21": { x: 700, y: 560 },
    "BR-11": { x: 860, y: 560 },

    // Special facilities — fan out from CR-1 (right) and CR-5 (left)
    // matches old visualizer layout (way2.txt proportions)
    "RS-1": { x: 120, y: 500 },   // Refuel station — lower-left of CR-5
    "CS-1": { x: 990, y: 270 },   // Catering service — upper-right of CR-1 (E-30)
    "BW-1": { x: 1040, y: 390 },  // Baggage warehouse — right of CR-1 (E-31)
    "CG-1": { x: 960, y: 458 },   // Car garage — right of CR-1 (E-33)
    "XT-1": { x: 1030, y: 500 },  // Exit — lower-right of CR-1 (E-32)
};

// =========================================================================
// Node type → visual style
// =========================================================================
const NODE_STYLES = {
    runwayEntrance:  { color: "#3b82f6", radius: 14, shape: "diamond" },
    planeParking:    { color: "#22c55e", radius: 12, shape: "rect" },
    planeCrossroad:  { color: "#64748b", radius: 6,  shape: "circle" },
    crossRoad:       { color: "#64748b", radius: 6,  shape: "circle" },
    followMeStation: { color: "#06b6d4", radius: 10, shape: "triangle" },
    planeServicing:  { color: "#f59e0b", radius: 8,  shape: "circle" },
    gate:            { color: "#a855f7", radius: 9,  shape: "rect" },
    baggageRoom:     { color: "#78716c", radius: 7,  shape: "rect" },
    cateringService: { color: "#f97316", radius: 8,  shape: "circle" },
    baggageWarehouse:{ color: "#78716c", radius: 8,  shape: "rect" },
    carGarage:       { color: "#6b7280", radius: 8,  shape: "rect" },
    refuelStation:   { color: "#eab308", radius: 9,  shape: "diamond" },
    exit:            { color: "#94a3b8", radius: 7,  shape: "circle" },
};

// =========================================================================
// Canvas setup
// =========================================================================
const canvas = document.getElementById("airport-canvas");
const ctx = canvas.getContext("2d");
let W, H;
let scale = 1.0, offsetX = 50, offsetY = 20;
let dragging = false, dragStartX = 0, dragStartY = 0;
let mapData = null;
let snapshotData = null;

// =========================================================================
// Event-driven vehicle positions (updated at 500 ms from GC events)
// =========================================================================
let evtVehiclePos = {};   // vehicleId → {kind:'node'|'edge', node?, edgeName?, from?, to?}
let lastEvtSeq = 0;

function resize() {
    const container = document.getElementById("canvas-container");
    W = container.clientWidth;
    H = container.clientHeight;
    canvas.width = W * devicePixelRatio;
    canvas.height = H * devicePixelRatio;
    canvas.style.width = W + "px";
    canvas.style.height = H + "px";
    ctx.setTransform(devicePixelRatio, 0, 0, devicePixelRatio, 0, 0);
    draw();
}
window.addEventListener("resize", resize);

// Pan
canvas.addEventListener("mousedown", (e) => { dragging = true; dragStartX = e.clientX; dragStartY = e.clientY; });
canvas.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    offsetX += e.clientX - dragStartX;
    offsetY += e.clientY - dragStartY;
    dragStartX = e.clientX;
    dragStartY = e.clientY;
    draw();
});
canvas.addEventListener("mouseup", () => { dragging = false; });
canvas.addEventListener("mouseleave", () => { dragging = false; });

// Zoom
canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    const zoomFactor = e.deltaY > 0 ? 0.9 : 1.1;
    const rect = canvas.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    const my = e.clientY - rect.top;

    offsetX = mx - (mx - offsetX) * zoomFactor;
    offsetY = my - (my - offsetY) * zoomFactor;
    scale *= zoomFactor;
    draw();
});

function tx(x) { return x * scale + offsetX; }
function ty(y) { return y * scale + offsetY; }

// =========================================================================
// Drawing
// =========================================================================
function draw() {
    ctx.clearRect(0, 0, W, H);

    // Background grid
    ctx.strokeStyle = "#1a2035";
    ctx.lineWidth = 0.5;
    const gs = 50 * scale;
    for (let x = (offsetX % gs); x < W; x += gs) {
        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
    }
    for (let y = (offsetY % gs); y < H; y += gs) {
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
    }

    // Runway (decorative)
    drawRunway();

    if (!mapData) return;

    // Build occupancy lookup from snapshot
    const nodeOcc = {};
    const edgeOcc = {};
    if (snapshotData && snapshotData.gc) {
        const gc = snapshotData.gc;
        if (gc.nodes) gc.nodes.forEach(n => { nodeOcc[n.name] = (n.occupiedBy || []).slice(); });
        if (gc.edges) gc.edges.forEach(e => { edgeOcc[e.name] = (e.occupiedBy || []).slice(); });
    }

    // --- Event-driven overrides (sub-second accuracy) ---
    Object.entries(evtVehiclePos).forEach(([vid, pos]) => {
        const tag = `vehicle:${vid}`;
        // Evict from all snapshot slots
        [...Object.values(nodeOcc), ...Object.values(edgeOcc)].forEach(arr => {
            const i = arr.indexOf(tag); if (i !== -1) arr.splice(i, 1);
        });
        // Place at event-derived position
        if (pos.kind === 'node' && pos.node) {
            (nodeOcc[pos.node] ??= []).push(tag);
        } else if (pos.kind === 'edge' && pos.edgeName) {
            (edgeOcc[pos.edgeName] ??= []).push(tag);
        }
    });

    // Draw edges
    if (mapData.edges) {
        mapData.edges.forEach(edge => {
            const c1 = NODE_COORDS[edge.node1];
            const c2 = NODE_COORDS[edge.node2];
            if (!c1 || !c2) return;

            const isPlaneRoad = edge.type && edge.type.includes("planeRoad");
            const occ = edgeOcc[edge.name] || [];

            ctx.beginPath();
            ctx.moveTo(tx(c1.x), ty(c1.y));
            ctx.lineTo(tx(c2.x), ty(c2.y));

            if (occ.length > 0) {
                ctx.strokeStyle = "#f59e0b";
                ctx.lineWidth = 3 * scale;
            } else if (isPlaneRoad) {
                ctx.strokeStyle = "#2d4a6f";
                ctx.lineWidth = 2.5 * scale;
            } else {
                ctx.strokeStyle = "#1e3045";
                ctx.lineWidth = 1.5 * scale;
            }
            ctx.stroke();

            // Draw vehicles on edges
            if (occ.length > 0) {
                const mx = (tx(c1.x) + tx(c2.x)) / 2;
                const my = (ty(c1.y) + ty(c2.y)) / 2;
                occ.forEach((v, i) => {
                    drawVehicleIcon(mx + i * 15 * scale, my - 12 * scale, v);
                });
            }
        });
    }

    // Draw nodes
    if (mapData.nodes) {
        mapData.nodes.forEach(node => {
            const c = NODE_COORDS[node.name];
            if (!c) return;

            const style = NODE_STYLES[node.type] || { color: "#555", radius: 6, shape: "circle" };
            const occ = nodeOcc[node.name] || [];
            const hasPlane = occ.some(o => o.startsWith("plane:"));
            const hasVehicle = occ.some(o => o.startsWith("vehicle:") || o.startsWith("BUS-"));

            const x = tx(c.x), y = ty(c.y);
            const r = style.radius * scale;

            // Glow if occupied
            if (hasPlane || hasVehicle) {
                ctx.save();
                ctx.shadowColor = hasPlane ? "#ef4444" : "#06b6d4";
                ctx.shadowBlur = 15 * scale;
                ctx.fillStyle = hasPlane ? "rgba(239,68,68,0.2)" : "rgba(6,182,212,0.15)";
                ctx.beginPath();
                ctx.arc(x, y, r * 2, 0, Math.PI * 2);
                ctx.fill();
                ctx.restore();
            }

            // Draw node shape
            ctx.fillStyle = style.color;
            ctx.strokeStyle = occ.length > 0 ? "#ffffff" : style.color;
            ctx.lineWidth = occ.length > 0 ? 2 * scale : 1 * scale;

            if (style.shape === "rect") {
                ctx.beginPath();
                ctx.rect(x - r, y - r * 0.7, r * 2, r * 1.4);
                ctx.fill(); ctx.stroke();
            } else if (style.shape === "diamond") {
                ctx.beginPath();
                ctx.moveTo(x, y - r);
                ctx.lineTo(x + r, y);
                ctx.lineTo(x, y + r);
                ctx.lineTo(x - r, y);
                ctx.closePath();
                ctx.fill(); ctx.stroke();
            } else if (style.shape === "triangle") {
                ctx.beginPath();
                ctx.moveTo(x, y - r);
                ctx.lineTo(x + r, y + r * 0.7);
                ctx.lineTo(x - r, y + r * 0.7);
                ctx.closePath();
                ctx.fill(); ctx.stroke();
            } else {
                ctx.beginPath();
                ctx.arc(x, y, r, 0, Math.PI * 2);
                ctx.fill(); ctx.stroke();
            }

            // Node label
            ctx.fillStyle = "#8892a4";
            ctx.font = `${Math.max(9, 10 * scale)}px 'Segoe UI', sans-serif`;
            ctx.textAlign = "center";
            ctx.fillText(node.name, x, y + r + 12 * scale);

            // Draw occupants
            occ.forEach((o, i) => {
                const ox = x + (i - occ.length / 2) * 18 * scale;
                const oy = y - r - 14 * scale;
                drawVehicleIcon(ox, oy, o);
            });
        });
    }

    // Terminal building (decorative)
    drawTerminal();
}

function drawRunway() {
    const rx1 = tx(-40), ry = ty(180);
    const rx2 = tx(80);
    const rw = 20 * scale;

    // Runway strip
    ctx.fillStyle = "#1a2744";
    ctx.fillRect(rx1, ry - rw, rx2 - rx1 + 20 * scale, rw * 2);

    // Center line dashes
    ctx.setLineDash([10 * scale, 8 * scale]);
    ctx.strokeStyle = "#3b82f6";
    ctx.lineWidth = 2 * scale;
    ctx.beginPath();
    ctx.moveTo(rx1, ry);
    ctx.lineTo(rx2 + 10 * scale, ry);
    ctx.stroke();
    ctx.setLineDash([]);

    // Label
    ctx.fillStyle = "#3b82f6";
    ctx.font = `bold ${12 * scale}px 'Segoe UI', sans-serif`;
    ctx.textAlign = "center";
    ctx.fillText("RUNWAY", (rx1 + rx2) / 2, ry - rw - 5 * scale);
}

function drawTerminal() {
    // Terminal building behind gates
    const tx1 = tx(170), ty1 = ty(530);
    const tw = tx(920) - tx1, th = 30 * scale;

    ctx.fillStyle = "rgba(139, 92, 246, 0.08)";
    ctx.strokeStyle = "rgba(139, 92, 246, 0.3)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.roundRect(tx1, ty1, tw, th, 4 * scale);
    ctx.fill(); ctx.stroke();

    ctx.fillStyle = "#7c3aed";
    ctx.font = `${11 * scale}px 'Segoe UI', sans-serif`;
    ctx.textAlign = "center";
    ctx.fillText("T E R M I N A L", tx1 + tw / 2, ty1 + th / 2 + 4 * scale);
}

function drawVehicleIcon(x, y, label) {
    const s = 6 * scale;
    // GC prefixes all vehicle ids with "vehicle:" in occupiedBy lists
    const clean = label.startsWith("vehicle:") ? label.slice(8) : label;

    if (label.startsWith("plane:")) {
        // Plane icon — red triangle
        ctx.fillStyle = "#ef4444";
        ctx.beginPath();
        ctx.moveTo(x, y - s);
        ctx.lineTo(x + s, y + s * 0.6);
        ctx.lineTo(x - s, y + s * 0.6);
        ctx.closePath();
        ctx.fill();

        ctx.fillStyle = "#ffffff";
        ctx.font = `bold ${Math.max(7, 8 * scale)}px sans-serif`;
        ctx.textAlign = "center";
        ctx.fillText(label.replace("plane:", "").substring(0, 6), x, y + s + 10 * scale);
    } else if (clean.startsWith("CT-")) {
        // Catering truck icon — amber rectangle
        ctx.fillStyle = "#f59e0b";
        ctx.fillRect(x - s, y - s * 0.6, s * 2, s * 1.2);

        ctx.fillStyle = "#000000";
        ctx.font = `bold ${Math.max(6, 7 * scale)}px sans-serif`;
        ctx.textAlign = "center";
        ctx.fillText(clean.substring(0, 6), x, y + s + 8 * scale);
    } else if (clean.startsWith("RT-")) {
        // Refueler truck icon — green rectangle
        ctx.fillStyle = "#22c55e";
        ctx.fillRect(x - s, y - s * 0.6, s * 2, s * 1.2);

        ctx.fillStyle = "#000000";
        ctx.font = `bold ${Math.max(6, 7 * scale)}px sans-serif`;
        ctx.textAlign = "center";
        ctx.fillText(clean.substring(0, 6), x, y + s + 8 * scale);
    } else if (clean.startsWith("BUS-") || clean.includes("bus")) {
        // Bus icon — pink square
        ctx.fillStyle = "#ec4899";
        ctx.fillRect(x - s, y - s * 0.6, s * 2, s * 1.2);

        ctx.fillStyle = "#ffffff";
        ctx.font = `${Math.max(6, 7 * scale)}px sans-serif`;
        ctx.textAlign = "center";
        ctx.fillText(clean, x, y + s + 8 * scale);
    } else {
        // Other vehicle — cyan circle
        ctx.fillStyle = "#06b6d4";
        ctx.beginPath();
        ctx.arc(x, y, s * 0.7, 0, Math.PI * 2);
        ctx.fill();

        ctx.fillStyle = "#ffffff";
        ctx.font = `${Math.max(6, 7 * scale)}px sans-serif`;
        ctx.textAlign = "center";
        ctx.fillText(clean.substring(0, 6), x, y + s + 8 * scale);
    }
}

// =========================================================================
// Sidebar updates
// =========================================================================
function updateSidebar(data) {
    // Flights
    const flightsEl = document.getElementById("flights-list");
    const flights = (data.flights && data.flights.flights) || [];
    document.getElementById("flight-count").textContent = flights.length;
    flightsEl.innerHTML = flights.map(f => `
        <div class="item">
            <span class="id">${f.flightId}</span>
            <span class="status-badge status-${(f.status||'').toLowerCase().replace(/\s+/g,'')}">${f.status || '?'}</span>
        </div>
    `).join("") || '<div style="color:#555;padding:8px">No flights</div>';

    // Sessions
    const sessionsEl = document.getElementById("sessions-list");
    const sessions = (data.gc && data.gc.sessions) || [];
    document.getElementById("session-count").textContent = sessions.length;
    sessionsEl.innerHTML = sessions.map(s => `
        <div class="item">
            <span class="id">${s.flightId}</span>
            <span style="font-size:10px;color:#8892a4">${s.parkingNode || ''}</span>
            <span class="status-badge status-${(s.status||'').toLowerCase().replace(/\s+/g,'')}">${s.status || '?'}</span>
        </div>
    `).join("") || '<div style="color:#555;padding:8px">No sessions</div>';

    // Buses
    const busesEl = document.getElementById("buses-list");
    const buses = (data.buses && data.buses.buses) || [];
    document.getElementById("bus-count").textContent = buses.length;
    busesEl.innerHTML = buses.map(b => `
        <div class="item">
            <span class="id">${b.busId}</span>
            <span style="font-size:10px;color:#8892a4">${b.currentNode || ''} ${b.flightId ? '→'+b.flightId : ''}</span>
            <span class="status-badge status-${(b.status||'').toLowerCase()}">${b.status || '?'}</span>
        </div>
    `).join("") || '<div style="color:#555;padding:8px">No buses</div>';

    // Handling
    const handlingEl = document.getElementById("handling-list");
    const tasks = (data.handling && data.handling.tasks) || [];
    document.getElementById("handling-count").textContent = tasks.length;
    handlingEl.innerHTML = tasks.map(t => {
        const checks = Object.entries(t.checklist || {}).map(([k,v]) =>
            `<span style="margin-right:4px" class="status-badge status-${v.status}">${k}</span>`
        ).join("");
        return `
            <div class="item" style="flex-direction:column;align-items:flex-start">
                <div style="display:flex;justify-content:space-between;width:100%">
                    <span class="id">${t.flightId}</span>
                    <span class="status-badge status-${(t.status||'').toLowerCase().replace(/[_\s]/g,'')}">${t.status}</span>
                </div>
                <div style="margin-top:4px">${checks}</div>
            </div>
        `;
    }).join("") || '<div style="color:#555;padding:8px">No handling tasks</div>';

    // Catering trucks
    const cateringEl = document.getElementById("catering-list");
    const cateringTrucks = data.catering_trucks || [];
    document.getElementById("catering-count").textContent = cateringTrucks.length;
    cateringEl.innerHTML = cateringTrucks.map(t => `
        <div class="item">
            <span class="id">${t.id}</span>
            <span style="font-size:10px;color:#8892a4">${t.currentLocation || ''}</span>
            <span class="status-badge status-${(t.status||'').toLowerCase()}">${t.status || '?'}</span>
        </div>
    `).join("") || '<div style="color:#555;padding:8px">No catering trucks</div>';
}

// =========================================================================
// Data fetching
// =========================================================================
async function fetchMap() {
    try {
        const r = await fetch("/api/map");
        mapData = await r.json();
    } catch (e) {
        console.error("Failed to fetch map:", e);
    }
}

async function pollEvents() {
    try {
        const r = await fetch(`/api/gc-events?since=${lastEvtSeq}`);
        const data = await r.json();
        let changed = false;
        for (const ev of (data.events || [])) {
            if (ev.seq <= lastEvtSeq) continue;
            lastEvtSeq = ev.seq;
            const p = ev.payload || {};
            const vid = p.vehicleId;
            if (!vid) continue;
            if (ev.type === 'vehicle.init' || ev.type === 'vehicle.leave_edge') {
                evtVehiclePos[vid] = { kind: 'node', node: p.node || p.to };
                changed = true;
            } else if (ev.type === 'vehicle.enter_edge') {
                evtVehiclePos[vid] = { kind: 'edge', edgeName: p.edge, from: p.from, to: p.to };
                changed = true;
            }
        }
        if (lastEvtSeq > 0) {
            document.getElementById('evt-count').textContent = `⚡ seq ${lastEvtSeq}`;
        }
        if (changed) draw();
    } catch (e) { /* silent */ }
}

async function fetchSnapshot() {
    try {
        const r = await fetch("/api/snapshot");
        snapshotData = await r.json();
        document.getElementById("update-time").textContent =
            "Updated: " + new Date().toLocaleTimeString();
        updateSidebar(snapshotData);
        draw();
    } catch (e) {
        console.error("Failed to fetch snapshot:", e);
        document.getElementById("update-time").textContent = "Connection error";
    }
}

// =========================================================================
// Demo
// =========================================================================
let demoRunning = false;
let demoSource = null;

// =========================================================================
// Log panel
// =========================================================================
let logPanelOpen = false;
let logPollTimer = null;
let logSince = 0;
const LOG_COLORS = {
  'ground-control':     '#06b6d4',
  'information-panel':  '#a78bfa',
  'followme':           '#34d399',
  'board':              '#f59e0b',
  'handling-supervisor':'#fb923c',
  'passenger-bus':      '#f472b6',
  'catering':           '#fbbf24',
};

function _escHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

function toggleLogs() {
  logPanelOpen = !logPanelOpen;
  document.body.classList.toggle('logs-open', logPanelOpen);
  resize();
  if (logPanelOpen) {
    logSince = Math.floor(Date.now() / 1000) - 30;
    fetchLogTail(true);
    logPollTimer = setInterval(() => fetchLogTail(false), 2500);
  } else {
    clearInterval(logPollTimer);
    logPollTimer = null;
  }
}

function clearLogPanel() {
  document.getElementById('log-panel-body').innerHTML = '';
}

async function fetchLogTail(initial) {
  const since = initial ? 0 : logSince;
  const n = initial ? 150 : 0;
  logSince = Math.floor(Date.now() / 1000);
  try {
    const qs = since ? `?since=${since}` : `?n=${n}`;
    const resp = await fetch(`/api/logs/tail${qs}`);
    const data = await resp.json();
    const body = document.getElementById('log-panel-body');
    const filter = document.getElementById('log-filter').value;
    let added = 0;
    for (const [svc, lines] of Object.entries(data)) {
      if (filter !== 'all' && svc !== filter) continue;
      const color = LOG_COLORS[svc] || '#8892a4';
      for (const line of lines) {
        if (!line.trim()) continue;
        const div = document.createElement('div');
        div.className = 'log-line';
        div.innerHTML = `<span style="color:${color};font-weight:600">[${svc}]</span> ` +
                        `<span style="color:#9ca3af">${_escHtml(line)}</span>`;
        body.appendChild(div);
        added++;
      }
    }
    while (body.children.length > 2000) body.removeChild(body.firstChild);
    const autoScroll = document.getElementById('log-autoscroll');
    if (autoScroll && autoScroll.checked && added > 0) body.scrollTop = body.scrollHeight;
  } catch(e) {
    console.warn('log fetch error', e);
  }
}

function openDemoModal() {
    document.getElementById("demo-modal").style.display = "flex";
}
function closeDemo() {
    if (demoSource) { demoSource.close(); demoSource = null; }
    if (demoPollTimer) { clearInterval(demoPollTimer); demoPollTimer = null; }
    document.getElementById("demo-modal").style.display = "none";
    demoRunning = false;
    document.getElementById("demo-btn").disabled = false;
    document.getElementById("demo-btn").textContent = "▶ DEMO";
}

function logDemo(msg, kind) {
    const el = document.getElementById("demo-log");
    const colors = { start: "#06b6d4", done: "#22c55e", error: "#ef4444", launched: "#f59e0b" };
    const color = colors[kind] || "#c8d6e5";
    const line = document.createElement("div");
    line.style.color = color;
    if (kind === "start" || kind === "done") line.style.fontWeight = "700";
    line.textContent = msg;
    el.appendChild(line);
    el.scrollTop = el.scrollHeight;
}

let demoPollTimer = null;
let demoPollSessionId = null;
let demoPollIdx = 0;

function startDemoPoll(sessionId) {
    demoPollSessionId = sessionId;
    demoPollIdx = 0;
    demoPollTimer = setInterval(async () => {
        try {
            const r = await fetch(`/api/demo/status/${sessionId}`);
            const d = await r.json();
            const logs = d.logs || [];
            // Append only new messages
            for (let i = demoPollIdx; i < logs.length; i++) {
                logDemo(logs[i].msg, logs[i].kind);
            }
            demoPollIdx = logs.length;
            if (d.done) {
                clearInterval(demoPollTimer);
                demoPollTimer = null;
                demoRunning = false;
                document.getElementById("demo-btn").disabled = false;
                document.getElementById("demo-btn").textContent = "▶ DEMO";
                document.getElementById("demo-start-btn").disabled = false;
            }
        } catch(e) {
            logDemo(`⚠ Ошибка опроса статуса: ${e}`, "error");
        }
    }, 2500);
}

function startDemo() {
    if (demoRunning) return;
    demoRunning = true;
    openDemoModal();

    const log = document.getElementById("demo-log");
    log.innerHTML = "";
    document.getElementById("demo-btn").disabled = true;
    document.getElementById("demo-btn").textContent = "⏳ DEMO";
    document.getElementById("demo-start-btn").disabled = true;

    if (demoSource) { demoSource.close(); }
    if (demoPollTimer) { clearInterval(demoPollTimer); demoPollTimer = null; }

    demoSource = new EventSource("/api/demo/landing");

    demoSource.onmessage = (e) => {
        try {
            const d = JSON.parse(e.data);
            logDemo(d.msg, d.kind);
            if (d.kind === "launched") {
                // SSE stream ends here — switch to background polling
                demoSource.close();
                demoSource = null;
                startDemoPoll(d.sessionId);
            } else if (d.kind === "done" || d.kind === "error") {
                demoSource.close();
                demoSource = null;
                demoRunning = false;
                document.getElementById("demo-btn").disabled = false;
                document.getElementById("demo-btn").textContent = "▶ DEMO";
                document.getElementById("demo-start-btn").disabled = false;
            }
        } catch {}
    };

    demoSource.onerror = () => {
        if (!demoRunning) return; // already closed intentionally
        logDemo("⚠ SSE соединение прервано", "error");
        demoSource.close();
        demoSource = null;
        demoRunning = false;
        document.getElementById("demo-btn").disabled = false;
        document.getElementById("demo-btn").textContent = "▶ DEMO";
        document.getElementById("demo-start-btn").disabled = false;
    };
}

// =========================================================================
// Init
// =========================================================================
(async function init() {
    await fetchMap();
    resize();
    await fetchSnapshot();
    setInterval(fetchSnapshot, 2000);  // snapshot: full state every 2 s
    setInterval(pollEvents, 500);       // events:   vehicle moves in ~500 ms
})();
</script>
</body>
</html>
"""

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    log.info("Starting Airport Visualizer on port %d", PORT)
    log.info("  GC=%s:%d  INFO=%s:%d  HANDLING=%s:%d  BUS=%s:%d",
             GC_HOST, GC_PORT, INFO_HOST, INFO_PORT, HANDLING_HOST, HANDLING_PORT, BUS_HOST, BUS_PORT)
    uvicorn.run(app, host="0.0.0.0", port=PORT, log_level="info")
