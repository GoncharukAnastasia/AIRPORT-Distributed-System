#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-health}"
INTERVAL="${2:-2}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v jq >/dev/null 2>&1; then
  echo "[watch] jq is required. Install with: brew install jq"
  exit 1
fi

clear_screen() {
  printf '\033c'
}

watch_health() {
  while true; do
    clear_screen
    echo "[watch:health] $(date '+%Y-%m-%d %H:%M:%S')"
    for u in \
      "http://localhost:8081/health" \
      "http://localhost:8082/health" \
      "http://localhost:8083/health" \
      "http://localhost:8084/health" \
      "http://localhost:8085/health" \
      "http://localhost:8003/health" \
      "http://localhost:8004/health" \
      "http://localhost:8005/health"; do
      name="$(echo "$u" | sed 's#http://localhost:##; s#/health##')"
      body="$(curl -s "$u" || true)"
      status="$(echo "$body" | jq -r '.status // "DOWN"' 2>/dev/null || echo "DOWN")"
      service="$(echo "$body" | jq -r '.service // "unknown"' 2>/dev/null || echo "unknown")"
      printf '%-8s %-20s %s\n' "$name" "$service" "$status"
    done
    sleep "$INTERVAL"
  done
}

watch_flights() {
  while true; do
    clear_screen
    echo "[watch:flights] $(date '+%Y-%m-%d %H:%M:%S')"
    curl -s "http://localhost:8082/v1/flights" | jq '{count: (.flights|length), flights: [.flights[] | {flightId, status, phase, gate, planeParking, scheduledTime}]}'
    sleep "$INTERVAL"
  done
}

watch_passengers() {
  while true; do
    clear_screen
    echo "[watch:passengers+tickets] $(date '+%Y-%m-%d %H:%M:%S')"
    echo "--- passengers ---"
    curl -s "http://localhost:8004/v1/passengers" | jq '[.[] | {id, name, flightId, state, isVIP}]'
    echo
    echo "--- tickets ---"
    curl -s "http://localhost:8003/v1/tickets" | jq '[.[] | {ticketId, flightId, passengerId, status, checkInTime}]'
    sleep "$INTERVAL"
  done
}

watch_events() {
  local since="0"
  while true; do
    clear_screen
    echo "[watch:ground-events] $(date '+%Y-%m-%d %H:%M:%S')"
    body="$(curl -s "http://localhost:8081/v1/visualizer/events?since=$since" || true)"
    echo "$body" | jq .
    since="$(echo "$body" | jq -r '.events[-1].seq // .lastSeq // 0' 2>/dev/null || echo 0)"
    sleep "$INTERVAL"
  done
}

case "$MODE" in
  health) watch_health ;;
  flights) watch_flights ;;
  passengers) watch_passengers ;;
  events) watch_events ;;
  *)
    echo "Usage: $0 {health|flights|passengers|events} [intervalSec]"
    exit 1
    ;;
esac
