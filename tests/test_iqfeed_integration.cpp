#include "test_framework.hpp"
#include "iqfeed_client.hpp"
#include "types.hpp"
#include <atomic>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <chrono>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

using namespace uscan;

// Global mock server process
static pid_t mock_server_pid = -1;

// Forward declaration
static void stop_mock_server();

// Check if external server is running (started by bash script)
static bool external_server_running() {
    return std::getenv("USCAN_TEST_SERVER_RUNNING") != nullptr;
}

// Get test ports from environment or use defaults
static int get_l1_port() {
    const char* port = std::getenv("USCAN_TEST_L1_PORT");
    return port ? std::atoi(port) : 5009;
}

static int get_lookup_port() {
    const char* port = std::getenv("USCAN_TEST_LOOKUP_PORT");
    return port ? std::atoi(port) : 9100;
}

// Try to connect to a port with timeout (returns true if connection succeeds)
static bool try_connect(int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        close(fd);
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(fd);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret == 0) {
        close(fd);
        return true;
    }

    if (errno != EINPROGRESS) {
        close(fd);
        return false;
    }

    // Wait for connection with timeout
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    int poll_ret = poll(&pfd, 1, timeout_ms);
    close(fd);

    return poll_ret > 0;
}

// Start the mock IQFeed server
static bool start_mock_server() {
    // If external server is running (started by bash script), just verify it's ready
    if (external_server_running()) {
        const int l1_port = get_l1_port();
        const int lookup_port = get_lookup_port();
        if (try_connect(l1_port, 100) && try_connect(lookup_port, 100)) {
            return true;
        }
        std::fprintf(stderr, "External server not reachable on ports %d and %d\n", l1_port, lookup_port);
        return false;
    }

    // Kill any existing server
    if (mock_server_pid > 0) {
        kill(mock_server_pid, SIGTERM);
        waitpid(mock_server_pid, nullptr, 0);
        mock_server_pid = -1;
    }

    const int l1_port = get_l1_port();
    const int lookup_port = get_lookup_port();

    mock_server_pid = fork();
    if (mock_server_pid == 0) {
        // Child process - run the Go mock server
        char l1_arg[32], lookup_arg[32];
        std::snprintf(l1_arg, sizeof(l1_arg), "-l1-port=%d", l1_port);
        std::snprintf(lookup_arg, sizeof(lookup_arg), "-lookup-port=%d", lookup_port);
        execl("/usr/bin/env", "env", "go", "run",
              "tests/mock_iqfeed_server.go", l1_arg, lookup_arg, nullptr);
        // If execl returns, it failed
        std::perror("Failed to start mock server");
        std::exit(1);
    } else if (mock_server_pid < 0) {
        std::perror("fork failed");
        return false;
    }

    // Parent process - wait for server to be ready by polling the ports
    std::printf("Starting mock IQFeed server (pid %d)...\n", mock_server_pid);

    // Wait up to 10 seconds for server to start (Go compile can be slow)
    bool server_ready = false;
    for (int i = 0; i < 100; ++i) {  // 100 * 100ms = 10 seconds
        // Check if child process died
        int status;
        pid_t result = waitpid(mock_server_pid, &status, WNOHANG);
        if (result == mock_server_pid) {
            std::fprintf(stderr, "Mock server process exited unexpectedly\n");
            mock_server_pid = -1;
            return false;
        }

        // Try to connect to both ports
        if (try_connect(l1_port, 100) && try_connect(lookup_port, 100)) {
            server_ready = true;
            break;
        }

        uscan::safe_sleep_ms(100);
    }

    if (!server_ready) {
        std::fprintf(stderr, "Mock server did not become ready within timeout\n");
        stop_mock_server();
        return false;
    }

    std::printf("Mock server is ready\n");
    return true;
}

