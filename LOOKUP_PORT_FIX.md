# Lookup Port Connection Failure Fix

## Problem

When the Lookup port (9100) connection fails during symbol search, the scanner gets **stuck** waiting forever for symbols that will never arrive.

### Root Cause

**IQFeed Error 10054**: "Connection reset by peer"

The Lookup port connection was being reset by IQFeed during symbol search, but:
1. ❌ We logged the error but didn't abort the search
2. ❌ Scanner kept waiting for symbols indefinitely
3. ❌ UI showed no error - just stuck on "Fetching symbols"
4. ❌ Only receiving timestamp heartbeats, no progress

### Error Message from IQFeed

```
STATUS    LookupError    276    4    2026-05-10 13:17:14
Send Failed to Client: Bytes Queued 46 : Error 10054 :
First 256 bytes of data LS,JPFAF,3,1,PT JAPFA COMFEED INDONESIA TBK,
```

**Error 10054** = WSAECONNRESET = Connection reset by peer

---

## Solution

### 1. Detect Lookup Port Connection Failures

**Location:** `read_lookup_data()` in `src/iqfeed_client.cpp`

**Changes:**

#### A. Handle recv() returning 0 (connection closed)

```cpp
if (bytes_read == 0) {
    log_warn("Lookup connection closed by server during symbol search");

    // If we're searching symbols, this is fatal - abort the search
    if (searching_symbols_) {
        last_error_ = "Lookup port closed during symbol search (IQFeed Error 10054)";
        searching_symbols_ = false;
        state_ = ConnectionState::Error;
        symbol_search_results_.clear();
        last_incremental_save_count_ = 0;
    }

    return false;
}
```

#### B. Handle recv() errors (ECONNRESET, etc.)

```cpp
if (bytes_read < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        log_warn("Lookup recv() failed: %s", strerror(errno));

        // If we're searching symbols, this is fatal
        if (searching_symbols_) {
            last_error_ = std::string("Lookup port error: ") + strerror(errno);
            searching_symbols_ = false;
            state_ = ConnectionState::Error;
            symbol_search_results_.clear();
            last_incremental_save_count_ = 0;
        }
    }
    return false;
}
```

#### C. Handle poll() connection errors (POLLHUP, POLLERR, POLLNVAL)

```cpp
// Check for connection errors (POLLHUP, POLLERR, POLLNVAL)
if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
    log_warn("Lookup port connection error (revents: 0x%x)", pfd.revents);

    // If we're searching symbols, this is fatal
    if (searching_symbols_) {
        last_error_ = "Lookup port connection lost during symbol search";
        searching_symbols_ = false;
        state_ = ConnectionState::Error;
        symbol_search_results_.clear();
        last_incremental_save_count_ = 0;
    }

    return false;
}
```

**Behavior:**
- Detects connection closed/reset during symbol search
- Sets clear error message for user
- Aborts symbol search immediately
- Clears partial results (prevents corrupted data)
- Sets client state to Error

---

### 2. Propagate Error to Scanner

**Location:** `Scanner::update()` in `src/scanner.cpp`

**Change:**

```cpp
// Process incoming data
if (client_) {
    client_->process();

    // Check if client encountered an error
    if (client_->state() == ConnectionState::Error) {
        last_error_ = client_->last_error();
        state_ = ScannerState::Error;
        progress_ = Progress();
        log_warn("Scanner: Client error detected: %s", last_error_.c_str());
        return;
    }

    // ... rest of update logic
}
```

**Behavior:**
- Checks client state after each `process()` call
- If client is in Error state:
  - Copies error message to scanner
  - Sets scanner state to Error
  - Clears progress indicator
  - Logs the error
  - Returns immediately (stops processing)

---

## Testing

### Test Scenario: Lookup Port Disconnects During Search

**Setup:**
1. Start symbol search
2. Simulate connection reset (e.g., network glitch, IQFeed restart)

**Expected Behavior:**

**Before Fix:**
```
[VERBOSE] SBF: Received 276 symbols so far
[WARN] Lookup connection closed by server
[VERBOSE] TCP RECV: T,20260510 09:34:21
[VERBOSE] TCP RECV: T,20260510 09:34:22
... (stuck forever, only timestamps)
```
- Scanner stuck in "LoadingSymbols" state
- UI shows "Fetching symbols (276 received)" - never updates
- User has to kill app

**After Fix:**
```
[VERBOSE] SBF: Received 276 symbols so far
[WARN] Lookup connection closed by server during symbol search
[WARN] Scanner: Client error detected: Lookup port closed during symbol search (IQFeed Error 10054)
```
- Scanner immediately transitions to Error state
- UI shows: "State: Error"
- Error message displayed: "Lookup port closed during symbol search (IQFeed Error 10054)"
- User can see what went wrong
- User can click "Refresh Symbols" to retry

---

## Error Messages

### User-Friendly Messages

| Scenario | Error Message |
|----------|---------------|
| Connection closed (Error 10054) | "Lookup port closed during symbol search (IQFeed Error 10054)" |
| Connection reset (ECONNRESET) | "Lookup port error: Connection reset by peer" |
| Poll error (POLLHUP) | "Lookup port connection lost during symbol search" |
| Generic recv error | "Lookup port error: [system error message]" |

### Verbose Logging

All errors logged at `log_warn()` level:
- `[WARN] Lookup connection closed by server during symbol search`
- `[WARN] Lookup recv() failed: Connection reset by peer`
- `[WARN] Lookup port connection error (revents: 0x19)`
- `[WARN] Scanner: Client error detected: <error message>`

---

## Why This Happens

IQFeed may reset the Lookup port connection for several reasons:

1. **Rate limiting** - Too many concurrent requests
2. **Data overload** - Symbol list too large, buffer overflow
3. **Network issues** - Temporary connection problem
4. **IQFeed restart** - Docker container restart, service restart
5. **Timeout** - Idle connection timeout
6. **Resource limits** - IQFeed running out of resources

---

## Recovery

### Automatic Recovery (Not Implemented Yet)

Could add automatic retry logic:
```cpp
// Future enhancement: retry symbol search on failure
if (retry_count < MAX_RETRIES) {
    log_info("Retrying symbol search (attempt %d/%d)", retry_count + 1, MAX_RETRIES);
    retry_count++;
    // Wait 2 seconds, then retry
    // reconnect to lookup port
    // send SBF command again
}
```

### Manual Recovery (Current Implementation)

User must:
1. See error message in UI: "Lookup port closed during symbol search"
2. Click "Refresh Symbols" button to retry
3. App reconnects to Lookup port
4. Sends SBF command again
5. Tries symbol search again

---

## Summary

### Before Fix
❌ Lookup port failures went unnoticed
❌ Scanner stuck waiting forever
❌ UI frozen, no error shown
❌ User forced to kill app
❌ No recovery possible

### After Fix
✅ Lookup port failures immediately detected
✅ Symbol search aborted cleanly
✅ Scanner transitions to Error state
✅ UI shows clear error message
✅ User can retry with "Refresh Symbols"
✅ All errors logged for debugging

**Key Principle:** **Never wait forever - detect failures fast, fail cleanly, show errors clearly**
