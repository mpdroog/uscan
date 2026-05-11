#include "test_framework.hpp"
#include "scanner.hpp"
#include "symbol_db.hpp"
#include "iqfeed_client.hpp"
#include <chrono>
#include <thread>
#include <cstdio>
#include <unistd.h>

using namespace uscan;
using namespace test;

// Maximum time allowed for any operation on main thread (50ms)
constexpr int64_t MAX_BLOCKING_TIME_MS = 50;

// Helper: measure execution time of a function
template<typename Func>
int64_t measure_ms(Func&& func) {
    auto start = std::chrono::steady_clock::now();
    func();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

// Helper: create temporary database
class TempDB {
public:
    TempDB() {
        std::snprintf(path_, sizeof(path_), "/tmp/uscan_test_%d_%ld.db",
                     static_cast<int>(getpid()),
                     static_cast<long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    ~TempDB() {
        std::remove(path_);
    }

    const char* path() const { return path_; }

private:
    char path_[256];
};

// Test 1: Scanner initialization should not block
TEST(async_scanner_initialize_nonblocking) {
    Config config;
    config.iqfeed_host = "127.0.0.1";  // Localhost (won't connect, but that's OK)
    config.iqfeed_l1_port = 5009;
    config.verbose = false;

    TempDB tmp;
    config.db_path = tmp.path();

    Scanner scanner(std::move(config));

    // Measure initialization time
    int64_t elapsed_ms = measure_ms([&]() {
        // initialize() will fail to connect, but should still be non-blocking
        (void)scanner.initialize();
    });

    std::fprintf(stderr, "  Scanner initialization took %lld ms\n", static_cast<long long>(elapsed_ms));

    // Should complete within 50ms (even if connection fails)
    ASSERT_LT(elapsed_ms, MAX_BLOCKING_TIME_MS);

    return true;
}

// Test 2: Scanner update loop should not block
TEST(async_scanner_update_nonblocking) {
    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = 5009;
    config.verbose = false;

    TempDB tmp;
    config.db_path = tmp.path();

    Scanner scanner(std::move(config));
    (void)scanner.initialize();

    // Simulate main loop - each update() call should be fast
    int64_t max_time = 0;
    for (int i = 0; i < 100; i++) {
        int64_t elapsed_ms = measure_ms([&]() {
            scanner.update();
        });

        if (elapsed_ms > max_time) {
            max_time = elapsed_ms;
        }

        ASSERT_LT(elapsed_ms, MAX_BLOCKING_TIME_MS);
    }

    std::fprintf(stderr, "  100 update() calls, max time: %lld ms\n", static_cast<long long>(max_time));

    return true;
}

// Test 3: Symbol refresh should return immediately (async)
TEST(async_scanner_refresh_symbols_nonblocking) {
    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = 5009;
    config.verbose = false;

    TempDB tmp;
    config.db_path = tmp.path();

    Scanner scanner(std::move(config));
    (void)scanner.initialize();

    // refresh_symbols() should kick off async work and return immediately
    int64_t elapsed_ms = measure_ms([&]() {
        scanner.refresh_symbols();
    });

    std::fprintf(stderr, "  refresh_symbols() took %lld ms\n", static_cast<long long>(elapsed_ms));

    ASSERT_LT(elapsed_ms, MAX_BLOCKING_TIME_MS);

    return true;
}

// Test 4: DB save should be async
TEST(async_db_save_symbols_async) {
    TempDB tmp;
    SymbolDB db(tmp.path());
    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    // Create 10,000 symbols (would take >1 second to save synchronously)
    std::vector<SymbolInfo> symbols;
    symbols.reserve(10000);
    for (int i = 0; i < 10000; i++) {
        char sym[16];
        std::snprintf(sym, sizeof(sym), "SYM%d", i);
        symbols.emplace_back(sym, "Test Symbol", "NASDAQ", "Technology");
    }

    // Create DB worker and enqueue save
    DBWorker worker(db);

    std::atomic<bool> completed{false};

    // Enqueue should return immediately
    int64_t elapsed_ms = measure_ms([&]() {
        worker.enqueue_save(symbols, [&](bool success, const std::string& error) {
            completed = true;
        });
    });

    std::fprintf(stderr, "  Enqueuing 10,000 symbols took %lld ms\n", static_cast<long long>(elapsed_ms));

    // Enqueue should be instant
    ASSERT_LT(elapsed_ms, MAX_BLOCKING_TIME_MS);

    // Should not be completed yet
    ASSERT_FALSE(completed);

    // Wait for completion (max 10 seconds)
    for (int i = 0; i < 100 && !completed; i++) {
        uscan::safe_sleep_ms(100);
    }

    // Should eventually complete
    ASSERT_TRUE(completed);

    worker.stop();
    db.close();

    return true;
}

// Test 5: DB load should be async
TEST(async_db_load_symbols_async) {
    TempDB tmp;
    SymbolDB db(tmp.path());
    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    // Save some symbols first (synchronously for test setup)
    std::vector<SymbolInfo> symbols;
    for (int i = 0; i < 5000; i++) {
        char sym[16];
        std::snprintf(sym, sizeof(sym), "SYM%d", i);
        symbols.emplace_back(sym, "Test Symbol", "NASDAQ", "Technology");
    }
    auto save_result = db.save_symbols(symbols);
    ASSERT_TRUE(save_result.ok());

    // Create DB worker and enqueue load
    DBWorker worker(db);

    std::atomic<bool> completed{false};
    std::size_t loaded_count = 0;

    // Enqueue should return immediately
    int64_t elapsed_ms = measure_ms([&]() {
        worker.enqueue_load([&](const std::vector<SymbolInfo>& loaded) {
            loaded_count = loaded.size();
            completed = true;
        });
    });

    std::fprintf(stderr, "  Enqueuing load of 5,000 symbols took %lld ms\n", static_cast<long long>(elapsed_ms));

    // Enqueue should be instant
    ASSERT_LT(elapsed_ms, MAX_BLOCKING_TIME_MS);

    // Should not be completed yet
    ASSERT_FALSE(completed);

    // Wait for completion (max 5 seconds)
    for (int i = 0; i < 50 && !completed; i++) {
        uscan::safe_sleep_ms(100);
    }

    // Should eventually complete
    ASSERT_TRUE(completed);
    ASSERT_EQ(loaded_count, 5000);

    worker.stop();
    db.close();

    return true;
}

// Test 6: Price update batching should not block
TEST(async_db_price_updates_batched) {
    TempDB tmp;
    SymbolDB db(tmp.path());
    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    // Save one symbol first
    std::vector<SymbolInfo> symbols;
    symbols.emplace_back("AAPL", "Apple Inc", "NASDAQ", "Technology");
    auto save_result = db.save_symbols(symbols);
    ASSERT_TRUE(save_result.ok());

    // Queue 1000 price updates (should be instant)
    int64_t max_time = 0;
    for (int i = 0; i < 1000; i++) {
        int64_t elapsed_ms = measure_ms([&]() {
            db.queue_price_update("AAPL", 100.0 + static_cast<double>(i));
        });

        if (elapsed_ms > max_time) {
            max_time = elapsed_ms;
        }

        // Queueing should be very fast (<10ms even with lock)
        ASSERT_LT(elapsed_ms, 10);
    }

    std::fprintf(stderr, "  1,000 queue_price_update() calls, max time: %lld ms\n", static_cast<long long>(max_time));

    // Flush should work (this one can be slower, it's on worker thread)
    auto flush_result = db.flush_price_updates();
    ASSERT_TRUE(flush_result.ok());

    db.close();

    return true;
}

// Test 7: UI responsiveness during async operations (60fps test)
TEST(async_ui_responsive_60fps) {
    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = 5009;
    config.verbose = false;

    TempDB tmp;
    config.db_path = tmp.path();

    Scanner scanner(std::move(config));
    (void)scanner.initialize();
    scanner.refresh_symbols();

    // Simulate 60 UI frames (1 second at 60fps)
    // Each frame should complete within 16ms for 60fps
    int64_t max_frame_time = 0;
    for (int frame = 0; frame < 60; frame++) {
        int64_t frame_time_ms = measure_ms([&]() {
            scanner.update();
        });

        if (frame_time_ms > max_frame_time) {
            max_frame_time = frame_time_ms;
        }

        // Allow some margin, but should be close to 16ms
        ASSERT_LT(frame_time_ms, 20);
    }

    std::fprintf(stderr, "  60 frames simulated, max frame time: %lld ms (target: 16ms)\n",
                static_cast<long long>(max_frame_time));

    return true;
}
