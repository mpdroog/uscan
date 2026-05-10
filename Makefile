# Pre-market Gap Scanner
# Requires: brew install glfw sqlite3

CXX := clang++

# Base flags for all code
BASE_FLAGS := -std=c++17 -fno-common -fstack-protector-strong

# Strict flags for our code only
STRICT_FLAGS := \
	-Wall -Wextra -Wpedantic -Werror \
	-Wconversion -Wsign-conversion \
	-Wshadow -Wdouble-promotion \
	-Wformat=2 -Wformat-security \
	-Wnull-dereference -Wuninitialized \
	-Wstrict-aliasing=2 -Wcast-align \
	-Wold-style-cast -Woverloaded-virtual \
	-Wno-unused-parameter \
	-fno-exceptions \
	-Wthread-safety \
	-Wthread-safety-beta \
	-D_FORTIFY_SOURCE=2

# Relaxed flags for vendor code (third-party libraries)
VENDOR_FLAGS := -Wall -Wno-everything

# Build modes: Release, Debug (ASan+UBSan), or ThreadSanitizer
DEBUG ?= 0
TSAN ?= 0

# Track build mode to force rebuild when switching modes
BUILD_MODE_FILE := .build_mode
ifeq ($(TSAN), 1)
	BUILD_MODE := tsan
else ifeq ($(DEBUG), 1)
	BUILD_MODE := debug
else
	BUILD_MODE := release
endif

# Read previous build mode
PREV_BUILD_MODE := $(shell cat $(BUILD_MODE_FILE) 2>/dev/null || echo "none")

# Force clean if build mode changed
ifneq ($(BUILD_MODE),$(PREV_BUILD_MODE))
$(info Build mode changed from $(PREV_BUILD_MODE) to $(BUILD_MODE) - forcing rebuild)
FORCE_CLEAN := 1
endif

ifeq ($(TSAN), 1)
	# ThreadSanitizer mode (cannot combine with ASan)
	BASE_FLAGS += -g -O1 -DDEBUG -fsanitize=thread,undefined
	BASE_FLAGS += -fno-omit-frame-pointer -fno-optimize-sibling-calls
	LDFLAGS += -fsanitize=thread,undefined
	export TSAN_OPTIONS := halt_on_error=1 history_size=7 second_deadlock_stack=1
else ifeq ($(DEBUG), 1)
	# Debug mode with AddressSanitizer + UndefinedBehaviorSanitizer
	BASE_FLAGS += -g -O0 -DDEBUG -fsanitize=address,undefined
	LDFLAGS += -fsanitize=address,undefined
else
	# Release mode (optimized, no sanitizers)
	BASE_FLAGS += -O2 -DNDEBUG -flto
	LDFLAGS += -flto
endif

# macOS frameworks and libraries
LDFLAGS += -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
LDFLAGS += -L/opt/homebrew/lib -L/usr/local/lib
LDFLAGS += -lglfw -lsqlite3

# Include paths
INCLUDES := -I/opt/homebrew/include -I/usr/local/include
INCLUDES += -Ivendor/imgui -Ivendor/imgui/backends
INCLUDES += -Isrc

# Flags for different targets
CXXFLAGS_APP := $(BASE_FLAGS) $(STRICT_FLAGS) $(INCLUDES)
CXXFLAGS_VENDOR := $(BASE_FLAGS) $(VENDOR_FLAGS) $(INCLUDES)

# Source files
IMGUI_SRC := vendor/imgui/imgui.cpp \
	vendor/imgui/imgui_draw.cpp \
	vendor/imgui/imgui_tables.cpp \
	vendor/imgui/imgui_widgets.cpp \
	vendor/imgui/backends/imgui_impl_glfw.cpp \
	vendor/imgui/backends/imgui_impl_opengl3.cpp

APP_SRC := src/main.cpp \
	src/iqfeed_client.cpp \
	src/symbol_db.cpp \
	src/scanner.cpp \
	src/db_worker.cpp