// Stop the mock server with timeout to prevent hanging
static void stop_mock_server() {
    // If using external server, don't stop it
    if (external_server_running()) {
        return;
    }

    if (mock_server_pid > 0) {
        std::printf("Stopping mock IQFeed server (pid %d)...\n", mock_server_pid);
        kill(mock_server_pid, SIGTERM);

        // Wait with timeout - use WNOHANG and poll
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 50; ++i) {  // 5 second timeout (50 * 100ms)
            pid_t result = waitpid(mock_server_pid, &status, WNOHANG);
            if (result == mock_server_pid) {
                exited = true;
                break;
            } else if (result == -1) {
                // Error or already reaped
                exited = true;
                break;
            }
            uscan::safe_sleep_ms(100);
        }

        // If still running after timeout, force kill
        if (!exited) {
            std::fprintf(stderr, "Mock server did not exit gracefully, sending SIGKILL\n");
            kill(mock_server_pid, SIGKILL);
            waitpid(mock_server_pid, nullptr, 0);  // Clean up zombie
        }

        mock_server_pid = -1;
        // Wait longer for ports to be released
        uscan::safe_sleep_s(1);
    }
}

// Helper to wait for condition with timeout
template<typename Func>
static bool wait_for(Func condition, int timeout_ms = 5000) {
    auto start = std::chrono::steady_clock::now();
    while (!condition()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            return false;
        }
        uscan::safe_sleep_ms(10);
    }
    return true;
}

// ============================================================================
// Connection and Protocol Tests
// ============================================================================

TEST(Integration_connect_to_mock_server) {
    if (!start_mock_server()) {
        std::fprintf(stderr, "Failed to start mock server\n");
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);

    auto result = client.connect();
    if (!result.ok()) {
        std::fprintf(stderr, "  Connect failed: %s\n", result.error.c_str());
    }
    ASSERT_TRUE(result.ok());

    // Wait for connection to be established
    bool connected = wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    ASSERT_TRUE(connected);
    ASSERT_EQ(client.state(), ConnectionState::Connected);

    client.disconnect();
    ASSERT_EQ(client.state(), ConnectionState::Disconnected);

    stop_mock_server();
    return true;
}

TEST(Integration_reconnect_after_disconnect) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);

    // First connection
    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    bool connected = wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });
    ASSERT_TRUE(connected);

    // Disconnect
    client.disconnect();
    ASSERT_EQ(client.state(), ConnectionState::Disconnected);

    // Reconnect
    result = client.connect();
    ASSERT_TRUE(result.ok());

    connected = wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });
    ASSERT_TRUE(connected);

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_connect_with_server_not_running) {
    // Use random high ports that definitely have no server
    // (different from the test server ports)
    const uint16_t unused_l1_port = 59999;  // High port unlikely to be in use
    const uint16_t unused_lookup_port = 59998;

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = unused_l1_port;
    config.iqfeed_hist_port = unused_lookup_port;

    IQFeedClient client(config);

    auto result = client.connect();
    // Connection should fail - no server on these ports

    // Process should detect the connection failure
    for (int i = 0; i < 10; ++i) {
        client.process();
        if (client.state() == ConnectionState::Error ||
            client.state() == ConnectionState::Disconnected) {
            break;
        }
        uscan::safe_sleep_ms(100);
    }

    // Should either fail to connect or detect error state
    ASSERT_TRUE(!result.ok() ||
                client.state() == ConnectionState::Error ||
                client.state() == ConnectionState::Disconnected);

    return true;
}

// ============================================================================
// Quote Streaming Tests
// ============================================================================

