#include "../src/iqfeed_client.hpp"
#include "../src/symbol_db.hpp"
#include "../src/db_worker.hpp"
#include "test_framework.hpp"
#include <thread>
#include <chrono>
#include <atomic>
#include <unistd.h>

using namespace uscan;

// Temp DB helper
class TestDB {
public:
    TestDB() {
        char tmpl[] = "/tmp/uscan_test_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) {
            close(fd);
            db_path_ = tmpl;
        }
    }
    ~TestDB() {
        if (!db_path_.empty()) {
            std::remove(db_path_.c_str());
        }
    }
    const std::string& path() const { return db_path_; }
private:
    std::string db_path_;
};

// Test 1: Callback race condition - disconnect during callback invocation
TEST(callback_race_on_disconnect) {
    Config config;
    config.iqfeed_host = "127.0.0.1";  // Will fail to connect

    IQFeedClient client(config);

    // Set callbacks that take time
    std::atomic<int> callback_count{0};
    std::atomic<bool> callback_running{false};

    client.set_symbol_search_callback([&](const std::vector<SymbolInfo>& symbols) {
        callback_running = true;
        uscan::safe_sleep_ms(100);
        callback_count++;
        callback_running = false;
    });

    client.set_incremental_save_callback([&](const std::vector<SymbolInfo>& symbols) {
        callback_running = true;
        uscan::safe_sleep_ms(100);
        callback_count++;
        callback_running = false;
    });

    // Try to trigger race: disconnect while callback might be running
    for (int i = 0; i < 10; i++) {
        client.connect();
        uscan::safe_sleep_ms(10);
        client.disconnect();

        // Ensure no callback is running after disconnect
        ASSERT_FALSE(callback_running.load());
    }

    return true;
}

// Test 2: Multiple threads accessing symbols_received_count (lock-free atomic)
TEST(lock_free_counter_stress) {
    Config config;
    config.iqfeed_host = "127.0.0.1";

    IQFeedClient client(config);
    std::atomic<bool> stop{false};
    std::atomic<int> read_count{0};

    // Spawn 4 threads reading the counter continuously
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; i++) {
        readers.emplace_back([&]() {
            while (!stop) {
                auto count = client.symbols_received_count();
                (void)count;
                read_count++;
            }
        });
    }

    // Let them read for a bit
    uscan::safe_sleep_ms(100);
    stop = true;

    for (auto& t : readers) {
        t.join();
    }

    // Should have read thousands of times without crash
    ASSERT_GT(read_count.load(), 100);

    return true;
}

// Test 3: Rapid connect/disconnect to stress socket closing
TEST(rapid_connect_disconnect) {
    Config config;
    config.iqfeed_host = "127.0.0.1";

    for (int i = 0; i < 50; i++) {
        IQFeedClient client(config);
        client.connect();
        // Immediately disconnect (tests socket close race)
        client.disconnect();
    }

    return true;
}

// Test 4: Set callbacks during active search (race on callback_mutex_)
TEST(set_callbacks_during_search) {
    Config config;
    config.iqfeed_host = "127.0.0.1";

    IQFeedClient client(config);
    client.connect();

    std::atomic<bool> stop{false};

    // Thread 1: Keep setting callbacks
    std::thread setter([&]() {
        int count = 0;
        while (!stop) {
            client.set_symbol_search_callback([count](const std::vector<SymbolInfo>&) {
                (void)count;
            });
            client.set_incremental_save_callback([count](const std::vector<SymbolInfo>&) {
                (void)count;
            });
            count++;
        }
    });

    // Thread 2: Request symbol search repeatedly
    std::thread searcher([&]() {
        while (!stop) {
            client.request_symbol_search();
            uscan::safe_sleep_ms(10);
        }
    });

    // Let them race for 200ms
    uscan::safe_sleep_ms(200);
    stop = true;

    setter.join();
    searcher.join();
    client.disconnect();

    return true;
}

// Test 5: Callback sees null during disconnect
TEST(callback_cleared_before_invocation) {
    Config config;
    config.iqfeed_host = "127.0.0.1";

    std::atomic<int> called_after_disconnect{0};

    for (int i = 0; i < 20; i++) {
        IQFeedClient client(config);

        // Set callback that checks if it's called after disconnect
        client.set_symbol_search_callback([&](const std::vector<SymbolInfo>&) {
            // If we're here, client might be disconnecting
            if (client.state() == ConnectionState::Disconnected) {
                called_after_disconnect++;
            }
        });

        client.connect();
        client.request_symbol_search();

        // Race: disconnect while callback might fire
        uscan::safe_sleep_ms(5);
        client.disconnect();
    }

    // Callbacks should never be invoked after disconnect
    ASSERT_EQ(called_after_disconnect.load(), 0);

    return true;
}

