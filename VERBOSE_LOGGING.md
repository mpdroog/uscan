# Verbose Logging Implementation

The `-v` or `--verbose` flag has been added to enable detailed logging of TCP commands and SQLite operations.

## Usage

```bash
./uscan -v          # Enable verbose logging
./uscan --verbose   # Alternative syntax
```

## What Gets Logged

### TCP Operations (iqfeed_client.cpp)
- **Command Send**: Every TCP command sent to IQFeed (watch, unwatch, SBF)
  - Example: `[VERBOSE] TCP Send: w @ESH24`
  
- **Message Receive**: Every TCP message received from IQFeed
  - Example: `[VERBOSE] TCP Recv: @ESH24,12345.50,100,...`

### SQLite Operations (symbol_db.cpp)
- **Database Open**: When database connection is established
  - Example: `[VERBOSE] SQLite: Opening database: uscan.db`
  
- **Save Symbols**: When symbols are saved to database
  - Example: `[VERBOSE] SQLite: Saving 5000 symbols in transaction`
  
- **Load Symbols**: When symbols are loaded from database
  - Example: `[VERBOSE] SQLite: Loading symbols from database`
  
- **Flush Price Updates**: When batched price updates are written
  - Example: `[VERBOSE] SQLite: Flushing 1234 batched price updates`

### DB Worker Operations (db_worker.cpp)
- **Save Request**: When DB worker processes symbol save
  - Example: `[VERBOSE] DBWorker: Processing SAVE_SYMBOLS request (5000 symbols)`
  
- **Load Request**: When DB worker processes symbol load
  - Example: `[VERBOSE] DBWorker: Processing LOAD_SYMBOLS request`
  
- **Flush Request**: When DB worker processes price update flush
  - Example: `[VERBOSE] DBWorker: Processing FLUSH_UPDATES request`

### Scanner Operations (scanner.cpp)
- **Cache Load**: When loading symbols from database cache
  - Example: `[VERBOSE] Scanner: Loading symbols from database cache (age: 3600 seconds)`
  
- **Fresh Fetch**: When fetching symbols from IQFeed using SBF
  - Example: `[VERBOSE] Scanner: Fetching fresh symbols from IQFeed using SBF command`
  
- **Periodic Flush**: When triggering periodic DB flush (every 5 seconds)
  - Example: `[VERBOSE] Scanner: Enqueuing periodic DB flush (every 5 seconds)`

## Implementation Details

- Global verbose flag: `uscan::detail::g_verbose`
- Set via `uscan::set_verbose(true)` in main.cpp
- All logging goes to stderr with `[VERBOSE]` prefix
- Uses printf-style formatting for efficiency
- Zero overhead when verbose mode is disabled (early return in log_verbose())

## Files Modified

1. **src/types.hpp** - Added log_verbose() function and global flag
2. **src/main.cpp** - Added CLI parsing for -v/--verbose
3. **src/iqfeed_client.cpp** - Added TCP send/recv logging
4. **src/symbol_db.cpp** - Added SQLite operation logging
5. **src/db_worker.cpp** - Added worker thread operation logging
6. **src/scanner.cpp** - Added scanner state transition logging
