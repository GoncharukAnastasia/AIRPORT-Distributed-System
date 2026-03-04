curl -s -X POST "http://localhost:8084/v1/planes/airborne" \
  -H "Content-Type: application/json" \
  -d '{"flightId":"SU100","gcHost":"localhost","gcPort":8081,"pollSec":2,"touchdownDelaySec":1}'