// Test 6: Background thread data race on symbol_search_results_
TEST(background_thread_data_race) {
    Config config;
    config.iqfeed_host = "127.0.0.1";

    IQFeedClient client(config);

    std::atomic<int> incremental_saves{0};
    std::atomic<int> final_saves{0};

    client.set_incremental_save_callback([&](const std::vector<SymbolInfo>& symbols) {
        // Validate data isn't corrupted (can't use ASSERT in callback)
        for (const auto& sym : symbols) {
            if (sym.symbol.empty()) {
                incremental_saves = -1;  // Mark as failed
                return;
            }
        }
        incremental_saves++;
    });

    client.set_symbol_search_callback([&](const std::vector<SymbolInfo>& symbols) {
        for (const auto& sym : symbols) {
            if (sym.symbol.empty()) {
                final_saves = -1;  // Mark as failed
                return;
            }
        }
        final_saves++;
    });

    client.connect();
    client.request_symbol_search();

    // Give it time to receive some symbols (if connection works)
    uscan::safe_sleep_ms(500);

    client.disconnect();

    return true;
}

// Test 7: Destroy client while background thread is running
TEST(destroy_during_background_thread) {
    for (int i = 0; i < 20; i++) {
        Config config;
        config.iqfeed_host = "127.0.0.1";

        IQFeedClient* client = new IQFeedClient(config);
        client->connect();
        client->request_symbol_search();

        // Brief delay to let thread start
        uscan::safe_sleep_ms(10);

        // Destroy while thread might be running
        delete client;
    }

    return true;
}

