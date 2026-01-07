#!/bin/bash
# .github/scripts/push-to-loki.sh
LOKI_URL="$1"
RUN_ID="$2"
TEST_NAME="$3"

while IFS= read -r line; do
  # Escape quotes for JSON
  CLEAN_LINE=$(echo "$line" | sed 's/"/\\"/g')
  TIMESTAMP=$(date +%s%N)

  # Push to Loki API
  curl -H "Content-Type: application/json" -X POST -s \
    -d "{\"streams\": [{\"stream\": {\"job\": \"macos-build\", \"run_id\": \"$RUN_ID\", \"test_name\": \"$TEST_NAME\"}, \"values\": [[\"$TIMESTAMP\", \"$CLEAN_LINE\"]]}]}" \
    "$LOKI_URL"
done

