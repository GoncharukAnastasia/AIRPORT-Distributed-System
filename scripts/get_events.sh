SINCE=0
while true; do
  RESP=$(curl -s "http://localhost:8081/v1/visualizer/events?since=$SINCE")
  echo "$RESP"
#  echo "$RESP" | jq '.events'
#  SINCE=$(echo "$RESP" | jq -r '.latestSeq')
  sleep 1
done