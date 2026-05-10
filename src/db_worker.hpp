#pragma once

#include "thread_safety.hpp"
#include "types.hpp"
#include <thread>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <functional>

namespace uscan {

class SymbolDB;

// Database worker thread for async DB operations
// Based on ucharts request-queue + worker thread pattern
class DBWorker final {
public:
    using SaveCallback = std::function<void(bool success, const std::string& error)>;
    using LoadCallback = std::function<void(const std::vector<SymbolInfo>& symbols)>;

    explicit DBWorker(SymbolDB& db);
    ~DBWorker();

    USCAN_NON_COPYABLE_NON_MOVABLE(DBWorker);

    // Enqueue async operations (non-blocking, returns immediately)
    void enqueue_save(const std::vector<SymbolInfo>& symbols, SaveCallback callback) EXCLUDES(mutex_);
    void enqueue_load(LoadCallback callback) EXCLUDES(mutex_);
    void enqueue_flush(std::function<void()> callback = nullptr) EXCLUDES(mutex_);

    // Shutdown worker (drains queue and joins thread)
    void stop();

private:
    enum class RequestType {
        SAVE_SYMBOLS,
        LOAD_SYMBOLS,
        FLUSH_UPDATES
    };

    struct Request {
        RequestType type;
        std::vector<SymbolInfo> symbols;  // For SAVE_SYMBOLS
        SaveCallback save_callback;
        LoadCallback load_callback;
        std::function<void()> flush_callback;
    };

    void worker_loop() NO_THREAD_SAFETY_ANALYSIS;
    void process_request(const Request& req);

    SymbolDB& db_;
    std::thread worker_;
    std::vector<Request> requests_ GUARDED_BY(mutex_);
    Mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> running_{true};
};

} // namespace uscan
