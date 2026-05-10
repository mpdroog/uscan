#include "test_framework.hpp"
#include "types.hpp"
#include <cmath>

using namespace uscan;

// Test Quote gap calculations
TEST(Quote_gap_percent_positive) {
    Quote q;
    q.prev_close = 100.0;
    q.last_price = 110.0;

    ASSERT_NEAR(q.gap_percent(), 10.0, 0.001);
    return true;
}

TEST(Quote_gap_percent_negative) {
    Quote q;
    q.prev_close = 100.0;
    q.last_price = 90.0;

    ASSERT_NEAR(q.gap_percent(), -10.0, 0.001);
    return true;
}

TEST(Quote_gap_percent_zero_prev_close) {
    Quote q;
    q.prev_close = 0.0;
    q.last_price = 100.0;

    ASSERT_EQ(q.gap_percent(), 0.0);
    return true;
}

TEST(Quote_gap_percent_uses_extended_price) {
    Quote q;
    q.prev_close = 100.0;
    q.last_price = 105.0;
    q.extended_price = 115.0;

    // Should use extended_price when > 0
    ASSERT_NEAR(q.gap_percent(), 15.0, 0.001);
    return true;
}

TEST(Quote_gap_dollars) {
    Quote q;
    q.prev_close = 100.0;
    q.last_price = 112.50;

    ASSERT_NEAR(q.gap_dollars(), 12.50, 0.001);
    return true;
}

TEST(Quote_current_price_prefers_extended) {
    Quote q;
    q.last_price = 100.0;
    q.extended_price = 0.0;

    ASSERT_EQ(q.current_price(), 100.0);

    q.extended_price = 105.0;
    ASSERT_EQ(q.current_price(), 105.0);
    return true;
}

// Test Gapper construction from Quote
TEST(Gapper_from_quote) {
    Quote q("AAPL");
    q.prev_close = 150.0;
    q.last_price = 165.0;
    q.extended_volume = 50000;
    q.avg_volume = 1000000;
    q.float_shares = 16000000000;
    q.high_52wk = 200.0;
    q.low_52wk = 120.0;
    q.sector = "Technology";
    q.name = "Apple Inc.";

    Gapper g(q);

    ASSERT_STR_EQ(g.symbol.c_str(), "AAPL");
    ASSERT_NEAR(g.price, 165.0, 0.001);
    ASSERT_NEAR(g.gap_percent, 10.0, 0.001);
    ASSERT_EQ(g.premarket_volume, 50000);
    ASSERT_EQ(g.avg_volume, 1000000);
    ASSERT_NEAR(g.high_52wk, 200.0, 0.001);
    ASSERT_STR_EQ(g.sector.c_str(), "Technology");
    return true;
}

// Test Gapper sorting (by gap % descending)
TEST(Gapper_comparison) {
    Gapper g1;
    g1.gap_percent = 15.0;

    Gapper g2;
    g2.gap_percent = 10.0;

    // operator< returns true if g1 should come before g2 (higher gap %)
    ASSERT_TRUE(g1 < g2);
    ASSERT_FALSE(g2 < g1);
    return true;
}

// Test Result type
TEST(Result_success) {
    auto result = Result<int>::success(42);

    ASSERT_TRUE(result.ok());
    ASSERT_FALSE(result.failed());
    ASSERT_EQ(result.get(), 42);
    return true;
}

TEST(Result_failure) {
    auto result = Result<int>::failure("Something went wrong");

    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.failed());
    ASSERT_STR_EQ(result.error.c_str(), "Something went wrong");
    return true;
}

TEST(Result_void_success) {
    auto result = Result<void>::ok_result();

    ASSERT_TRUE(result.ok());
    ASSERT_FALSE(result.failed());
    return true;
}

TEST(Result_void_failure) {
    auto result = Result<void>::failure("Error occurred");

    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.failed());
    ASSERT_STR_EQ(result.error.c_str(), "Error occurred");
    return true;
}

// Test ConnectionState and ScannerState to_string
TEST(ConnectionState_to_string) {
    ASSERT_STR_EQ(to_string(ConnectionState::Disconnected), "Disconnected");
    ASSERT_STR_EQ(to_string(ConnectionState::Connecting), "Connecting");
    ASSERT_STR_EQ(to_string(ConnectionState::Connected), "Connected");
    ASSERT_STR_EQ(to_string(ConnectionState::Error), "Error");
    return true;
}

TEST(ScannerState_to_string) {
    ASSERT_STR_EQ(to_string(ScannerState::Idle), "Idle");
    ASSERT_STR_EQ(to_string(ScannerState::LoadingSymbols), "Loading Symbols");
    ASSERT_STR_EQ(to_string(ScannerState::Subscribing), "Subscribing");
    ASSERT_STR_EQ(to_string(ScannerState::Scanning), "Scanning");
    ASSERT_STR_EQ(to_string(ScannerState::Error), "Error");
    return true;
}

// Test Config defaults
TEST(Config_defaults) {
    Config config;

    ASSERT_NEAR(config.min_price, 1.0, 0.001);
    ASSERT_NEAR(config.max_price, 20.0, 0.001);
    ASSERT_NEAR(config.fallback_max_price, 100.0, 0.001);
    ASSERT_NEAR(config.min_gap_percent, 5.0, 0.001);
    ASSERT_EQ(config.min_premarket_volume, 50000);
    ASSERT_EQ(config.max_results, 20u);
    ASSERT_TRUE(config.gap_up_only);
    ASSERT_EQ(config.refresh_interval_ms, 3000);
    return true;
}

// Test SymbolInfo construction
TEST(SymbolInfo_construction) {
    SymbolInfo info("AAPL", "Apple Inc.", "NASDAQ", "Technology");

    ASSERT_STR_EQ(info.symbol.c_str(), "AAPL");
    ASSERT_STR_EQ(info.name.c_str(), "Apple Inc.");
    ASSERT_STR_EQ(info.exchange.c_str(), "NASDAQ");
    ASSERT_STR_EQ(info.sector.c_str(), "Technology");
    return true;
}

TEST(SymbolInfo_symbol_only_construction) {
    SymbolInfo info("TSLA");

    ASSERT_STR_EQ(info.symbol.c_str(), "TSLA");
    ASSERT_TRUE(info.name.empty());
    ASSERT_TRUE(info.exchange.empty());
    ASSERT_TRUE(info.sector.empty());
    return true;
}
