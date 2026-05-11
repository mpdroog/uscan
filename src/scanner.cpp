#include "scanner.hpp"
#include <algorithm>
#include <ctime>

namespace uscan {

namespace {

// Maximum symbols to subscribe at once (to avoid overwhelming the feed)
constexpr std::size_t SUBSCRIBE_BATCH_SIZE = 100;

} // anonymous namespace

Scanner::Scanner(Config config)
    : config_(std::move(config))
    , last_update_(std::chrono::steady_clock::now())
    , last_db_flush_(std::chrono::steady_clock::now()) {}

Scanner::~Scanner() {
    shutdown();
}

Result<void> Scanner::initialize() {
    // Step 1: Opening database
    state_ = ScannerState::Connecting;
    progress_ = Progress("Opening database", 1, 4);

    db_ = std::make_unique<SymbolDB>(config_.db_path);
    auto db_result = db_->open();
    if (db_result.failed()) {
        last_error_ = db_result.error;
        state_ = ScannerState::Error;
        progress_ = Progress();
        return db_result;
    }

    // Create DB worker thread
    db_worker_ = std::make_unique<DBWorker>(*db_);

    // Step 2: Connecting to IQFeed
    progress_ = Progress("Connecting to IQFeed", 2, 4);

    client_ = std::make_unique<IQFeedClient>(config_);

    // Set quote callback
    client_->set_quote_callback([this](const Quote& q) {
        process_quote(q);
    });

    auto connect_result = client_->connect();
    if (connect_result.failed()) {
        last_error_ = connect_result.error;
        state_ = ScannerState::Error;
        progress_ = Progress();
        return connect_result;
    }

    // Step 3: Load or fetch symbols (progress set in that function)
    load_or_fetch_symbols();

    return Result<void>::ok_result();
}

void Scanner::shutdown() {
    // =========================================================================
    // SHUTDOWN ORDERING INVARIANT - DO NOT REORDER
    // =========================================================================
    // The shutdown sequence MUST follow this order to prevent use-after-free:
    //
    // 1. Disconnect IQFeed client FIRST
    //    - Stops background symbol search thread
    //    - Clears all callbacks (prevents callbacks from accessing Scanner)
    //    - Closes sockets (makes recv() fail immediately in background thread)
    //    - Joins the background thread (blocks until it exits)
    //
    // 2. Stop DB worker SECOND
    //    - At this point, no callbacks can enqueue new work
    //    - Drains the work queue (processes pending saves)
    //    - Joins the DB worker thread
    //
    // 3. Flush and close database LAST
    //    - Safe because no threads are accessing the DB
    //
    // VIOLATION: If DB is closed before client disconnect, callbacks from
    // the background thread may try to save to a closed database.
    // =========================================================================

    // Step 1: Disconnect client to stop background threads and clear callbacks
    if (client_) {
        client_->unwatch_all();
        client_->disconnect();  // Joins background thread, clears callbacks
    }

    // Step 2: Stop DB worker (no more callbacks can enqueue saves)
    if (db_worker_) {
        db_worker_->stop();  // Drains queue and joins thread
        db_worker_.reset();
    }

    // Step 3: Flush any remaining price updates and close database
    if (db_) {
        auto flush_result = db_->flush_price_updates();
        if (flush_result.failed()) {
            log_warn("Failed to flush price updates during shutdown: %s", flush_result.error.c_str());
        }
        db_->close();
    }

    state_ = ScannerState::Idle;
}

void Scanner::update() {
    if (state_ == ScannerState::Error || state_ == ScannerState::Idle) {
        return;
    }

    // Process incoming data
    if (client_) {
        client_->process();

        // Check if client encountered an error
        if (client_->state() == ConnectionState::Error) {
            last_error_ = client_->last_error();
            state_ = ScannerState::Error;
            progress_ = Progress();
            log_warn("Scanner: Client error detected: %s", last_error_.c_str());
            return;
        }

        // Update progress if fetching symbols
        if (state_ == ScannerState::LoadingSymbols && client_->is_searching_symbols()) {
            const auto symbol_count = client_->symbols_received_count();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Fetching symbols from IQFeed (%zu received)", symbol_count);
            progress_ = Progress(buf, 3, 4);
        }
    }

    // Continue subscribing if not done
    if (state_ == ScannerState::Subscribing) {
        subscribe_to_symbols();
    }

    const auto now = std::chrono::steady_clock::now();

    // Update gappers periodically
    const auto gapper_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_);
    if (gapper_elapsed.count() >= config_.refresh_interval_ms) {
        update_gappers();
        last_update_ = now;
    }

