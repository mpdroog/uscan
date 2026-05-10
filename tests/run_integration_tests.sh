#!/bin/bash
# Integration test runner with proper server management
# The mock server runs for the duration of all tests
#
# Usage: ./tests/run_integration_tests.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Use random high ports to avoid conflicts
export USCAN_TEST_L1_PORT=$((10000 + RANDOM % 50000))
export USCAN_TEST_LOOKUP_PORT=$((10000 + RANDOM % 50000))

# Ensure ports are different
while [ "$USCAN_TEST_L1_PORT" -eq "$USCAN_TEST_LOOKUP_PORT" ]; do
    USCAN_TEST_LOOKUP_PORT=$((10000 + RANDOM % 50000))
done

echo "Using ports: L1=$USCAN_TEST_L1_PORT, Lookup=$USCAN_TEST_LOOKUP_PORT"

# PIDs to track
MOCK_SERVER_PID=""

cleanup() {
    local exit_code=$?
    echo ""
    echo "Cleaning up..."
    if [ -n "$MOCK_SERVER_PID" ]; then
        kill "$MOCK_SERVER_PID" 2>/dev/null || true
        wait "$MOCK_SERVER_PID" 2>/dev/null || true
    fi
    exit $exit_code
}

trap cleanup EXIT INT TERM

wait_for_port() {
    local port=$1
    local timeout=${2:-10}
    local elapsed=0

    while ! nc -z 127.0.0.1 "$port" 2>/dev/null; do
        sleep 0.1
        elapsed=$((elapsed + 1))
        if [ $elapsed -ge $((timeout * 10)) ]; then
            echo "ERROR: Timeout waiting for port $port"
            return 1
        fi
    done
    return 0
}

cd "$PROJECT_DIR"

# Build everything
echo "Building..."
make -j4 vendor 2>/dev/null || true
make -j4 tests/test_main.o tests/test_iqfeed_integration.o \
    src/symbol_db.o src/iqfeed_client.o src/scanner.o src/db_worker.o 2>&1 | grep -v "^clang" || true

echo "Linking test runner..."
clang++ tests/test_main.o tests/test_iqfeed_integration.o \
    src/symbol_db.o src/iqfeed_client.o src/scanner.o src/db_worker.o \
    -o tests/test_integration_runner \
    -flto -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo \
    -L/usr/local/lib -lglfw -lsqlite3 2>&1

echo "Building mock server..."
go build -o tests/mock_iqfeed_server tests/mock_iqfeed_server.go

echo ""
echo "Starting mock IQFeed server on ports $USCAN_TEST_L1_PORT and $USCAN_TEST_LOOKUP_PORT..."
./tests/mock_iqfeed_server -l1-port="$USCAN_TEST_L1_PORT" -lookup-port="$USCAN_TEST_LOOKUP_PORT" &
MOCK_SERVER_PID=$!

# Wait for server to be ready
if ! wait_for_port "$USCAN_TEST_L1_PORT" 10; then
    echo "Mock server failed to start on L1 port"
    exit 1
fi
if ! wait_for_port "$USCAN_TEST_LOOKUP_PORT" 10; then
    echo "Mock server failed to start on Lookup port"
    exit 1
fi

echo "Mock server ready (PID: $MOCK_SERVER_PID)"
echo ""
echo "========================================"
echo "Running integration tests..."
echo "========================================"
echo ""

# Set environment for tests to find the server
export USCAN_TEST_SERVER_RUNNING=1

./tests/test_integration_runner

echo ""
echo "Done!"
