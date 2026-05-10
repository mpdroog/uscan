#include "iqfeed_client.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <thread>
#include <chrono>

namespace uscan {

namespace {

constexpr int RECV_BUFFER_SIZE = 65536;
constexpr int POLL_TIMEOUT_MS = 10;
constexpr int CONNECT_TIMEOUT_MS = 5000;  // 5 second connection timeout
constexpr int SYMBOL_SEARCH_TIMEOUT_MS = 60000;  // 60 second symbol search timeout
constexpr const char* PROTOCOL_CMD = "S,SET PROTOCOL,6.2\r\n";

// Helper: perform connect with timeout using non-blocking socket
USCAN_NODISCARD bool connect_with_timeout(int fd, const sockaddr* addr, socklen_t len, int timeout_ms) noexcept {
    // Set non-blocking for connect
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return false;

    // Start non-blocking connect
    const int ret = ::connect(fd, addr, len);
    if (ret == 0) {
        // Immediate connection (unlikely but possible on localhost)
        return true;
    }

    if (errno != EINPROGRESS) {
        // Real error
        return false;
    }

    // Wait for connection with timeout using poll
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    const int poll_ret = poll(&pfd, 1, timeout_ms);
    if (poll_ret <= 0) {
        // Timeout (0) or error (-1)
        return false;
    }

    // Check if connection succeeded
    int error = 0;
    socklen_t error_len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
        return false;
    }

    if (error != 0) {
        errno = error;
        return false;
    }

    return true;
}

// Level 1 field indices (from IQFeed docs)
// Fundamental message (F,symbol,...)
enum class FundField : std::size_t {
    MsgType = 0,
    Symbol = 1,
    ExchangeID = 2,
    PE = 3,
    AvgVolume = 4,
    High52Week = 5,
    Low52Week = 6,
    // ... many more fields
    CompanyName = 48,
    // ... more fields
    FloatShares = 52,
    // ... more fields
};

// Summary/Update message (P/Q,symbol,...)
enum class QuoteField : std::size_t {
    MsgType = 0,
    Symbol = 1,
    Bid = 3,
    Ask = 4,
    Last = 7,
    TotalVolume = 11,
    High = 14,
    Low = 15,
    Close = 16,  // Previous close
    Open = 28,
    ExtendedTrade = 42,
    ExtendedTradeSize = 43,
};

USCAN_NODISCARD bool set_nonblocking(int fd) noexcept {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

} // anonymous namespace

IQFeedClient::IQFeedClient(const Config& config)
    : config_(config) {
    recv_buffer_.reserve(RECV_BUFFER_SIZE);
}

IQFeedClient::~IQFeedClient() {
    disconnect();
}

Result<void> IQFeedClient::connect() {
    if (state_ == ConnectionState::Connected) {
        return Result<void>::ok_result();
    }

    state_ = ConnectionState::Connecting;

    // Create socket
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        last_error_ = std::string("socket() failed: ") + strerror(errno);
        state_ = ConnectionState::Error;
        return Result<void>::failure(last_error_);
    }

