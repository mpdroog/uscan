#pragma once

#include "types.hpp"
#include "iqfeed_client.hpp"
#include "symbol_db.hpp"
#include "db_worker.hpp"
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>

namespace uscan {

// Pre-market gap scanner
class Scanner final {
public:
    explicit Scanner(Config config);
    ~Scanner();

    USCAN_NON_COPYABLE_NON_MOVABLE(Scanner);

    // Lifecycle
    USCAN_NODISCARD Result<void> initialize();
    void shutdown();

    // Main loop processing
    void update();

    // Manual refresh
    void refresh_symbols();

    // Get current gappers (sorted by gap %)
    USCAN_NODISCARD std::vector<Gapper> get_gappers() const;

    // State accessors
    USCAN_NODISCARD ScannerState state() const noexcept;
    USCAN_NODISCARD ConnectionState connection_state() const noexcept;
    USCAN_NODISCARD const std::string& last_error() const noexcept;
    USCAN_NODISCARD Progress progress() const noexcept;

    // Statistics
    USCAN_NODISCARD std::size_t symbols_watched() const noexcept;
    USCAN_NODISCARD std::size_t symbols_in_db() const noexcept;
    USCAN_NODISCARD std::size_t messages_received() const noexcept;
    USCAN_NODISCARD bool using_fallback_range() const noexcept;

    // Config accessors
    USCAN_NODISCARD const Config& config() const noexcept;
    Config& config() noexcept;

private:
    void load_or_fetch_symbols();
    void subscribe_to_symbols();
    void process_quote(const Quote& quote);
    void update_gappers();

    // Async callbacks
    void on_symbols_loaded(const std::vector<SymbolInfo>& symbols);
    void on_symbols_saved(bool success, const std::string& error);

    Config config_;
    std::unique_ptr<IQFeedClient> client_;
    std::unique_ptr<SymbolDB> db_;
    std::unique_ptr<DBWorker> db_worker_;

    std::vector<SymbolInfo> symbols_;
    std::vector<Gapper> gappers_;

    mutable std::mutex gappers_mutex_;
    std::atomic<ScannerState> state_{ScannerState::Idle};
    std::string last_error_;
    Progress progress_;

    std::chrono::steady_clock::time_point last_update_;
    std::chrono::steady_clock::time_point last_db_flush_;
    std::atomic<bool> using_fallback_{false};
    std::atomic<std::size_t> subscribe_index_{0};
    std::atomic<bool> symbols_loading_{false};
    std::atomic<bool> symbols_saving_{false};
};

} // namespace uscan
