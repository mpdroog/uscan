#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <optional>
#include <cstdio>
#include <cstdarg>

namespace uscan {

// Simple logging to stderr
inline void log_error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void log_error(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[ERROR] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

inline void log_warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void log_warn(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[WARN] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

// Global verbose flag (set from Config)
namespace detail {
inline bool g_verbose = false;
}

inline void set_verbose(bool enabled) {
    detail::g_verbose = enabled;
}

inline void log_verbose(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void log_verbose(const char* fmt, ...) {
    if (!detail::g_verbose) return;
    std::va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[VERBOSE] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

// Compiler annotations
#define USCAN_NODISCARD [[nodiscard]]
#define USCAN_MAYBE_UNUSED [[maybe_unused]]
#define USCAN_FALLTHROUGH [[fallthrough]]

// Delete copy/move macros
#define USCAN_NON_COPYABLE(Class) \
    Class(const Class&) = delete; \
    Class& operator=(const Class&) = delete

#define USCAN_NON_MOVABLE(Class) \
    Class(Class&&) = delete; \
    Class& operator=(Class&&) = delete

#define USCAN_NON_COPYABLE_NON_MOVABLE(Class) \
    USCAN_NON_COPYABLE(Class); \
    USCAN_NON_MOVABLE(Class)

// Scanner configuration
struct Config {
    // Network
    std::string iqfeed_host{"127.0.0.1"};
    uint16_t iqfeed_l1_port{5009};
    uint16_t iqfeed_hist_port{9100};
    uint16_t iqfeed_http_port{8080};

    // Filtering
    double min_price{1.0};
    double max_price{20.0};
    double fallback_max_price{100.0};
    double min_gap_percent{5.0};
    int64_t min_premarket_volume{50000};
    std::size_t max_results{20};

    // Gap direction: true = up only, false = both
    bool gap_up_only{true};

    // Refresh interval in milliseconds
    int refresh_interval_ms{3000};

    // Database
    std::string db_path{"uscan.db"};

    // Logging
    bool verbose{false};

    Config() = default;
    USCAN_NON_COPYABLE(Config);
    Config(Config&&) noexcept = default;
    Config& operator=(Config&&) noexcept = default;
};

// Symbol info from IQFeed search
struct SymbolInfo {
    std::string symbol;
    std::string name;
    std::string exchange;
    std::string sector;

    SymbolInfo() = default;

    explicit SymbolInfo(std::string sym)
        : symbol(std::move(sym)) {}

    SymbolInfo(std::string sym, std::string nm, std::string exch, std::string sec)
        : symbol(std::move(sym))
        , name(std::move(nm))
        , exchange(std::move(exch))
        , sector(std::move(sec)) {}
};

// Quote data from Level 1 feed
struct Quote {
    std::string symbol;

    // Prices
    double last_price{0.0};
    double bid{0.0};
    double ask{0.0};
    double prev_close{0.0};
    double open{0.0};
    double high{0.0};
    double low{0.0};

    // Extended hours (pre-market)
    double extended_price{0.0};
    int64_t extended_volume{0};

    // Volume
    int64_t volume{0};
    int64_t avg_volume{0};

    // 52-week range
    double high_52wk{0.0};
    double low_52wk{0.0};

    // Float (shares outstanding)
    int64_t float_shares{0};

    // Sector info
    std::string sector;
    std::string name;

    // Timestamps
    std::chrono::system_clock::time_point last_update;

    // Calculated fields
    USCAN_NODISCARD double gap_percent() const noexcept {
        if (prev_close <= 0.0) return 0.0;
        const double current = (extended_price > 0.0) ? extended_price : last_price;
        return ((current - prev_close) / prev_close) * 100.0;
    }

    USCAN_NODISCARD double gap_dollars() const noexcept {
        const double current = (extended_price > 0.0) ? extended_price : last_price;
        return current - prev_close;
    }

    USCAN_NODISCARD double current_price() const noexcept {
        return (extended_price > 0.0) ? extended_price : last_price;
    }

    USCAN_NODISCARD int64_t premarket_volume() const noexcept {
        return extended_volume;
    }

    Quote() = default;
    explicit Quote(std::string sym) : symbol(std::move(sym)) {}
};

// Gapper entry for display
struct Gapper {
    std::string symbol;
    std::string name;
    std::string sector;

    double price{0.0};
    double prev_close{0.0};
    double gap_percent{0.0};
    double gap_dollars{0.0};

    int64_t premarket_volume{0};
    int64_t avg_volume{0};
    int64_t float_shares{0};

    double high_52wk{0.0};
    double low_52wk{0.0};

    Gapper() = default;

    explicit Gapper(const Quote& q)
        : symbol(q.symbol)
        , name(q.name)
        , sector(q.sector)
        , price(q.current_price())
        , prev_close(q.prev_close)
        , gap_percent(q.gap_percent())
        , gap_dollars(q.gap_dollars())
        , premarket_volume(q.premarket_volume())
        , avg_volume(q.avg_volume)
        , float_shares(q.float_shares)
        , high_52wk(q.high_52wk)
        , low_52wk(q.low_52wk) {}

    // Sort by gap percent descending
    USCAN_NODISCARD bool operator<(const Gapper& other) const noexcept {
        return gap_percent > other.gap_percent;
    }
};

// Regional quote from Level 1 regional updates (R messages)
struct RegionalQuote {
    std::string symbol;
    std::string exchange;
    double bid{0.0};
    double ask{0.0};
    double last{0.0};
    int64_t bid_size{0};
    int64_t ask_size{0};

    RegionalQuote() = default;
};

// Trade correction message (C messages)
struct TradeCorrection {
    std::string symbol;
    char correction_type{'D'};  // D=Delete, A=Insert, X=TradeDelete, I=TradeInsert
    double price{0.0};
    int64_t size{0};
    std::string trade_id;

    TradeCorrection() = default;
};

// News headline message (N messages)
struct NewsHeadline {
    std::string headline_id;
    std::string source;
    std::string symbols;  // Colon-separated symbol list
    std::string headline;
    std::string timestamp;

    NewsHeadline() = default;
};

// Connection state
enum class ConnectionState : uint8_t {
    Disconnected = 0,
    Connecting,
    Connected,
    Error
};

USCAN_NODISCARD inline const char* to_string(ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::Disconnected: return "Disconnected";
        case ConnectionState::Connecting:   return "Connecting";
        case ConnectionState::Connected:    return "Connected";
        case ConnectionState::Error:        return "Error";
    }
    return "Unknown";
}

// Scanner state
enum class ScannerState : uint8_t {
    Idle = 0,
    Connecting,
    LoadingSymbols,
    Subscribing,
    Scanning,
    Error
};

// Progress tracking for multi-step operations
struct Progress {
    std::string current_step;   // Description of current step
    int step_number{0};          // Current step number (1-based)
    int total_steps{0};          // Total number of steps
    int step_progress{-1};       // Progress within step (0-100), -1 = indeterminate

    Progress() = default;
    Progress(std::string step, int num, int total, int progress = -1)
        : current_step(std::move(step))
        , step_number(num)
        , total_steps(total)
        , step_progress(progress) {}
};

USCAN_NODISCARD inline const char* to_string(ScannerState state) noexcept {
    switch (state) {
        case ScannerState::Idle:           return "Idle";
        case ScannerState::Connecting:     return "Connecting";
        case ScannerState::LoadingSymbols: return "Loading Symbols";
        case ScannerState::Subscribing:    return "Subscribing";
        case ScannerState::Scanning:       return "Scanning";
        case ScannerState::Error:          return "Error";
    }
    return "Unknown";
}

// Result type for operations that can fail
template<typename T>
struct Result {
    std::optional<T> value;
    std::string error;

    USCAN_NODISCARD bool ok() const noexcept { return value.has_value(); }
    USCAN_NODISCARD bool failed() const noexcept { return !value.has_value(); }

    USCAN_NODISCARD const T& get() const { return value.value(); }
    USCAN_NODISCARD T& get() { return value.value(); }

    static Result success(T val) {
        Result r;
        r.value = std::move(val);
        return r;
    }

    static Result failure(std::string err) {
        Result r;
        r.error = std::move(err);
        return r;
    }
};

// Specialization for void
template<>
struct Result<void> {
    bool success{false};
    std::string error;

    USCAN_NODISCARD bool ok() const noexcept { return success; }
    USCAN_NODISCARD bool failed() const noexcept { return !success; }

    static Result ok_result() {
        Result r;
        r.success = true;
        return r;
    }

    static Result failure(std::string err) {
        Result r;
        r.success = false;
        r.error = std::move(err);
        return r;
    }
};

} // namespace uscan
