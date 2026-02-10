#include "../include/debug_trace.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <cctype>
#include <vector>
#include <algorithm>

#if defined(_WIN32)
#include <process.h>
#define ATG_GETPID _getpid
#else
#include <unistd.h>
#define ATG_GETPID getpid
#endif

namespace {
struct RingRecord {
    long long monoMs = 0;
    unsigned long long frame = 0;
    unsigned long long tid = 0;
    char component[32]{};
    char message[512]{};
};

struct SnapshotBucket {
    long long windowStartMs = -1;
    uint64_t sampleCount = 0;
    std::unordered_map<std::string, uint64_t> tokenCounts;
};

struct TraceState {
    bool enabled = false;
    bool sinkFile = true;
    bool sinkStdout = false;
    bool sinkRing = false;
    bool jsonEnabled = false;
    std::string sessionDirectory;
    std::mutex lock;
    std::unordered_map<std::string, std::unique_ptr<std::ofstream>> streams;
    std::unique_ptr<std::ofstream> jsonStream;
    std::chrono::steady_clock::time_point monotonicStart = std::chrono::steady_clock::now();
    std::atomic<unsigned long long> frameIndex{0};
    std::atomic<bool> dumpRequested{false};
    std::string dumpReason = "requested";
    std::vector<RingRecord> ring;
    size_t ringCapacity = 32768;
    size_t ringWriteIndex = 0;
    bool ringWrapped = false;
    int snapshotIntervalMs = 1000;
    bool snapshotMode = true;
    std::unordered_map<std::string, SnapshotBucket> snapshotBuckets;
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

void configureSinksFromArguments(int argc, char **argv) {
    g_traceState.sinkFile = true;
    g_traceState.sinkStdout = false;
    g_traceState.sinkRing = false;
    g_traceState.jsonEnabled = false;
    g_traceState.snapshotIntervalMs = 1000;
    g_traceState.snapshotMode = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string sinksPrefix = "--debug-trace-sinks=";
        const std::string jsonPrefix = "--debug-trace-json=";
        const std::string highFreqPrefix = "--debug-trace-highfreq-ms=";
        const std::string snapshotPrefix = "--debug-trace-snapshot-ms=";
        const std::string snapshotModePrefix = "--debug-trace-snapshot=";
        if (arg.rfind(sinksPrefix, 0) == 0) {
            g_traceState.sinkFile = false;
            g_traceState.sinkStdout = false;
            g_traceState.sinkRing = false;
            const std::string sinks = arg.substr(sinksPrefix.size());
            if (sinks.find("file") != std::string::npos) g_traceState.sinkFile = true;
            if (sinks.find("stdout") != std::string::npos) g_traceState.sinkStdout = true;
            if (sinks.find("ring") != std::string::npos) g_traceState.sinkRing = true;
        }
        else if (arg.rfind(jsonPrefix, 0) == 0) {
            const std::string jsonValue = arg.substr(jsonPrefix.size());
            g_traceState.jsonEnabled = (jsonValue == "1" || jsonValue == "true" || jsonValue == "on");
        }
        else if (arg.rfind(highFreqPrefix, 0) == 0) {
            const std::string interval = arg.substr(highFreqPrefix.size());
            const int parsed = std::atoi(interval.c_str());
            g_traceState.snapshotIntervalMs = (parsed >= 0) ? parsed : 1000;
        }
        else if (arg.rfind(snapshotPrefix, 0) == 0) {
            const std::string interval = arg.substr(snapshotPrefix.size());
            const int parsed = std::atoi(interval.c_str());
            g_traceState.snapshotIntervalMs = (parsed >= 0) ? parsed : 1000;
        }
        else if (arg.rfind(snapshotModePrefix, 0) == 0) {
            const std::string value = arg.substr(snapshotModePrefix.size());
            g_traceState.snapshotMode = !(value == "0" || value == "false" || value == "off");
        }
    }
}

bool isCriticalEventMessage(const char *text) {
    if (text == nullptr) return false;
    std::string lower(text);
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char *keywords[] = {
        "error",
        "failed",
        "warning",
        "anomaly",
        "overflow",
        "invalid",
        "rejected",
        "fallback",
        "stall_",
        "watchdog",
        "abort",
        "exception",
        "panic",
        "crash",
        "spike",
        "assert"
    };
    for (const char *k : keywords) {
        if (lower.find(k) != std::string::npos) return true;
    }
    return false;
}

std::string messageToken(const char *message) {
    std::string token;
    if (message == nullptr) return "message";
    for (const char *p = message; *p != '\0'; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '=' || *p == ':') break;
        token.push_back(*p);
    }
    if (token.empty()) token = "message";
    return token;
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

void appendRingRecord(
    long long monoMs,
    unsigned long long frame,
    unsigned long long tid,
    const std::string &component,
    const char *message);

void emitLogToSinksLocked(
    const std::string &componentName,
    const std::string &timestamp,
    long long monotonicMs,
    unsigned long long frame,
    unsigned long long threadIdHash,
    const char *message)
{
    if (message == nullptr) return;

    if (g_traceState.sinkFile) {
        std::ofstream *stream = getStreamForComponent(componentName);
        if (stream != nullptr) {
            (*stream) << "[" << timestamp << "]"
                      << " [mono_ms=" << monotonicMs << "]"
                      << " [frame=" << frame << "]"
                      << " [tid=" << threadIdHash << "] "
                      << message
                      << "\n";
            stream->flush();
        }
    }

    if (g_traceState.sinkStdout) {
        std::fprintf(
            stdout,
            "[%s] [mono_ms=%lld] [frame=%llu] [tid=%llu] [%s] %s\n",
            timestamp.c_str(),
            monotonicMs,
            frame,
            threadIdHash,
            componentName.c_str(),
            message);
        std::fflush(stdout);
    }

    if (g_traceState.jsonEnabled && g_traceState.jsonStream && g_traceState.jsonStream->is_open()) {
        (*g_traceState.jsonStream)
            << "{\"ts\":\"" << timestamp
            << "\",\"mono_ms\":" << monotonicMs
            << ",\"frame\":" << frame
            << ",\"tid\":" << threadIdHash
            << ",\"component\":\"" << componentName
            << "\",\"msg\":\"";
        for (const char *p = message; *p != '\0'; ++p) {
            if (*p == '"' || *p == '\\') (*g_traceState.jsonStream) << '\\';
            (*g_traceState.jsonStream) << *p;
        }
        (*g_traceState.jsonStream) << "\"}\n";
        g_traceState.jsonStream->flush();
    }

    appendRingRecord(monotonicMs, frame, threadIdHash, componentName, message);
}

void flushSnapshotBucketLocked(
    const std::string &componentName,
    SnapshotBucket &bucket,
    const std::string &timestamp,
    long long monotonicMs,
    unsigned long long frame,
    unsigned long long threadIdHash)
{
    if (bucket.sampleCount == 0) return;

    std::vector<std::pair<std::string, uint64_t>> tokens(bucket.tokenCounts.begin(), bucket.tokenCounts.end());
    std::sort(tokens.begin(), tokens.end(), [](const std::pair<std::string, uint64_t> &a, const std::pair<std::string, uint64_t> &b) {
        return a.second > b.second;
    });
    if (tokens.size() > 6) tokens.resize(6);

    std::ostringstream ss;
    ss << "snapshot interval_ms=" << g_traceState.snapshotIntervalMs
       << " samples=" << bucket.sampleCount
       << " tokens=";
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) ss << ",";
        ss << tokens[i].first << ":" << tokens[i].second;
    }

    const std::string summary = ss.str();
    emitLogToSinksLocked(componentName, timestamp, monotonicMs, frame, threadIdHash, summary.c_str());

    bucket.sampleCount = 0;
    bucket.tokenCounts.clear();
    bucket.windowStartMs = monotonicMs;
}