    // Connect to IQFeed L1 port
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.iqfeed_l1_port);

    if (inet_pton(AF_INET, config_.iqfeed_host.c_str(), &addr.sin_addr) <= 0) {
        last_error_ = "Invalid address: " + config_.iqfeed_host;
        close(socket_fd_);
        socket_fd_ = -1;
        state_ = ConnectionState::Error;
        return Result<void>::failure(last_error_);
    }

    // Connect with timeout to avoid hanging on unresponsive servers
    if (!connect_with_timeout(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr), CONNECT_TIMEOUT_MS)) {
        last_error_ = std::string("connect() failed (timeout=") + std::to_string(CONNECT_TIMEOUT_MS) + "ms): " + strerror(errno);
        close(socket_fd_);
        socket_fd_ = -1;
        state_ = ConnectionState::Error;
        return Result<void>::failure(last_error_);
    }

    // Socket is already non-blocking from connect_with_timeout, verify it
    if (!set_nonblocking(socket_fd_)) {
        last_error_ = "Failed to set non-blocking mode";
        close(socket_fd_);
        socket_fd_ = -1;
        state_ = ConnectionState::Error;
        return Result<void>::failure(last_error_);
    }

    // Increase TCP receive buffer for L1 streaming data
    int rcvbuf_size = 256 * 1024;  // 256KB
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
        log_verbose("Warning: Failed to set SO_RCVBUF on L1 port: %s (non-fatal)", strerror(errno));
    } else {
        log_verbose("Set L1 port receive buffer to %d bytes", rcvbuf_size);
    }

    // Set protocol version
    if (!send_command(PROTOCOL_CMD)) {
        disconnect();
        return Result<void>::failure(last_error_);
    }

    // Connect to Lookup port (9100) for symbol search
    log_verbose("Connecting to IQFeed Lookup port %d for symbol search", config_.iqfeed_hist_port);
    lookup_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (lookup_fd_ < 0) {
        last_error_ = std::string("lookup socket() failed: ") + strerror(errno);
        close(socket_fd_);
        socket_fd_ = -1;
        state_ = ConnectionState::Error;
        return Result<void>::failure(last_error_);
    }

    sockaddr_in lookup_addr{};
    lookup_addr.sin_family = AF_INET;
    lookup_addr.sin_port = htons(config_.iqfeed_hist_port);

    if (inet_pton(AF_INET, config_.iqfeed_host.c_str(), &lookup_addr.sin_addr) <= 0) {
        last_error_ = "Invalid address for lookup port: " + config_.iqfeed_host;
        close(socket_fd_);
        close(lookup_fd_);
        socket_fd_ = -1;
        lookup_fd_ = -1;
        state_ = ConnectionState::Error;
        return Result<void>::failure(last_error_);
    }

    // Connect with timeout to avoid hanging on unresponsive servers
    if (!connect_with_timeout(lookup_fd_, reinterpret_cast<sockaddr*>(&lookup_addr), sizeof(lookup_addr), CONNECT_TIMEOUT_MS)) {
        last_error_ = std::string("lookup connect() failed (timeout=") + std::to_string(CONNECT_TIMEOUT_MS) + "ms): " + strerror(errno);
        close(socket_fd_);
        close(lookup_fd_);
        socket_fd_ = -1;
        lookup_fd_ = -1;
        state_ = ConnectionState::Error;
        return Result<void>::failure(last_error_);
    }

    // Socket is already non-blocking from connect_with_timeout, verify it
    if (!set_nonblocking(lookup_fd_)) {
        last_error_ = "Failed to set lookup socket non-blocking";
        close(socket_fd_);
        close(lookup_fd_);
        socket_fd_ = -1;
        lookup_fd_ = -1;
        state_ = ConnectionState::Error;
        return Result<void>::failure(last_error_);
    }

    // Increase TCP receive buffer to 1MB (symbol data comes in FAST!)
    // This prevents kernel buffer overflow when we can't read fast enough
    int lookup_rcvbuf_size = 1024 * 1024;  // 1MB
    if (setsockopt(lookup_fd_, SOL_SOCKET, SO_RCVBUF, &lookup_rcvbuf_size, sizeof(lookup_rcvbuf_size)) < 0) {
        log_verbose("Warning: Failed to set SO_RCVBUF on Lookup port: %s (non-fatal)", strerror(errno));
        // Non-fatal, continue anyway
    } else {
        log_verbose("Set Lookup port receive buffer to %d bytes", lookup_rcvbuf_size);
    }

    // Set protocol on lookup port too
    const std::string lookup_protocol = "S,SET PROTOCOL,6.2\r\n";
    if (send(lookup_fd_, lookup_protocol.c_str(), lookup_protocol.size(), 0) < 0) {
        last_error_ = std::string("lookup protocol send failed: ") + strerror(errno);
        close(socket_fd_);
        close(lookup_fd_);
        socket_fd_ = -1;
        lookup_fd_ = -1;
        state_ = ConnectionState::Error;
        return Result<void>::failure(last_error_);
    }
    log_verbose("Lookup port connected successfully");

    state_ = ConnectionState::Connected;
    return Result<void>::ok_result();
}

