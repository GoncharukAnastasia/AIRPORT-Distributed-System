#!/usr/bin/env bash
# Инициализация автобусов в Passenger Bus
set -euo pipefail

HOST="${BUS_HOST:-localhost}"
PORT="${BUS_PORT:-8086}"

echo "=== Init Passenger Buses ==="
curl -sS -X POST "http://${HOST}:${PORT}/v1/bus/init" \
  -H "Content-Type: application/json" \
  -d '{
    "buses": [
      {"busId": "BUS-1", "capacity": 20, "currentNode": "G-11"},
      {"busId": "BUS-2", "capacity": 20, "currentNode": "G-21"},
      {"busId": "BUS-3", "capacity": 20, "currentNode": "G-31"},
      {"busId": "BUS-4", "capacity": 20, "currentNode": "G-41"},
      {"busId": "BUS-5", "capacity": 20, "currentNode": "G-51"}
    ]
  }' | python3 -m json.tool || jq .

echo ""
echo "=== Bus Fleet ==="
curl -sS "http://${HOST}:${PORT}/v1/bus/vehicles" | python3 -m json.tool || jq .
