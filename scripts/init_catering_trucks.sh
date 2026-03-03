#!/usr/bin/env bash
# Инициализирует машины катеринга в сервисе CateringTruck.
# Запускать после того как все сервисы подняты (docker-compose up).

CT_URL="${CATERING_URL:-http://localhost:8086}"

echo "Инициализация CT-1 на FS-1..."
curl -s -X POST "${CT_URL}/v1/catering-trucks/init" \
  -H "Content-Type: application/json" \
  -d '{"id":"CT-1","location":"FS-1"}' | python3 -m json.tool

echo ""
echo "Инициализация CT-2 на FS-1..."
curl -s -X POST "${CT_URL}/v1/catering-trucks/init" \
  -H "Content-Type: application/json" \
  -d '{"id":"CT-2","location":"FS-1"}' | python3 -m json.tool

echo ""
echo "Список машин катеринга:"
curl -s "${CT_URL}/v1/catering-trucks" | python3 -m json.tool
