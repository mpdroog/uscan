#include "test_framework.hpp"
#include "symbol_db.hpp"
#include <cstdio>
#include <unistd.h>

using namespace uscan;

namespace {

// Helper to create a temporary test database
class TestDB {
public:
    TestDB() {
        // Create unique temp file
        char tmpl[] = "/tmp/uscan_test_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) {
            close(fd);
            db_path_ = tmpl;
        }
    }

    ~TestDB() {
        // Clean up temp file
        if (!db_path_.empty()) {
            std::remove(db_path_.c_str());
            // Also remove WAL and SHM files
            std::remove((db_path_ + "-wal").c_str());
            std::remove((db_path_ + "-shm").c_str());
        }
    }

    const std::string& path() const { return db_path_; }

private:
    std::string db_path_;
};

} // anonymous namespace

TEST(SymbolDB_open_creates_tables) {
    TestDB tmp;
    SymbolDB db(tmp.path());

    auto result = db.open();
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(db.is_open());
    ASSERT_EQ(db.symbol_count(), 0u);

    db.close();
    ASSERT_FALSE(db.is_open());
    return true;
}

TEST(SymbolDB_save_and_load_symbols) {
    TestDB tmp;
    SymbolDB db(tmp.path());

    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    // Save some symbols
    std::vector<SymbolInfo> symbols = {
        SymbolInfo("AAPL", "Apple Inc.", "NASDAQ", "Technology"),
        SymbolInfo("GOOGL", "Alphabet Inc.", "NASDAQ", "Technology"),
        SymbolInfo("MSFT", "Microsoft Corp.", "NASDAQ", "Technology")
    };

    auto save_result = db.save_symbols(symbols);
    ASSERT_TRUE(save_result.ok());
    ASSERT_EQ(db.symbol_count(), 3u);

    // Load symbols back
    auto load_result = db.load_symbols();
    ASSERT_TRUE(load_result.ok());

    const auto& loaded = load_result.get();
    ASSERT_EQ(loaded.size(), 3u);

    // Symbols should be sorted alphabetically
    ASSERT_STR_EQ(loaded[0].symbol.c_str(), "AAPL");
    ASSERT_STR_EQ(loaded[1].symbol.c_str(), "GOOGL");
    ASSERT_STR_EQ(loaded[2].symbol.c_str(), "MSFT");

    return true;
}

TEST(SymbolDB_update_symbol_price) {
    TestDB tmp;
    SymbolDB db(tmp.path());

    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    // Save a symbol
    std::vector<SymbolInfo> symbols = {
        SymbolInfo("AAPL", "Apple Inc.", "NASDAQ", "Technology")
    };
    auto save_result = db.save_symbols(symbols);
    ASSERT_TRUE(save_result.ok());

    // Update its price
    auto update_result = db.update_symbol_price("AAPL", 150.50);
    ASSERT_TRUE(update_result.ok());

    // Get symbols in price range
    auto range_result = db.get_symbols_in_range(100.0, 200.0);
    ASSERT_TRUE(range_result.ok());
    ASSERT_EQ(range_result.get().size(), 1u);

    // Should not be in a different range
    auto empty_result = db.get_symbols_in_range(200.0, 300.0);
    ASSERT_TRUE(empty_result.ok());
    ASSERT_EQ(empty_result.get().size(), 0u);

    return true;
}

TEST(SymbolDB_get_symbols_in_range) {
    TestDB tmp;
    SymbolDB db(tmp.path());

    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    // Save symbols and set prices
    std::vector<SymbolInfo> symbols = {
        SymbolInfo("LOW", "", "", ""),
        SymbolInfo("MID", "", "", ""),
        SymbolInfo("HIGH", "", "", "")
    };
    auto save_result = db.save_symbols(symbols);
    ASSERT_TRUE(save_result.ok());

    // Update prices
    ASSERT_TRUE(db.update_symbol_price("LOW", 5.0).ok());
    ASSERT_TRUE(db.update_symbol_price("MID", 15.0).ok());
    ASSERT_TRUE(db.update_symbol_price("HIGH", 50.0).ok());

    // Get symbols in $1-$20 range
    auto range_result = db.get_symbols_in_range(1.0, 20.0);
    ASSERT_TRUE(range_result.ok());
    ASSERT_EQ(range_result.get().size(), 2u);  // LOW and MID

    // Get symbols in $1-$100 range
    auto wide_result = db.get_symbols_in_range(1.0, 100.0);
    ASSERT_TRUE(wide_result.ok());
    ASSERT_EQ(wide_result.get().size(), 3u);  // All three

    return true;
}

