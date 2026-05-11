package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"
)

// Timeout and interval constants
const (
	readTimeout       = 30 * time.Second
	writeTimeout      = 5 * time.Second
	updateTimeout     = 60 * time.Second
	updateInterval    = 1 * time.Second
	timestampInterval = 5 * time.Second
	symbolSendDelay   = 1 * time.Millisecond
)

// Field array sizes for IQFeed messages
const (
	fundamentalFieldCount = 60
	quoteFieldCount       = 50
)

// isTimeoutError checks if an error is a network timeout
func isTimeoutError(err error) bool {
	if err == nil {
		return false
	}
	netErr, ok := err.(net.Error)
	return ok && netErr.Timeout()
}

// MockIQFeedServer simulates IQFeed for integration testing
type MockIQFeedServer struct {
	l1Port     int
	lookupPort int
	l1Listener net.Listener
	lookupListener net.Listener

	// Active connections
	l1Conns     []net.Conn
	lookupConns []net.Conn
	connMu      sync.Mutex

	// Watched symbols per connection
	watched map[net.Conn]map[string]bool
	watchMu sync.Mutex

	// Per-connection cancel channels for immediate cleanup
	connCancel map[net.Conn]chan struct{}
	cancelMu   sync.Mutex

	// Symbol database for SBF queries
	symbols []SymbolData

	// Control
	stopChan chan struct{}
	wg       sync.WaitGroup
}

type SymbolData struct {
	Symbol   string
	Name     string
	Exchange string
	Type     int // 1=equity, 2=index, etc
}

// NewMockIQFeedServer creates a new mock server
func NewMockIQFeedServer(l1Port, lookupPort int) *MockIQFeedServer {
	server := &MockIQFeedServer{
		l1Port:     l1Port,
		lookupPort: lookupPort,
		watched:    make(map[net.Conn]map[string]bool),
		connCancel: make(map[net.Conn]chan struct{}),
		stopChan:   make(chan struct{}),
	}

	// Populate test symbols
	server.symbols = []SymbolData{
		{"AAPL", "APPLE INC", "NASDAQ", 1},
		{"MSFT", "MICROSOFT CORP", "NASDAQ", 1},
		{"TSLA", "TESLA INC", "NASDAQ", 1},
		{"GOOGL", "ALPHABET INC CLASS A", "NASDAQ", 1},
		{"AMZN", "AMAZON.COM INC", "NASDAQ", 1},
		{"META", "META PLATFORMS INC", "NASDAQ", 1},
		{"NVDA", "NVIDIA CORP", "NASDAQ", 1},
		{"SPY", "SPDR S&P 500 ETF", "NYSE", 2}, // ETF - should be filtered
		{"^SPX", "S&P 500 INDEX", "INDEX", 3}, // Index - should be filtered
	}

	return server
}

// Start starts both Level 1 and Lookup servers
func (s *MockIQFeedServer) Start() error {
	// Start L1 port (quotes)
	var err error
	s.l1Listener, err = net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", s.l1Port))
	if err != nil {
		return fmt.Errorf("failed to start L1 listener: %w", err)
	}
	log.Printf("Mock IQFeed L1 listening on %s", s.l1Listener.Addr())

	// Start Lookup port (symbol search)
	s.lookupListener, err = net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", s.lookupPort))
	if err != nil {
		if closeErr := s.l1Listener.Close(); closeErr != nil {
			log.Printf("error closing L1 listener during cleanup: %v", closeErr)
		}
		return fmt.Errorf("failed to start Lookup listener: %w", err)
	}
	log.Printf("Mock IQFeed Lookup listening on %s", s.lookupListener.Addr())

	// Accept connections on both ports
	s.wg.Add(2)
	go s.acceptL1Connections()
	go s.acceptLookupConnections()

	return nil
}

// Stop shuts down the server
func (s *MockIQFeedServer) Stop() {
	close(s.stopChan)

	if s.l1Listener != nil {
		if err := s.l1Listener.Close(); err != nil {
			log.Printf("error closing L1 listener: %v", err)
		}
	}
	if s.lookupListener != nil {
		if err := s.lookupListener.Close(); err != nil {
			log.Printf("error closing lookup listener: %v", err)
		}
	}

	s.connMu.Lock()
	for _, conn := range s.l1Conns {
		if err := conn.Close(); err != nil {
			log.Printf("error closing L1 connection: %v", err)
		}
	}
	for _, conn := range s.lookupConns {
		if err := conn.Close(); err != nil {
			log.Printf("error closing lookup connection: %v", err)
		}
	}
	s.connMu.Unlock()

	s.wg.Wait()
	log.Println("Mock IQFeed server stopped")
}

