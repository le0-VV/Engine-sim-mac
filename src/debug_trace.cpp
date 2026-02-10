#include "../include/debug_trace.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>

#if defined(_WIN32)
#include <process.h>
#define ATG_GETPID _getpid
#else
#include <unistd.h>
#define ATG_GETPID getpid
#endif

namespace {
struct TraceState {
    bool enabled = false;
    std::string sessionDirectory;
    std::mutex lock;
    std::unordered_map<std::string, std::unique_ptr<std::ofstream>> streams;
    std::chrono::steady_clock::time_point monotonicStart = std::chrono::steady_clock::now();
    std::atomic<unsigned long long> frameIndex{0};
};

TraceState g_traceState;

std::string sanitizeComponentName(const char *component) {
    if (component == nullptr || component[0] == '\0') return "main";

    std::string sanitized(component);
    for (char &c : sanitized) {
        const bool valid =
            (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '_'
            || c == '-';
        if (!valid) c = '_';
    }

    return sanitized;
}

std::string buildDefaultSessionDirectory() {
    const auto now = std::chrono::system_clock::now();
    const auto nowTime = std::chrono::system_clock::to_time_t(now);
    const std::tm localTime = *std::localtime(&nowTime);

    std::ostringstream sessionName;
    sessionName << std::put_time(&localTime, "%Y%m%d-%H%M%S");
    sessionName << "-pid" << static_cast<long long>(ATG_GETPID());

    const std::filesystem::path root = std::filesystem::path("logs") / "debug";
    return (root / sessionName.str()).string();
}

std::string resolveSessionDirectoryFromArguments(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string prefix = "--debug-trace=";
        if (arg.rfind(prefix, 0) == 0) {
            const std::string value = arg.substr(prefix.size());
            if (!value.empty()) return value;
            return buildDefaultSessionDirectory();
        }

        if (arg == "--debug-trace") {
            if (i + 1 < argc && argv[i + 1] != nullptr && argv[i + 1][0] != '-') {
                return argv[i + 1];
            }

            return buildDefaultSessionDirectory();
        }
    }

    return "";
}

std::string timestampNow() {
    const auto now = std::chrono::system_clock::now();
    const auto nowTime = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::tm localTime = *std::localtime(&nowTime);

    std::ostringstream timestamp;
    timestamp << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    timestamp << "." << std::setfill('0') << std::setw(3) << millis.count();

    return timestamp.str();
}

long long monotonicMillisNow() {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - g_traceState.monotonicStart).count();
}

std::ofstream *getStreamForComponent(const std::string &component) {
    auto found = g_traceState.streams.find(component);
    if (found != g_traceState.streams.end()) return found->second.get();

    const std::filesystem::path path =
        std::filesystem::path(g_traceState.sessionDirectory) / (component + ".log");
    auto stream = std::make_unique<std::ofstream>(path.string(), std::ios::out | std::ios::app);
    if (!stream->is_open()) return nullptr;

    std::ofstream *raw = stream.get();
    g_traceState.streams[component] = std::move(stream);
    return raw;
}
} /* namespace */

bool DebugTrace::InitializeFromArguments(int argc, char **argv) {
    const std::string requestedDirectory = resolveSessionDirectoryFromArguments(argc, argv);
    if (requestedDirectory.empty()) {
        g_traceState.enabled = false;
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(requestedDirectory, ec);
    if (ec) {
        std::fprintf(
            stderr,
            "[engine-sim] debug trace setup failed for '%s': %s\n",
            requestedDirectory.c_str(),
            ec.message().c_str());
        std::fflush(stderr);
        g_traceState.enabled = false;
        return false;
    }

    g_traceState.sessionDirectory = requestedDirectory;
    g_traceState.enabled = true;
    g_traceState.monotonicStart = std::chrono::steady_clock::now();
    g_traceState.frameIndex.store(0);

    Log("main", "debug trace enabled; session_dir=%s", g_traceState.sessionDirectory.c_str());
    Log(
        "main",
        "active categories: main app mainloop script window input simulator audio audio_thread delta_engine metal_device device error_system");
    return true;
}

void DebugTrace::Shutdown() {
    if (!g_traceState.enabled) return;

    Log("main", "debug trace shutting down");

    std::lock_guard<std::mutex> guard(g_traceState.lock);
    g_traceState.streams.clear();
    g_traceState.enabled = false;
}

bool DebugTrace::IsEnabled() {
    return g_traceState.enabled;
}

std::string DebugTrace::SessionDirectory() {
    return g_traceState.sessionDirectory;
}

void DebugTrace::SetFrameIndex(unsigned long long frameIndex) {
    g_traceState.frameIndex.store(frameIndex);
}

unsigned long long DebugTrace::GetFrameIndex() {
    return g_traceState.frameIndex.load();
}

void DebugTrace::Log(const char *component, const char *format, ...) {
    if (!g_traceState.enabled || format == nullptr) return;

    char messageBuffer[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(messageBuffer, sizeof(messageBuffer), format, args);
    va_end(args);

    const std::string componentName = sanitizeComponentName(component);
    const std::string timestamp = timestampNow();
    const long long monotonicMs = monotonicMillisNow();
    const unsigned long long frame = g_traceState.frameIndex.load();
    const auto threadIdHash = static_cast<unsigned long long>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));

    std::lock_guard<std::mutex> guard(g_traceState.lock);
    std::ofstream *stream = getStreamForComponent(componentName);
    if (stream == nullptr) return;

    (*stream) << "[" << timestamp << "]"
              << " [mono_ms=" << monotonicMs << "]"
              << " [frame=" << frame << "]"
              << " [tid=" << threadIdHash << "] "
              << messageBuffer
              << "\n";
    stream->flush();
}