void IQFeedClient::disconnect() {
    // Set stop flags first
    searching_symbols_ = false;
    state_ = ConnectionState::Disconnected;

    // Clear callbacks to prevent accessing destroyed scanner (thread-safe)
    {
        MutexLock lock(callback_mutex_);
        symbol_search_callback_ = nullptr;
        incremental_save_callback_ = nullptr;
    }

    // Close sockets - this makes recv() in background thread fail immediately
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    if (lookup_fd_ >= 0) {
        close(lookup_fd_);
        lookup_fd_ = -1;
    }

    // Join thread - should exit immediately since socket closed and flags set
    if (symbol_search_thread_.joinable()) {
        symbol_search_thread_.join();
        log_verbose("Symbol search thread joined");
    }

    recv_buffer_.clear();
    lookup_buffer_.clear();
    watched_symbols_.clear();
}

ConnectionState IQFeedClient::state() const noexcept {
    return state_;
}

const std::string& IQFeedClient::last_error() const noexcept {
    return last_error_;
}

Result<void> IQFeedClient::watch(const std::string& symbol) {
    if (state_ != ConnectionState::Connected) {
        return Result<void>::failure("Not connected");
    }

    const std::string cmd = "w" + symbol + "\r\n";
    if (!send_command(cmd)) {
        return Result<void>::failure(last_error_);
    }

    // Initialize quote entry
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        if (quotes_.find(symbol) == quotes_.end()) {
            quotes_[symbol] = Quote(symbol);
        }
    }

    watched_symbols_.push_back(symbol);
    return Result<void>::ok_result();
}

Result<void> IQFeedClient::unwatch(const std::string& symbol) {
    if (state_ != ConnectionState::Connected) {
        return Result<void>::failure("Not connected");
    }

    const std::string cmd = "r" + symbol + "\r\n";
    if (!send_command(cmd)) {
        return Result<void>::failure(last_error_);
    }

    auto it = std::find(watched_symbols_.begin(), watched_symbols_.end(), symbol);
    if (it != watched_symbols_.end()) {
        watched_symbols_.erase(it);
    }

    return Result<void>::ok_result();
}

void IQFeedClient::unwatch_all() {
    for (const auto& sym : watched_symbols_) {
        const std::string cmd = "r" + sym + "\r\n";
        if (!send_command(cmd)) {
            log_verbose("Failed to unwatch %s during cleanup: %s", sym.c_str(), last_error_.c_str());
        }
    }
    watched_symbols_.clear();
}

Result<void> IQFeedClient::request_symbol_search() {
    if (state_ != ConnectionState::Connected) {
        return Result<void>::failure("Not connected");
    }

    if (lookup_fd_ < 0) {
        return Result<void>::failure("Lookup port not connected");
    }

    // Send SBF (Symbol By Filter) command over Lookup port
    // Format: SBF,[Field To Search],[Search String],[Filter Type],[Filter Value]
    // s = search by symbol, * = all symbols, t = filter by type, 1 = equity securities
    log_verbose("SBF: Starting symbol search for all EQUITY securities");
    const std::string cmd = "SBF,s,*,t,1\r\n";

    const ssize_t sent = send(lookup_fd_, cmd.c_str(), cmd.size(), 0);
    if (sent < 0) {
        last_error_ = std::string("SBF send() failed: ") + strerror(errno);
        return Result<void>::failure(last_error_);
    }

    log_verbose("TCP SEND (Lookup): %s", cmd.c_str());

    // Clear previous results and start collecting (thread-safe)
    {
        MutexLock lock(symbol_search_mutex_);
        symbol_search_results_.clear();
        last_incremental_save_count_ = 0;
    }
    searching_symbols_ = true;

    // Start background thread for maximum read speed
    // Thread will read continuously until !ENDMSG! without main loop delays
    if (symbol_search_thread_.joinable()) {
        symbol_search_thread_.join();  // Wait for previous search to complete
    }
    symbol_search_thread_ = std::thread(&IQFeedClient::symbol_search_worker, this);

    return Result<void>::ok_result();
}