TEST(SymbolDB_clear) {
    TestDB tmp;
    SymbolDB db(tmp.path());

    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    // Save some symbols
    std::vector<SymbolInfo> symbols = {
        SymbolInfo("AAPL"),
        SymbolInfo("GOOGL")
    };
    auto save_result = db.save_symbols(symbols);
    ASSERT_TRUE(save_result.ok());
    ASSERT_EQ(db.symbol_count(), 2u);

    // Clear
    auto clear_result = db.clear();
    ASSERT_TRUE(clear_result.ok());
    ASSERT_EQ(db.symbol_count(), 0u);
    ASSERT_FALSE(db.has_symbols());

    return true;
}

TEST(SymbolDB_has_symbols) {
    TestDB tmp;
    SymbolDB db(tmp.path());

    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    ASSERT_FALSE(db.has_symbols());

    std::vector<SymbolInfo> symbols = {SymbolInfo("AAPL")};
    ASSERT_TRUE(db.save_symbols(symbols).ok());

    ASSERT_TRUE(db.has_symbols());

    return true;
}

TEST(SymbolDB_last_update_timestamp) {
    TestDB tmp;
    SymbolDB db(tmp.path());

    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    // Save symbols (this sets the timestamp)
    std::vector<SymbolInfo> symbols = {SymbolInfo("AAPL")};
    auto save_result = db.save_symbols(symbols);
    ASSERT_TRUE(save_result.ok());

    // Get timestamp
    auto ts_result = db.last_update_timestamp();
    ASSERT_TRUE(ts_result.ok());

    // Should be a recent timestamp (within last 10 seconds)
    const auto now = static_cast<int64_t>(std::time(nullptr));
    ASSERT_TRUE(ts_result.get() > 0);
    ASSERT_TRUE(now - ts_result.get() < 10);

    return true;
}

TEST(SymbolDB_operations_on_closed_db_fail) {
    TestDB tmp;
    SymbolDB db(tmp.path());

    // Don't open the database

    std::vector<SymbolInfo> symbols = {SymbolInfo("AAPL")};
    auto save_result = db.save_symbols(symbols);
    ASSERT_FALSE(save_result.ok());

    auto load_result = db.load_symbols();
    ASSERT_FALSE(load_result.ok());

    auto clear_result = db.clear();
    ASSERT_FALSE(clear_result.ok());

    return true;
}

TEST(SymbolDB_upsert_on_duplicate_symbol) {
    TestDB tmp;
    SymbolDB db(tmp.path());

    auto open_result = db.open();
    ASSERT_TRUE(open_result.ok());

    // Save initial symbol
    std::vector<SymbolInfo> symbols1 = {
        SymbolInfo("AAPL", "Apple", "NASDAQ", "Tech")
    };
    auto save_result1 = db.save_symbols(symbols1);
    ASSERT_TRUE(save_result1.ok());
    ASSERT_EQ(db.symbol_count(), 1u);

    // Save same symbol with different data
    std::vector<SymbolInfo> symbols2 = {
        SymbolInfo("AAPL", "Apple Inc.", "NASDAQ", "Technology")
    };
    auto save_result2 = db.save_symbols(symbols2);
    ASSERT_TRUE(save_result2.ok());

    // Should still only have 1 symbol (upsert)
    ASSERT_EQ(db.symbol_count(), 1u);

    // Load and check updated data
    auto load_result = db.load_symbols();
    ASSERT_TRUE(load_result.ok());
    ASSERT_EQ(load_result.get().size(), 1u);
    ASSERT_STR_EQ(load_result.get()[0].name.c_str(), "Apple Inc.");

    return true;
}