IMGUI_OBJ := $(IMGUI_SRC:.cpp=.o)
APP_OBJ := $(APP_SRC:.cpp=.o)
OBJ := $(IMGUI_OBJ) $(APP_OBJ)

TARGET := uscan

.PHONY: all clean vendor run test test-tsan test-threading test-integration test-all

all: vendor
ifdef FORCE_CLEAN
	@rm -f $(OBJ) $(TARGET) tests/*.o
	@echo "$(BUILD_MODE)" > $(BUILD_MODE_FILE)
endif
	@$(MAKE) $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

# Compile vendor code with relaxed flags
vendor/imgui/%.o: vendor/imgui/%.cpp
	$(CXX) $(CXXFLAGS_VENDOR) -c $< -o $@

vendor/imgui/backends/%.o: vendor/imgui/backends/%.cpp
	$(CXX) $(CXXFLAGS_VENDOR) -c $< -o $@

# Compile our code with strict flags
src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS_APP) -c $< -o $@

vendor:
	@if [ ! -d "vendor/imgui" ]; then \
		echo "Downloading Dear ImGui..."; \
		mkdir -p vendor; \
		curl -sL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.tar.gz | tar -xz -C vendor; \
		mv vendor/imgui-1.91.8 vendor/imgui; \
	fi

clean:
	rm -f $(OBJ) $(TARGET)
	rm -f tests/test_runner tests/test_integration_runner tests/*.o tests/test_thread_safety
	rm -f tests/mock_iqfeed_server
	rm -f $(BUILD_MODE_FILE)

distclean: clean
	rm -rf vendor/imgui

run: all
	./$(TARGET)

# Unit tests
TEST_SRC := tests/test_main.cpp tests/test_types.cpp tests/test_symbol_db.cpp tests/test_iqfeed_parsing.cpp tests/test_scanner.cpp tests/test_async_nonblocking.cpp
TEST_OBJ := $(TEST_SRC:.cpp=.o)
TEST_APP_OBJ := src/symbol_db.o src/iqfeed_client.o src/scanner.o src/db_worker.o

tests/%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS_APP) -Itests -c $< -o $@

test: vendor $(TEST_OBJ) $(TEST_APP_OBJ)
	$(CXX) $(TEST_OBJ) $(TEST_APP_OBJ) -o tests/test_runner $(LDFLAGS)
	./tests/test_runner

# Integration tests with mock IQFeed server
test-integration:
	./tests/run_integration_tests.sh

# Thread safety tests with ThreadSanitizer
test-tsan:
	@echo "Building with ThreadSanitizer..."
	$(MAKE) clean
	TSAN=1 $(MAKE) test-threading
	@echo ""
	@echo "Running thread safety tests with ThreadSanitizer..."
	@echo "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:history_size=7"
	TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:history_size=7" ./tests/test_thread_safety

# Build and run threading tests (use TSAN=1 for sanitizer)
test-threading: tests/test_thread_safety.cpp src/iqfeed_client.o src/symbol_db.o src/scanner.o src/db_worker.o
	$(CXX) $(CXXFLAGS_APP) -Wno-unused-result tests/test_thread_safety.cpp \
		src/iqfeed_client.o src/symbol_db.o src/scanner.o src/db_worker.o \
		-o tests/test_thread_safety $(LDFLAGS)

# Run all tests with sanitizers
test-all: test test-integration test-tsan
	@echo ""
	@echo "All tests passed!"

# Dependencies
src/main.o: src/types.hpp src/scanner.hpp src/iqfeed_client.hpp src/symbol_db.hpp
src/iqfeed_client.o: src/iqfeed_client.hpp src/types.hpp
src/symbol_db.o: src/symbol_db.hpp src/types.hpp src/thread_safety.h
src/scanner.o: src/scanner.hpp src/types.hpp src/iqfeed_client.hpp src/db_worker.hpp
src/db_worker.o: src/db_worker.hpp src/symbol_db.hpp src/thread_safety.h
tests/test_types.o: src/types.hpp
tests/test_symbol_db.o: src/symbol_db.hpp src/types.hpp