void IQFeedClient::set_symbol_search_callback(SymbolListCallback cb) {
    MutexLock lock(callback_mutex_);
    symbol_search_callback_ = std::move(cb);
}

void IQFeedClient::set_incremental_save_callback(SymbolListCallback cb) {
    MutexLock lock(callback_mutex_);
    incremental_save_callback_ = std::move(cb);
}

void IQFeedClient::process() {
    if (state_ != ConnectionState::Connected) return;
    (void)read_data();  // Read from L1 port (quotes)

    // Symbol search is handled by background thread for maximum speed
    // No need to read lookup port here
}

void IQFeedClient::set_quote_callback(QuoteCallback cb) {
    quote_callback_ = std::move(cb);
}

const std::unordered_map<std::string, Quote>& IQFeedClient::quotes() const noexcept {
    return quotes_;
}

std::size_t IQFeedClient::watched_count() const noexcept {
    return watched_symbols_.size();
}

std::size_t IQFeedClient::message_count() const noexcept {
    return message_count_;
}

std::size_t IQFeedClient::symbols_received_count() const noexcept {
    // Lock-free read for UI updates
    return symbols_received_count_.load();
}

bool IQFeedClient::is_searching_symbols() const noexcept {
    return searching_symbols_;
}

bool IQFeedClient::send_command(const std::string& cmd) {
    if (socket_fd_ < 0) {
        last_error_ = "Socket not connected";
        return false;
    }

    log_verbose("TCP SEND: %s", cmd.c_str());

    const auto sent = send(socket_fd_, cmd.c_str(), cmd.size(), 0);
    if (sent < 0) {
        last_error_ = std::string("send() failed: ") + strerror(errno);
        return false;
    }

    if (static_cast<std::size_t>(sent) != cmd.size()) {
        last_error_ = "Incomplete send";
        return false;
    }

    return true;
}

bool IQFeedClient::read_data() {
    struct pollfd pfd{};
    pfd.fd = socket_fd_;
    pfd.events = POLLIN;

    const int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
    if (ret < 0) {
        if (errno != EINTR) {
            last_error_ = std::string("poll() failed: ") + strerror(errno);
            state_ = ConnectionState::Error;
        }
        return false;
    }

    if (ret == 0 || !(pfd.revents & POLLIN)) {
        return false;  // No data available
    }

    char buffer[RECV_BUFFER_SIZE];
    const auto bytes_read = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            last_error_ = std::string("recv() failed: ") + strerror(errno);
            state_ = ConnectionState::Error;
        }
        return false;
    }

    if (bytes_read == 0) {
        last_error_ = "Connection closed by server";
        state_ = ConnectionState::Disconnected;
        return false;
    }

    buffer[bytes_read] = '\0';
    recv_buffer_ += buffer;

    // Process complete lines
    std::size_t pos;
    while ((pos = recv_buffer_.find('\n')) != std::string::npos) {
        std::string line = recv_buffer_.substr(0, pos);
        recv_buffer_.erase(0, pos + 1);

        // Remove trailing CR if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty()) {
            parse_message(line);
        }
    }

    return true;
}

