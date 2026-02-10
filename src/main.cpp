#include "../include/engine_sim_application.h"
#include "../include/debug_trace.h"

#include <exception>
#include <csignal>

namespace {
void EngineSimSignalHandler(int signalCode) {
    DebugTrace::Log("main", "signal handler triggered code=%d", signalCode);
    std::_Exit(128 + signalCode);
}
}

int main(int argc, char **argv) {
    DebugTrace::InitializeFromArguments(argc, argv);
    DebugTrace::Log("main", "installing terminate/signal handlers");

    std::set_terminate([]() {
        DebugTrace::Log("main", "std::terminate triggered");
        std::abort();
    });

    std::signal(SIGABRT, EngineSimSignalHandler);
    std::signal(SIGSEGV, EngineSimSignalHandler);
    std::signal(SIGBUS, EngineSimSignalHandler);

    EngineSimApplication application;
#if defined(__APPLE__)
    application.initialize(nullptr, ysContextObject::DeviceAPI::Metal);
#else
    application.initialize(nullptr, ysContextObject::DeviceAPI::OpenGL4_0);
#endif
    application.run();
    application.destroy();
    DebugTrace::Shutdown();

    return 0;
}
