#include "types.hpp"
#include "scanner.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <cstring>
#include <algorithm>

namespace {

// Window title
constexpr const char* WINDOW_TITLE = "Pre-Market Gap Scanner";

// GLFW error callback
void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// Format large numbers with K/M suffixes
USCAN_NODISCARD const char* format_volume(int64_t vol, char* buf, std::size_t buf_size) {
    if (vol >= 1'000'000) {
        std::snprintf(buf, buf_size, "%.2fM", static_cast<double>(vol) / 1'000'000.0);
    } else if (vol >= 1'000) {
        std::snprintf(buf, buf_size, "%.1fK", static_cast<double>(vol) / 1'000.0);
    } else {
        std::snprintf(buf, buf_size, "%" PRId64, vol);
    }
    return buf;
}

// Format float shares
USCAN_NODISCARD const char* format_float(int64_t shares, char* buf, std::size_t buf_size) {
    if (shares <= 0) {
        std::snprintf(buf, buf_size, "N/A");
    } else if (shares >= 1'000'000'000) {
        std::snprintf(buf, buf_size, "%.2fB", static_cast<double>(shares) / 1'000'000'000.0);
    } else if (shares >= 1'000'000) {
        std::snprintf(buf, buf_size, "%.2fM", static_cast<double>(shares) / 1'000'000.0);
    } else if (shares >= 1'000) {
        std::snprintf(buf, buf_size, "%.1fK", static_cast<double>(shares) / 1'000.0);
    } else {
        std::snprintf(buf, buf_size, "%" PRId64, shares);
    }
    return buf;
}

// Simple spinner animation (updates every 10 frames for readability)
const char* get_spinner_char() {
    static const char* spinner = "/-\\|";
    static char buf[2] = {0};
    static int counter = 0;
    static int frame_skip = 0;

    if (++frame_skip >= 10) {
        counter = (counter + 1) % 4;
        frame_skip = 0;
    }

    buf[0] = spinner[counter];
    buf[1] = '\0';
    return buf;
}

// Render status bar
void render_status_bar(const uscan::Scanner& scanner) {
    ImGui::Separator();

    // Connection state
    const auto conn_state = scanner.connection_state();
    ImVec4 conn_color;
    switch (conn_state) {
        case uscan::ConnectionState::Connected:
            conn_color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
            break;
        case uscan::ConnectionState::Connecting:
            conn_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            break;
        case uscan::ConnectionState::Error:
            conn_color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
            break;
        default:
            conn_color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            break;
    }

    // First line: Connection, State, and basic stats
    ImGui::TextColored(conn_color, "IQFeed: %s", uscan::to_string(conn_state));
    ImGui::SameLine();

    const auto state = scanner.state();
    const auto progress = scanner.progress();

    if (state == uscan::ScannerState::LoadingSymbols ||
        state == uscan::ScannerState::Subscribing ||
        state == uscan::ScannerState::Connecting) {
        ImGui::Text(" | State: %s %s", uscan::to_string(state), get_spinner_char());
    } else {
        ImGui::Text(" | State: %s", uscan::to_string(state));
    }
    ImGui::SameLine();

    ImGui::Text(" | Watching: %zu", scanner.symbols_watched());
    ImGui::SameLine();
    ImGui::Text(" | DB: %zu", scanner.symbols_in_db());
    ImGui::SameLine();
    ImGui::Text(" | Msgs: %zu", scanner.messages_received());

    // Second line: Progress details (if in progress)
    if (progress.step_number > 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                          "Progress: Step %d/%d - %s",
                          progress.step_number, progress.total_steps,
                          progress.current_step.c_str());
    }

    if (scanner.using_fallback_range()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), " | Using extended price range ($1-$100)");
    }

    // Show error if any
    if (!scanner.last_error().empty() && scanner.state() == uscan::ScannerState::Error) {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: %s", scanner.last_error().c_str());
    }

    // Show warnings (non-fatal errors)
    if (!scanner.last_error().empty() && scanner.state() != uscan::ScannerState::Error) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "%s", scanner.last_error().c_str());
    }
}

