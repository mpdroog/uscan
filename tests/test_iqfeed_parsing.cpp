#include "test_framework.hpp"
#include "iqfeed_client.hpp"
#include "types.hpp"
#include <vector>
#include <string>
#include <cmath>

using namespace uscan;

// Helper to access private parse functions via a test wrapper
// Since we can't directly test private methods, we test observable behavior

// Test split function behavior via observable effects
TEST(IQFeedClient_construction) {
    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = 5009;

    IQFeedClient client(config);

    ASSERT_EQ(client.state(), ConnectionState::Disconnected);
    ASSERT_EQ(client.watched_count(), 0u);
    ASSERT_EQ(client.message_count(), 0u);
    return true;
}

TEST(IQFeedClient_watch_without_connection_fails) {
    Config config;
    IQFeedClient client(config);

    auto result = client.watch("AAPL");
    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.error.find("Not connected") != std::string::npos);
    return true;
}

TEST(IQFeedClient_unwatch_without_connection_fails) {
    Config config;
    IQFeedClient client(config);

    auto result = client.unwatch("AAPL");
    ASSERT_FALSE(result.ok());
    return true;
}

TEST(IQFeedClient_disconnect_from_disconnected) {
    Config config;
    IQFeedClient client(config);

    // Should not crash
    client.disconnect();
    ASSERT_EQ(client.state(), ConnectionState::Disconnected);
    return true;
}

TEST(IQFeedClient_quotes_empty_initially) {
    Config config;
    IQFeedClient client(config);

    const auto& quotes = client.quotes();
    ASSERT_TRUE(quotes.empty());
    return true;
}

// Test Quote edge cases
TEST(Quote_gap_with_zero_last_price) {
    Quote q;
    q.prev_close = 100.0;
    q.last_price = 0.0;
    q.extended_price = 0.0;

    // current_price is 0, gap calculation should still work
    ASSERT_EQ(q.current_price(), 0.0);
    ASSERT_NEAR(q.gap_percent(), -100.0, 0.001);  // -100% gap
    return true;
}

TEST(Quote_gap_with_very_small_prev_close) {
    Quote q;
    q.prev_close = 0.01;
    q.last_price = 0.02;

    ASSERT_NEAR(q.gap_percent(), 100.0, 0.001);  // 100% gap
    return true;
}

TEST(Quote_premarket_volume_accessor) {
    Quote q;
    q.extended_volume = 123456;

    ASSERT_EQ(q.premarket_volume(), 123456);
    return true;
}

// Test Gapper edge cases
TEST(Gapper_with_negative_gap) {
    Quote q("AAPL");
    q.prev_close = 100.0;
    q.last_price = 80.0;

    Gapper g(q);

    ASSERT_NEAR(g.gap_percent, -20.0, 0.001);
    ASSERT_NEAR(g.gap_dollars, -20.0, 0.001);
    return true;
}

TEST(Gapper_with_very_large_gap) {
    Quote q("PENNY");
    q.prev_close = 0.50;
    q.last_price = 2.50;

    Gapper g(q);

    ASSERT_NEAR(g.gap_percent, 400.0, 0.001);  // 400% gap
    ASSERT_NEAR(g.gap_dollars, 2.0, 0.001);
    return true;
}

// Test Result edge cases
TEST(Result_string_value) {
    auto result = Result<std::string>::success("hello");

    ASSERT_TRUE(result.ok());
    ASSERT_STR_EQ(result.get().c_str(), "hello");
    return true;
}

TEST(Result_vector_value) {
    std::vector<int> vec = {1, 2, 3, 4, 5};
    auto result = Result<std::vector<int>>::success(std::move(vec));

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.get().size(), 5u);
    ASSERT_EQ(result.get()[0], 1);
    ASSERT_EQ(result.get()[4], 5);
    return true;
}

TEST(Result_empty_error_message) {
    auto result = Result<int>::failure("");

    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.error.empty());
    return true;
}

// Test Config move semantics
TEST(Config_move_construction) {
    Config config1;
    config1.min_price = 5.0;
    config1.max_price = 25.0;
    config1.db_path = "custom.db";

    Config config2(std::move(config1));

    ASSERT_NEAR(config2.min_price, 5.0, 0.001);
    ASSERT_NEAR(config2.max_price, 25.0, 0.001);
    ASSERT_STR_EQ(config2.db_path.c_str(), "custom.db");
    return true;
}

// Test SymbolInfo edge cases
TEST(SymbolInfo_empty_fields) {
    SymbolInfo info("", "", "", "");

    ASSERT_TRUE(info.symbol.empty());
    ASSERT_TRUE(info.name.empty());
    ASSERT_TRUE(info.exchange.empty());
    ASSERT_TRUE(info.sector.empty());
    return true;
}

TEST(SymbolInfo_special_characters) {
    SymbolInfo info("BRK.A", "Berkshire Hathaway Inc.", "NYSE", "Financials");

    ASSERT_STR_EQ(info.symbol.c_str(), "BRK.A");
    return true;
}

// Test state enumeration coverage
TEST(ConnectionState_all_values) {
    ASSERT_EQ(static_cast<uint8_t>(ConnectionState::Disconnected), 0);
    ASSERT_EQ(static_cast<uint8_t>(ConnectionState::Connecting), 1);
    ASSERT_EQ(static_cast<uint8_t>(ConnectionState::Connected), 2);
    ASSERT_EQ(static_cast<uint8_t>(ConnectionState::Error), 3);
    return true;
}