bool IQFeedClient::read_lookup_data() {
    if (lookup_fd_ < 0) {
        return false;
    }

    struct pollfd pfd{};
    pfd.fd = lookup_fd_;
    pfd.events = POLLIN;

    const int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
    if (ret < 0) {
        if (errno != EINTR) {
            log_warn("Lookup poll() failed: %s", strerror(errno));

            // If we're searching symbols, this is fatal
            if (searching_symbols_) {
                last_error_ = std::string("Lookup poll error: ") + strerror(errno);
                searching_symbols_ = false;
                state_ = ConnectionState::Error;
                symbols_received_count_ = 0;

                MutexLock lock(symbol_search_mutex_);
                symbol_search_results_.clear();
                last_incremental_save_count_ = 0;
            }
        }
        return false;
    }

    // Check for connection errors (POLLHUP, POLLERR, POLLNVAL)
    if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
        log_warn("Lookup port connection error (revents: 0x%x)", pfd.revents);

        // If we're searching symbols, this is fatal
        if (searching_symbols_) {
            last_error_ = "Lookup port connection lost during symbol search";
            searching_symbols_ = false;
            state_ = ConnectionState::Error;
            symbols_received_count_ = 0;

            MutexLock lock(symbol_search_mutex_);
            symbol_search_results_.clear();
            last_incremental_save_count_ = 0;
        }

        return false;
    }

    if (ret == 0 || !(pfd.revents & POLLIN)) {
        return false;  // No data available
    }

    char buffer[RECV_BUFFER_SIZE];
    const auto bytes_read = recv(lookup_fd_, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            log_warn("Lookup recv() failed: %s", strerror(errno));

            // If we're searching symbols, this is fatal
            if (searching_symbols_) {
                last_error_ = std::string("Lookup port error: ") + strerror(errno);
                searching_symbols_ = false;
                state_ = ConnectionState::Error;
                symbols_received_count_ = 0;

                MutexLock lock(symbol_search_mutex_);
                symbol_search_results_.clear();
                last_incremental_save_count_ = 0;
            }
        }
        return false;
    }

    if (bytes_read == 0) {
        log_warn("Lookup connection closed by server during symbol search");

        // If we're searching symbols, this is fatal - abort the search
        if (searching_symbols_) {
            last_error_ = "Lookup port closed during symbol search (IQFeed Error 10054)";
            searching_symbols_ = false;
            state_ = ConnectionState::Error;
            symbols_received_count_ = 0;

            MutexLock lock(symbol_search_mutex_);
            symbol_search_results_.clear();
            last_incremental_save_count_ = 0;
        }

        return false;
    }

    buffer[bytes_read] = '\0';
    lookup_buffer_ += buffer;

    // Process complete lines
    std::size_t pos;
    while ((pos = lookup_buffer_.find('\n')) != std::string::npos) {
        std::string line = lookup_buffer_.substr(0, pos);
        lookup_buffer_.erase(0, pos + 1);

        // Remove trailing CR if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty()) {
            log_verbose("TCP RECV (Lookup): %s", line.c_str());
            parse_lookup_message(line);
        }
    }

    return true;
}

void IQFeedClient::parse_message(const std::string& line) {
    if (line.empty()) return;

    ++message_count_;

    log_verbose("TCP RECV: %s", line.c_str());

    // Check for error messages: E,[Error Text],
    if (!line.empty() && line[0] == 'E') {
        const auto fields = split(line, ',');
        if (fields.size() >= 2 && fields[0] == "E") {
            const std::string error_msg = fields.size() > 1 ? fields[1] : "Unknown error";
            log_warn("IQFeed L1 Error: %s", error_msg.c_str());
            // Don't set state to error for L1 errors - they might be symbol-not-found which is non-fatal
            // Just log it
            return;
        }
    }

    const auto fields = split(line, ',');
    if (fields.empty()) return;

    const char msg_type = fields[0].empty() ? '\0' : fields[0][0];

    switch (msg_type) {
        case 'F':
            parse_fundamental(fields);
            break;
        case 'P':
            parse_summary(fields);
            break;
        case 'Q':
            parse_update(fields);
            break;
        case 'S':
            parse_system(fields);
            break;
        case 'n':  // Symbol not found
            if (fields.size() > 1) {
                log_verbose("Symbol not found: %s", fields[1].c_str());
            }
            break;
        case 'T':  // Timestamp (periodic server time updates)
            // Just ignore these - they're heartbeat messages
            break;
        default:
            // Ignore unknown message types
            break;
    }
}

