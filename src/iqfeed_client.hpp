#pragma once

#include "types.hpp"
#include "thread_safety.hpp"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>

namespace uscan {

// Callback types
using QuoteCallback = std::function<void(const Quote&)>;
using SymbolListCallback = std::function<void(const std::vector<SymbolInfo>&)>;
using RegionalQuoteCallback = std::function<void(const RegionalQuote&)>;
using TradeCorrectionCallback = std::function<void(const TradeCorrection&)>;
using NewsCallback = std::function<void(const NewsHeadline&)>;
using SymbolLimitCallback = std::function<void(const std::string&)>;

// IQFeed Level 1 TCP client
class IQFeedClient final {
public:
    explicit IQFeedClient(const Config& config);
    ~IQFeedClient();

    USCAN_NON_COPYABLE_NON_MOVABLE(IQFeedClient);

    // Connection management
    USCAN_NODISCARD Result<void> connect();
    void disconnect() EXCLUDES(callback_mutex_);
    USCAN_NODISCARD ConnectionState state() const noexcept;
    USCAN_NODISCARD const std::string& last_error() const noexcept;

    // Symbol operations
    USCAN_NODISCARD Result<void> watch(const std::string& symbol);
    USCAN_NODISCARD Result<void> unwatch(const std::string& symbol);
    void unwatch_all();

    // Symbol search via TCP SBF command (non-blocking)
    USCAN_NODISCARD Result<void> request_symbol_search() EXCLUDES(symbol_search_mutex_);
    void set_symbol_search_callback(SymbolListCallback cb) EXCLUDES(callback_mutex_);
    void set_incremental_save_callback(SymbolListCallback cb) EXCLUDES(callback_mutex_);

    // Process incoming data (call from main loop)
    void process();

    // Set callback for quote updates
    void set_quote_callback(QuoteCallback cb);

    // Set callbacks for additional message types
    void set_regional_quote_callback(RegionalQuoteCallback cb);
    void set_trade_correction_callback(TradeCorrectionCallback cb);
    void set_news_callback(NewsCallback cb);
    void set_symbol_limit_callback(SymbolLimitCallback cb);

    // Get current quotes
    USCAN_NODISCARD const std::unordered_map<std::string, Quote>& quotes() const noexcept;

    // Statistics
    USCAN_NODISCARD std::size_t watched_count() const noexcept;
    USCAN_NODISCARD std::size_t message_count() const noexcept;
    USCAN_NODISCARD std::size_t symbols_received_count() const noexcept;
    USCAN_NODISCARD bool is_searching_symbols() const noexcept;

private:
    // Socket operations
    USCAN_NODISCARD bool send_command(const std::string& cmd);
    USCAN_NODISCARD bool read_data();           // Read from L1 port
    USCAN_NODISCARD bool read_lookup_data() EXCLUDES(symbol_search_mutex_);    // Read from Lookup port
    void parse_message(const std::string& line);
    void parse_lookup_message(const std::string& line) EXCLUDES(symbol_search_mutex_, callback_mutex_);

    // Message parsers
    void parse_fundamental(const std::vector<std::string>& fields);
    void parse_summary(const std::vector<std::string>& fields);
    void parse_update(const std::vector<std::string>& fields);
    void parse_system(const std::vector<std::string>& fields);
    void parse_regional(const std::vector<std::string>& fields);
    void parse_correction(const std::vector<std::string>& fields);
    void parse_news(const std::vector<std::string>& fields);

    // Dynamic field index lookup (returns fallback if field not in map)
    std::size_t get_field_index(const char* field_name, std::size_t fallback) const;

    // Utility
    static std::vector<std::string> split(const std::string& s, char delim);
    static double parse_double(const std::string& s) noexcept;
    static int64_t parse_int64(const std::string& s) noexcept;

    const Config& config_;

    // Socket file descriptors - atomic for thread-safe access from background thread
    // std::atomic<int> works in all build modes (no special libraries needed)
    std::atomic<int> socket_fd_{-1};    // Level 1 port (5009)
    std::atomic<int> lookup_fd_{-1};    // Lookup port (9100)

    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    std::string last_error_;
    std::string recv_buffer_;     // Buffer for Level 1 messages
    std::string lookup_buffer_;   // Buffer for Lookup port messages

    std::unordered_map<std::string, Quote> quotes_ GUARDED_BY(quotes_mutex_);
    std::vector<std::string> watched_symbols_;
    mutable Mutex quotes_mutex_;

    QuoteCallback quote_callback_;
    RegionalQuoteCallback regional_quote_callback_;
    TradeCorrectionCallback trade_correction_callback_;
    NewsCallback news_callback_;
    SymbolLimitCallback symbol_limit_callback_;
    std::atomic<std::size_t> message_count_{0};

    // Dynamic field mapping - populated from S,CURRENT UPDATE FIELDNAMES
    std::unordered_map<std::string, std::size_t> field_index_map_ GUARDED_BY(field_map_mutex_);
    mutable Mutex field_map_mutex_;
    bool field_map_initialized_ GUARDED_BY(field_map_mutex_) = false;

    // Protocol state
    std::string protocol_version_;
    std::atomic<bool> protocol_validated_{false};
    std::atomic<bool> lookup_protocol_validated_{false};

    // SBF (Symbol By Filter) search state - thread-safe
    Mutex callback_mutex_;  // Protects callbacks from being cleared during invocation
    SymbolListCallback symbol_search_callback_ GUARDED_BY(callback_mutex_);
    SymbolListCallback incremental_save_callback_ GUARDED_BY(callback_mutex_);

    // Symbol search data - thread-safe (accessed from background thread)
    Mutex symbol_search_mutex_;
    std::vector<SymbolInfo> symbol_search_results_ GUARDED_BY(symbol_search_mutex_);
    std::size_t last_incremental_save_count_ GUARDED_BY(symbol_search_mutex_){0};
    std::atomic<bool> searching_symbols_{false};
    std::atomic<std::size_t> symbols_received_count_{0};  // Lock-free for UI

    // Background thread for maximum speed symbol search
    std::thread symbol_search_thread_;
    void symbol_search_worker();  // Background thread function
};

} // namespace uscan
