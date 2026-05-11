#include "symbol_db.hpp"
#include <ctime>

namespace uscan {

namespace {

constexpr const char* CREATE_TABLES_SQL = R"(
CREATE TABLE IF NOT EXISTS symbols (
    symbol TEXT PRIMARY KEY NOT NULL,
    name TEXT,
    exchange TEXT,
    sector TEXT,
    last_price REAL DEFAULT 0,
    updated_at INTEGER DEFAULT 0
);

CREATE TABLE IF NOT EXISTS metadata (
    key TEXT PRIMARY KEY NOT NULL,
    value TEXT
);

CREATE INDEX IF NOT EXISTS idx_symbols_price ON symbols(last_price);
)";

} // anonymous namespace

SymbolDB::SymbolDB(const std::string& db_path)
    : db_path_(db_path) {}

SymbolDB::~SymbolDB() {
    close();
}

Result<void> SymbolDB::open() {
    if (db_ != nullptr) {
        return Result<void>::ok_result();
    }

    log_verbose("SQLite: Opening database: %s", db_path_.c_str());

    const int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        const std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return Result<void>::failure("Failed to open database: " + err);
    }

    // Enable WAL mode for better concurrency
    auto wal_result = exec("PRAGMA journal_mode=WAL");
    if (wal_result.failed()) {
        return wal_result;
    }

    // Create tables if they don't exist
    return create_tables();
}

void SymbolDB::close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SymbolDB::is_open() const noexcept {
    return db_ != nullptr;
}

Result<void> SymbolDB::save_symbols(const std::vector<SymbolInfo>& symbols) {
    if (db_ == nullptr) {
        return Result<void>::failure("Database not open");
    }

    log_verbose("SQLite: Saving %zu symbols in transaction", symbols.size());

    // Begin transaction
    auto begin_result = exec("BEGIN TRANSACTION");
    if (begin_result.failed()) {
        return begin_result;
    }

    // Prepare insert statement
    const char* insert_sql = "INSERT OR REPLACE INTO symbols (symbol, name, exchange, sector, updated_at) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        auto rollback_result = exec("ROLLBACK");
        if (rollback_result.failed()) {
            log_error("Rollback failed: %s", rollback_result.error.c_str());
        }
        return Result<void>::failure(std::string("Prepare failed: ") + sqlite3_errmsg(db_));
    }

    const auto now = static_cast<int64_t>(std::time(nullptr));

    for (const auto& sym : symbols) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, sym.symbol.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, sym.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, sym.exchange.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, sym.sector.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, now);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            auto rollback_result = exec("ROLLBACK");
            if (rollback_result.failed()) {
                log_error("Rollback failed: %s", rollback_result.error.c_str());
            }
            return Result<void>::failure(std::string("Insert failed: ") + sqlite3_errmsg(db_));
        }
    }

    sqlite3_finalize(stmt);

    // Update metadata
    const char* meta_sql = "INSERT OR REPLACE INTO metadata (key, value) VALUES ('last_symbol_update', ?)";
    rc = sqlite3_prepare_v2(db_, meta_sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            log_warn("Failed to update metadata timestamp: %s", sqlite3_errmsg(db_));
            // Non-fatal: continue with commit
        }
    }

    return exec("COMMIT");
}

Result<std::vector<SymbolInfo>> SymbolDB::load_symbols() {
    if (db_ == nullptr) {
        return Result<std::vector<SymbolInfo>>::failure("Database not open");
    }

    log_verbose("SQLite: Loading symbols from database");

    std::vector<SymbolInfo> symbols;

    const char* sql = "SELECT symbol, name, exchange, sector FROM symbols ORDER BY symbol";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<std::vector<SymbolInfo>>::failure(std::string("Prepare failed: ") + sqlite3_errmsg(db_));
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SymbolInfo info;

        const char* sym = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* exch = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* sect = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

        if (sym) info.symbol = sym;
        if (name) info.name = name;
        if (exch) info.exchange = exch;
        if (sect) info.sector = sect;

        symbols.push_back(std::move(info));
    }

    sqlite3_finalize(stmt);

    return Result<std::vector<SymbolInfo>>::success(std::move(symbols));
}