void IQFeedClient::parse_fundamental(const std::vector<std::string>& fields) {
    if (fields.size() < 2) return;

    const std::string& symbol = fields[1];

    std::lock_guard<std::mutex> lock(quotes_mutex_);
    auto it = quotes_.find(symbol);
    if (it == quotes_.end()) return;

    Quote& q = it->second;

    // Parse fundamental data fields
    if (fields.size() > static_cast<std::size_t>(FundField::AvgVolume)) {
        q.avg_volume = parse_int64(fields[static_cast<std::size_t>(FundField::AvgVolume)]);
    }
    if (fields.size() > static_cast<std::size_t>(FundField::High52Week)) {
        q.high_52wk = parse_double(fields[static_cast<std::size_t>(FundField::High52Week)]);
    }
    if (fields.size() > static_cast<std::size_t>(FundField::Low52Week)) {
        q.low_52wk = parse_double(fields[static_cast<std::size_t>(FundField::Low52Week)]);
    }
    if (fields.size() > static_cast<std::size_t>(FundField::CompanyName)) {
        q.name = fields[static_cast<std::size_t>(FundField::CompanyName)];
    }
    if (fields.size() > static_cast<std::size_t>(FundField::FloatShares)) {
        q.float_shares = parse_int64(fields[static_cast<std::size_t>(FundField::FloatShares)]);
    }

    q.last_update = std::chrono::system_clock::now();

    if (quote_callback_) {
        quote_callback_(q);
    }
}

void IQFeedClient::parse_summary(const std::vector<std::string>& fields) {
    if (fields.size() < 20) return;

    const std::string& symbol = fields[1];

    std::lock_guard<std::mutex> lock(quotes_mutex_);
    auto it = quotes_.find(symbol);
    if (it == quotes_.end()) return;

    Quote& q = it->second;

    if (fields.size() > static_cast<std::size_t>(QuoteField::Bid)) {
        q.bid = parse_double(fields[static_cast<std::size_t>(QuoteField::Bid)]);
    }
    if (fields.size() > static_cast<std::size_t>(QuoteField::Ask)) {
        q.ask = parse_double(fields[static_cast<std::size_t>(QuoteField::Ask)]);
    }
    if (fields.size() > static_cast<std::size_t>(QuoteField::Last)) {
        q.last_price = parse_double(fields[static_cast<std::size_t>(QuoteField::Last)]);
    }
    if (fields.size() > static_cast<std::size_t>(QuoteField::TotalVolume)) {
        q.volume = parse_int64(fields[static_cast<std::size_t>(QuoteField::TotalVolume)]);
    }
    if (fields.size() > static_cast<std::size_t>(QuoteField::High)) {
        q.high = parse_double(fields[static_cast<std::size_t>(QuoteField::High)]);
    }
    if (fields.size() > static_cast<std::size_t>(QuoteField::Low)) {
        q.low = parse_double(fields[static_cast<std::size_t>(QuoteField::Low)]);
    }
    if (fields.size() > static_cast<std::size_t>(QuoteField::Close)) {
        q.prev_close = parse_double(fields[static_cast<std::size_t>(QuoteField::Close)]);
    }
    if (fields.size() > static_cast<std::size_t>(QuoteField::Open)) {
        q.open = parse_double(fields[static_cast<std::size_t>(QuoteField::Open)]);
    }
    if (fields.size() > static_cast<std::size_t>(QuoteField::ExtendedTrade)) {
        q.extended_price = parse_double(fields[static_cast<std::size_t>(QuoteField::ExtendedTrade)]);
    }
    if (fields.size() > static_cast<std::size_t>(QuoteField::ExtendedTradeSize)) {
        q.extended_volume = parse_int64(fields[static_cast<std::size_t>(QuoteField::ExtendedTradeSize)]);
    }

    q.last_update = std::chrono::system_clock::now();

    if (quote_callback_) {
        quote_callback_(q);
    }
}

void IQFeedClient::parse_update(const std::vector<std::string>& fields) {
    // Update messages have same format as summary
    parse_summary(fields);
}

void IQFeedClient::parse_system(const std::vector<std::string>& fields) {
    // System messages: S,CURRENT UPDATE FIELDNAMES,...
    // Or: S,SERVER CONNECTED
    // Just log for now
    USCAN_MAYBE_UNUSED const auto& msg = fields;
    (void)msg;
}

// HTTP removed - using TCP SBF command instead

