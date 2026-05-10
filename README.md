# uscan - Pre-Market Gap Scanner

Real-time pre-market gap scanner using IQFeed Level 1 data with Dear ImGui.

## Features

- Real-time gap scanning via IQFeed TCP streaming
- SQLite symbol caching
- Async non-blocking with background DB writes
- Auto-widen price range when no gappers found

## Requirements

- macOS 12+
- GLFW 3.x, SQLite3 (`brew install glfw sqlite3`)
- IQFeed subscription + docker-iqfeed proxy

## Build & Run

```bash
make                # Release build
make DEBUG=1        # Debug (AddressSanitizer)
make test           # Unit tests
make test-all       # All tests including ThreadSanitizer

./uscan             # Run scanner
./uscan -v          # Verbose logging
```

## Configuration

Defaults in `src/types.hpp`:

| Setting | Default |
|---------|---------|
| Price range | $1 - $20 (fallback $100) |
| Min gap | 5% |
| Min PM volume | 50,000 |
| Gap direction | Up only |

## IQFeed

Connects to localhost:
- Port 5009: Level 1 quotes
- Port 9100: Symbol lookup

## License

MIT
