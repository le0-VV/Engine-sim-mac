#include "../include/engine_sim_application.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    EngineSimApplication application;
#if defined(__APPLE__)
    application.initialize(nullptr, ysContextObject::DeviceAPI::Metal);
#else
    application.initialize(nullptr, ysContextObject::DeviceAPI::OpenGL4_0);
#endif
    application.run();
    application.destroy();

    return 0;
}