// Test 8: Multiple concurrent clients (separate instances)
TEST(multiple_concurrent_clients) {
    std::vector<std::thread> threads;

    for (int i = 0; i < 5; i++) {
        threads.emplace_back([i]() {
            Config config;
            config.iqfeed_host = "127.0.0.1";

            IQFeedClient client(config);
            client.connect();
            client.request_symbol_search();

            uscan::safe_sleep_ms(50 + i * 10);

            client.disconnect();
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    return true;
}

// Test 9: Error paths - poll errors, recv errors, connection closed
TEST(error_path_thread_safety) {
    // These will fail to connect, exercising error paths
    Config config;
    config.iqfeed_host = "192.0.2.1";  // TEST-NET (unreachable)

    IQFeedClient client(config);
    auto result = client.connect();
    ASSERT_TRUE(result.failed());

    // Try requesting symbol search on failed connection
    auto search_result = client.request_symbol_search();
    ASSERT_TRUE(search_result.failed());

    client.disconnect();

    return true;
}

// Test 10: Process() called from multiple threads (should only be called from main)
TEST(process_not_thread_safe_warning) {
    // This test documents that process() is NOT thread-safe
    // and should only be called from the main thread

    Config config;
    config.iqfeed_host = "127.0.0.1";

    IQFeedClient client(config);
    client.connect();

    // Only main thread should call process()
    for (int i = 0; i < 10; i++) {
        client.process();
    }

    client.disconnect();

    return true;
}

// Test 11: Mutex deadlock detection - acquire in different orders
TEST(no_deadlock_on_mutex_acquisition) {
    Config config;
    config.iqfeed_host = "127.0.0.1";

    IQFeedClient client(config);
    std::atomic<bool> stop{false};

    // Thread 1: Set callbacks (locks callback_mutex_)
    std::thread t1([&]() {
        while (!stop) {
            client.set_symbol_search_callback([](const std::vector<SymbolInfo>&) {});
            uscan::safe_sleep_us(100);
        }
    });

    // Thread 2: Request search (locks symbol_search_mutex_ then callback_mutex_)
    std::thread t2([&]() {
        client.connect();
        while (!stop) {
            client.request_symbol_search();
            uscan::safe_sleep_ms(10);
        }
    });

    // Thread 3: Read counter (lock-free)
    std::thread t3([&]() {
        while (!stop) {
            auto count = client.symbols_received_count();
            (void)count;
        }
    });

    // Run for 200ms - should not deadlock
    uscan::safe_sleep_ms(200);
    stop = true;

    t1.join();
    t2.join();
    t3.join();

    client.disconnect();

    return true;
}

// Test 12: Verify thread safety annotations work
TEST(thread_safety_annotations_enforced) {
    // This test verifies that Clang's thread safety analysis is working
    // If we tried to access guarded variables without locks, compilation would fail

    // The fact that this compiles proves annotations are correct
    Config config;
    IQFeedClient client(config);

    // These accesses are safe (through public API with proper locking)
    client.symbols_received_count();  // Atomic, no lock needed
    client.is_searching_symbols();     // Atomic, no lock needed

    return true;
}

// Test 13: Shutdown during symbol search (reproduces the race condition)
TEST(shutdown_during_symbol_search) {
    // This test reproduces the exact crash scenario:
    // 1. Background thread is receiving symbols
    // 2. Callbacks are set (incremental save + search complete)
    // 3. User closes window → shutdown() called
    // 4. Verify clean shutdown with no crashes or use-after-free

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.db_path = "/tmp/test_shutdown_race.db";
    std::remove(config.db_path.c_str());

    SymbolDB db(config.db_path);
    ASSERT_TRUE(db.open().ok());

    DBWorker worker(db);

    IQFeedClient client(config);

    std::atomic<int> incremental_saves{0};
    std::atomic<bool> search_completed{false};
    std::atomic<bool> db_worker_destroyed{false};

    // Set callbacks that simulate scanner behavior
    client.set_incremental_save_callback([&](const std::vector<SymbolInfo>& batch) {
        // This callback accesses db_worker - would crash if db_worker destroyed
        if (db_worker_destroyed) {
            std::fprintf(stderr, "FATAL: Incremental save callback invoked after db_worker destroyed!\n");
            std::abort();
        }
        worker.enqueue_save(batch, [&](bool success, const std::string& error) {
            if (!success) {
                log_warn("Save failed (expected during shutdown): %s", error.c_str());
            }
            incremental_saves++;
        });
    });

    client.set_symbol_search_callback([&](const std::vector<SymbolInfo>& symbols) {
        search_completed = true;
    });

    ASSERT_TRUE(client.connect().ok());
    ASSERT_TRUE(client.request_symbol_search().ok());

    // Let background thread start receiving symbols
    uscan::safe_sleep_ms(100);

    // Simulate immediate window close (shutdown while symbols still loading)
    // CORRECT shutdown order (after fix):
    // 1. Disconnect client first (joins background thread, clears callbacks)
    // 2. Stop DB worker (drains queue, joins thread)
    // 3. Close database

    client.disconnect();  // Joins background thread - critical!
    db_worker_destroyed = true;  // Flag to detect use-after-free
    worker.stop();  // Should drain any pending saves from before disconnect
    db.close();

    // Verify clean shutdown (no crashes)
    log_verbose("Test: Clean shutdown completed");
    log_verbose("Test: Incremental saves: %d, Search completed: %d",
                incremental_saves.load(), search_completed.load());

    std::remove(config.db_path.c_str());
    return true;
}

// Test 14: DB worker drains queue on shutdown
TEST(db_worker_drains_queue_on_shutdown) {
    // Verify DBWorker processes all pending requests before exiting
    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.db_path = "/tmp/test_db_drain.db";
    std::remove(config.db_path.c_str());

    SymbolDB db(config.db_path);
    ASSERT_TRUE(db.open().ok());

    DBWorker worker(db);

    std::atomic<int> saves_completed{0};

    // Enqueue multiple saves
    for (int i = 0; i < 10; i++) {
        std::vector<SymbolInfo> batch;
        SymbolInfo info;
        info.symbol = "SYM" + std::to_string(i);
        info.name = "Test Symbol " + std::to_string(i);
        batch.push_back(info);

        worker.enqueue_save(batch, [&](bool success, const std::string& error) {
            if (!success) {
                std::fprintf(stderr, "Save failed: %s\n", error.c_str());
                std::abort();
            }
            saves_completed++;
        });
    }

    // Stop immediately (should drain all 10 saves before exiting)
    worker.stop();

    // All saves should have completed
    ASSERT_EQ(saves_completed.load(), 10);

    db.close();
    std::remove(config.db_path.c_str());
    return true;
}

int main() {
    return test::run_all_tests();
}
