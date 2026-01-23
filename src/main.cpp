#include "../include/engine_sim_application.h"

#include <cstdio>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    std::fprintf(stderr, "[engine-sim] starting\n");
    std::fflush(stderr);

    EngineSimApplication application;
    application.initialize(nullptr, ysContextObject::DeviceAPI::OpenGL4_0);
    application.run();
    application.destroy();

    std::fprintf(stderr, "[engine-sim] exiting\n");
    std::fflush(stderr);

    return 0;
}
