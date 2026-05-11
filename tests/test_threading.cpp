#include "../src/scanner.hpp"
#include "test_framework.hpp"
#include <thread>
#include <chrono>
#include <unistd.h>

// Temp DB helper for tests
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

using namespace uscan;

TEST(rapid_init_shutdown) {
    // Try 10 rapid init/shutdown cycles to catch threading bugs
    for (int i = 0; i < 10; i++) {
        TestDB tmp;
        Config config;
        config.db_path = tmp.path();
        config.iqfeed_host = "127.0.0.1";  // Will fail to connect, that's fine

        Scanner scanner(std::move(config));
        scanner.initialize();
        uscan::safe_sleep_ms(10);
        scanner.shutdown();
    }

    return true;
}

TEST(shutdown_during_symbol_search) {
    TestDB tmp;
    Config config;
    config.db_path = tmp.path();
    config.iqfeed_host = "127.0.0.1";

    Scanner scanner(std::move(config));
    scanner.initialize();
    scanner.refresh_symbols();
    uscan::safe_sleep_ms(50);
    scanner.shutdown();

    return true;
}

TEST(concurrent_scanners) {
    TestDB tmp1;
    TestDB tmp2;

    Config config1;
    config1.db_path = tmp1.path();
    config1.iqfeed_host = "127.0.0.1";

    Config config2;
    config2.db_path = tmp2.path();
    config2.iqfeed_host = "127.0.0.1";

    std::thread t1([&tmp1]() {
        Config cfg;
        cfg.db_path = tmp1.path();
        cfg.iqfeed_host = "127.0.0.1";
        Scanner scanner(std::move(cfg));
        scanner.initialize();
        uscan::safe_sleep_ms(100);
        scanner.shutdown();
    });

    std::thread t2([&tmp2]() {
        Config cfg;
        cfg.db_path = tmp2.path();
        cfg.iqfeed_host = "127.0.0.1";
        Scanner scanner(std::move(cfg));
        scanner.initialize();
        uscan::safe_sleep_ms(100);
        scanner.shutdown();
    });

    t1.join();
    t2.join();

    return true;
}

int main() {
    return test::run_all_tests();
}