// removeL1Conn removes a connection from the L1 connections slice
func (s *MockIQFeedServer) removeL1Conn(conn net.Conn) {
	s.connMu.Lock()
	defer s.connMu.Unlock()
	for i, c := range s.l1Conns {
		if c == conn {
			s.l1Conns = append(s.l1Conns[:i], s.l1Conns[i+1:]...)
			return
		}
	}
}

// removeLookupConn removes a connection from the lookup connections slice
func (s *MockIQFeedServer) removeLookupConn(conn net.Conn) {
	s.connMu.Lock()
	defer s.connMu.Unlock()
	for i, c := range s.lookupConns {
		if c == conn {
			s.lookupConns = append(s.lookupConns[:i], s.lookupConns[i+1:]...)
			return
		}
	}
}

func (s *MockIQFeedServer) acceptL1Connections() {
	defer s.wg.Done()

	for {
		conn, err := s.l1Listener.Accept()
		if err != nil {
			select {
			case <-s.stopChan:
				return
			default:
				log.Printf("L1 accept error: %v", err)
				continue
			}
		}

		s.connMu.Lock()
		s.l1Conns = append(s.l1Conns, conn)
		s.connMu.Unlock()

		s.watchMu.Lock()
		s.watched[conn] = make(map[string]bool)
		s.watchMu.Unlock()

		s.wg.Add(1)
		go s.handleL1Connection(conn)
	}
}

func (s *MockIQFeedServer) acceptLookupConnections() {
	defer s.wg.Done()

	for {
		conn, err := s.lookupListener.Accept()
		if err != nil {
			select {
			case <-s.stopChan:
				return
			default:
				log.Printf("Lookup accept error: %v", err)
				continue
			}
		}

		s.connMu.Lock()
		s.lookupConns = append(s.lookupConns, conn)
		s.connMu.Unlock()

		s.wg.Add(1)
		go s.handleLookupConnection(conn)
	}
}

func (s *MockIQFeedServer) handleL1Connection(conn net.Conn) {
	defer s.wg.Done()

	// Create cancel channel for this connection's goroutines
	cancelChan := make(chan struct{})
	s.cancelMu.Lock()
	s.connCancel[conn] = cancelChan
	s.cancelMu.Unlock()

	defer func() {
		// Close cancel channel to stop all goroutines for this connection
		s.cancelMu.Lock()
		if ch, exists := s.connCancel[conn]; exists {
			close(ch)
			delete(s.connCancel, conn)
		}
		s.cancelMu.Unlock()

		s.removeL1Conn(conn)
		if err := conn.Close(); err != nil {
			log.Printf("error closing L1 connection: %v", err)
		}
	}()

	log.Printf("L1 client connected: %s", conn.RemoteAddr())

	scanner := bufio.NewScanner(conn)
	for {
		// Set read deadline to prevent hanging forever if client stops sending
		if err := conn.SetReadDeadline(time.Now().Add(readTimeout)); err != nil {
			log.Printf("L1 SetReadDeadline error: %v", err)
			break
		}

		if !scanner.Scan() {
			break
		}
		line := scanner.Text()
		line = strings.TrimSpace(line)

		log.Printf("L1 RECV: %s", line)

		if err := s.handleL1Command(conn, line); err != nil {
			log.Printf("L1 command error: %v", err)
			return
		}
	}

	if err := scanner.Err(); err != nil {
		// Don't log timeout errors as warnings - they're expected during shutdown
		if !isTimeoutError(err) {
			log.Printf("L1 scanner error: %v", err)
		}
	}

	s.watchMu.Lock()
	delete(s.watched, conn)
	s.watchMu.Unlock()

	log.Printf("L1 client disconnected: %s", conn.RemoteAddr())
}