TEST(ScannerState_all_values) {
    ASSERT_EQ(static_cast<uint8_t>(ScannerState::Idle), 0);
    ASSERT_EQ(static_cast<uint8_t>(ScannerState::Connecting), 1);
    ASSERT_EQ(static_cast<uint8_t>(ScannerState::LoadingSymbols), 2);
    ASSERT_EQ(static_cast<uint8_t>(ScannerState::Subscribing), 3);
    ASSERT_EQ(static_cast<uint8_t>(ScannerState::Scanning), 4);
    ASSERT_EQ(static_cast<uint8_t>(ScannerState::Error), 5);
    return true;
}

// Test Quote timestamp update
TEST(Quote_timestamp_default) {
    Quote q;

    // Default-initialized time_point should be epoch
    auto epoch = std::chrono::system_clock::time_point{};
    ASSERT_TRUE(q.last_update == epoch);
    return true;
}

// Stress test: many gappers sorting
TEST(Gapper_sorting_many_items) {
    std::vector<Gapper> gappers;

    // Create 100 gappers with random gap percentages
    for (int i = 0; i < 100; ++i) {
        Gapper g;
        g.symbol = "SYM" + std::to_string(i);
        g.gap_percent = static_cast<double>(i % 50);  // 0-49%
        gappers.push_back(g);
    }

    // Sort using operator<
    std::sort(gappers.begin(), gappers.end());

    // Verify sorted in descending order
    for (std::size_t i = 1; i < gappers.size(); ++i) {
        ASSERT_TRUE(gappers[i-1].gap_percent >= gappers[i].gap_percent);
    }

    return true;
}

// Test Quote default values
TEST(Quote_all_defaults_zero) {
    Quote q;

    ASSERT_EQ(q.last_price, 0.0);
    ASSERT_EQ(q.bid, 0.0);
    ASSERT_EQ(q.ask, 0.0);
    ASSERT_EQ(q.prev_close, 0.0);
    ASSERT_EQ(q.open, 0.0);
    ASSERT_EQ(q.high, 0.0);
    ASSERT_EQ(q.low, 0.0);
    ASSERT_EQ(q.extended_price, 0.0);
    ASSERT_EQ(q.extended_volume, 0);
    ASSERT_EQ(q.volume, 0);
    ASSERT_EQ(q.avg_volume, 0);
    ASSERT_EQ(q.high_52wk, 0.0);
    ASSERT_EQ(q.low_52wk, 0.0);
    ASSERT_EQ(q.float_shares, 0);
    ASSERT_TRUE(q.sector.empty());
    ASSERT_TRUE(q.name.empty());

    return true;
}

// ============================================================================
// Error Handling Tests
// ============================================================================

// Test connection to unreachable host fails gracefully
TEST(IQFeedClient_connect_unreachable_host) {
    Config config;
    config.iqfeed_host = "192.0.2.1";  // TEST-NET-1, guaranteed unreachable
    config.iqfeed_l1_port = 5009;
    config.iqfeed_hist_port = 9100;

    IQFeedClient client(config);

    auto result = client.connect();
    // Should fail - unreachable host
    ASSERT_FALSE(result.ok());
    // State should be Error
    ASSERT_EQ(client.state(), ConnectionState::Error);
    // Error message should be set
    ASSERT_FALSE(client.last_error().empty());

    return true;
}

// Test multiple disconnect calls are safe
TEST(IQFeedClient_multiple_disconnect_safe) {
    Config config;
    IQFeedClient client(config);

    // Multiple disconnects should not crash
    client.disconnect();
    client.disconnect();
    client.disconnect();

    ASSERT_EQ(client.state(), ConnectionState::Disconnected);
    return true;
}

// Test watch on empty symbol fails
TEST(IQFeedClient_watch_empty_symbol) {
    Config config;
    IQFeedClient client(config);

    // Can't watch when not connected
    auto result = client.watch("");
    ASSERT_FALSE(result.ok());
    return true;
}

// Test process on disconnected client is safe
TEST(IQFeedClient_process_when_disconnected) {
    Config config;
    IQFeedClient client(config);

    // Should not crash when called on disconnected client
    client.process();
    client.process();
    client.process();

    ASSERT_EQ(client.state(), ConnectionState::Disconnected);
    return true;
}

// Test unwatch_all on disconnected client is safe
TEST(IQFeedClient_unwatch_all_when_disconnected) {
    Config config;
    IQFeedClient client(config);

    // Should not crash
    client.unwatch_all();
    ASSERT_EQ(client.watched_count(), 0u);
    return true;
}

// Test callback setting when disconnected
TEST(IQFeedClient_set_callbacks_when_disconnected) {
    Config config;
    IQFeedClient client(config);

    bool callback_called = false;

    // Setting callbacks should work even when disconnected
    client.set_quote_callback([&](const Quote&) { callback_called = true; });
    client.set_regional_quote_callback([&](const RegionalQuote&) {});
    client.set_trade_correction_callback([&](const TradeCorrection&) {});
    client.set_news_callback([&](const NewsHeadline&) {});
    client.set_symbol_limit_callback([&](const std::string&) {});
    client.set_symbol_search_callback([&](const std::vector<SymbolInfo>&) {});
    client.set_incremental_save_callback([&](const std::vector<SymbolInfo>&) {});

    // Callback should not have been called
    ASSERT_FALSE(callback_called);
    return true;
}

// Test request_symbol_search when disconnected fails
TEST(IQFeedClient_symbol_search_when_disconnected) {
    Config config;
    IQFeedClient client(config);

    auto result = client.request_symbol_search();
    ASSERT_FALSE(result.ok());
    return true;
}

// Test is_searching_symbols returns false when not searching
TEST(IQFeedClient_is_searching_false_initially) {
    Config config;
    IQFeedClient client(config);

    ASSERT_FALSE(client.is_searching_symbols());
    ASSERT_EQ(client.symbols_received_count(), 0u);
    return true;
}