std::vector<std::string> IQFeedClient::split(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string item;

    while (std::getline(iss, item, delim)) {
        result.push_back(item);
    }

    return result;
}

double IQFeedClient::parse_double(const std::string& s) noexcept {
    if (s.empty()) return 0.0;

    // Apple libc++ doesn't support from_chars for floating-point in C++17
    // Use strtod instead
    char* end = nullptr;
    const double result = std::strtod(s.c_str(), &end);

    // Check if conversion was successful
    if (end == s.c_str()) {
        return 0.0;
    }

    return result;
}

int64_t IQFeedClient::parse_int64(const std::string& s) noexcept {
    if (s.empty()) return 0;

    int64_t result = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);

    if (ec != std::errc()) {
        return 0;
    }

    return result;
}

void IQFeedClient::parse_lookup_message(const std::string& line) {
    // Check for error messages: E,[Error Text], or E,!SYNTAX_ERROR!,
    if (!line.empty() && line[0] == 'E') {
        const auto fields = split(line, ',');
        if (fields.size() >= 2 && fields[0] == "E") {
            const std::string error_msg = fields.size() > 1 ? fields[1] : "Unknown error";
            log_warn("IQFeed Lookup Error: %s", error_msg.c_str());
            last_error_ = "Lookup port error: " + error_msg;

            // If we get an error during symbol search, abort the search
            if (searching_symbols_) {
                searching_symbols_ = false;
                state_ = ConnectionState::Error;
                symbols_received_count_ = 0;

                MutexLock lock(symbol_search_mutex_);
                symbol_search_results_.clear();
                last_incremental_save_count_ = 0;
            }
            return;
        }
    }

    // Check for end marker
    if (line == "!ENDMSG!") {
        // Copy data while holding locks, then release before calling callbacks
        std::vector<SymbolInfo> final_batch;
        std::vector<SymbolInfo> all_symbols;
        std::size_t total_count = 0;

        // Copy all data under symbol_search_mutex_
        {
            MutexLock lock(symbol_search_mutex_);
            total_count = symbol_search_results_.size();
            log_verbose("SBF: Search complete, received %zu symbols total", total_count);

            // Check if there's a final batch to save
            if (symbol_search_results_.size() > last_incremental_save_count_) {
                final_batch.assign(
                    symbol_search_results_.begin() + static_cast<long>(last_incremental_save_count_),
                    symbol_search_results_.end()
                );
            }

            // Copy all symbols for completion callback
            all_symbols = symbol_search_results_;

            // Clear state
            symbol_search_results_.clear();
            last_incremental_save_count_ = 0;
        }

        // Copy callbacks while holding callback_mutex_ to prevent race with disconnect()
        SymbolListCallback inc_save_cb;
        SymbolListCallback search_cb;
        {
            MutexLock lock(callback_mutex_);
            inc_save_cb = incremental_save_callback_;
            search_cb = symbol_search_callback_;
        }

        // Release locks, now call callbacks (safe - we have local copies)
        if (!final_batch.empty() && inc_save_cb) {
            log_verbose("SBF: Saving final batch of %zu symbols", final_batch.size());
            inc_save_cb(final_batch);
        }

        searching_symbols_ = false;
        symbols_received_count_ = 0;  // Reset counter

        if (search_cb) {
            search_cb(all_symbols);
        }

        return;
    }

    // SBF response format (comma-delimited): MessageID,Symbol,ListedMarketID,SecurityTypeID,Name,<more fields>
    // Header line: MessageID,Symbol,ListedMarketID,SecurityTypeID,Name,...
    // Skip header line
    if (line.find("MessageID") != std::string::npos ||
        line.find("Symbol") == 0) {
        return;
    }

    const auto fields = split(line, ',');
    if (fields.size() < 5) return;  // Need at least: MessageID, Symbol, ListedMarketID, SecurityTypeID, Name

    // Field 3: SecurityTypeID (1 = equity, 2 = index, 3 = mutual fund, 4 = money market, 5 = trust/ETF)
    const std::string& security_type = fields[3];
    if (security_type != "1") {
        return;  // Only accept equities (not ETFs, indices, etc.)
    }

    // Field 1: Symbol
    std::string symbol = fields[1];

    // Clean up symbol (remove quotes if present)
    if (!symbol.empty() && symbol[0] == '"') {
        symbol = symbol.substr(1);
    }
    if (!symbol.empty() && symbol.back() == '"') {
        symbol.pop_back();
    }

    // Skip empty or invalid symbols
    if (symbol.empty() || symbol.find(' ') != std::string::npos) {
        return;
    }

    // Skip symbols starting with dots (OTC/foreign)
    if (symbol[0] == '.') {
        return;
    }

    // Field 4: Name
    std::string name = fields.size() > 4 ? fields[4] : "";

    // Field 2: ListedMarketID (exchange) - we'll use this as exchange
    std::string exchange = fields.size() > 2 ? fields[2] : "";

    SymbolInfo info;
    info.symbol = std::move(symbol);
    info.name = std::move(name);
    info.exchange = std::move(exchange);

    // Lock, add symbol, check for batch save, unlock
    std::vector<SymbolInfo> batch_to_save;
    std::size_t current_count = 0;

    {
        MutexLock lock(symbol_search_mutex_);
        symbol_search_results_.push_back(std::move(info));
        current_count = symbol_search_results_.size();

        // Update atomic counter for lock-free UI reads
        symbols_received_count_ = current_count;

        // Log every 100 symbols for progress feedback
        if (current_count % 100 == 0) {
            log_verbose("SBF: Received %zu symbols so far", current_count);
        }

        // Incremental save every 500 symbols - copy batch while holding lock
        // Don't check callback here (it's guarded by callback_mutex_)
        if (current_count >= last_incremental_save_count_ + 500) {
            batch_to_save.assign(
                symbol_search_results_.begin() + static_cast<long>(last_incremental_save_count_),
                symbol_search_results_.end()
            );

            last_incremental_save_count_ = current_count;
        }
    }

    // Copy callback while holding callback_mutex_ to prevent race with disconnect()
    SymbolListCallback inc_save_cb;
    if (!batch_to_save.empty()) {
        MutexLock lock(callback_mutex_);
        inc_save_cb = incremental_save_callback_;
    }

    // Release locks, now call callback (safe - we have local copy)
    if (!batch_to_save.empty() && inc_save_cb) {
        log_verbose("SBF: Saving incremental batch of %zu symbols (total: %zu)",
                   batch_to_save.size(), current_count);
        inc_save_cb(batch_to_save);
    }
}