func (s *MockIQFeedServer) handleL1Command(conn net.Conn, cmd string) error {
	if strings.HasPrefix(cmd, "S,SET PROTOCOL,") {
		// Protocol handshake
		version := strings.TrimPrefix(cmd, "S,SET PROTOCOL,")
		if err := s.send(conn, fmt.Sprintf("S,CURRENT PROTOCOL,%s\r\n", version)); err != nil {
			return err
		}
		if err := s.send(conn, "S,SERVER CONNECTED\r\n"); err != nil {
			return err
		}
		if err := s.send(conn, "S,KEY,99999\r\n"); err != nil {
			return err
		}
		if err := s.send(conn, "S,CUST,mock_server,127.0.0.1,5009,0,0,0,0,0,0\r\n"); err != nil {
			return err
		}
		// Send complete fieldnames matching what the client expects
		// These field names match IQFeed documentation and the client's dynamic field mapping
		fieldnames := "S,CURRENT UPDATE FIELDNAMES,Symbol,Reserved," +
			"Bid,Ask,Bid Size,Ask Size,Most Recent Trade,Most Recent Trade Size," +
			"Reserved2,Reserved3,Total Volume,Reserved4,Reserved5," +
			"High,Low,Close,Reserved6,Reserved7,Reserved8,Reserved9," +
			"Reserved10,Reserved11,Reserved12,Reserved13,Reserved14,Reserved15," +
			"Open,Reserved16,Reserved17,Reserved18,Reserved19,Reserved20," +
			"Reserved21,Reserved22,Reserved23,Reserved24,Reserved25,Reserved26," +
			"Reserved27,Reserved28,Reserved29,Extended Trade,Extended Trade Size\r\n"
		if err := s.send(conn, fieldnames); err != nil {
			return err
		}

		// Start sending periodic timestamps
		s.cancelMu.Lock()
		cancelChan := s.connCancel[conn]
		s.cancelMu.Unlock()
		s.wg.Add(1)
		go s.sendTimestamps(conn, cancelChan)

	} else if strings.HasPrefix(cmd, "w") {
		// Watch symbol
		symbol := strings.TrimPrefix(cmd, "w")
		symbol = strings.ToUpper(strings.TrimSpace(symbol))

		// Special test symbols for R/C/N messages
		if symbol == "TEST_REGIONAL" {
			// Send a regional quote message
			if err := s.send(conn, "R,TEST,NYSE,100.50,100.55,500,600,100.52\r\n"); err != nil {
				return err
			}
			return nil
		}
		if symbol == "TEST_CORRECTION" {
			// Send a trade correction message
			if err := s.send(conn, "C,TEST,D,100.50,1000,TRADE123\r\n"); err != nil {
				return err
			}
			return nil
		}
		if symbol == "TEST_NEWS" {
			// Send a news headline message with current timestamp
			newsTime := time.Now().Format("2006-01-02 15:04:05")
			if err := s.send(conn, fmt.Sprintf("N,12345,Reuters,AAPL:MSFT,Tech stocks rise on earnings,%s\r\n", newsTime)); err != nil {
				return err
			}
			return nil
		}

		s.watchMu.Lock()
		if watchMap, exists := s.watched[conn]; exists {
			watchMap[symbol] = true
		}
		s.watchMu.Unlock()

		// Send fundamental data
		if err := s.sendFundamental(conn, symbol); err != nil {
			return err
		}

		// Send summary (initial quote)
		if err := s.sendSummary(conn, symbol); err != nil {
			return err
		}

		// Start sending updates
		s.cancelMu.Lock()
		cancelChan := s.connCancel[conn]
		s.cancelMu.Unlock()
		s.wg.Add(1)
		go s.sendUpdates(conn, symbol, cancelChan)

	} else if strings.HasPrefix(cmd, "r") {
		// Unwatch symbol
		symbol := strings.TrimPrefix(cmd, "r")
		symbol = strings.ToUpper(strings.TrimSpace(symbol))

		s.watchMu.Lock()
		if watchMap, exists := s.watched[conn]; exists {
			delete(watchMap, symbol)
		}
		s.watchMu.Unlock()

	} else {
		// Unknown command
		if err := s.send(conn, "E,!SYNTAX_ERROR!,\r\n"); err != nil {
			return err
		}
	}

	return nil
}

func (s *MockIQFeedServer) handleLookupConnection(conn net.Conn) {
	defer s.wg.Done()
	defer func() {
		s.removeLookupConn(conn)
		if err := conn.Close(); err != nil {
			log.Printf("error closing lookup connection: %v", err)
		}
	}()

	log.Printf("Lookup client connected: %s", conn.RemoteAddr())

	scanner := bufio.NewScanner(conn)
	for {
		// Set read deadline to prevent hanging forever if client stops sending
		if err := conn.SetReadDeadline(time.Now().Add(readTimeout)); err != nil {
			log.Printf("Lookup SetReadDeadline error: %v", err)
			break
		}

		if !scanner.Scan() {
			break
		}
		line := scanner.Text()
		line = strings.TrimSpace(line)

		log.Printf("Lookup RECV: %s", line)

		if err := s.handleLookupCommand(conn, line); err != nil {
			log.Printf("Lookup command error: %v", err)
			return
		}
	}

	if err := scanner.Err(); err != nil {
		// Don't log timeout errors as warnings - they're expected during shutdown
		if !isTimeoutError(err) {
			log.Printf("Lookup scanner error: %v", err)
		}
	}

	log.Printf("Lookup client disconnected: %s", conn.RemoteAddr())
}