// Render gapper table
void render_gapper_table(const std::vector<uscan::Gapper>& gappers) {
    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SortMulti |
        ImGuiTableFlags_ScrollY;

    constexpr int column_count = 11;

    if (ImGui::BeginTable("GappersTable", column_count, table_flags)) {
        // Setup columns
        ImGui::TableSetupScrollFreeze(0, 1);  // Freeze header row
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Gap %", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 70.0f);
        ImGui::TableSetupColumn("Gap $", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("PM Vol", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Avg Vol", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Float", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("52w High", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("52w Low", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Sector", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Render rows
        char buf[64];
        int row_num = 1;

        for (const auto& gapper : gappers) {
            ImGui::TableNextRow();

            // Row number
            ImGui::TableNextColumn();
            ImGui::Text("%d", row_num++);

            // Symbol
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(gapper.symbol.c_str());

            // Price
            ImGui::TableNextColumn();
            ImGui::Text("$%.2f", gapper.price);

            // Gap %
            ImGui::TableNextColumn();
            const ImVec4 gap_color = gapper.gap_percent >= 0
                ? ImVec4(0.0f, 0.8f, 0.0f, 1.0f)
                : ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
            ImGui::TextColored(gap_color, "%+.2f%%", gapper.gap_percent);

            // Gap $
            ImGui::TableNextColumn();
            ImGui::TextColored(gap_color, "%+.2f", gapper.gap_dollars);

            // Pre-market volume
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_volume(gapper.premarket_volume, buf, sizeof(buf)));

            // Average volume
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_volume(gapper.avg_volume, buf, sizeof(buf)));

            // Float
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_float(gapper.float_shares, buf, sizeof(buf)));

            // 52-week high
            ImGui::TableNextColumn();
            if (gapper.high_52wk > 0) {
                ImGui::Text("$%.2f", gapper.high_52wk);
            } else {
                ImGui::TextUnformatted("N/A");
            }

            // 52-week low
            ImGui::TableNextColumn();
            if (gapper.low_52wk > 0) {
                ImGui::Text("$%.2f", gapper.low_52wk);
            } else {
                ImGui::TextUnformatted("N/A");
            }

            // Sector
            ImGui::TableNextColumn();
            if (!gapper.sector.empty()) {
                ImGui::TextUnformatted(gapper.sector.c_str());
            } else {
                ImGui::TextDisabled("--");
            }
        }

        ImGui::EndTable();
    }
}

// Render main window
void render_main_window(uscan::Scanner& scanner) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("MainWindow", nullptr, window_flags);

    // Header with controls
    ImGui::Text("Pre-Market Gap Scanner");
    ImGui::SameLine();

    const bool is_loading = (scanner.state() == uscan::ScannerState::Connecting ||
                             scanner.state() == uscan::ScannerState::LoadingSymbols ||
                             scanner.state() == uscan::ScannerState::Subscribing);
    if (is_loading) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button("Refresh Symbols")) {
        scanner.refresh_symbols();
    }

    if (is_loading) {
        ImGui::EndDisabled();
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Fetch fresh symbols from IQFeed (async via TCP SBF)");
    }

    ImGui::SameLine();
    ImGui::Text("| Min Gap: %.1f%% | Price: $%.0f-$%.0f | Min PM Vol: %lld",
                scanner.config().min_gap_percent,
                scanner.config().min_price,
                scanner.using_fallback_range() ? scanner.config().fallback_max_price : scanner.config().max_price,
                static_cast<long long>(scanner.config().min_premarket_volume));

    ImGui::Separator();

    // Gapper table
    const auto gappers = scanner.get_gappers();

    if (gappers.empty()) {
        const auto state = scanner.state();
        const auto progress = scanner.progress();

        // Show loading messages based on state with progress details
        if (state == uscan::ScannerState::Connecting ||
            state == uscan::ScannerState::LoadingSymbols ||
            state == uscan::ScannerState::Subscribing) {

            // Show step progress
            if (progress.step_number > 0) {
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                    "%s Step %d/%d: %s",
                    get_spinner_char(),
                    progress.step_number,
                    progress.total_steps,
                    progress.current_step.c_str());

                // Show progress bar if step has progress percentage
                if (progress.step_progress >= 0 && progress.step_progress <= 100) {
                    const float progress_fraction = static_cast<float>(progress.step_progress) / 100.0f;
                    char overlay[32];
                    std::snprintf(overlay, sizeof(overlay), "%d%%", progress.step_progress);
                    ImGui::ProgressBar(progress_fraction, ImVec2(-1.0f, 0.0f), overlay);
                }

                ImGui::TextDisabled("Async operation in progress (non-blocking)");
            } else {
                // Fallback if progress not set
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                    "%s %s...", get_spinner_char(), uscan::to_string(state));
                ImGui::TextDisabled("Async operation in progress");
            }
        } else if (state == uscan::ScannerState::Scanning) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                "No gappers found matching criteria. Waiting for pre-market data...");
            ImGui::TextDisabled("Monitoring %zu symbols for gaps >= %.1f%%",
                               scanner.symbols_watched(),
                               scanner.config().min_gap_percent);
        } else if (state == uscan::ScannerState::Error) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Error occurred. Check status bar for details.");
        } else {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Initializing scanner...");
        }
    } else {
        ImGui::Text("Top %zu Gappers:", gappers.size());
        ImGui::SameLine();
        ImGui::TextDisabled("(Async DB writes batched every 5s)");
        render_gapper_table(gappers);
    }

    // Status bar at bottom
    render_status_bar(scanner);

    ImGui::End();
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    // Parse command-line arguments
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: %s [OPTIONS]\n", argv[0]);
            std::printf("Options:\n");
            std::printf("  -v, --verbose    Enable verbose logging (shows TCP commands and SQLite operations)\n");
            std::printf("  -h, --help       Show this help message\n");
            return 0;
        }
    }

    // Setup GLFW
    glfwSetErrorCallback(glfw_error_callback);

    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    // macOS: request OpenGL 3.2 core profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    // Get primary monitor work area for auto-scaling to screen size
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    int monitor_x, monitor_y, monitor_width, monitor_height;
    glfwGetMonitorWorkarea(primary, &monitor_x, &monitor_y, &monitor_width, &monitor_height);

    // Use reasonable size: 1600x900 or 75% of screen, whichever is smaller
    const int max_width = std::min(1600, static_cast<int>(monitor_width * 0.75));
    const int max_height = std::min(900, static_cast<int>(monitor_height * 0.75));
    const int window_width = max_width;
    const int window_height = max_height;

    // Create window with auto-scaled size
    GLFWwindow* window = glfwCreateWindow(window_width, window_height, WINDOW_TITLE, nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    // Center window on screen
    const int pos_x = monitor_x + (monitor_width - window_width) / 2;
    const int pos_y = monitor_y + (monitor_height - window_height) / 2;
    glfwSetWindowPos(window, pos_x, pos_y);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // Enable vsync

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(15, 15);
    style.FramePadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(12, 8);
    style.ScrollbarSize = 16;

    // Increase font scale for better readability
    io.FontGlobalScale = 1.2f;

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // Initialize scanner
    uscan::Config config;
    // Config is already set with defaults matching user's requirements:
    // min_gap_percent = 5.0, min_premarket_volume = 50000, gap_up_only = true
    // min_price = 1.0, max_price = 20.0, fallback_max_price = 100.0
    config.verbose = verbose;

    // Enable global verbose logging
    uscan::set_verbose(verbose);

    if (verbose) {
        std::fprintf(stderr, "[VERBOSE] Verbose logging enabled\n");
    }

    uscan::Scanner scanner(std::move(config));

    auto init_result = scanner.initialize();
    if (init_result.failed()) {
        std::fprintf(stderr, "Failed to initialize scanner: %s\n", init_result.error.c_str());
        // Continue anyway - will show error in UI
    }

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Update scanner
        scanner.update();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render UI
        render_main_window(scanner);

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    scanner.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
