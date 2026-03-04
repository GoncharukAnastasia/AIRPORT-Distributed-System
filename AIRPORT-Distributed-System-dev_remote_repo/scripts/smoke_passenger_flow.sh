#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v jq >/dev/null 2>&1; then
  echo "[smoke] jq is required. Install with: brew install jq"
  exit 1
fi

step() {
  echo
  echo "============================================================"
  echo "[smoke] $*"
  echo "============================================================"
}

need_health() {
  local url="$1"
  local tries=40
  for _ in $(seq 1 "$tries"); do
    if curl -fsS "$url" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  echo "[smoke] service is not healthy: $url"
  exit 1
}

assert_eq() {
  local actual="$1"
  local expected="$2"
  local msg="$3"
  if [[ "$actual" != "$expected" ]]; then
    echo "[smoke][FAIL] $msg"
    echo "  expected: $expected"
    echo "  actual:   $actual"
    exit 1
  fi
}

step "Check health for all services"
for u in \
  "http://localhost:8081/health" \
  "http://localhost:8082/health" \
  "http://localhost:8083/health" \
  "http://localhost:8084/health" \
  "http://localhost:8085/health" \
  "http://localhost:8003/health" \
  "http://localhost:8004/health" \
  "http://localhost:8005/health"; do
  need_health "$u"
  echo "[ok] $u"
done

TS="$(date +%s)"
DEPART_ISO="$(date -u -v+65M '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || python3 - <<'PY'
from datetime import datetime, timezone, timedelta
print((datetime.now(timezone.utc) + timedelta(minutes=65)).strftime('%Y-%m-%dT%H:%M:%SZ'))
PY
)"
ARRIVE_ISO="$(date -u -v+10M '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || python3 - <<'PY'
from datetime import datetime, timezone, timedelta
print((datetime.now(timezone.utc) + timedelta(minutes=10)).strftime('%Y-%m-%dT%H:%M:%SZ'))
PY
)"

DEPART_FLIGHT="FLD-${TS}"
ARRIVE_FLIGHT="FLA-${TS}"

step "Init flights in InformationPanel"
INIT_PAYLOAD="$(cat <<JSON
{
  "flights": [
    {
      "flightId": "$DEPART_FLIGHT",
      "planeId": "PL-$TS",
      "type": "depart",
      "fromCity": "Moscow",
      "toCity": "Paris",
      "scheduledTime": "$DEPART_ISO",
      "status": "RegistrationOpen",
      "autoManage": false
    },
    {
      "flightId": "$ARRIVE_FLIGHT",
      "planeId": "PLA-$TS",
      "type": "arrive",
      "fromCity": "Sochi",
      "toCity": "Moscow",
      "scheduledTime": "$ARRIVE_ISO",
      "status": "Scheduled",
      "autoManage": true
    }
  ]
}
JSON
)"

INIT_RESP="$(curl -sS -X POST "http://localhost:8082/v1/flights/init" -H "Content-Type: application/json" -d "$INIT_PAYLOAD")"
echo "$INIT_RESP" | jq .
assert_eq "$(echo "$INIT_RESP" | jq -r '.ok')" "true" "init flights failed"

step "Create passenger with explicit flight"
CREATE_PAYLOAD="$(cat <<JSON
{
  "name": "Alice_$TS",
  "flightId": "$DEPART_FLIGHT",
  "baggageWeight": 12.5,
  "menuType": "fish",
  "isVIP": false
}
JSON
)"

PASSENGER_RESP="$(curl -sS -X POST "http://localhost:8004/v1/passengers" -H "Content-Type: application/json" -d "$CREATE_PAYLOAD")"
echo "$PASSENGER_RESP" | jq .

PASSENGER_ID="$(echo "$PASSENGER_RESP" | jq -r '.id')"
TICKET_ID="$(echo "$PASSENGER_RESP" | jq -r '.ticket.ticketId // empty')"
STATE="$(echo "$PASSENGER_RESP" | jq -r '.state')"

if [[ -z "$PASSENGER_ID" || "$PASSENGER_ID" == "null" ]]; then
  echo "[smoke][FAIL] passenger id is empty"
  exit 1