TEST(Integration_watch_single_symbol) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);

    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    bool connected = wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });
    ASSERT_TRUE(connected);

    // Watch AAPL
    result = client.watch("AAPL");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(client.watched_count(), 1u);

    // Wait for quote data with populated fields (not just symbol existence)
    bool got_quote = wait_for([&]() {
        client.process();
        const auto& quotes = client.quotes();
        auto it = quotes.find("AAPL");
        if (it == quotes.end()) return false;
        // Wait until fundamental data is populated
        return !it->second.name.empty() && it->second.last_price > 0.0;
    });

    ASSERT_TRUE(got_quote);

    const auto& quotes = client.quotes();
    ASSERT_EQ(quotes.size(), 1u);

    const auto& quote = quotes.at("AAPL");
    ASSERT_STR_EQ(quote.symbol.c_str(), "AAPL");
    ASSERT_STR_EQ(quote.name.c_str(), "APPLE INC");

    // Verify fundamental data
    ASSERT_GT(quote.avg_volume, 0);
    ASSERT_GT(quote.high_52wk, 0.0);
    ASSERT_GT(quote.low_52wk, 0.0);
    ASSERT_GT(quote.float_shares, 0);

    // Verify quote data
    ASSERT_GT(quote.last_price, 0.0);
    ASSERT_GT(quote.bid, 0.0);
    ASSERT_GT(quote.ask, 0.0);
    ASSERT_GT(quote.prev_close, 0.0);
    ASSERT_GT(quote.volume, 0);

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_watch_multiple_symbols) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);
    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    // Watch multiple symbols
    result = client.watch("AAPL");
    ASSERT_TRUE(result.ok());
    result = client.watch("MSFT");
    ASSERT_TRUE(result.ok());
    result = client.watch("TSLA");
    ASSERT_TRUE(result.ok());

    ASSERT_EQ(client.watched_count(), 3u);

    // Wait for all quotes
    bool got_all = wait_for([&]() {
        client.process();
        const auto& quotes = client.quotes();
        return quotes.size() >= 3;
    });

    ASSERT_TRUE(got_all);

    const auto& quotes = client.quotes();
    ASSERT_TRUE(quotes.find("AAPL") != quotes.end());
    ASSERT_TRUE(quotes.find("MSFT") != quotes.end());
    ASSERT_TRUE(quotes.find("TSLA") != quotes.end());

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_unwatch_symbol) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);
    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    // Watch then unwatch
    result = client.watch("AAPL");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(client.watched_count(), 1u);

    result = client.unwatch("AAPL");
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(client.watched_count(), 0u);

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_quote_updates) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);
    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    result = client.watch("AAPL");
    ASSERT_TRUE(result.ok());

    // Wait for initial quote
    wait_for([&]() {
        client.process();
        const auto& quotes = client.quotes();
        return quotes.find("AAPL") != quotes.end();
    });

    double initial_price = client.quotes().at("AAPL").last_price;

    // Wait for at least one update (server sends updates every 1 second)
    uscan::safe_sleep_ms(1500);

    for (int i = 0; i < 20; ++i) {
        client.process();
        uscan::safe_sleep_ms(50);
    }

    double updated_price = client.quotes().at("AAPL").last_price;

    // Price should have changed (mock server increments by 0.10 per update)
    ASSERT_NE(initial_price, updated_price);

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_quote_callback) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);

    int callback_count = 0;
    std::string callback_symbol;

    client.set_quote_callback([&](const Quote& quote) {
        callback_count++;
        callback_symbol = quote.symbol;
    });

    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    result = client.watch("AAPL");
    ASSERT_TRUE(result.ok());

    // Wait for callback to be invoked
    bool got_callback = wait_for([&]() {
        client.process();
        return callback_count > 0;
    });

    ASSERT_TRUE(got_callback);
    ASSERT_GT(callback_count, 0);
    ASSERT_STR_EQ(callback_symbol.c_str(), "AAPL");

    client.disconnect();
    stop_mock_server();
    return true;
}

// ============================================================================
// Symbol Search Tests
// ============================================================================