func (s *MockIQFeedServer) handleLookupCommand(conn net.Conn, cmd string) error {
	if strings.HasPrefix(cmd, "S,SET PROTOCOL,") {
		// Protocol handshake - just acknowledge
		version := strings.TrimPrefix(cmd, "S,SET PROTOCOL,")
		if err := s.send(conn, fmt.Sprintf("S,CURRENT PROTOCOL,%s\r\n", version)); err != nil {
			return err
		}
	} else if strings.HasPrefix(cmd, "SBF,") {
		// Symbol By Filter query
		// Format: SBF,s,*,t,1
		// For simplicity, just send all equity symbols

		for _, sym := range s.symbols {
			// LS format: LS,Symbol,ExchangeID,SecurityTypeID,Name,...
			msg := fmt.Sprintf("LS,%s,%s,%d,%s,\r\n",
				sym.Symbol, sym.Exchange, sym.Type, sym.Name)
			if err := s.send(conn, msg); err != nil {
				// Connection broken - no point trying to send ENDMSG
				return err
			}

			// Simulate realistic delay (symbols arrive fast but not instant)
			time.Sleep(symbolSendDelay)
		}

		// Send end marker
		if err := s.send(conn, "!ENDMSG!\r\n"); err != nil {
			return err
		}

	} else {
		// Unknown command
		if err := s.send(conn, "E,!SYNTAX_ERROR!,\r\n"); err != nil {
			return err
		}
	}

	return nil
}

func (s *MockIQFeedServer) sendFundamental(conn net.Conn, symbol string) error {
	// F message format: F,Symbol,ExchangeID,PE,AvgVolume,High52Wk,Low52Wk,...,CompanyName,...,FloatShares,...
	// Simplified version with key fields

	name := fmt.Sprintf("%s CORP", symbol)
	if symbol == "AAPL" {
		name = "APPLE INC"
	} else if symbol == "MSFT" {
		name = "MICROSOFT CORP"
	} else if symbol == "TSLA" {
		name = "TESLA INC"
	}

	// Build message with proper field indices
	fields := make([]string, fundamentalFieldCount)
	fields[0] = "F"
	fields[1] = symbol           // Index 1: Symbol
	fields[2] = "NASDAQ"          // Index 2: Exchange
	fields[3] = "25.5"            // Index 3: PE
	fields[4] = "50000000"        // Index 4: Avg Volume
	fields[5] = "195.00"          // Index 5: 52-week High
	fields[6] = "125.00"          // Index 6: 52-week Low

	// Fill intermediate fields
	for i := 7; i < 48; i++ {
		fields[i] = ""
	}

	fields[48] = name             // Index 48: Company Name
	fields[49] = ""
	fields[50] = ""
	fields[51] = ""
	fields[52] = "16500000000"    // Index 52: Float Shares

	// Fill remaining fields
	for i := 53; i < fundamentalFieldCount; i++ {
		fields[i] = ""
	}

	msg := strings.Join(fields, ",") + "\r\n"
	return s.send(conn, msg)
}

func (s *MockIQFeedServer) sendSummary(conn net.Conn, symbol string) error {
	// P message format (Summary/Update)
	// P,Symbol,Reserved,Bid,Ask,BidSize,AskSize,Last,LastSize,Volume,...,High,Low,Close,Open,...,ExtPrice,ExtVol,...

	fields := make([]string, quoteFieldCount)
	fields[0] = "P"
	fields[1] = symbol            // Index 1: Symbol
	fields[2] = ""                // Index 2: Reserved
	fields[3] = "150.25"          // Index 3: Bid
	fields[4] = "150.30"          // Index 4: Ask
	fields[5] = "100"             // Index 5: Bid Size
	fields[6] = "100"             // Index 6: Ask Size
	fields[7] = "150.28"          // Index 7: Last Price
	fields[8] = "100"             // Index 8: Last Size
	fields[9] = ""
	fields[10] = ""
	fields[11] = "1000000"        // Index 11: Total Volume
	fields[12] = ""
	fields[13] = ""
	fields[14] = "151.00"         // Index 14: High
	fields[15] = "149.50"         // Index 15: Low
	fields[16] = "148.00"         // Index 16: Previous Close

	// Fill intermediate fields
	for i := 17; i < 28; i++ {
		fields[i] = ""
	}

	fields[28] = "149.75"         // Index 28: Open

	// Fill intermediate fields
	for i := 29; i < 42; i++ {
		fields[i] = ""
	}

	fields[42] = "149.00"         // Index 42: Extended Price (pre-market)
	fields[43] = "50000"          // Index 43: Extended Volume

	// Fill remaining fields
	for i := 44; i < quoteFieldCount; i++ {
		fields[i] = ""
	}

	msg := strings.Join(fields, ",") + "\r\n"
	return s.send(conn, msg)
}

