#include "test_framework.hpp"
#include "scanner.hpp"
#include "types.hpp"

using namespace uscan;

// Test Scanner construction and config
TEST(Scanner_construction_with_default_config) {
    Config config;
    Scanner scanner(std::move(config));

    ASSERT_EQ(scanner.state(), ScannerState::Idle);
    ASSERT_EQ(scanner.connection_state(), ConnectionState::Disconnected);
    ASSERT_EQ(scanner.symbols_watched(), 0u);
    ASSERT_EQ(scanner.symbols_in_db(), 0u);
    ASSERT_FALSE(scanner.using_fallback_range());

    return true;
}

TEST(Scanner_config_access) {
    Config config;
    config.min_gap_percent = 10.0;
    config.min_price = 5.0;

    Scanner scanner(std::move(config));

    ASSERT_NEAR(scanner.config().min_gap_percent, 10.0, 0.001);
    ASSERT_NEAR(scanner.config().min_price, 5.0, 0.001);

    return true;
}

TEST(Scanner_mutable_config_access) {
    Config config;
    Scanner scanner(std::move(config));

    // Modify via mutable accessor
    scanner.config().min_gap_percent = 15.0;

    ASSERT_NEAR(scanner.config().min_gap_percent, 15.0, 0.001);

    return true;
}

TEST(Scanner_get_gappers_empty_initially) {
    Config config;
    Scanner scanner(std::move(config));

    auto gappers = scanner.get_gappers();
    ASSERT_TRUE(gappers.empty());

    return true;
}

TEST(Scanner_last_error_empty_initially) {
    Config config;
    Scanner scanner(std::move(config));

    ASSERT_TRUE(scanner.last_error().empty());

    return true;
}

TEST(Scanner_messages_received_zero_initially) {
    Config config;
    Scanner scanner(std::move(config));

    ASSERT_EQ(scanner.messages_received(), 0u);

    return true;
}

// Test initialization failure (no IQFeed running)
TEST(Scanner_initialize_fails_without_iqfeed) {
    Config config;
    config.iqfeed_host = "127.0.0.1";
    config.iqfeed_l1_port = 59999;  // Invalid port

    Scanner scanner(std::move(config));

    auto result = scanner.initialize();

    // Should fail to connect
    ASSERT_FALSE(result.ok());
    ASSERT_FALSE(scanner.last_error().empty());
    ASSERT_EQ(scanner.state(), ScannerState::Error);

    return true;
}

// Test shutdown from idle state
TEST(Scanner_shutdown_from_idle) {
    Config config;
    Scanner scanner(std::move(config));

    // Should not crash
    scanner.shutdown();

    ASSERT_EQ(scanner.state(), ScannerState::Idle);

    return true;
}

// Test update from idle state
TEST(Scanner_update_from_idle) {
    Config config;
    Scanner scanner(std::move(config));

    // Should not crash
    scanner.update();

    ASSERT_EQ(scanner.state(), ScannerState::Idle);

    return true;
}

// Test config validation
TEST(Scanner_config_validation) {
    Config config;

    // Ensure all config values are sensible
    ASSERT_GT(config.min_price, 0.0);
    ASSERT_GT(config.max_price, config.min_price);
    ASSERT_GT(config.fallback_max_price, config.max_price);
    ASSERT_GT(config.min_gap_percent, 0.0);
    ASSERT_GT(config.min_premarket_volume, 0);
    ASSERT_GT(config.max_results, 0u);
    ASSERT_GT(config.refresh_interval_ms, 0);

    return true;
}

// Test db_path default
TEST(Scanner_db_path_default) {
    Config config;

    ASSERT_STR_EQ(config.db_path.c_str(), "uscan.db");

    return true;
}