Result<void> SymbolDB::update_symbol_price(const std::string& symbol, double price) {
    if (db_ == nullptr) {
        return Result<void>::failure("Database not open");
    }

    const char* sql = "UPDATE symbols SET last_price = ?, updated_at = ? WHERE symbol = ?";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<void>::failure(std::string("Prepare failed: ") + sqlite3_errmsg(db_));
    }

    const auto now = static_cast<int64_t>(std::time(nullptr));

    sqlite3_bind_double(stmt, 1, price);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_text(stmt, 3, symbol.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return Result<void>::failure(std::string("Update failed: ") + sqlite3_errmsg(db_));
    }

    return Result<void>::ok_result();
}

Result<std::vector<SymbolInfo>> SymbolDB::get_symbols_in_range(double min_price, double max_price) {
    if (db_ == nullptr) {
        return Result<std::vector<SymbolInfo>>::failure("Database not open");
    }

    std::vector<SymbolInfo> symbols;

    const char* sql = "SELECT symbol, name, exchange, sector FROM symbols "
                      "WHERE last_price >= ? AND last_price <= ? ORDER BY symbol";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Result<std::vector<SymbolInfo>>::failure(std::string("Prepare failed: ") + sqlite3_errmsg(db_));
    }

    sqlite3_bind_double(stmt, 1, min_price);
    sqlite3_bind_double(stmt, 2, max_price);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SymbolInfo info;

        const char* sym = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* exch = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* sect = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

        if (sym) info.symbol = sym;
        if (name) info.name = name;
        if (exch) info.exchange = exch;
        if (sect) info.sector = sect;

        symbols.push_back(std::move(info));
    }

    sqlite3_finalize(stmt);

    return Result<std::vector<SymbolInfo>>::success(std::move(symbols));
}

std::size_t SymbolDB::symbol_count() {
    if (db_ == nullptr) return 0;

    const char* sql = "SELECT COUNT(*) FROM symbols";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    std::size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<std::size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return count;
}

bool SymbolDB::has_symbols() {
    return symbol_count() > 0;
}

Result<void> SymbolDB::clear() {
    if (db_ == nullptr) {
        return Result<void>::failure("Database not open");
    }

    return exec("DELETE FROM symbols");
}

Result<int64_t> SymbolDB::last_update_timestamp() {
    if (db_ == nullptr) {
        return Result<int64_t>::failure("Database not open");
    }

    const char* sql = "SELECT value FROM metadata WHERE key = 'last_symbol_update'";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<int64_t>::failure(std::string("Prepare failed: ") + sqlite3_errmsg(db_));
    }

    int64_t timestamp = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        timestamp = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return Result<int64_t>::success(timestamp);
}

Result<void> SymbolDB::create_tables() {
    return exec(CREATE_TABLES_SQL);
}

Result<void> SymbolDB::exec(const char* sql) {
    char* err_msg = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        const std::string err = err_msg ? err_msg : "Unknown error";
        sqlite3_free(err_msg);
        return Result<void>::failure("SQL error: " + err);
    }

    return Result<void>::ok_result();
}

void SymbolDB::queue_price_update(const std::string& symbol, double price) {
    const auto now = static_cast<int64_t>(std::time(nullptr));

    MutexLock lock(update_mutex_);
    pending_updates_.push_back(PriceUpdate{symbol, price, now});
}

Result<void> SymbolDB::flush_price_updates() {
    // Move pending updates to local vector (minimize lock time)
    std::vector<PriceUpdate> updates;
    {
        MutexLock lock(update_mutex_);
        updates = std::move(pending_updates_);
        pending_updates_.clear();
    }

    // Nothing to flush - success regardless of whether db is open
    if (updates.empty()) {
        return Result<void>::ok_result();
    }

    // Only need database to be open if we actually have updates to write
    if (db_ == nullptr) {
        return Result<void>::failure("Database not open");
    }

    log_verbose("SQLite: Flushing %zu batched price updates", updates.size());

    // Batch update in single transaction
    auto result = exec("BEGIN TRANSACTION");
    if (result.failed()) {
        return result;
    }

    const char* sql = "UPDATE symbols SET last_price = ?, updated_at = ? WHERE symbol = ?";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        (void)exec("ROLLBACK");
        return Result<void>::failure(std::string("Prepare failed: ") + sqlite3_errmsg(db_));
    }

    for (const auto& update : updates) {
        sqlite3_bind_double(stmt, 1, update.price);
        sqlite3_bind_int64(stmt, 2, update.timestamp);
        sqlite3_bind_text(stmt, 3, update.symbol.c_str(), -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            (void)exec("ROLLBACK");
            return Result<void>::failure(std::string("Update failed: ") + sqlite3_errmsg(db_));
        }

        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    return exec("COMMIT");
}

} // namespace uscan
