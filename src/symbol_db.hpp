#pragma once

#include "types.hpp"
#include "thread_safety.hpp"
#include <string>
#include <vector>
#include <sqlite3.h>
#include <ctime>

namespace uscan {

// SQLite database for caching symbols in price range
class SymbolDB final {
public:
    explicit SymbolDB(const std::string& db_path);
    ~SymbolDB();

    USCAN_NON_COPYABLE_NON_MOVABLE(SymbolDB);

    // Database operations
    USCAN_NODISCARD Result<void> open();
    void close();
    USCAN_NODISCARD bool is_open() const noexcept;

    // Symbol operations
    USCAN_NODISCARD Result<void> save_symbols(const std::vector<SymbolInfo>& symbols);
    USCAN_NODISCARD Result<std::vector<SymbolInfo>> load_symbols();
    USCAN_NODISCARD Result<void> update_symbol_price(const std::string& symbol, double price);
    USCAN_NODISCARD Result<std::vector<SymbolInfo>> get_symbols_in_range(double min_price, double max_price);

    // Batched price updates (non-blocking)
    void queue_price_update(const std::string& symbol, double price) EXCLUDES(update_mutex_);
    USCAN_NODISCARD Result<void> flush_price_updates() EXCLUDES(update_mutex_);

    // Utility
    USCAN_NODISCARD std::size_t symbol_count();
    USCAN_NODISCARD bool has_symbols();
    USCAN_NODISCARD Result<void> clear();

    // Get last update time
    USCAN_NODISCARD Result<int64_t> last_update_timestamp();

private:
    struct PriceUpdate {
        std::string symbol;
        double price;
        int64_t timestamp;
    };

    USCAN_NODISCARD Result<void> create_tables();
    USCAN_NODISCARD Result<void> exec(const char* sql);

    std::string db_path_;
    sqlite3* db_{nullptr};

    // Batched price updates
    std::vector<PriceUpdate> pending_updates_ GUARDED_BY(update_mutex_);
    mutable Mutex update_mutex_;
};

} // namespace uscan