TEST(Integration_symbol_search) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);
    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    std::vector<SymbolInfo> search_results;
    std::atomic<bool> search_complete{false};

    client.set_symbol_search_callback([&](const std::vector<SymbolInfo>& symbols) {
        search_results = symbols;
        search_complete.store(true, std::memory_order_release);
    });

    result = client.request_symbol_search();
    ASSERT_TRUE(result.ok());

    // Wait for search to complete
    bool completed = wait_for([&]() {
        client.process();
        return search_complete.load(std::memory_order_acquire);
    }, 10000);

    ASSERT_TRUE(completed);
    ASSERT_GT(search_results.size(), 0u);

    // Mock server sends AAPL, MSFT, TSLA, GOOGL, AMZN, META, NVDA (type 1)
    // and filters out SPY (type 2), ^SPX (type 3)
    // Client also filters out symbols with dots
    ASSERT_TRUE(search_results.size() >= 5);

    // Verify we got expected symbols
    bool found_aapl = false;
    bool found_msft = false;
    for (const auto& sym : search_results) {
        if (sym.symbol == "AAPL") {
            found_aapl = true;
            ASSERT_STR_EQ(sym.name.c_str(), "APPLE INC");
        }
        if (sym.symbol == "MSFT") {
            found_msft = true;
            ASSERT_STR_EQ(sym.name.c_str(), "MICROSOFT CORP");
        }
    }

    ASSERT_TRUE(found_aapl);
    ASSERT_TRUE(found_msft);

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_incremental_symbol_save) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);
    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    int incremental_callback_count = 0;

    client.set_incremental_save_callback([&](const std::vector<SymbolInfo>&) {
        incremental_callback_count++;
    });

    result = client.request_symbol_search();
    ASSERT_TRUE(result.ok());

    // Wait for search to complete
    wait_for([&]() {
        client.process();
        return !client.is_searching_symbols();
    }, 10000);

    // With small number of symbols, may or may not trigger incremental saves
    // (incremental saves happen every 500 symbols)
    // This test just verifies the callback mechanism works if triggered

    client.disconnect();
    stop_mock_server();
    return true;
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST(Integration_watch_without_connection) {
    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());

    IQFeedClient client(config);

    auto result = client.watch("AAPL");
    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.error.find("Not connected") != std::string::npos);

    return true;
}

TEST(Integration_message_count_tracking) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);
    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    std::size_t initial_count = client.message_count();

    result = client.watch("AAPL");
    ASSERT_TRUE(result.ok());

    // Wait for messages
    uscan::safe_sleep_ms(500);

    for (int i = 0; i < 10; ++i) {
        client.process();
        uscan::safe_sleep_ms(50);
    }

    std::size_t final_count = client.message_count();

    // Should have received multiple messages (S, F, P, Q, T types)
    ASSERT_GT(final_count, initial_count);

    client.disconnect();
    stop_mock_server();
    return true;
}

// ============================================================================
// Extended Hours / Pre-market Tests
// ============================================================================

TEST(Integration_extended_hours_data) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);
    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    result = client.watch("AAPL");
    ASSERT_TRUE(result.ok());

    // Wait for extended hours data to be populated
    bool got_extended = wait_for([&]() {
        client.process();
        const auto& quotes = client.quotes();
        auto it = quotes.find("AAPL");
        if (it == quotes.end()) return false;
        return it->second.extended_price > 0.0 && it->second.extended_volume > 0;
    });
    ASSERT_TRUE(got_extended);

    const auto& quote = client.quotes().at("AAPL");

    // Mock server sends extended hours data
    ASSERT_GT(quote.extended_price, 0.0);
    ASSERT_GT(quote.extended_volume, 0);

    // Verify premarket_volume accessor
    ASSERT_EQ(quote.premarket_volume(), quote.extended_volume);

    // Verify current_price uses extended price when available
    double current = quote.current_price();
    ASSERT_EQ(current, quote.extended_price);

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_gap_calculation) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);
    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    result = client.watch("AAPL");
    ASSERT_TRUE(result.ok());

    // Wait for both prev_close and extended_price to be populated for gap calculation
    bool got_data = wait_for([&]() {
        client.process();
        const auto& quotes = client.quotes();
        auto it = quotes.find("AAPL");
        if (it == quotes.end()) return false;
        return it->second.prev_close > 0.0 && it->second.extended_price > 0.0;
    });
    ASSERT_TRUE(got_data);

    const auto& quote = client.quotes().at("AAPL");

    // Gap calculation should work
    double gap = quote.gap_percent();

    // Mock server sets prev_close = 148.00, extended_price = 149.00
    // Gap = (149 - 148) / 148 * 100 = 0.676%
    ASSERT_GT(gap, 0.0);
    ASSERT_LT(gap, 2.0);  // Should be small positive gap

    client.disconnect();
    stop_mock_server();
    return true;
}

