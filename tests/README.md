# IQFeed Integration Tests

This directory contains integration tests for the IQFeed client using a mock IQFeed server written in Go.

## Overview

The integration test suite verifies the IQFeed client's ability to:
- Connect to IQFeed servers
- Handle the IQFeed protocol correctly
- Stream real-time quotes (watch/unwatch symbols)
- Search for symbols (SBF queries)
- Parse fundamental and quote data
- Handle errors gracefully

## Components

### Mock IQFeed Server (`mock_iqfeed_server.go`)

A standalone Go server that simulates IQFeed's behavior:
- **Level 1 Port (5009)**: Handles real-time quote streaming
- **Lookup Port (9100)**: Handles symbol search (SBF) queries

The mock server implements:
- Protocol handshake (`S,SET PROTOCOL,6.2`)
- Watch/unwatch commands (`w<SYMBOL>`, `r<SYMBOL>`)
- Fundamental data messages (F)
- Summary messages (P)
- Update messages (Q)
- Timestamp heartbeats (T)
- Symbol search responses (LS)
- Symbol search completion (!ENDMSG!)

### Integration Tests (`test_iqfeed_integration.cpp`)

Comprehensive test suite with 14 tests covering:
1. **Connection Management**
   - Connecting to mock server
   - Reconnecting after disconnect
   - Handling connection failures

2. **Quote Streaming**
   - Watching single symbols
   - Watching multiple symbols
   - Unwatching symbols
   - Receiving quote updates
   - Quote callbacks

3. **Symbol Search**
   - SBF queries
   - Incremental save callbacks
   - Search completion detection

4. **Error Handling**
   - Operations without connection
   - Message count tracking

5. **Data Parsing**
   - Extended hours data
   - Gap calculations

## Running the Tests

### Prerequisites

- Go compiler (for building the mock server)
- C++ compiler with C++17 support
- All dependencies from main Makefile

### Run Integration Tests

```bash
make test-integration
```

This command will:
1. Build the mock IQFeed server from Go source
2. Compile the integration test suite
3. Run all integration tests
4. Clean up temporary files

### Run All Tests

```bash
make test-all
```

Runs:
- Unit tests
- Integration tests
- Thread safety tests (with ThreadSanitizer)

## Test Results

Current status (as of last run):
- **Passing**: 8/14 tests
- **Failing**: 6/14 tests

### Passing Tests
- ✅ Integration_connect_to_mock_server
- ✅ Integration_reconnect_after_disconnect
- ✅ Integration_watch_multiple_symbols
- ✅ Integration_unwatch_symbol
- ✅ Integration_quote_callback
- ✅ Integration_incremental_symbol_save
- ✅ Integration_watch_without_connection
- ✅ Integration_message_count_tracking

### Failing Tests
- ❌ Integration_connect_with_server_not_running (state detection issue)
- ❌ Integration_watch_single_symbol (name field not populated)
- ❌ Integration_quote_updates (price not updating)
- ❌ Integration_symbol_search (search completion timeout)
- ❌ Integration_extended_hours_data (extended_price not populated)
- ❌ Integration_gap_calculation (extended_price dependency)

## Known Issues

1. **Field Parsing**: Some quote fields (name, extended_price) may not be parsed correctly
   - This could indicate field index mismatches between mock server and client
   - The client expects specific field indices (see src/iqfeed_client.cpp enums)

2. **Timing Issues**: Some tests may fail due to timing/synchronization
   - Symbol search may timeout before receiving !ENDMSG!
   - Quote updates may not arrive within the test's wait window

3. **Port Cleanup**: Mock server processes may not release ports immediately
   - Added 1-second delay after stopping server
   - Consider using random ports if issues persist

## Debugging

To see detailed mock server output:
```bash
# Run tests with verbose logging
./tests/test_integration_runner 2>&1 | grep -E "SEND|RECV|Mock"
```

To manually run the mock server:
```bash
cd tests
go run mock_iqfeed_server.go
```

Then connect with telnet to test manually:
```bash
# Level 1 port
telnet localhost 5009

# Lookup port
telnet localhost 9100
```

## Next Steps

To improve test reliability:

1. **Fix Field Indices**: Ensure mock server sends data with correct field indices
   - CompanyName should be at index 48 in F messages
   - ExtendedTrade should be at index 42 in P/Q messages
   - ExtendedTradeSize should be at index 43 in P/Q messages

2. **Add Test Logging**: Enable verbose logging in tests to see actual vs expected values

3. **Increase Timeouts**: Some tests may need longer wait times for async operations

4. **Single Server Instance**: Consider running one mock server for all tests instead of start/stop per test

## Architecture Notes

The mock server uses goroutines for concurrent connection handling:
- Each client connection gets its own goroutine
- Watched symbols trigger background update goroutines (1/second)
- Symbol search runs in background to prevent blocking

The test framework:
- Spawns mock server as child process (`fork()`/`exec()`)
- Uses `wait_for()` helper for async operations
- Properly cleans up server processes on test completion
