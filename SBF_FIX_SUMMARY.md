# Symbol Search Fix Summary

## Problem Found

The scanner "didn't do much" because it had **corrupted symbol data** in the database:

### Issues:
1. **CSV header saved as symbols** - Database contained entries like:
   ```
   MessageID|SecurityTypeID|Symbol
   S|1|MLORF
   ```
   The parser was treating CSV header fields as actual stock symbols!

2. **Wrong SBF command format** - Was sending:
   ```
   SBF,SYMBOL,*,EQUITY
   ```
   This is not valid IQFeed syntax.

3. **No filtering in parse_sbf_symbol()** - Parser accepted:
   - CSV header lines
   - International stocks (exchange 56 = London, 52 = Toronto, etc.)
   - ETFs and non-equity securities
   - OTC/Pink sheet stocks (symbols starting with `.`)

4. **Wrong delimiter** - Parser used comma (`,`) but SBF responses use pipe (`|`)

## Fixes Applied

### 1. Fixed SBF Command Format (iqfeed_client.cpp:202)

**Before:**
```cpp
const std::string cmd = "SBF,SYMBOL,*,EQUITY\r\n";
```

**After:**
```cpp
// Format: SBF,e,[LISTEDISET],[SICCODE],1
// e = equity, [NASD] = NASDAQ, [NYSE] = NYSE, 1 = Symbol By Filter
const std::string cmd = "SBF,e,[NASD]||[NYSE],,1\r\n";
```

### 2. Fixed SBF Response Parsing (iqfeed_client.cpp:512-536)

**Now filters:**
- ✅ **Skip header lines** - Checks for "SecurityTypeID" and "Symbol"
- ✅ **Equities only** - SecurityTypeID == "1" (not ETFs, indices, mutual funds)
- ✅ **US exchanges only** - Accepts: 7=NASDAQ, 8=AMEX, 11=NYSE Arca, 12=BATS, 14=NYSE, 19=NYSE American
- ✅ **No OTC stocks** - Skips symbols starting with `.`
- ✅ **Proper delimiter** - Uses pipe (`|`) instead of comma (`,`)

**Code:**
```cpp
void IQFeedClient::parse_sbf_symbol(const std::string& line) {
    // Skip header line
    if (line.find("SecurityTypeID") != std::string::npos ||
        line.find("Symbol") == 0) {
        return;
    }

    const auto fields = split(line, '|');  // Use pipe, not comma
    if (fields.size() < 4) return;

    // Field 0: SecurityTypeID (1 = equity only)
    if (fields[0] != "1") {
        return;  // Skip ETFs, indices, etc.
    }

    // Field 1: Symbol
    std::string symbol = fields[1];

    // Skip OTC/foreign symbols starting with .
    if (symbol[0] == '.') {
        return;
    }

    // Field 3: Exchange - only major US exchanges
    std::string exchange = fields[3];
    if (exchange != "7" && exchange != "8" && exchange != "11" &&
        exchange != "12" && exchange != "14" && exchange != "19") {
        return;  // Skip foreign and OTC exchanges
    }

    // Add valid US equity symbol
    symbol_search_results_.push_back(SymbolInfo{symbol, fields[2], exchange, ""});
}
```

### 3. Cleared Corrupted Database

Ran:
```bash
sqlite3 uscan.db "DELETE FROM symbols; DELETE FROM metadata;"
```

This forces the scanner to fetch fresh symbols using the fixed SBF command.

## Testing

Run the scanner with verbose mode to see the new symbol fetch:

```bash
./uscan -v 2>&1 | tee new_log.txt
```

You should see:
1. `[VERBOSE] TCP SEND: SBF,e,[NASD]||[NYSE],,1` - Correct SBF command
2. `[VERBOSE] TCP RECV: 1|AAPL|Apple Inc|7|...` - Symbol responses with pipe delimiter
3. `[VERBOSE] TCP RECV: !ENDMSG!` - End of symbol list
4. `[VERBOSE] SQLite: Saving N symbols in transaction` - Saving to database
5. Symbols should be **US equities only** (no international, no ETFs, no header junk)

## What to Expect Now

The scanner should now:
1. ✅ Fetch thousands of US equity symbols (NASDAQ + NYSE)
2. ✅ Filter out foreign stocks, ETFs, and OTC
3. ✅ Subscribe to valid symbols only
4. ✅ Receive quotes for US stocks trading in pre-market
5. ✅ Display gaps when stocks gap up/down in pre-market

**Note**: You need to be running IQFeed with a valid subscription for this to work. The scanner will show errors if IQFeed is not running or you don't have real-time data permissions.

## Files Modified

1. **src/iqfeed_client.cpp:202** - Fixed SBF command format
2. **src/iqfeed_client.cpp:512-536** - Fixed SBF response parsing
3. **uscan.db** - Cleared corrupted symbol data

## Verification Checklist

After running `./uscan -v`:

- [ ] SBF command sent: `SBF,e,[NASD]||[NYSE],,1`
- [ ] Symbols received with pipe delimiter: `1|AAPL|Apple Inc|7|...`
- [ ] No header lines saved as symbols
- [ ] Only US exchanges (7, 8, 11, 12, 14, 19)
- [ ] Only equities (SecurityTypeID = 1)
- [ ] No symbols starting with `.`
- [ ] Database populated with thousands of symbols
- [ ] Scanner shows gaps when stocks gap in pre-market

Run this to verify symbol quality:
```bash
sqlite3 uscan.db "SELECT COUNT(*) FROM symbols;"  # Should be thousands
sqlite3 uscan.db "SELECT symbol, name, exchange FROM symbols LIMIT 10;"  # Should be real US stocks
```