    // Flush DB price updates every 5 seconds
    const auto db_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_db_flush_);
    if (db_elapsed.count() >= 5000 && db_worker_) {
        log_verbose("Scanner: Enqueuing periodic DB flush (every 5 seconds)");
        db_worker_->enqueue_flush();
        last_db_flush_ = now;
    }
}

void Scanner::refresh_symbols() {
    state_ = ScannerState::LoadingSymbols;

    // Clear existing subscriptions
    if (client_) {
        client_->unwatch_all();
    }

    // Clear database
    if (db_) {
        auto clear_result = db_->clear();
        if (clear_result.failed()) {
            log_warn("Failed to clear symbol database: %s", clear_result.error.c_str());
        }
    }

    symbols_.clear();
    gappers_.clear();
    subscribe_index_ = 0;
    using_fallback_ = false;

    // Fetch fresh symbols
    load_or_fetch_symbols();
}

std::vector<Gapper> Scanner::get_gappers() const {
    std::lock_guard<std::mutex> lock(gappers_mutex_);
    return gappers_;
}

ScannerState Scanner::state() const noexcept {
    return state_;
}

ConnectionState Scanner::connection_state() const noexcept {
    return client_ ? client_->state() : ConnectionState::Disconnected;
}

const std::string& Scanner::last_error() const noexcept {
    return last_error_;
}

Progress Scanner::progress() const noexcept {
    return progress_;
}

std::size_t Scanner::symbols_watched() const noexcept {
    return client_ ? client_->watched_count() : 0;
}

std::size_t Scanner::symbols_in_db() const noexcept {
    return db_ ? db_->symbol_count() : 0;
}

std::size_t Scanner::messages_received() const noexcept {
    return client_ ? client_->message_count() : 0;
}

bool Scanner::using_fallback_range() const noexcept {
    return using_fallback_;
}

const Config& Scanner::config() const noexcept {
    return config_;
}

Config& Scanner::config() noexcept {
    return config_;
}

void Scanner::load_or_fetch_symbols() {
    // Step 3: Loading or fetching symbols
    state_ = ScannerState::LoadingSymbols;

    // Check if we have cached symbols (no expiration - they stay forever)
    if (db_->has_symbols()) {
        auto ts_result = db_->last_update_timestamp();
        if (ts_result.ok()) {
            const auto now = static_cast<int64_t>(std::time(nullptr));
            const auto age = now - ts_result.get();

            // Load cached symbols asynchronously (no age check - use forever)
            log_verbose("Scanner: Loading symbols from database cache (age: %lld seconds)", static_cast<long long>(age));
            progress_ = Progress("Loading symbols from cache", 3, 4);
            symbols_loading_ = true;
            db_worker_->enqueue_load([this](const std::vector<SymbolInfo>& symbols) {
                on_symbols_loaded(symbols);
            });
            return;
        }
    }

    // Fetch fresh symbols from IQFeed using TCP SBF command (non-blocking)
    log_verbose("Scanner: Fetching fresh symbols from IQFeed using SBF command");
    progress_ = Progress("Fetching symbols from IQFeed", 3, 4);

    // Set incremental save callback (saves every 500 symbols)
    client_->set_incremental_save_callback([this](const std::vector<SymbolInfo>& batch) {
        // Save batch to database asynchronously (non-blocking)
        db_worker_->enqueue_save(batch, [](bool success, const std::string& error) {
            if (!success) {
                log_warn("Incremental symbol save failed: %s", error.c_str());
            }
        });
    });

    // Set callback for when SBF completes
    client_->set_symbol_search_callback([this](const std::vector<SymbolInfo>& symbols) {
        // Callback is called from main thread during process()
        symbols_ = symbols;

        if (symbols_.empty()) {
            last_error_ = "No symbols found";
            state_ = ScannerState::Error;
        } else {
            // Symbols already saved incrementally, just move to subscribing
            state_ = ScannerState::Subscribing;
        }
    });

    // Send SBF command (returns immediately)
    auto result = client_->request_symbol_search();
    if (result.failed()) {
        last_error_ = "Failed to request symbol search: " + result.error;
        state_ = ScannerState::Error;
    }
}

