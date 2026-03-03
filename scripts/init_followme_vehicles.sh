curl -s -X POST http://localhost:8083/v1/vehicles/init \
  -H "Content-Type: application/json" \
  -d '{
    "vehicles": [
      {"vehicleId":"FM-1","currentNode":"FS-1","status":"empty"},
      {"vehicleId":"FM-2","currentNode":"FS-1","status":"empty"}
    ]
  }'