void IQFeedClient::symbol_search_worker() {
    // Background thread: Read symbol data as fast as possible until !ENDMSG!
    log_verbose("Symbol search worker thread started");

    // Track start time for overall timeout protection
    const auto start_time = std::chrono::steady_clock::now();

    // Aggressive reading: tight loop when data available (like original fix)
    // IQFeed sends symbols VERY fast - we must drain the buffer continuously!
    while (searching_symbols_ && state_ == ConnectionState::Connected) {
        // Check overall timeout to prevent hanging forever if !ENDMSG! never arrives
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > SYMBOL_SEARCH_TIMEOUT_MS) {
            log_warn("Symbol search worker timed out after %d ms", SYMBOL_SEARCH_TIMEOUT_MS);
            last_error_ = "Symbol search timed out";
            searching_symbols_ = false;
            state_ = ConnectionState::Error;
            break;
        }

        bool data_read = false;

        // Read in tight loop until no more data (prevents buffer overflow)
        while (searching_symbols_ && state_ == ConnectionState::Connected) {
            if (read_lookup_data()) {
                data_read = true;  // Keep reading
            } else {
                break;  // No more data available right now
            }
        }

        // Brief sleep only when no data was available (avoid busy-wait)
        if (!data_read && searching_symbols_ && state_ == ConnectionState::Connected) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    log_verbose("Symbol search worker thread exiting (searching=%d, state=%d)",
               static_cast<int>(searching_symbols_),
               static_cast<int>(state_.load()));
}

} // namespace uscan
