NOW=$(date +%s)

curl -s -X POST http://localhost:8082/v1/flights/init \
  -H "Content-Type: application/json" \
  -d "{
    \"flights\": [
      {\"flightId\":\"SU100\",\"scheduledAt\":$NOW,\"status\":\"Scheduled\",\"phase\":\"airborne\"},
      {\"flightId\":\"SU200\",\"scheduledAt\":$NOW,\"status\":\"Parked\",\"phase\":\"grounded\",\"parkingNode\":\"P-3\"}
    ]
  }"


