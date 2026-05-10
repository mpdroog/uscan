# Progress Tracking UI Enhancement

## Problem
The async implementation was great for non-blocking operations, but the UI didn't give users clear feedback about what was happening during multi-step operations. Users couldn't tell:
- How many steps there were in total
- Which step was currently executing
- Progress within each step

## Solution Implemented

Added a comprehensive progress tracking system that shows:
1. **Current step description** - Clear text of what's happening
2. **Step number / total steps** - e.g., "Step 2/4"
3. **Progress percentage** - For steps that can report progress (like subscribing)

### New Progress Struct (types.hpp)

```cpp
struct Progress {
    std::string current_step;   // Description of current step
    int step_number{0};          // Current step number (1-based)
    int total_steps{0};          // Total number of steps
    int step_progress{-1};       // Progress within step (0-100), -1 = indeterminate

    Progress() = default;
    Progress(std::string step, int num, int total, int progress = -1);
};
```

### The 4 Steps

The scanner now tracks these steps during startup/refresh:

1. **Opening database** (Step 1/4)
   - Opens SQLite database
   - Creates DB worker thread
   - Quick (< 10ms)

2. **Connecting to IQFeed** (Step 2/4)
   - Establishes TCP connection to IQFeed
   - Sends protocol handshake
   - Quick (< 50ms if IQFeed running)

3. **Loading symbols** (Step 3/4)
   - **From cache**: "Loading symbols from cache"
   - **Fresh fetch**: "Fetching symbols from IQFeed"
   - Can take a few seconds for fresh fetch

4. **Subscribing to symbols** (Step 4/4)
   - Shows progress: "Subscribing to symbols" with percentage bar
   - Subscribes in batches (100 at a time)
   - Progress bar shows: 0% → 100%

## UI Improvements

### Status Bar (Bottom)
**Before:**
```
IQFeed: Connected | State: Loading Symbols | Watching: 0 | ...
```

**After:**
```
IQFeed: Connected | Step 3/4: Fetching symbols from IQFeed ⠋ | Watching: 0 | ...
```

Shows exactly which step is running with spinner animation.

### Main Area (Center)
**Before:**
```
⠋ Loading symbols from cache/IQFeed...
Async operation in progress (non-blocking)
```

**After:**
```
⠋ Step 3/4: Fetching symbols from IQFeed

[████████████████████░░░░░░░░] 75%

Async operation in progress (non-blocking)
```

Shows:
- Step number and description
- Progress bar (for subscribing step)
- Percentage complete

### Refresh Button
Now disabled during **all** async operations:
- Connecting
- Loading symbols
- Subscribing

Previously only disabled during "LoadingSymbols" state.

## Code Changes

### 1. types.hpp
- Added `Progress` struct
- Added `ScannerState::Connecting`

### 2. scanner.hpp/cpp
- Added `Progress progress_` member
- Added `Progress progress() const` accessor
- Updated `initialize()` to set progress for each step
- Updated `load_or_fetch_symbols()` to set step 3 progress
- Updated `subscribe_to_symbols()` to show percentage progress

### 3. main.cpp
- Updated status bar to show "Step X/Y: Description"
- Updated main area to show progress bar for steps with percentages
- Updated "Refresh Symbols" button to be disabled during all loading states

## User Experience Flow

### Fresh Symbol Fetch
1. User clicks "Refresh Symbols"
2. UI shows:
   ```
   Step 1/4: Opening database ⠋
   Step 2/4: Connecting to IQFeed ⠋
   Step 3/4: Fetching symbols from IQFeed ⠋
   Step 4/4: Subscribing to symbols [████░░░░] 50%
   ```
3. When done, shows normal scanning UI

### Cached Symbol Load
1. App starts with cached symbols
2. UI shows:
   ```
   Step 1/4: Opening database ⠋
   Step 2/4: Connecting to IQFeed ⠋
   Step 3/4: Loading symbols from cache ⠋
   Step 4/4: Subscribing to symbols [████████] 95%
   ```
3. Much faster since step 3 is cached

## Benefits

1. **Transparency** - Users know exactly what's happening
2. **Progress visibility** - Can see how far along the process is
3. **Confidence** - Progress bar movement shows it's working (not frozen)
4. **Debugging** - Easy to see if stuck on a specific step
5. **Professional feel** - Polished UX typical of production apps

## Testing

Run the app and watch the progress:
```bash
./uscan -v 2>&1 | tee progress_test.log
```

You should see:
1. Status bar updates through steps 1-4
2. Main area shows step descriptions
3. Progress bar animates during step 4 (subscribing)
4. Refresh button disabled until scanning state
5. Spinner animation continues throughout

## Files Modified

1. **src/types.hpp** - Added Progress struct, added ScannerState::Connecting
2. **src/scanner.hpp** - Added progress_ member and accessor
3. **src/scanner.cpp** - Set progress in initialize(), load_or_fetch_symbols(), subscribe_to_symbols()
4. **src/main.cpp** - Updated UI to display progress in status bar and main area

The progress tracking makes the async operations much more transparent and user-friendly!