fi
if [[ -z "$TICKET_ID" ]]; then
  echo "[smoke][FAIL] ticket id is empty"
  exit 1
fi
if [[ "$STATE" != "GotTicket" && "$STATE" != "CheckedIn" ]]; then
  echo "[smoke][FAIL] unexpected initial passenger state: $STATE"
  exit 1
fi

echo "[smoke] passengerId=$PASSENGER_ID"
echo "[smoke] ticketId=$TICKET_ID"

step "Verify ticket in TicketSales"
VERIFY_PAYLOAD="$(cat <<JSON
{
  "ticketId": "$TICKET_ID",
  "passengerId": "$PASSENGER_ID",
  "flightId": "$DEPART_FLIGHT",
  "passengerName": "Alice_$TS"
}
JSON
)"
VERIFY_RESP="$(curl -sS -X POST "http://localhost:8003/v1/tickets/verify" -H "Content-Type: application/json" -d "$VERIFY_PAYLOAD")"
echo "$VERIFY_RESP" | jq .
assert_eq "$(echo "$VERIFY_RESP" | jq -r '.valid')" "true" "ticket verification failed"

step "Run explicit passenger check-in"
CHECKIN_RESP="$(curl -sS -X POST "http://localhost:8004/v1/passengers/$PASSENGER_ID/checkin")"
echo "$CHECKIN_RESP" | jq .
assert_eq "$(echo "$CHECKIN_RESP" | jq -r '.state')" "CheckedIn" "passenger check-in failed"

step "Get IDs for bus and transition CheckedIn -> OnBus"
IDS_RESP="$(curl -sS "http://localhost:8004/v1/passengersId/flight/$DEPART_FLIGHT")"
echo "$IDS_RESP" | jq .
FOUND_ID="$(echo "$IDS_RESP" | jq -r --arg PID "$PASSENGER_ID" '.passengers[]? | select(. == $PID)')"
if [[ -z "$FOUND_ID" ]]; then
  echo "[smoke][FAIL] passenger id was not returned by /v1/passengersId/flight"
  exit 1
fi

step "Set boarded status"
BOARD_PAYLOAD="$(cat <<JSON
{
  "passenger_ids": ["$PASSENGER_ID"]
}
JSON
)"
BOARD_RESP="$(curl -sS -X POST "http://localhost:8004/v1/passengers/board" -H "Content-Type: application/json" -d "$BOARD_PAYLOAD")"
echo "$BOARD_RESP" | jq .
if [[ "$(echo "$BOARD_RESP" | jq -r '.updated')" -lt 1 ]]; then
  echo "[smoke][FAIL] board update count < 1"
  exit 1
fi

step "Check final passenger state"
FINAL_PASSENGER="$(curl -sS "http://localhost:8004/v1/passengers/$PASSENGER_ID")"
echo "$FINAL_PASSENGER" | jq .
assert_eq "$(echo "$FINAL_PASSENGER" | jq -r '.state')" "Boarded" "final state is not Boarded"

step "Negative scenario: fake ticket"
NEG_CREATE_PAYLOAD="$(cat <<JSON
{
  "name": "Eve_$TS",
  "flightId": "$DEPART_FLIGHT",
  "baggageWeight": 5,
  "menuType": "meat",
  "isVIP": false
}
JSON
)"
NEG_PASSENGER_RESP="$(curl -sS -X POST "http://localhost:8004/v1/passengers" -H "Content-Type: application/json" -d "$NEG_CREATE_PAYLOAD")"
NEG_PASSENGER_ID="$(echo "$NEG_PASSENGER_RESP" | jq -r '.id')"

curl -sS -X POST "http://localhost:8004/ui/fake_ticket" \
  -H "Content-Type: application/json" \
  -d "{\"passengerId\":\"$NEG_PASSENGER_ID\"}" | jq .

NEG_CHECKIN="$(curl -sS -X POST "http://localhost:8004/v1/passengers/$NEG_PASSENGER_ID/checkin")"
echo "$NEG_CHECKIN" | jq .
assert_eq "$(echo "$NEG_CHECKIN" | jq -r '.state')" "CameToAirport" "fake ticket was expected to be rejected"

step "Smoke test completed successfully"
echo "[smoke][OK] All checks passed"
