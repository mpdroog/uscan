# Error Handling Implementation

## IQFeed Error Formats

IQFeed returns errors in the following formats:

### General Errors
```
E,[Error Text],
```
Example: `E,Invalid symbol,`

### Syntax Errors
```
E,!SYNTAX_ERROR!,
```

### Symbol Not Found
```
n,[Symbol]
```
Example: `n,INVALID_SYMBOL`

### Errors with Request ID
```
[RequestID],E,[Error Text],
```

---

## Error Handling by Port

### Lookup Port (9100) - Symbol Search

**Location:** `parse_lookup_message()` in `src/iqfeed_client.cpp`

**Behavior:**
- Detects `E,` prefix at start of line
- Logs error: `log_warn("IQFeed Lookup Error: %s", error_msg)`
- Sets `last_error_` for UI display
- **Aborts symbol search** if in progress:
  - Sets `searching_symbols_ = false`
  - Sets `state_ = ConnectionState::Error`
  - Clears partial results
- Scanner will transition to `ScannerState::Error` state
- User sees error message in UI

**Example:**
```
TCP RECV (Lookup): E,Invalid filter type,
[WARN] IQFeed Lookup Error: Invalid filter type
```

### Level 1 Port (5009) - Streaming Quotes

**Location:** `parse_message()` in `src/iqfeed_client.cpp`

**Behavior:**
- Detects `E,` prefix at start of line
- Logs error: `log_warn("IQFeed L1 Error: %s", error_msg)`
- **Does NOT set state to Error** (non-fatal)
  - Reason: Symbol-not-found errors are common and expected
  - Scanner continues operating normally
  - Only logs for debugging

**Example:**
```
TCP RECV: E,Symbol limit reached,AAPL
[WARN] IQFeed L1 Error: Symbol limit reached
```

### Symbol Not Found

**Location:** `parse_message()` switch case 'n'

**Behavior:**
- Logs: `log_verbose("Symbol not found: %s", symbol)`
- Non-fatal, scanner continues

**Example:**
```
TCP RECV: n,INVALID123
[VERBOSE] Symbol not found: INVALID123
```

---

## Error Handling in Cleanup/Shutdown

### Unwatching Symbols

**Location:** `IQFeedClient::unwatch_all()`

**Behavior:**
- Best-effort cleanup during shutdown
- Logs failures but continues: `log_verbose("Failed to unwatch %s: %s", symbol, error)`
- Does not abort shutdown

**Example:**
```
[VERBOSE] Failed to unwatch AAPL during cleanup: Connection already closed
```

### Database Flush

**Location:** `Scanner::shutdown()`

**Behavior:**
- Attempts to flush pending price updates
- Logs failure: `log_warn("Failed to flush price updates during shutdown: %s", error)`
- Continues with shutdown anyway

**Example:**
```
[WARN] Failed to flush price updates during shutdown: database is locked
```

---

## Network/Socket Errors

### Connection Errors

**Location:** `IQFeedClient::connect()`

**Returns:** `Result<void>::failure(error_message)`

**Behavior:**
- Sets `state_ = ConnectionState::Error`
- Sets `last_error_` with descriptive message
- Scanner sets `state_ = ScannerState::Error`
- User sees error in UI

**Examples:**
```cpp
// Socket creation failed
Result<void>::failure("socket() failed: Address family not supported")

// Connection refused
Result<void>::failure("connect() failed: Connection refused")

// Lookup port unavailable
Result<void>::failure("Lookup port not connected")
```

### Read/Write Errors

**Location:** `read_data()` and `read_lookup_data()`

**Behavior:**

1. **poll() failure:**
   - Sets `last_error_ = "poll() failed: [errno]"`
   - Sets `state_ = ConnectionState::Error`
   - Returns false

2. **recv() failure:**
   - Ignores `EAGAIN` and `EWOULDBLOCK` (normal non-blocking behavior)
   - For other errors:
     - Sets `last_error_ = "recv() failed: [errno]"`
     - Sets `state_ = ConnectionState::Error`
   - Returns false

3. **Connection closed:**
   - Sets `last_error_ = "Connection closed by server"`
   - Sets `state_ = ConnectionState::Disconnected`
   - Returns false

### Lookup Port Read Errors

**Location:** `read_lookup_data()`

**Behavior:**
- Same as `read_data()` but uses `log_warn()` instead of setting state
- Reason: Lookup port is only used during symbol search, not critical path
- Logs: `log_warn("Lookup poll() failed: %s", strerror(errno))`

---

## Error Propagation to UI

### Scanner State Machine

```
Idle -> Connecting -> LoadingSymbols -> Subscribing -> Scanning
                ↓            ↓              ↓
              Error ← ← ← ← Error ← ← ← ← Error
```

### Error Display

**Location:** `src/main.cpp` UI rendering

**Displays:**
1. Scanner state: `scanner.state()` shows `ScannerState::Error`
2. Error message: `scanner.last_error()` shows detailed message
3. Connection state: `scanner.connection_state()` shows connection status

**Example UI:**
```
State: Error
Error: Lookup port error: Invalid filter type
Connection: Error
```

---

## Testing Error Handling

### Test Invalid Command

Temporarily modify SBF command to send invalid filter type:

```cpp
// In request_symbol_search()
const std::string cmd = "SBF,s,*,INVALID,1\r\n";  // Wrong filter type
```

**Expected behavior:**
- IQFeed returns: `E,Invalid filter type,`
- Log shows: `[WARN] IQFeed Lookup Error: Invalid filter type`
- Scanner state: `Error`
- UI shows error message

### Test Connection Failure

Stop IQFeed docker container:

```bash
docker stop iqfeed
```

**Expected behavior:**
- Connect fails: `connect() failed: Connection refused`
- Scanner state: `Error`
- UI shows: "Connection refused"

### Test Symbol Not Found

Watch invalid symbol (would need to add test code):

```cpp
client_->watch("INVALID_SYMBOL_12345");
```

**Expected behavior:**
- Receives: `n,INVALID_SYMBOL_12345`
- Logs: `[VERBOSE] Symbol not found: INVALID_SYMBOL_12345`
- Scanner continues normally (non-fatal)

---

## Summary

### Critical Errors (Abort Operation)
✅ Lookup port errors during symbol search → Abort search, show error
✅ Socket/network failures → Set Error state, show message
✅ Connection failures → Cannot proceed, show error

### Non-Fatal Errors (Log Only)
✅ L1 port errors (symbol limits, etc.) → Log, continue
✅ Symbol not found → Log, continue
✅ Cleanup failures (unwatch, flush) → Log, continue shutdown

### Error Visibility
✅ All errors logged with `log_warn()` or `log_verbose()`
✅ Critical errors set `last_error_` for UI display
✅ Scanner state reflects error condition
✅ User always sees what went wrong

**Key Principle:** **Fail loudly (log everything), fail gracefully (don't crash), fail informatively (tell user what happened)**
