#ifndef ATG_ENGINE_SIM_DEBUG_TRACE_H
#define ATG_ENGINE_SIM_DEBUG_TRACE_H

#include <string>

class DebugTrace {
public:
    static bool InitializeFromArguments(int argc, char **argv);
    static void Shutdown();
    static void RequestDump(const char *reason);

    static bool IsEnabled();
    static std::string SessionDirectory();
    static void SetFrameIndex(unsigned long long frameIndex);
    static unsigned long long GetFrameIndex();

    static void Log(const char *component, const char *format, ...);
};

#endif /* ATG_ENGINE_SIM_DEBUG_TRACE_H */
