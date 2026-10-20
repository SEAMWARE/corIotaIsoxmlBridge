# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - the verbs this repo's tests use; corTest sources it into every section
#
# A test starts a broker that loads THIS build of the plugin (by path - nothing is installed), on a port
# of its own, with ramDB as its store: no database server is needed. A test waits on and asserts on what
# the API says, never on the broker's log - the log is kept for reading when a test fails.
#
#   ISOXML_BROKER   the broker binary (default: coraine on PATH - the coraine package, or
#                 `. <dir>/install/env` after coraine-build)
#   ISOXML_PORT     its port (default 19034 - clear of a broker on 1026, of coraine's own test ports and of the other bridges' tests)
#   ISOXML_PLUGIN   the plugin under test (default: iotaisoxml.so in this repo)
#   ISOXML_TEST_DIR scratch directory: the bridge configuration, the broker's pid and log
#
ISOXML_BROKER="${ISOXML_BROKER:-coraine}"
ISOXML_PORT="${ISOXML_PORT:-19034}"
ISOXML_PLUGIN="${ISOXML_PLUGIN:-${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}/iotaisoxml.so}"
ISOXML_TEST_DIR="${ISOXML_TEST_DIR:-/tmp/iotaisoxml-bridge-test}"


# -----------------------------------------------------------------------------
#
# brokerStart [broker options...] - start the broker with the plugin and $ISOXML_TEST_DIR/bridges.json
#
# Returns once the port answers; 1 (with the log's tail on stderr) if the broker died on the way.
#
brokerStart() {
  brokerStop

  "$ISOXML_BROKER" --port "$ISOXML_PORT" --foreground --pretty-print 2 --httpEndpoint "http://localhost:$ISOXML_PORT" \
                 --database ramDB --bridges "$ISOXML_PLUGIN" --bridgeConfig "$ISOXML_TEST_DIR/bridges.json" "$@" \
                 > "$ISOXML_TEST_DIR/broker.log" 2>&1 &
  echo $! > "$ISOXML_TEST_DIR/broker.pid"

  local i
  for ((i = 0; i < 100; i++)); do
    curl -s -o /dev/null "localhost:$ISOXML_PORT/ngsi-ld/v1/types" && return 0
    if ! kill -0 "$(cat "$ISOXML_TEST_DIR/broker.pid")" 2>/dev/null; then
      echo "brokerStart: the broker died - $ISOXML_TEST_DIR/broker.log:" >&2
      tail -20 "$ISOXML_TEST_DIR/broker.log" >&2
      return 1
    fi
    sleep 0.1
  done

  echo "brokerStart: no answer on port $ISOXML_PORT after 10s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# brokerStop - stop the broker brokerStart started (by its pid, never by name)
#
brokerStop() {
  local pidFile="$ISOXML_TEST_DIR/broker.pid"
  [ -f "$pidFile" ] || return 0

  local pid i
  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  rm -f "$pidFile"
}


# -----------------------------------------------------------------------------
#
# The southbound port - where the MICS POSTs task data (default 17896 - clear of iotagent-isoxml's 7896) -
# and the mock MICS's port
#
ISOXML_SOUTH_PORT="${ISOXML_SOUTH_PORT:-17896}"
ISOXML_MICS_PORT="${ISOXML_MICS_PORT:-18896}"


# -----------------------------------------------------------------------------
#
# taskPost <XML> [content type] - task data POSTed to /iot/isoxml, as the MICS does; the status, then the body
#
taskPost() {
  curl -s -w "\n%{http_code}\n" -X POST "localhost:$ISOXML_SOUTH_PORT/iot/isoxml" -H "Content-Type: ${2:-application/xml}" \
       --data-binary "$1" > "$ISOXML_TEST_DIR/post.out"
  tail -1 "$ISOXML_TEST_DIR/post.out"
  head -n -1 "$ISOXML_TEST_DIR/post.out" | sed "/^$/d"
}


# -----------------------------------------------------------------------------
#
# micsStart / micsStop - the mock MICS (micsMock.py); what it was sent is in $ISOXML_TEST_DIR/mics.log
#
micsStart() {
  python3 "$(dirname "${BASH_SOURCE[0]}")/micsMock.py" --port "$ISOXML_MICS_PORT" --log "$ISOXML_TEST_DIR/mics.log" \
          > "$ISOXML_TEST_DIR/mics.out" 2>&1 &
  echo $! > "$ISOXML_TEST_DIR/mics.pid"

  local i
  for ((i = 0; i < 50; i++)); do
    (echo > "/dev/tcp/127.0.0.1/$ISOXML_MICS_PORT") 2>/dev/null && return 0
    sleep 0.1
  done

  echo "micsStart: no answer on port $ISOXML_MICS_PORT" >&2
  return 1
}

micsStop() {
  [ -f "$ISOXML_TEST_DIR/mics.pid" ] && kill "$(cat "$ISOXML_TEST_DIR/mics.pid")" 2>/dev/null
  rm -f "$ISOXML_TEST_DIR/mics.pid"
}


# -----------------------------------------------------------------------------
#
# micsAwait <n> - wait for the MICS to have been sent <n> task data; what it was sent, the log emptied
#
micsAwait() {
  local i
  for ((i = 0; i < 50; i++)); do
    [ "$(wc -l < "$ISOXML_TEST_DIR/mics.log" 2>/dev/null || echo 0)" -ge "$1" ] && break
    sleep 0.1
  done
  cat "$ISOXML_TEST_DIR/mics.log" 2>/dev/null
  : > "$ISOXML_TEST_DIR/mics.log"
}


# -----------------------------------------------------------------------------
#
# entityAwait <id> <text> [secs] - wait until the entity (keyValues) has <text>
#
entityAwait() {
  local url="localhost:$ISOXML_PORT/ngsi-ld/v1/entities/$1?options=keyValues" secs="${3:-5}" i

  for ((i = 0; i < secs * 10; i++)); do
    curl -s "$url" | grep -qF -- "$2" && return 0
    sleep 0.1
  done

  echo "entityAwait: $1 is $(curl -s "$url"), without '$2', after ${secs}s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# entityGet <id> [options] - the entity, its members sorted
#
entityGet() {
  curl -s "localhost:$ISOXML_PORT/ngsi-ld/v1/entities/$1${2:+?options=$2}" \
    | python3 -c 'import sys, json; print(json.dumps(json.load(sys.stdin), indent=2, sort_keys=True))'
}


# -----------------------------------------------------------------------------
#
# attrPatch <id> <attr> <value as JSON> - PATCH an attribute's value; the HTTP status
#
attrPatch() {
  curl -s -o /dev/null -w "PATCH $2: %{http_code}\n" -X PATCH "localhost:$ISOXML_PORT/ngsi-ld/v1/entities/$1/attrs/$2" \
       -H 'Content-Type: application/json' -d "{\"value\": $3}"
}