void Scanner::subscribe_to_symbols() {
    if (symbols_.empty()) {
        state_ = ScannerState::Scanning;
        progress_ = Progress();  // Clear progress when scanning
        return;
    }

    // Subscribe in batches to avoid overwhelming the feed
    const std::size_t start = subscribe_index_;
    const std::size_t end = std::min(start + SUBSCRIBE_BATCH_SIZE, symbols_.size());

    // Step 4: Subscribing to symbols (with progress)
    const int percent = static_cast<int>((static_cast<double>(end) / static_cast<double>(symbols_.size())) * 100.0);
    progress_ = Progress("Subscribing to symbols", 4, 4, percent);

    for (std::size_t i = start; i < end; ++i) {
        auto watch_result = client_->watch(symbols_[i].symbol);
        if (watch_result.failed()) {
            log_warn("Failed to watch %s: %s", symbols_[i].symbol.c_str(), watch_result.error.c_str());
        }
    }

    subscribe_index_ = end;

    if (subscribe_index_ >= symbols_.size()) {
        state_ = ScannerState::Scanning;
        progress_ = Progress();  // Clear progress when done
    }
}

void Scanner::process_quote(const Quote& quote) {
    // Queue price update for batched flushing (non-blocking)
    if (db_ && quote.current_price() > 0.0) {
        db_->queue_price_update(quote.symbol, quote.current_price());
    }
}

void Scanner::update_gappers() {
    if (!client_) return;

    const auto& quotes = client_->quotes();
    std::vector<Gapper> new_gappers;
    new_gappers.reserve(config_.max_results);

    const double min_price = config_.min_price;
    const double max_price = using_fallback_ ? config_.fallback_max_price : config_.max_price;

    for (const auto& [symbol, quote] : quotes) {
        // Skip if no previous close
        if (quote.prev_close <= 0.0) continue;

        const double price = quote.current_price();

        // Price filter
        if (price < min_price || price > max_price) continue;

        // Gap filter
        const double gap_pct = quote.gap_percent();

        if (config_.gap_up_only && gap_pct < config_.min_gap_percent) continue;
        if (!config_.gap_up_only && std::abs(gap_pct) < config_.min_gap_percent) continue;

        // Volume filter
        if (quote.premarket_volume() < config_.min_premarket_volume) continue;

        new_gappers.emplace_back(quote);
    }

    // Sort by gap percent (descending for gap up)
    std::sort(new_gappers.begin(), new_gappers.end());

    // Limit results
    if (new_gappers.size() > config_.max_results) {
        new_gappers.resize(config_.max_results);
    }

    // Check if we need to widen the price range
    if (new_gappers.empty() && !using_fallback_) {
        using_fallback_ = true;
        // Re-run with wider range
        update_gappers();
        return;
    }

    // Update gappers
    {
        std::lock_guard<std::mutex> lock(gappers_mutex_);
        gappers_ = std::move(new_gappers);
    }
}

void Scanner::on_symbols_loaded(const std::vector<SymbolInfo>& symbols) {
    symbols_ = symbols;
    symbols_loading_ = false;

    if (symbols_.empty()) {
        last_error_ = "No symbols loaded from database";
        state_ = ScannerState::Error;
    } else {
        state_ = ScannerState::Subscribing;
    }
}

void Scanner::on_symbols_saved(bool success, const std::string& error) {
    symbols_saving_ = false;

    if (!success) {
        last_error_ = "Warning: Failed to save symbols: " + error;
        // Non-fatal - continue anyway
    }
}

} // namespace uscan
