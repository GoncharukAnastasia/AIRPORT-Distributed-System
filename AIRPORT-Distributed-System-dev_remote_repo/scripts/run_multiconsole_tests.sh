#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v osascript >/dev/null 2>&1; then
  echo "[launcher] osascript is required (macOS only)."
  exit 1
fi

SCRIPT_HEALTH="cd '$ROOT_DIR'; bash scripts/watch_airport_console.sh health 2"
SCRIPT_FLIGHTS="cd '$ROOT_DIR'; bash scripts/watch_airport_console.sh flights 2"
SCRIPT_PASSENGERS="cd '$ROOT_DIR'; bash scripts/watch_airport_console.sh passengers 2"
SCRIPT_EVENTS="cd '$ROOT_DIR'; bash scripts/watch_airport_console.sh events 2"
SCRIPT_SMOKE="cd '$ROOT_DIR'; bash scripts/smoke_passenger_flow.sh; echo; echo '[launcher] smoke finished. Press Enter to close window.'; read -r _"

osascript <<OSA
 tell application "Terminal"
   activate
   do script "$SCRIPT_HEALTH"
   do script "$SCRIPT_FLIGHTS"
   do script "$SCRIPT_PASSENGERS"
   do script "$SCRIPT_EVENTS"
   do script "$SCRIPT_SMOKE"
 end tell
OSA

echo "[launcher] Opened Terminal sessions: health, flights, passengers, events, smoke"