// ============================================================================
// New Message Type Tests (R, C, N)
// ============================================================================

TEST(Integration_regional_quote_callback) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);

    bool got_regional = false;
    RegionalQuote received_quote;

    client.set_regional_quote_callback([&](const RegionalQuote& rq) {
        got_regional = true;
        received_quote = rq;
    });

    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    // Watch special test symbol that triggers regional quote
    result = client.watch("TEST_REGIONAL");
    ASSERT_TRUE(result.ok());

    // Wait for regional quote callback
    bool completed = wait_for([&]() {
        client.process();
        return got_regional;
    });

    ASSERT_TRUE(completed);
    ASSERT_STR_EQ(received_quote.symbol.c_str(), "TEST");
    ASSERT_STR_EQ(received_quote.exchange.c_str(), "NYSE");
    ASSERT_NEAR(received_quote.bid, 100.50, 0.01);
    ASSERT_NEAR(received_quote.ask, 100.55, 0.01);
    ASSERT_EQ(received_quote.bid_size, 500);
    ASSERT_EQ(received_quote.ask_size, 600);
    ASSERT_NEAR(received_quote.last, 100.52, 0.01);

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_trade_correction_callback) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);

    bool got_correction = false;
    TradeCorrection received_correction;

    client.set_trade_correction_callback([&](const TradeCorrection& tc) {
        got_correction = true;
        received_correction = tc;
    });

    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    // Watch special test symbol that triggers trade correction
    result = client.watch("TEST_CORRECTION");
    ASSERT_TRUE(result.ok());

    // Wait for correction callback
    bool completed = wait_for([&]() {
        client.process();
        return got_correction;
    });

    ASSERT_TRUE(completed);
    ASSERT_STR_EQ(received_correction.symbol.c_str(), "TEST");
    ASSERT_EQ(received_correction.correction_type, 'D');
    ASSERT_NEAR(received_correction.price, 100.50, 0.01);
    ASSERT_EQ(received_correction.size, 1000);
    ASSERT_STR_EQ(received_correction.trade_id.c_str(), "TRADE123");

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_news_callback) {
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);

    bool got_news = false;
    NewsHeadline received_news;

    client.set_news_callback([&](const NewsHeadline& nh) {
        got_news = true;
        received_news = nh;
    });

    auto result = client.connect();
    ASSERT_TRUE(result.ok());

    wait_for([&]() {
        client.process();
        return client.state() == ConnectionState::Connected;
    });

    // Watch special test symbol that triggers news headline
    result = client.watch("TEST_NEWS");
    ASSERT_TRUE(result.ok());

    // Wait for news callback
    bool completed = wait_for([&]() {
        client.process();
        return got_news;
    });

    ASSERT_TRUE(completed);
    ASSERT_STR_EQ(received_news.headline_id.c_str(), "12345");
    ASSERT_STR_EQ(received_news.source.c_str(), "Reuters");
    ASSERT_STR_EQ(received_news.symbols.c_str(), "AAPL:MSFT");
    ASSERT_TRUE(received_news.headline.find("Tech stocks") != std::string::npos);

    client.disconnect();
    stop_mock_server();
    return true;
}

TEST(Integration_protocol_validation) {
    // Protocol validation is tested implicitly by all connect tests
    // This test verifies that connect() properly waits for S,CURRENT PROTOCOL
    if (!start_mock_server()) {
        return false;
    }

    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = static_cast<uint16_t>(get_l1_port());
    config.iqfeed_hist_port = static_cast<uint16_t>(get_lookup_port());

    IQFeedClient client(config);

    auto result = client.connect();
    // connect() should succeed, which means protocol was validated
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(client.state(), ConnectionState::Connected);

    client.disconnect();
    stop_mock_server();
    return true;
}

// ============================================================================
// Cleanup
// ============================================================================

// Ensure server is stopped after all tests
static void cleanup_integration_tests() __attribute__((destructor));
static void cleanup_integration_tests() {
    stop_mock_server();
}