void appendRingRecord(
    long long monoMs,
    unsigned long long frame,
    unsigned long long tid,
    const std::string &component,
    const char *message)
{
    if (!g_traceState.sinkRing || g_traceState.ringCapacity == 0) return;
    if (g_traceState.ring.empty()) g_traceState.ring.resize(g_traceState.ringCapacity);

    RingRecord &record = g_traceState.ring[g_traceState.ringWriteIndex];
    record.monoMs = monoMs;
    record.frame = frame;
    record.tid = tid;
    std::snprintf(record.component, sizeof(record.component), "%s", component.c_str());
    std::snprintf(record.message, sizeof(record.message), "%s", message);

    g_traceState.ringWriteIndex = (g_traceState.ringWriteIndex + 1) % g_traceState.ringCapacity;
    if (g_traceState.ringWriteIndex == 0) g_traceState.ringWrapped = true;
}

void flushRingBinaryLocked(const char *reason) {
    if (!g_traceState.sinkRing || g_traceState.ring.empty()) return;
    const std::filesystem::path outPath =
        std::filesystem::path(g_traceState.sessionDirectory) / "trace.ring.bin";
    std::ofstream out(outPath.string(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;

    struct RingHeader {
        uint32_t magic = 0x45535452; /* RSTE */
        uint32_t version = 1;
        uint64_t count = 0;
        char reason[64]{};
    } header;

    const size_t count = g_traceState.ringWrapped ? g_traceState.ringCapacity : g_traceState.ringWriteIndex;
    header.count = static_cast<uint64_t>(count);
    std::snprintf(header.reason, sizeof(header.reason), "%s", reason != nullptr ? reason : "request");
    out.write(reinterpret_cast<const char *>(&header), sizeof(header));

    auto writeRecord = [&](const RingRecord &record) {
        out.write(reinterpret_cast<const char *>(&record), sizeof(record));
    };

    if (g_traceState.ringWrapped) {
        for (size_t i = g_traceState.ringWriteIndex; i < g_traceState.ringCapacity; ++i) {
            writeRecord(g_traceState.ring[i]);
        }
        for (size_t i = 0; i < g_traceState.ringWriteIndex; ++i) {
            writeRecord(g_traceState.ring[i]);
        }
    }
    else {
        for (size_t i = 0; i < g_traceState.ringWriteIndex; ++i) {
            writeRecord(g_traceState.ring[i]);
        }
    }
}

void flushDumpIfRequestedLocked() {
    if (!g_traceState.dumpRequested.exchange(false)) return;
    flushRingBinaryLocked(g_traceState.dumpReason.c_str());
    std::ofstream *mainStream = getStreamForComponent("main");
    if (mainStream != nullptr) {
        (*mainStream) << "[dump] trace dump completed reason=" << g_traceState.dumpReason << "\n";
        mainStream->flush();
    }
}
} /* namespace */

bool DebugTrace::InitializeFromArguments(int argc, char **argv) {
    const std::string requestedDirectory = resolveSessionDirectoryFromArguments(argc, argv);
    if (requestedDirectory.empty()) {
        g_traceState.enabled = false;
        return false;
    }

    configureSinksFromArguments(argc, argv);

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
    g_traceState.dumpRequested.store(false);
    g_traceState.dumpReason = "startup";
    g_traceState.ring.clear();
    g_traceState.ringWriteIndex = 0;
    g_traceState.ringWrapped = false;
    g_traceState.snapshotBuckets.clear();

    if (g_traceState.jsonEnabled) {
        const std::filesystem::path jsonPath =
            std::filesystem::path(g_traceState.sessionDirectory) / "trace.jsonl";
        g_traceState.jsonStream = std::make_unique<std::ofstream>(jsonPath.string(), std::ios::out | std::ios::app);
    }
    else {
        g_traceState.jsonStream.reset();
    }

    Log("main", "debug trace enabled; session_dir=%s", g_traceState.sessionDirectory.c_str());
    Log(
        "main",
        "active categories: main app mainloop script window input simulator audio audio_thread delta_engine metal_device device error_system");
    Log(
        "main",
        "trace sinks file=%d stdout=%d ring=%d json=%d",
        g_traceState.sinkFile ? 1 : 0,
        g_traceState.sinkStdout ? 1 : 0,
        g_traceState.sinkRing ? 1 : 0,
        g_traceState.jsonEnabled ? 1 : 0);
    Log("main", "trace snapshot mode=%d interval_ms=%d", g_traceState.snapshotMode ? 1 : 0, g_traceState.snapshotIntervalMs);
    Log("main", "cumulative counters reset point=startup");
    return true;
}

void DebugTrace::Shutdown() {
    if (!g_traceState.enabled) return;

    Log("main", "cumulative counters reset point=shutdown");
    Log("main", "debug trace shutting down");

    std::lock_guard<std::mutex> guard(g_traceState.lock);
    const std::string shutdownTs = timestampNow();
    const long long shutdownMono = monotonicMillisNow();
    const unsigned long long shutdownFrame = g_traceState.frameIndex.load();
    const auto shutdownTid = static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    for (auto &entry : g_traceState.snapshotBuckets) {
        flushSnapshotBucketLocked(entry.first, entry.second, shutdownTs, shutdownMono, shutdownFrame, shutdownTid);
    }
    flushDumpIfRequestedLocked();
    flushRingBinaryLocked("shutdown");
    g_traceState.streams.clear();
    g_traceState.jsonStream.reset();
    g_traceState.enabled = false;
}

void DebugTrace::RequestDump(const char *reason) {
    if (!g_traceState.enabled) return;
    g_traceState.dumpReason = (reason != nullptr) ? reason : "manual";
    g_traceState.dumpRequested.store(true);
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
    if (g_traceState.snapshotMode && g_traceState.snapshotIntervalMs > 0 && !isCriticalEventMessage(messageBuffer)) {
        SnapshotBucket &bucket = g_traceState.snapshotBuckets[componentName];
        if (bucket.windowStartMs < 0) bucket.windowStartMs = monotonicMs;
        if (monotonicMs - bucket.windowStartMs >= g_traceState.snapshotIntervalMs) {
            flushSnapshotBucketLocked(componentName, bucket, timestamp, monotonicMs, frame, threadIdHash);
        }

        const std::string token = messageToken(messageBuffer);
        ++bucket.sampleCount;
        ++bucket.tokenCounts[token];
        flushDumpIfRequestedLocked();
        return;
    }

    emitLogToSinksLocked(componentName, timestamp, monotonicMs, frame, threadIdHash, messageBuffer);
    flushDumpIfRequestedLocked();
}
