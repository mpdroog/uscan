#include "db_worker.hpp"
#include "symbol_db.hpp"
#include <utility>

namespace uscan {

DBWorker::DBWorker(SymbolDB& db)
    : db_(db)
    , worker_([this]() { worker_loop(); }) {
}

DBWorker::~DBWorker() {
    stop();
}

void DBWorker::enqueue_save(const std::vector<SymbolInfo>& symbols, SaveCallback callback) {
    Request req;
    req.type = RequestType::SAVE_SYMBOLS;
    req.symbols = symbols;
    req.save_callback = std::move(callback);

    {
        MutexLock lock(mutex_);
        requests_.push_back(std::move(req));
    }

    cond_.notify_one();
}

void DBWorker::enqueue_load(LoadCallback callback) {
    Request req;
    req.type = RequestType::LOAD_SYMBOLS;
    req.load_callback = std::move(callback);

    {
        MutexLock lock(mutex_);
        requests_.push_back(std::move(req));
    }

    cond_.notify_one();
}

void DBWorker::enqueue_flush(std::function<void()> callback) {
    Request req;
    req.type = RequestType::FLUSH_UPDATES;
    req.flush_callback = std::move(callback);

    {
        MutexLock lock(mutex_);
        requests_.push_back(std::move(req));
    }

    cond_.notify_one();
}

void DBWorker::stop() {
    running_ = false;

    {
        MutexLock lock(mutex_);
        // Wake up worker thread
    }
    cond_.notify_one();

    if (worker_.joinable()) {
        worker_.join();
    }
}

void DBWorker::worker_loop() {
    while (true) {  // Loop until explicitly broken (not just running_ check)
        Request req;
        bool has_request = false;

        {
            std::unique_lock<std::mutex> lock(mutex_.native());
            // Wait for work or shutdown signal
            while (requests_.empty() && running_) {
                cond_.wait(lock);
            }

            // Only exit when stopped AND queue is empty (drain all pending work)
            if (!running_ && requests_.empty()) {
                log_verbose("DBWorker: Exiting (stopped and queue empty)");
                break;
            }

            if (!requests_.empty()) {
                // Get next request
                req = std::move(requests_.front());
                requests_.erase(requests_.begin());
                has_request = true;
            }
        }  // Release lock before processing

        // Process request outside lock (copy-on-callback pattern)
        if (has_request) {
            process_request(req);
        }
    }
}

void DBWorker::process_request(const Request& req) {
    switch (req.type) {
    case RequestType::SAVE_SYMBOLS: {
        log_verbose("DBWorker: Processing SAVE_SYMBOLS request (%zu symbols)", req.symbols.size());
        auto result = db_.save_symbols(req.symbols);
        if (req.save_callback) {
            if (result.ok()) {
                req.save_callback(true, "");
            } else {
                req.save_callback(false, result.error);
            }
        }
        break;
    }

    case RequestType::LOAD_SYMBOLS: {
        log_verbose("DBWorker: Processing LOAD_SYMBOLS request");
        auto result = db_.load_symbols();
        if (req.load_callback) {
            if (result.ok()) {
                req.load_callback(result.get());
            } else {
                req.load_callback({});  // Empty vector on error
            }
        }
        break;
    }

    case RequestType::FLUSH_UPDATES: {
        log_verbose("DBWorker: Processing FLUSH_UPDATES request");
        (void)db_.flush_price_updates();
        if (req.flush_callback) {
            req.flush_callback();
        }
        break;
    }
    }
}

} // namespace uscan