func (s *MockIQFeedServer) sendUpdates(conn net.Conn, symbol string, connCancel <-chan struct{}) {
	defer s.wg.Done()

	// Send periodic quote updates while symbol is watched
	// Maximum updateTimeout of updates to prevent hanging forever
	ticker := time.NewTicker(updateInterval)
	defer ticker.Stop()

	timeout := time.After(updateTimeout)
	price := 150.0
	volume := 1000000

	for {
		select {
		case <-s.stopChan:
			return
		case <-connCancel:
			// Connection closed, stop immediately
			return
		case <-timeout:
			log.Printf("sendUpdates timeout for %s", symbol)
			return
		case <-ticker.C:
			// Check if still watched
			s.watchMu.Lock()
			watchMap, exists := s.watched[conn]
			if !exists {
				s.watchMu.Unlock()
				return
			}
			watched := watchMap[symbol]
			s.watchMu.Unlock()

			if !watched {
				return
			}

			// Simulate price movement
			price += 0.10
			volume += 10000

			// Q message (update)
			fields := make([]string, quoteFieldCount)
			fields[0] = "Q"
			fields[1] = symbol
			fields[2] = ""
			fields[3] = fmt.Sprintf("%.2f", price-0.05)  // Bid
			fields[4] = fmt.Sprintf("%.2f", price+0.05)  // Ask
			fields[5] = "100"
			fields[6] = "100"
			fields[7] = fmt.Sprintf("%.2f", price)       // Last
			fields[8] = "100"
			fields[9] = ""
			fields[10] = ""
			fields[11] = fmt.Sprintf("%d", volume)       // Volume
			fields[14] = fmt.Sprintf("%.2f", price+1.0)  // High
			fields[15] = "149.50"                        // Low
			fields[16] = "148.00"                        // Prev Close
			fields[28] = "149.75"                        // Open

			msg := strings.Join(fields, ",") + "\r\n"
			if err := s.send(conn, msg); err != nil {
				// Connection closed or error, stop sending
				return
			}
		}
	}
}

func (s *MockIQFeedServer) sendTimestamps(conn net.Conn, connCancel <-chan struct{}) {
	defer s.wg.Done()

	// Send periodic timestamp heartbeats
	// Maximum updateTimeout to prevent hanging forever
	ticker := time.NewTicker(timestampInterval)
	defer ticker.Stop()

	timeout := time.After(updateTimeout)

	for {
		select {
		case <-s.stopChan:
			return
		case <-connCancel:
			// Connection closed, stop immediately
			return
		case <-timeout:
			return
		case <-ticker.C:
			timestamp := time.Now().Format("20060102 15:04:05")
			if err := s.send(conn, fmt.Sprintf("T,%s\r\n", timestamp)); err != nil {
				// Connection closed or error, stop sending
				return
			}
		}
	}
}

func (s *MockIQFeedServer) send(conn net.Conn, msg string) error {
	log.Printf("SEND: %s", strings.TrimSpace(msg))
	// Set write deadline to prevent hanging if client stops reading
	if err := conn.SetWriteDeadline(time.Now().Add(writeTimeout)); err != nil {
		return err
	}
	_, err := conn.Write([]byte(msg))
	return err
}

func main() {
	l1Port := flag.Int("l1-port", 5009, "L1 (quotes) port")
	lookupPort := flag.Int("lookup-port", 9100, "Lookup (symbol search) port")
	flag.Parse()

	server := NewMockIQFeedServer(*l1Port, *lookupPort)

	if err := server.Start(); err != nil {
		log.Fatalf("Failed to start server: %v", err)
	}

	log.Printf("Mock IQFeed server running on L1=%d, Lookup=%d. Press Ctrl+C to stop.", *l1Port, *lookupPort)

	// Handle graceful shutdown
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	<-sigChan

	log.Println("Shutting down...")
	server.Stop()
}
