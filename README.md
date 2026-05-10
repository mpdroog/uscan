# uscan - Pre-Market Gap Scanner

A real-time pre-market gap scanner using IQFeed Level 1 data with a Dear ImGui interface.

## Features

- Real-time pre-market gap scanning via IQFeed Level 1 TCP streaming
- SQLite-based symbol caching for faster startup on subsequent runs
- Configurable filters: gap %, price range, pre-market volume
- Auto-widen price range ($1-$20 -> $1-$100) when no gappers found
- Top 20 gappers sorted by gap percentage
- Displays: Symbol, Price, Gap%, Gap$, PM Volume, Avg Volume, Float, 52wk Range, Sector

## Requirements

- macOS (tested on macOS 12+)
- GLFW 3.x
- SQLite3
- IQFeed subscription with Level 1 data access
- docker-iqfeed proxy running (see ~/go/src/github.com/mpdroog/docker-iqfeed)

## Installation

### Install dependencies (macOS)

```bash
brew install glfw sqlite3
```

### Build

```bash
# Release build
make

# Debug build (with AddressSanitizer)
make DEBUG=1

# Clean
make clean

# Full clean (including vendor)
make distclean
```

### Run tests

```bash
make test
```

## Usage

1. Start the docker-iqfeed proxy:
```bash
cd ~/go/src/github.com/mpdroog/docker-iqfeed
docker-compose up -d
```

2. Run the scanner:
```bash
./uscan
```

## Configuration

Default settings (configured in `src/types.hpp`):

| Setting | Default | Description |
|---------|---------|-------------|
| `min_price` | $1.00 | Minimum stock price |
| `max_price` | $20.00 | Maximum stock price |
| `fallback_max_price` | $100.00 | Extended max when no gappers found |
| `min_gap_percent` | 5.0% | Minimum gap percentage |
| `min_premarket_volume` | 50,000 | Minimum pre-market shares traded |
| `max_results` | 20 | Maximum gappers to display |
| `gap_up_only` | true | Only show gap ups (not downs) |
| `refresh_interval_ms` | 3000 | Quote refresh interval |

## IQFeed Connection

The scanner connects to:
- **Level 1 TCP** (port 5009): Real-time quotes
- **HTTP API** (port 8080): Symbol search

Ensure docker-iqfeed is running and accessible on localhost.

## Architecture

```
src/
├── types.hpp          # Data structures, Result type, logging
├── iqfeed_client.hpp  # IQFeed TCP/HTTP client
├── iqfeed_client.cpp
├── symbol_db.hpp      # SQLite symbol cache
├── symbol_db.cpp
├── scanner.hpp        # Gap scanning logic
├── scanner.cpp
└── main.cpp           # Dear ImGui UI

tests/
├── test_framework.hpp # Minimal test framework
├── test_main.cpp      # Test runner
├── test_types.cpp     # Type/struct tests
└── test_symbol_db.cpp # Database tests
```

## Compiler Flags

The project uses strict C++17 compiler flags for our code:
- `-Wall -Wextra -Wpedantic -Werror`
- `-Wconversion -Wsign-conversion`
- `-Wshadow -Wdouble-promotion`
- `-Wformat=2 -Wformat-security`
- `-fno-exceptions -fstack-protector-strong`

Vendor code (Dear ImGui) is compiled with relaxed flags.

## Best Practices

- Non-copyable types use `USCAN_NON_COPYABLE(Class)` macro
- Return values use `[[nodiscard]]` via `USCAN_NODISCARD` macro
- Error handling via `Result<T>` type
- All nodiscard results are checked and logged on failure

## License

MIT
