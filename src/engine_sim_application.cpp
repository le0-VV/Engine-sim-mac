#include "../include/engine_sim_application.h"

#include "../include/piston_object.h"
#include "../include/connecting_rod_object.h"
#include "../include/constants.h"
#include "../include/units.h"
#include "../include/crankshaft_object.h"
#include "../include/cylinder_bank_object.h"
#include "../include/cylinder_head_object.h"
#include "../include/ui_button.h"
#include "../include/combustion_chamber_object.h"
#include "../include/csv_io.h"
#include "../include/exhaust_system.h"
#include "../include/feedback_comb_filter.h"
#include "../include/utilities.h"
#include "../include/debug_trace.h"

#include "../scripting/include/compiler.h"

#include <chrono>
#include <stdlib.h>
#include <sstream>
#include <cstdio>
#include <cstdarg>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <os/log.h>
#endif

#if ATG_ENGINE_SIM_DISCORD_ENABLED && defined(_WIN32)
#include "../discord/Discord.h"
#endif

namespace {
struct MemorySnapshot {
    double rssMb = 0.0;
    double footprintMb = 0.0;
    double iosurfaceMb = -1.0;
    double mallocMetadataMb = -1.0;
    bool valid = false;
};

int g_mouseWheelEventsThisSecond = 0;

MemorySnapshot captureMemorySnapshot() {
    MemorySnapshot snapshot;
#if defined(__APPLE__)
    mach_task_basic_info_data_t basicInfo{};
    mach_msg_type_number_t basicInfoCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(
            mach_task_self(),
            MACH_TASK_BASIC_INFO,
            reinterpret_cast<task_info_t>(&basicInfo),
            &basicInfoCount) == KERN_SUCCESS) {
        snapshot.rssMb = static_cast<double>(basicInfo.resident_size) / (1024.0 * 1024.0);
        snapshot.valid = true;
    }

    task_vm_info_data_t vmInfo{};
    mach_msg_type_number_t vmInfoCount = TASK_VM_INFO_COUNT;
    if (task_info(
            mach_task_self(),
            TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&vmInfo),
            &vmInfoCount) == KERN_SUCCESS) {
        snapshot.footprintMb = static_cast<double>(vmInfo.phys_footprint) / (1024.0 * 1024.0);
        snapshot.valid = true;
    }
#endif

    return snapshot;
}

int countWidgetsRecursive(const UiElement *node) {
    if (node == nullptr) return 0;
    int count = 1;
    const size_t children = node->getChildCount();
    for (size_t i = 0; i < children; ++i) {
        count += countWidgetsRecursive(node->getChild(i));
    }

    return count;
}

const char *deviceApiName(ysContextObject::DeviceAPI api) {
    switch (api) {
    case ysContextObject::DeviceAPI::DirectX10: return "DirectX10";
    case ysContextObject::DeviceAPI::DirectX11: return "DirectX11";
    case ysContextObject::DeviceAPI::OpenGL4_0: return "OpenGL4_0";
    case ysContextObject::DeviceAPI::Vulkan: return "Vulkan";
    case ysContextObject::DeviceAPI::Metal: return "Metal";
    default: return "Unknown";
    }
}

const char *ysErrorName(ysError error) {
    switch (error) {
    case ysError::None: return "None";
    case ysError::InvalidParameter: return "InvalidParameter";
    case ysError::IncompatiblePlatforms: return "IncompatiblePlatforms";
    case ysError::NoPlatform: return "NoPlatform";
    case ysError::InvalidOperation: return "InvalidOperation";
    case ysError::CouldNotCreateGraphicsDevice: return "CouldNotCreateGraphicsDevice";
    case ysError::CouldNotObtainDevice: return "CouldNotObtainDevice";
    case ysError::ApiError: return "ApiError";
    case ysError::CouldNotCreateContext: return "CouldNotCreateContext";
    case ysError::NoDevice: return "NoDevice";
    case ysError::NoRenderTarget: return "NoRenderTarget";
    case ysError::NoContext: return "NoContext";
    case ysError::NoWindowSystem: return "NoWindowSystem";
    default: return "Unknown";
    }
}

void startupLog(const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    std::fprintf(stderr, "[engine-sim] %s\n", buffer);
    std::fflush(stderr);

#if defined(__APPLE__)
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "engine-sim %{public}s", buffer);
#endif
}
} /* namespace */

std::string EngineSimApplication::s_buildVersion = "0.1.12a";

EngineSimApplication::EngineSimApplication() {
    m_assetPath = "";

    m_geometryVertexBuffer = nullptr;
    m_geometryIndexBuffer = nullptr;

    m_paused = false;
    m_recording = false;
    m_screenResolutionIndex = 0;
    for (int i = 0; i < ScreenResolutionHistoryLength; ++i) {
        m_screenResolution[i][0] = m_screenResolution[i][1] = 0;
    }

    m_background = ysColor::srgbiToLinear(0x0E1012);
    m_foreground = ysColor::srgbiToLinear(0xFFFFFF);
    m_shadow = ysColor::srgbiToLinear(0x0E1012);
    m_highlight1 = ysColor::srgbiToLinear(0xEF4545);
    m_highlight2 = ysColor::srgbiToLinear(0xFFFFFF);
    m_pink = ysColor::srgbiToLinear(0xF394BE);
    m_red = ysColor::srgbiToLinear(0xEE4445);
    m_orange = ysColor::srgbiToLinear(0xF4802A);
    m_yellow = ysColor::srgbiToLinear(0xFDBD2E);
    m_blue = ysColor::srgbiToLinear(0x77CEE0);
    m_green = ysColor::srgbiToLinear(0xBDD869);

    m_displayHeight = (float)units::distance(2.0, units::foot);
    m_outputAudioBuffer = nullptr;
    m_audioSource = nullptr;

    m_torque = 0;
    m_dynoSpeed = 0;

    m_simulator = nullptr;
    m_engineView = nullptr;
    m_rightGaugeCluster = nullptr;
    m_temperatureGauge = nullptr;
    m_oscCluster = nullptr;
    m_performanceCluster = nullptr;
    m_loadSimulationCluster = nullptr;
    m_mixerCluster = nullptr;
    m_infoCluster = nullptr;
    m_iceEngine = nullptr;
    m_mainRenderTarget = nullptr;

    m_vehicle = nullptr;
    m_transmission = nullptr;

    m_oscillatorSampleOffset = 0;
    m_gameWindowHeight = 256;
    m_screenWidth = 256;
    m_screenHeight = 256;
    m_screen = 0;
    m_viewParameters.Layer0 = 0;
    m_viewParameters.Layer1 = 0;

    m_displayAngle = 0.0f;
}

EngineSimApplication::~EngineSimApplication() {
    /* void */
}

void EngineSimApplication::initialize(void *instance, ysContextObject::DeviceAPI api) {
    DebugTrace::Log("app", "initialize(void*, api=%d) begin", static_cast<int>(api));

    dbasic::Path modulePath = dbasic::GetModulePath();
    dbasic::Path confPath = modulePath.Append("delta.conf");

    std::string enginePath = "../dependencies/submodules/delta-studio/engines/basic";
    m_assetPath = ".";
    if (confPath.Exists()) {
        std::fstream confFile(confPath.ToString(), std::ios::in);

        std::getline(confFile, enginePath);
        std::getline(confFile, m_assetPath);
        enginePath = modulePath.Append(enginePath).ToString();
        m_assetPath = modulePath.Append(m_assetPath).ToString();

        confFile.close();
    }
    else {
        const auto pathExists = [](const std::filesystem::path &p) {
            std::error_code ec;
            return std::filesystem::exists(p, ec) && !ec;
        };

        const std::filesystem::path moduleFs(modulePath.ToString());
        const std::filesystem::path bundledResources = moduleFs.parent_path() / "Resources";
        const std::filesystem::path bundledAssets = bundledResources / "assets";
        const std::filesystem::path bundledEngine = bundledResources / "delta-basic";
        const bool hasBundledAssets =
            pathExists(bundledAssets)
            && (pathExists(bundledAssets / "assets.ysce") || pathExists(bundledAssets / "assets.interchange"));
        const bool hasBundledEngine =
            pathExists(bundledEngine / "fonts") && pathExists(bundledEngine / "shaders");

        if (hasBundledAssets && hasBundledEngine) {
            m_assetPath = bundledResources.string();
            enginePath = bundledEngine.string();
            DebugTrace::Log(
                "app",
                "using bundled resources asset_path=%s engine_path=%s",
                m_assetPath.c_str(),
                enginePath.c_str());
        }
        else {
            dbasic::Path search = modulePath;
            bool foundRoot = false;
            for (int i = 0; i < 8; ++i) {
                const dbasic::Path assetsDir = search.Append("assets");
                const dbasic::Path engineDir = search.Append("dependencies/submodules/delta-studio/engines/basic");
                if (assetsDir.Exists() && engineDir.Exists()) {
                    m_assetPath = search.ToString();
                    enginePath = engineDir.ToString();
                    foundRoot = true;
                    break;
                }

                dbasic::Path parent;
                search.GetParentPath(&parent);
                search = parent;
            }

            if (!foundRoot) {
                m_assetPath = modulePath.ToString();
                enginePath = modulePath.Append("../dependencies/submodules/delta-studio/engines/basic").ToString();
            }
        }
    }

    m_engine.GetConsole()->SetDefaultFontDirectory(enginePath + "/fonts/");
    DebugTrace::Log(
        "app",
        "initialize() resolved paths engine=%s asset_root=%s",
        enginePath.c_str(),
        m_assetPath.c_str());
    std::fprintf(
        stderr,
        "[engine-sim] initialize() paths: engine=%s assets-root=%s\n",
        enginePath.c_str(),
        m_assetPath.c_str());
    std::fflush(stderr);

    const std::string shaderPath = enginePath + "/shaders/";
    const std::string winTitle = "Engine Sim | AngeTheGreat | v" + s_buildVersion;
    dbasic::DeltaEngine::GameEngineSettings settings;
    settings.API = api;
    settings.DepthBuffer = false;
    settings.Instance = instance;
    settings.ShaderDirectory = shaderPath.c_str();
    settings.WindowTitle = winTitle.c_str();
    settings.WindowPositionX = 0;
    settings.WindowPositionY = 0;
    settings.WindowStyle = ysWindow::WindowStyle::Windowed;
    settings.WindowWidth = 1920;
    settings.WindowHeight = 1080;

    auto createWindowWithApi = [&](ysContextObject::DeviceAPI selectedApi) {
        settings.API = selectedApi;
        startupLog("CreateGameWindow attempt api=%s", deviceApiName(selectedApi));
        const ysError err = m_engine.CreateGameWindow(settings);
        if (err == ysError::None) {
            startupLog("CreateGameWindow succeeded api=%s", deviceApiName(selectedApi));
        }
        else {
            startupLog(
                "CreateGameWindow failed api=%s code=%d(%s)",
                deviceApiName(selectedApi),
                static_cast<int>(err),
                ysErrorName(err));
        }

        return err;
    };

    const ysError createWindowError = createWindowWithApi(api);
    if (createWindowError != ysError::None) {
        DebugTrace::Log("app", "CreateGameWindow failed: code=%d", static_cast<int>(createWindowError));
        return;
    }
    DebugTrace::Log("app", "CreateGameWindow succeeded");

    m_engine.GetDevice()->CreateSubRenderTarget(
        &m_mainRenderTarget,
        m_engine.GetScreenRenderTarget(),
        0,
        0,
        0,
        0);

    m_engine.InitializeShaderSet(&m_shaderSet);
    m_shaders.Initialize(
        &m_shaderSet,
        m_mainRenderTarget,
        m_engine.GetScreenRenderTarget(),
        m_engine.GetDefaultShaderProgram(),
        m_engine.GetDefaultInputLayout());
    m_engine.InitializeConsoleShaders(&m_shaderSet);
    m_engine.SetShaderSet(&m_shaderSet);

    m_shaders.SetClearColor(ysColor::srgbiToLinear(0x34, 0x98, 0xdb));

    m_assetManager.SetEngine(&m_engine);

    m_engine.GetDevice()->CreateIndexBuffer(
        &m_geometryIndexBuffer, sizeof(unsigned short) * 200000, nullptr);
    m_engine.GetDevice()->CreateVertexBuffer(
        &m_geometryVertexBuffer, sizeof(dbasic::Vertex) * 100000, nullptr);

    m_geometryGenerator.initialize(100000, 200000);

    initialize();
    DebugTrace::Log("app", "initialize(void*, api=%d) complete", static_cast<int>(api));
}

void EngineSimApplication::initialize() {
    DebugTrace::Log("app", "initialize() begin; asset_root=%s", m_assetPath.c_str());
    m_shaders.SetClearColor(ysColor::srgbiToLinear(0x34, 0x98, 0xdb));
    const std::string assetsDir = m_assetPath + "/assets";
    const std::string assetsBase = assetsDir + "/assets";
    if (dbasic::Path(assetsDir).Exists()) {
        const std::string sceneFile = assetsBase + ".ysce";
        if (dbasic::Path(sceneFile).Exists()) {
            const auto ioStart = std::chrono::steady_clock::now();
            ysError loadErr = m_assetManager.LoadSceneFile(assetsBase.c_str(), true);
            const auto ioEnd = std::chrono::steady_clock::now();
            DebugTrace::Log(
                "assets",
                "asset_io_latency operation=LoadSceneFile path=%s elapsed_ms=%.3f",
                sceneFile.c_str(),
                std::chrono::duration_cast<std::chrono::microseconds>(ioEnd - ioStart).count() / 1000.0);
            if (loadErr != ysError::None) {
                std::fprintf(stderr, "[engine-sim] LoadSceneFile failed: %d\n", (int)loadErr);
                std::fflush(stderr);
                DebugTrace::Log("assets", "LoadSceneFile failed: code=%d", static_cast<int>(loadErr));
                return;
            }
            const int textures = m_assetManager.GetTextureCount();
            const int audioAssets = m_assetManager.GetAudioAssetCount();
            const int materials = m_assetManager.GetMaterialCount();
            const int sceneObjects = m_assetManager.GetSceneObjectCount();
            const int actions = m_assetManager.GetActionCount();
            static int s_lastAssetTotal = -1;
            const int currentTotal = textures + audioAssets + materials + sceneObjects + actions;
            const int hit = (s_lastAssetTotal == currentTotal) ? currentTotal : 0;
            const int miss = (s_lastAssetTotal >= 0) ? std::abs(currentTotal - s_lastAssetTotal) : currentTotal;
            s_lastAssetTotal = currentTotal;
            DebugTrace::Log(
                "assets",
                "asset summary textures=%d audio=%d materials=%d scene_objects=%d actions=%d cache_hit=%d cache_miss=%d",
                textures,
                audioAssets,
                materials,
                sceneObjects,
                actions,
                hit,
                miss);
        }
        else {
            const auto compileIoStart = std::chrono::steady_clock::now();
            ysError compileErr = m_assetManager.CompileInterchangeFile(assetsBase.c_str(), 1.0f, true);
            const auto compileIoEnd = std::chrono::steady_clock::now();
            DebugTrace::Log(
                "assets",
                "asset_io_latency operation=CompileInterchangeFile path=%s elapsed_ms=%.3f",
                assetsBase.c_str(),
                std::chrono::duration_cast<std::chrono::microseconds>(compileIoEnd - compileIoStart).count() / 1000.0);
            if (compileErr != ysError::None) {
                std::fprintf(stderr, "[engine-sim] CompileInterchangeFile failed: %d\n", (int)compileErr);
                std::fflush(stderr);
                DebugTrace::Log("assets", "CompileInterchangeFile failed: code=%d", static_cast<int>(compileErr));
                return;
            }
            const auto loadIoStart = std::chrono::steady_clock::now();
            ysError loadErr = m_assetManager.LoadSceneFile(assetsBase.c_str(), true);
            const auto loadIoEnd = std::chrono::steady_clock::now();
            DebugTrace::Log(
                "assets",
                "asset_io_latency operation=LoadSceneFile path=%s elapsed_ms=%.3f",
                assetsBase.c_str(),
                std::chrono::duration_cast<std::chrono::microseconds>(loadIoEnd - loadIoStart).count() / 1000.0);
            if (loadErr != ysError::None) {
                std::fprintf(stderr, "[engine-sim] LoadSceneFile failed: %d\n", (int)loadErr);
                std::fflush(stderr);
                DebugTrace::Log("assets", "LoadSceneFile after compile failed: code=%d", static_cast<int>(loadErr));
                return;
            }
            const int textures = m_assetManager.GetTextureCount();
            const int audioAssets = m_assetManager.GetAudioAssetCount();
            const int materials = m_assetManager.GetMaterialCount();
            const int sceneObjects = m_assetManager.GetSceneObjectCount();
            const int actions = m_assetManager.GetActionCount();
            static int s_lastAssetTotal = -1;
            const int currentTotal = textures + audioAssets + materials + sceneObjects + actions;
            const int hit = (s_lastAssetTotal == currentTotal) ? currentTotal : 0;
            const int miss = (s_lastAssetTotal >= 0) ? std::abs(currentTotal - s_lastAssetTotal) : currentTotal;
            s_lastAssetTotal = currentTotal;
            DebugTrace::Log(
                "assets",
                "asset summary textures=%d audio=%d materials=%d scene_objects=%d actions=%d cache_hit=%d cache_miss=%d",
                textures,
                audioAssets,
                materials,
                sceneObjects,
                actions,
                hit,
                miss);
        }
    }
    else {
        std::fprintf(stderr, "[engine-sim] assets path not found: %s\n", assetsDir.c_str());
        std::fflush(stderr);
        DebugTrace::Log("assets", "assets path not found: %s", assetsDir.c_str());
        return;
    }

    m_textRenderer.SetEngine(&m_engine);
    m_textRenderer.SetRenderer(m_engine.GetUiRenderer());
    m_textRenderer.SetFont(m_engine.GetConsole()->GetFont());

    loadScript();
    DebugTrace::Log("script", "initial script loaded");

    m_audioBuffer.initialize(44100, 44100);
    m_audioBuffer.m_writePointer = (int)(44100 * 0.1);

    ysAudioParameters params;
    params.m_bitsPerSample = 16;
    params.m_channelCount = 1;
    params.m_sampleRate = 44100;
    m_outputAudioBuffer =
        m_engine.GetAudioDevice()->CreateBuffer(&params, 44100);

    m_audioSource = m_engine.GetAudioDevice()->CreateSource(m_outputAudioBuffer);
    m_audioSource->SetMode((m_simulator != nullptr && m_simulator->getEngine() != nullptr)
        ? ysAudioSource::Mode::Loop
        : ysAudioSource::Mode::Stop);
    m_audioSource->SetPan(0.0f);
    m_audioSource->SetVolume(1.0f);
    DebugTrace::Log("audio", "audio source initialized");

#if ATG_ENGINE_SIM_DISCORD_ENABLED && defined(_WIN32)
    // Create a global instance of discord-rpc
    CDiscord::CreateInstance();

    // Enable it, this needs to be set via a config file of some sort. 
    GetDiscordManager()->SetUseDiscord(true);
    DiscordRichPresence passMe = { 0 };

    std::string engineName = (m_iceEngine != nullptr)
        ? m_iceEngine->getName()
        : "Broken Engine";

    GetDiscordManager()->SetStatus(passMe, engineName, s_buildVersion);
#endif /* ATG_ENGINE_SIM_DISCORD_ENABLED && _WIN32 */
    DebugTrace::Log("app", "initialize() complete");
}

void EngineSimApplication::process(float frame_dt) {
    frame_dt = static_cast<float>(clamp(frame_dt, 1 / 200.0f, 1 / 30.0f));

    static double s_lastSimulationSpeed = -1.0;
    double speed = 1.0 / 1.0;
    if (m_engine.IsKeyDown(ysKey::Code::N1)) {
        speed = 1 / 10.0;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::N2)) {
        speed = 1 / 100.0;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::N3)) {
        speed = 1 / 200.0;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::N4)) {
        speed = 1 / 500.0;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::N5)) {
        speed = 1 / 1000.0;
    }

    if (m_engine.IsKeyDown(ysKey::Code::F1)) {
        m_displayAngle += frame_dt * 1.0f;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::F2)) {
        m_displayAngle -= frame_dt * 1.0f;
    }
    else if (m_engine.ProcessKeyDown(ysKey::Code::F3)) {
        m_displayAngle = 0.0f;
    }

    if (s_lastSimulationSpeed != speed) {
        DebugTrace::Log(
            "simulator",
            "simulation_speed changed old=%.6f new=%.6f",
            s_lastSimulationSpeed,
            speed);
        s_lastSimulationSpeed = speed;
    }

    m_simulator->setSimulationSpeed(speed);

    const double avgFramerate = clamp(m_engine.GetAverageFramerate(), 30.0f, 1000.0f);
    m_simulator->startFrame(1 / avgFramerate);

    auto proc_t0 = std::chrono::steady_clock::now();
    const int iterationCount = m_simulator->getFrameIterationCount();
    while (m_simulator->simulateStep()) {
        m_oscCluster->sample();
    }

    auto proc_t1 = std::chrono::steady_clock::now();

    m_simulator->endFrame();

    auto duration = proc_t1 - proc_t0;
    if (iterationCount > 0) {
        m_performanceCluster->addTimePerTimestepSample(
            (duration.count() / 1E9) / iterationCount);
    }

    const SampleOffset safeWritePosition = m_audioSource->GetCurrentWritePosition();
    const SampleOffset writePosition = m_audioBuffer.m_writePointer;
    const auto audioPrepStart = std::chrono::steady_clock::now();

    SampleOffset targetWritePosition =
        m_audioBuffer.getBufferIndex(safeWritePosition, (int)(44100 * 0.1));
    SampleOffset maxWrite = m_audioBuffer.offsetDelta(writePosition, targetWritePosition);

    SampleOffset currentLead = m_audioBuffer.offsetDelta(safeWritePosition, writePosition);
    SampleOffset newLead = m_audioBuffer.offsetDelta(safeWritePosition, targetWritePosition);

    if (currentLead > 44100 * 0.5) {
        m_audioBuffer.m_writePointer = m_audioBuffer.getBufferIndex(safeWritePosition, (int)(44100 * 0.05));
        currentLead = m_audioBuffer.offsetDelta(safeWritePosition, m_audioBuffer.m_writePointer);
        maxWrite = m_audioBuffer.offsetDelta(m_audioBuffer.m_writePointer, targetWritePosition);
    }

    if (currentLead > newLead) {
        maxWrite = 0;
    }

    int16_t *samples = new int16_t[maxWrite];
    const int readSamples = m_simulator->readAudioOutput(maxWrite, samples);

    for (SampleOffset i = 0; i < (SampleOffset)readSamples && i < maxWrite; ++i) {
        const int16_t sample = samples[i];
        if (m_oscillatorSampleOffset % 4 == 0) {
            m_oscCluster->getAudioWaveformOscilloscope()->addDataPoint(
                m_oscillatorSampleOffset,
                sample / (float)(INT16_MAX));
        }

        m_audioBuffer.writeSample(sample, m_audioBuffer.m_writePointer, (int)i);

        m_oscillatorSampleOffset = (m_oscillatorSampleOffset + 1) % (44100 / 10);
    }

    delete[] samples;

    if (readSamples > 0) {
        const SampleOffset beforeCommitWrite = m_audioBuffer.m_writePointer;
        SampleOffset size0, size1;
        void *data0, *data1;
        m_audioSource->LockBufferSegment(
            m_audioBuffer.m_writePointer, readSamples, &data0, &size0, &data1, &size1);

        m_audioBuffer.copyBuffer(
            reinterpret_cast<int16_t *>(data0), m_audioBuffer.m_writePointer, size0);
        m_audioBuffer.copyBuffer(
            reinterpret_cast<int16_t *>(data1),
            m_audioBuffer.getBufferIndex(m_audioBuffer.m_writePointer, size0),
            size1);

        m_audioSource->UnlockBufferSegments(data0, size0, data1, size1);
        m_audioBuffer.commitBlock(readSamples);
        if (m_audioBuffer.m_writePointer < beforeCommitWrite) {
            DebugTrace::Log(
                "audio",
                "transient_ring_wrap event=audio_buffer write_before=%d write_after=%d samples=%d",
                beforeCommitWrite,
                m_audioBuffer.m_writePointer,
                readSamples);
        }
    }

    m_performanceCluster->addInputBufferUsageSample(
        (double)m_simulator->getSynthesizerInputLatency() / m_simulator->getSynthesizerInputLatencyTarget());
    m_performanceCluster->addAudioLatencySample(
        m_audioBuffer.offsetDelta(m_audioSource->GetCurrentWritePosition(), m_audioBuffer.m_writePointer) / (44100 * 0.1));
    const auto audioPrepEnd = std::chrono::steady_clock::now();
    DebugTrace::Log(
        "audio",
        "subsystem_duration audio_prep_us=%lld",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(audioPrepEnd - audioPrepStart).count()));
}

void EngineSimApplication::render() {
    for (SimulationObject *object : m_objects) {
        object->generateGeometry();
    }

    m_viewParameters.Sublayer = 0;
    for (SimulationObject *object : m_objects) {
        object->render(&getViewParameters());
    }

    m_viewParameters.Sublayer = 1;
    for (SimulationObject *object : m_objects) {
        object->render(&getViewParameters());
    }

    m_viewParameters.Sublayer = 2;
    for (SimulationObject *object : m_objects) {
        object->render(&getViewParameters());
    }

    m_uiManager.render();
}

float EngineSimApplication::pixelsToUnits(float pixels) const {
    const float f = m_displayHeight / m_engineView->m_bounds.height();
    return pixels * f;
}

float EngineSimApplication::unitsToPixels(float units) const {
    const float f = m_engineView->m_bounds.height() / m_displayHeight;
    return units * f;
}

void EngineSimApplication::run() {
    DebugTrace::Log("app", "run() begin");
    if (m_simulator == nullptr) {
        startupLog("run aborted: simulator is null after initialization");
        return;
    }

    auto nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int framesSinceHeartbeat = 0;
    unsigned long long frameIndex = 0;
    bool lastFocusState = false;
    bool focusStateInitialized = false;
    bool firstFrameCompleteLogged = false;
    auto lastResizeEvent = std::chrono::steady_clock::now();
    bool resizeInProgress = false;
    int resizeEventsSinceCommit = 0;
    int previousScreenWidth = m_engine.GetScreenWidth();
    int previousScreenHeight = m_engine.GetScreenHeight();
    std::chrono::steady_clock::time_point inputDispatchTime = std::chrono::steady_clock::now();
    MemorySnapshot previousMemorySnapshot{};
    auto nextMemorySnapshot = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto expectedFrameEnd = std::chrono::steady_clock::now();
    double frameMsEwma = 0.0;
    bool frameMsEwmaInitialized = false;
    double memorySlopeEwma = 0.0;
    bool memorySlopeEwmaInitialized = false;
    const std::filesystem::path watchedScriptPath = std::filesystem::path(m_assetPath) / "assets" / "main.mr";
    bool scriptWatcherInitialized = false;
    std::filesystem::file_time_type watchedScriptWriteTime{};
    bool scriptWatchDebouncePending = false;
    auto scriptWatchPendingSince = std::chrono::steady_clock::now();
    auto nextScriptWatchPoll = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int lastAudioDeviceSampleRate = -1;

    while (true) {
        ++frameIndex;
        DebugTrace::SetFrameIndex(frameIndex);
        const auto frameCpuStart = std::chrono::steady_clock::now();

        m_engine.StartFrame();
        ++framesSinceHeartbeat;

        const bool frameWindowActive = (m_engine.GetGameWindow() != nullptr) ? m_engine.GetGameWindow()->IsActive() : false;
        DebugTrace::Log(
            "mainloop",
            "FrameBegin dt=%.6f window=%dx%d focused=%d",
            m_engine.GetFrameLength(),
            m_engine.GetScreenWidth(),
            m_engine.GetScreenHeight(),
            frameWindowActive ? 1 : 0);

        if (!focusStateInitialized || frameWindowActive != lastFocusState) {
            DebugTrace::Log("window", "focus %s; pause_policy=manual_only", frameWindowActive ? "gained" : "lost");
            lastFocusState = frameWindowActive;
            focusStateInitialized = true;
        }

        if (!m_engine.IsOpen()) {
            DebugTrace::Log("app", "run loop exit: window closed");
            break;
        }
        if (m_engine.ProcessKeyDown(ysKey::Code::Escape)) {
            DebugTrace::Log("input", "escape pressed; exiting run loop");
            break;
        }

        if (m_engine.ProcessKeyDown(ysKey::Code::Return)) {
            DebugTrace::Log("script", "reload requested via Return key");
            DebugTrace::Log("script", "filesystem_watcher_event source=manual_reload_key path=%s", watchedScriptPath.string().c_str());
            m_audioSource->SetMode(ysAudioSource::Mode::Stop);
            loadScript();
            if (m_simulator->getEngine() != nullptr) {
                m_audioSource->SetMode(ysAudioSource::Mode::Loop);
            }
        }
        if (m_engine.ProcessKeyDown(ysKey::Code::F10)) {
            DebugTrace::Log("mainloop", "on-demand dump requested via F10");
            DebugTrace::RequestDump("hotkey_f10");
        }

        if (m_engine.ProcessKeyDown(ysKey::Code::Tab)) {
            m_screen++;
            if (m_screen > 2) m_screen = 0;
            DebugTrace::Log("ui", "screen changed to %d", m_screen);
        }

        if (m_engine.ProcessKeyDown(ysKey::Code::F)) {
            if (m_engine.GetGameWindow()->GetWindowStyle() != ysWindow::WindowStyle::Fullscreen) {
                m_engine.GetGameWindow()->SetWindowStyle(ysWindow::WindowStyle::Fullscreen);
                m_infoCluster->setLogMessage("Entered fullscreen mode");
                DebugTrace::Log("window", "entered fullscreen");
            }
            else {
                m_engine.GetGameWindow()->SetWindowStyle(ysWindow::WindowStyle::Windowed);
                m_infoCluster->setLogMessage("Exited fullscreen mode");
                DebugTrace::Log("window", "exited fullscreen");
            }
        }

        m_gameWindowHeight = m_engine.GetGameWindow()->GetGameHeight();
        m_screenHeight = m_engine.GetGameWindow()->GetScreenHeight();
        m_screenWidth = m_engine.GetGameWindow()->GetScreenWidth();

        if (m_screenWidth != previousScreenWidth || m_screenHeight != previousScreenHeight) {
            ++resizeEventsSinceCommit;
            lastResizeEvent = std::chrono::steady_clock::now();
            if (!resizeInProgress) {
                resizeInProgress = true;
                DebugTrace::Log(
                    "window",
                    "resize begin from=%dx%d to=%dx%d",
                    previousScreenWidth,
                    previousScreenHeight,
                    m_screenWidth,
                    m_screenHeight);
            }
            previousScreenWidth = m_screenWidth;
            previousScreenHeight = m_screenHeight;
        }
        else if (resizeInProgress && std::chrono::steady_clock::now() - lastResizeEvent > std::chrono::milliseconds(250)) {
            resizeInProgress = false;
            DebugTrace::Log(
                "window",
                "resize end committed=%dx%d coalesced_events=%d",
                m_screenWidth,
                m_screenHeight,
                resizeEventsSinceCommit);
            resizeEventsSinceCommit = 0;
        }

        updateScreenSizeStability();

        const auto inputStart = std::chrono::steady_clock::now();
        auto inputEnd = inputStart;
        auto simStart = inputStart;
        auto simEnd = inputStart;
        DebugTrace::Log("mainloop", "allocation-heavy enter processEngineInput");
        processEngineInput();
        inputDispatchTime = std::chrono::steady_clock::now();
        {
            inputEnd = std::chrono::steady_clock::now();
            const auto inputMicros = std::chrono::duration_cast<std::chrono::microseconds>(inputEnd - inputStart).count();
            DebugTrace::Log("mainloop", "allocation-heavy leave processEngineInput duration_us=%lld", static_cast<long long>(inputMicros));
        }

        if (m_engine.ProcessKeyDown(ysKey::Code::Insert) &&
            m_engine.GetGameWindow()->IsActive()) {
            if (!isRecording() && readyToRecord()) {
                startRecording();
            }
            else if (isRecording()) {
                stopRecording();
            }
        }

        if (isRecording() && !readyToRecord()) {
            stopRecording();
        }

        if (!m_paused || m_engine.ProcessKeyDown(ysKey::Code::Right)) {
            simStart = std::chrono::steady_clock::now();
            DebugTrace::Log("mainloop", "allocation-heavy enter process");
            process(m_engine.GetFrameLength());
            simEnd = std::chrono::steady_clock::now();
            const auto simMicros = std::chrono::duration_cast<std::chrono::microseconds>(simEnd - simStart).count();
            DebugTrace::Log("mainloop", "allocation-heavy leave process duration_us=%lld", static_cast<long long>(simMicros));
        }

        const auto uiStart = std::chrono::steady_clock::now();
        DebugTrace::Log("mainloop", "allocation-heavy enter ui_update");
        m_uiManager.update(m_engine.GetFrameLength());
        const auto uiEnd = std::chrono::steady_clock::now();
        DebugTrace::Log(
            "mainloop",
            "allocation-heavy leave ui_update duration_us=%lld",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(uiEnd - uiStart).count()));

        const auto renderStart = std::chrono::steady_clock::now();
        DebugTrace::Log("mainloop", "allocation-heavy enter renderScene");
        renderScene();
        const auto renderEnd = std::chrono::steady_clock::now();
        DebugTrace::Log(
            "mainloop",
            "allocation-heavy leave renderScene duration_us=%lld",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(renderEnd - renderStart).count()));

        m_engine.EndFrame();

        if (isRecording()) {
            recordFrame();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextHeartbeat) {
            const float frameLength = m_engine.GetFrameLength();
            const double fps = (frameLength > 0.0f) ? 1.0 / frameLength : 0.0;
            DebugTrace::Log(
                "mainloop",
                "heartbeat frames=%d frame_dt=%.6f fps=%.2f avg_fps=%.2f screen=%dx%d game_h=%d wheel_coalesced=%d",
                framesSinceHeartbeat,
                frameLength,
                fps,
                m_engine.GetAverageFramerate(),
                m_screenWidth,
                m_screenHeight,
                m_gameWindowHeight,
                g_mouseWheelEventsThisSecond);
            DebugTrace::Log(
                "mainloop",
                "lock_contention_counters render_lock_proxy=%d shared_state_lock_proxy=%d",
                0,
                0);
            framesSinceHeartbeat = 0;
            g_mouseWheelEventsThisSecond = 0;
            nextHeartbeat = now + std::chrono::seconds(1);
        }

        if (now >= nextScriptWatchPoll) {
            std::error_code watchEc;
            if (std::filesystem::exists(watchedScriptPath, watchEc)) {
                const auto currentWriteTime = std::filesystem::last_write_time(watchedScriptPath, watchEc);
                if (!watchEc) {
                    if (!scriptWatcherInitialized) {
                        watchedScriptWriteTime = currentWriteTime;
                        scriptWatcherInitialized = true;
                    }
                    else if (currentWriteTime != watchedScriptWriteTime) {
                        watchedScriptWriteTime = currentWriteTime;
                        scriptWatchDebouncePending = true;
                        scriptWatchPendingSince = now;
                        DebugTrace::Log(
                            "script",
                            "filesystem_watcher_event path=%s action=modified",
                            watchedScriptPath.string().c_str());
                    }
                }
            }

            if (scriptWatchDebouncePending && (now - scriptWatchPendingSince) >= std::chrono::milliseconds(350)) {
                scriptWatchDebouncePending = false;
                DebugTrace::Log(
                    "script",
                    "filesystem_watcher_debounce action=settled path=%s reload_policy=manual",
                    watchedScriptPath.string().c_str());
            }

            if (m_outputAudioBuffer != nullptr && m_outputAudioBuffer->GetAudioParameters() != nullptr) {
                const int currentSampleRate = m_outputAudioBuffer->GetAudioParameters()->m_sampleRate;
                if (lastAudioDeviceSampleRate < 0) {
                    lastAudioDeviceSampleRate = currentSampleRate;
                }
                else if (currentSampleRate != lastAudioDeviceSampleRate) {
                    DebugTrace::Log(
                        "audio",
                        "audio_device_reconfigured old_sample_rate=%d new_sample_rate=%d",
                        lastAudioDeviceSampleRate,
                        currentSampleRate);
                    lastAudioDeviceSampleRate = currentSampleRate;
                }
            }

            nextScriptWatchPoll = now + std::chrono::seconds(1);
        }

        const auto frameCpuEnd = std::chrono::steady_clock::now();
        const auto frameCpuMicros = std::chrono::duration_cast<std::chrono::microseconds>(frameCpuEnd - frameCpuStart).count();
        const double frameCpuMs = static_cast<double>(frameCpuMicros) / 1000.0;
        const double inputMs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(inputEnd - inputStart).count()) / 1000.0;
        const double simMs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(simEnd - simStart).count()) / 1000.0;
        const double uiMs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(uiEnd - uiStart).count()) / 1000.0;
        const double renderMs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(renderEnd - renderStart).count()) / 1000.0;
        DebugTrace::Log("mainloop", "FrameEnd cpu_ms=%.3f", frameCpuMs);
        const auto inputToVisualMs = std::chrono::duration_cast<std::chrono::microseconds>(frameCpuEnd - inputDispatchTime).count() / 1000.0;
        DebugTrace::Log("mainloop", "input_to_visual_latency_ms=%.3f", inputToVisualMs);
        if (!firstFrameCompleteLogged) {
            DebugTrace::Log("mainloop", "first_frame_complete");
            firstFrameCompleteLogged = true;
        }
        if (frameCpuMs > 500.0) {
            DebugTrace::Log("mainloop", "stall_warning threshold=500ms cpu_ms=%.3f", frameCpuMs);
        }
        else if (frameCpuMs > 100.0) {
            DebugTrace::Log("mainloop", "stall_warning threshold=100ms cpu_ms=%.3f", frameCpuMs);
        }
        if (frameCpuMs > 1000.0) {
            DebugTrace::Log("mainloop", "watchdog_warning main_thread_unresponsive_window cpu_ms=%.3f", frameCpuMs);
        }
        if (!frameMsEwmaInitialized) {
            frameMsEwma = frameCpuMs;
            frameMsEwmaInitialized = true;
        }
        else {
            frameMsEwma = frameMsEwma * 0.95 + frameCpuMs * 0.05;
            if (frameCpuMs > frameMsEwma * 2.5 && frameCpuMs > 20.0) {
                DebugTrace::Log(
                    "mainloop",
                    "anomaly_detector frame_spike current_ms=%.3f baseline_ms=%.3f",
                    frameCpuMs,
                    frameMsEwma);
            }
        }
        expectedFrameEnd += std::chrono::milliseconds(16);
        const auto schedulerDriftUs =
            std::chrono::duration_cast<std::chrono::microseconds>(frameCpuEnd - expectedFrameEnd).count();
        DebugTrace::Log("mainloop", "scheduler_drift_us=%lld target_fps=60", static_cast<long long>(schedulerDriftUs));

        struct SlowEntry {
            const char *name;
            double ms;
        };
        SlowEntry entries[] = {
            {"processEngineInput", inputMs},
            {"simulate", simMs},
            {"ui_update", uiMs},
            {"renderScene", renderMs}
        };
        for (int i = 0; i < 4; ++i) {
            for (int j = i + 1; j < 4; ++j) {
                if (entries[j].ms > entries[i].ms) {
                    const SlowEntry tmp = entries[i];
                    entries[i] = entries[j];
                    entries[j] = tmp;
                }
            }
        }
        DebugTrace::Log(
            "mainloop",
            "top_slow_functions f1=%s:%.3fms f2=%s:%.3fms f3=%s:%.3fms",
            entries[0].name,
            entries[0].ms,
            entries[1].name,
            entries[1].ms,
            entries[2].name,
            entries[2].ms);

        if (frameCpuEnd >= nextMemorySnapshot) {
            MemorySnapshot snapshot = captureMemorySnapshot();
            if (snapshot.valid) {
                DebugTrace::Log(
                    "mainloop",
                    "memory_snapshot rss_mb=%.2f phys_footprint_mb=%.2f iosurface_mb=%.2f malloc_metadata_mb=%.2f",
                    snapshot.rssMb,
                    snapshot.footprintMb,
                    snapshot.iosurfaceMb,
                    snapshot.mallocMetadataMb);
                if (previousMemorySnapshot.valid) {
                    const double deltaMb = snapshot.footprintMb - previousMemorySnapshot.footprintMb;
                    const double slopeMbPerMin = deltaMb * 60.0;
                    if (slopeMbPerMin > 10.0) {
                        DebugTrace::Log(
                            "mainloop",
                            "memory_growth_warning slope_mb_per_min=%.2f delta_mb=%.2f",
                            slopeMbPerMin,
                            deltaMb);
                    }
                    if (!memorySlopeEwmaInitialized) {
                        memorySlopeEwma = slopeMbPerMin;
                        memorySlopeEwmaInitialized = true;
                    }
                    else {
                        memorySlopeEwma = memorySlopeEwma * 0.9 + slopeMbPerMin * 0.1;
                        if (slopeMbPerMin > memorySlopeEwma + 8.0) {
                            DebugTrace::Log(
                                "mainloop",
                                "anomaly_detector memory_spike current_slope_mb_per_min=%.3f baseline=%.3f",
                                slopeMbPerMin,
                                memorySlopeEwma);
                        }
                    }
                }
                previousMemorySnapshot = snapshot;
            }

            const int widgetCount = countWidgetsRecursive(m_uiManager.getRoot());
            DebugTrace::Log("ui", "object_counters widgets=%d", widgetCount);
            nextMemorySnapshot = frameCpuEnd + std::chrono::seconds(1);
        }
    }

    if (isRecording()) {
        stopRecording();
    }

    m_simulator->endAudioRenderingThread();
    DebugTrace::Log("app", "run() end");
}

void EngineSimApplication::destroy() {
    DebugTrace::Log("app", "destroy() begin");
    m_shaderSet.Destroy();

    m_engine.GetDevice()->DestroyGPUBuffer(m_geometryVertexBuffer);
    m_engine.GetDevice()->DestroyGPUBuffer(m_geometryIndexBuffer);

    m_assetManager.Destroy();
    m_engine.Destroy();

    m_simulator->destroy();
    m_audioBuffer.destroy();
    DebugTrace::Log("app", "destroy() complete");
}

void EngineSimApplication::loadEngine(
    Engine *engine,
    Vehicle *vehicle,
    Transmission *transmission)
{
    destroyObjects();

    if (m_simulator != nullptr) {
        m_simulator->releaseSimulation();
        delete m_simulator;
    }

    if (m_vehicle != nullptr) {
        delete m_vehicle;
        m_vehicle = nullptr;
    }

    if (m_transmission != nullptr) {
        delete m_transmission;
        m_transmission = nullptr;
    }

    if (m_iceEngine != nullptr) {
        m_iceEngine->destroy();
        delete m_iceEngine;
    }

    m_iceEngine = engine;
    m_vehicle = vehicle;
    m_transmission = transmission;

    if (engine == nullptr || vehicle == nullptr || transmission == nullptr) {
        m_simulator = nullptr;
        m_iceEngine = nullptr;
        m_viewParameters.Layer1 = 0;

        return;
    }

    m_simulator = engine->createSimulator(vehicle, transmission);

    createObjects(engine);

    m_viewParameters.Layer1 = engine->getMaxDepth();
    engine->calculateDisplacement();

    m_simulator->setSimulationFrequency(engine->getSimulationFrequency());

    Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
    audioParams.inputSampleNoise = static_cast<float>(engine->getInitialJitter());
    audioParams.airNoise = static_cast<float>(engine->getInitialNoise());
    audioParams.dF_F_mix = static_cast<float>(engine->getInitialHighFrequencyGain());
    m_simulator->synthesizer().setAudioParameters(audioParams);

    for (int i = 0; i < engine->getExhaustSystemCount(); ++i) {
        ImpulseResponse *response = engine->getExhaustSystem(i)->getImpulseResponse();

#if defined(_WIN32)
        ysWindowsAudioWaveFile waveFile;
        waveFile.OpenFile(response->getFilename().c_str());
        waveFile.InitializeInternalBuffer(waveFile.GetSampleCount());
        waveFile.FillBuffer(0);
        waveFile.CloseFile();

        m_simulator->synthesizer().initializeImpulseResponse(
            reinterpret_cast<const int16_t *>(waveFile.GetBuffer()),
            waveFile.GetSampleCount(),
            response->getVolume(),
            i
        );

        waveFile.DestroyInternalBuffer();
#else
        (void)response;
        (void)i;
#endif
    }

    m_simulator->startAudioRenderingThread();
}

void EngineSimApplication::drawGenerated(
    const GeometryGenerator::GeometryIndices &indices,
    int layer)
{
    drawGenerated(indices, layer, m_shaders.GetRegularFlags());
}

void EngineSimApplication::drawGeneratedUi(
    const GeometryGenerator::GeometryIndices &indices,
    int layer)
{
    drawGenerated(indices, layer, m_shaders.GetUiFlags());
}

void EngineSimApplication::drawGenerated(
    const GeometryGenerator::GeometryIndices &indices,
    int layer,
    dbasic::StageEnableFlags flags)
{
    m_engine.DrawGeneric(
        flags,
        m_geometryIndexBuffer,
        m_geometryVertexBuffer,
        sizeof(dbasic::Vertex),
        indices.BaseIndex,
        indices.BaseVertex,
        indices.FaceCount,
        false,
        layer);
}

void EngineSimApplication::configure(const ApplicationSettings &settings) {
    m_applicationSettings = settings;

    if (settings.startFullscreen) {
        m_engine.GetGameWindow()->SetWindowStyle(ysWindow::WindowStyle::Fullscreen);
    }

    m_background = ysColor::srgbiToLinear(m_applicationSettings.colorBackground);
    m_foreground = ysColor::srgbiToLinear(m_applicationSettings.colorForeground);
    m_shadow = ysColor::srgbiToLinear(m_applicationSettings.colorShadow);
    m_highlight1 = ysColor::srgbiToLinear(m_applicationSettings.colorHighlight1);
    m_highlight2 = ysColor::srgbiToLinear(m_applicationSettings.colorHighlight2);
    m_pink = ysColor::srgbiToLinear(m_applicationSettings.colorPink);
    m_red = ysColor::srgbiToLinear(m_applicationSettings.colorRed);
    m_orange = ysColor::srgbiToLinear(m_applicationSettings.colorOrange);
    m_yellow = ysColor::srgbiToLinear(m_applicationSettings.colorYellow);
    m_blue = ysColor::srgbiToLinear(m_applicationSettings.colorBlue);
    m_green = ysColor::srgbiToLinear(m_applicationSettings.colorGreen);
}

void EngineSimApplication::createObjects(Engine *engine) {
    for (int i = 0; i < engine->getCylinderCount(); ++i) {
        ConnectingRodObject *rodObject = new ConnectingRodObject;
        rodObject->initialize(this);
        rodObject->m_connectingRod = engine->getConnectingRod(i);
        m_objects.push_back(rodObject);

        PistonObject *pistonObject = new PistonObject;
        pistonObject->initialize(this);
        pistonObject->m_piston = engine->getPiston(i);
        m_objects.push_back(pistonObject);

        CombustionChamberObject *ccObject = new CombustionChamberObject;
        ccObject->initialize(this);
        ccObject->m_chamber = m_iceEngine->getChamber(i);
        m_objects.push_back(ccObject);
    }

    for (int i = 0; i < engine->getCrankshaftCount(); ++i) {
        CrankshaftObject *crankshaftObject = new CrankshaftObject;
        crankshaftObject->initialize(this);
        crankshaftObject->m_crankshaft = engine->getCrankshaft(i);
        m_objects.push_back(crankshaftObject);
    }

    for (int i = 0; i < engine->getCylinderBankCount(); ++i) {
        CylinderBankObject *cbObject = new CylinderBankObject;
        cbObject->initialize(this);
        cbObject->m_bank = engine->getCylinderBank(i);
        cbObject->m_head = engine->getHead(i);
        m_objects.push_back(cbObject);

        CylinderHeadObject *chObject = new CylinderHeadObject;
        chObject->initialize(this);
        chObject->m_head = engine->getHead(i);
        chObject->m_engine = engine;
        m_objects.push_back(chObject);
    }
}

void EngineSimApplication::destroyObjects() {
    for (SimulationObject *object : m_objects) {
        object->destroy();
        delete object;
    }

    m_objects.clear();
}

const SimulationObject::ViewParameters &
    EngineSimApplication::getViewParameters() const
{
    return m_viewParameters;
}

void EngineSimApplication::loadScript() {
    DebugTrace::Log("script", "loadScript begin");
    Engine *engine = nullptr;
    Vehicle *vehicle = nullptr;
    Transmission *transmission = nullptr;

#ifdef ATG_ENGINE_SIM_PIRANHA_ENABLED
    es_script::Compiler compiler;
    const auto compileStart = std::chrono::steady_clock::now();
    const auto scriptIoStart = std::chrono::steady_clock::now();
    DebugTrace::Log("script", "script_vm_call entry=compiler.initialize");
    compiler.initialize();
    const std::string scriptPath = m_assetPath + "/assets/main.mr";
    const std::string assetScriptLibraryPath = (std::filesystem::path(m_assetPath) / "es").string();
    compiler.addSearchPath(assetScriptLibraryPath.c_str());
    DebugTrace::Log("script", "added script search path=%s", assetScriptLibraryPath.c_str());
    DebugTrace::Log("script", "active script path=%s", scriptPath.c_str());
    DebugTrace::Log("script", "script_vm_call entry=compiler.compile");
    const bool compiled = compiler.compile(scriptPath.c_str());
    DebugTrace::Log("script", "script_vm_call exit=compiler.compile success=%d", compiled ? 1 : 0);
    const auto scriptIoEnd = std::chrono::steady_clock::now();
    DebugTrace::Log(
        "script",
        "asset_io_latency operation=load_script path=%s elapsed_ms=%.3f",
        scriptPath.c_str(),
        std::chrono::duration_cast<std::chrono::microseconds>(scriptIoEnd - scriptIoStart).count() / 1000.0);
    if (compiled) {
        DebugTrace::Log("script", "script_vm_call entry=compiler.execute");
        const es_script::Compiler::Output output = compiler.execute();
        DebugTrace::Log("script", "script_vm_call exit=compiler.execute");
        configure(output.applicationSettings);

        engine = output.engine;
        vehicle = output.vehicle;
        transmission = output.transmission;
    }
    else {
        engine = nullptr;
        vehicle = nullptr;
        transmission = nullptr;
    }

    DebugTrace::Log("script", "script_vm_call entry=compiler.destroy");
    compiler.destroy();
    DebugTrace::Log("script", "script_vm_call exit=compiler.destroy");
    const auto compileEnd = std::chrono::steady_clock::now();
    DebugTrace::Log(
        "script",
        "subsystem_duration script_compile_execute_us=%lld",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(compileEnd - compileStart).count()));
#endif /* ATG_ENGINE_SIM_PIRANHA_ENABLED */

    if (vehicle == nullptr) {
        Vehicle::Parameters vehParams;
        vehParams.mass = units::mass(1597, units::kg);
        vehParams.diffRatio = 3.42;
        vehParams.tireRadius = units::distance(10, units::inch);
        vehParams.dragCoefficient = 0.25;
        vehParams.crossSectionArea = units::distance(6.0, units::foot) * units::distance(6.0, units::foot);
        vehParams.rollingResistance = 2000.0;
        vehicle = new Vehicle;
        vehicle->initialize(vehParams);
    }

    if (transmission == nullptr) {
        const double gearRatios[] = { 2.97, 2.07, 1.43, 1.00, 0.84, 0.56 };
        Transmission::Parameters tParams;
        tParams.GearCount = 6;
        tParams.GearRatios = gearRatios;
        tParams.MaxClutchTorque = units::torque(1000.0, units::ft_lb);
        transmission = new Transmission;
        transmission->initialize(tParams);
    }

    loadEngine(engine, vehicle, transmission);
    refreshUserInterface();
    static ApplicationSettings s_lastSettings;
    static bool s_settingsInitialized = false;
    if (!s_settingsInitialized) {
        s_lastSettings = m_applicationSettings;
        s_settingsInitialized = true;
    }
    else {
        if (s_lastSettings.powerUnits != m_applicationSettings.powerUnits) {
            DebugTrace::Log(
                "script",
                "script_var_diff key=powerUnits old=%s new=%s",
                s_lastSettings.powerUnits.c_str(),
                m_applicationSettings.powerUnits.c_str());
        }
        if (s_lastSettings.torqueUnits != m_applicationSettings.torqueUnits) {
            DebugTrace::Log(
                "script",
                "script_var_diff key=torqueUnits old=%s new=%s",
                s_lastSettings.torqueUnits.c_str(),
                m_applicationSettings.torqueUnits.c_str());
        }
        if (s_lastSettings.startFullscreen != m_applicationSettings.startFullscreen) {
            DebugTrace::Log(
                "script",
                "script_var_diff key=startFullscreen old=%d new=%d",
                s_lastSettings.startFullscreen ? 1 : 0,
                m_applicationSettings.startFullscreen ? 1 : 0);
        }
        s_lastSettings = m_applicationSettings;
    }
    DebugTrace::Log("script", "loadScript complete");
}

void EngineSimApplication::processEngineInput() {
    if (m_iceEngine == nullptr) {
        return;
    }

    const float dt = m_engine.GetFrameLength();
    const bool fineControlMode = m_engine.IsKeyDown(ysKey::Code::Space);

    const int mouseWheel = m_engine.GetMouseWheel();
    const int mouseWheelDelta = mouseWheel - m_lastMouseWheel;
    m_lastMouseWheel = mouseWheel;
    if (mouseWheelDelta != 0) {
        DebugTrace::Log("input", "mouse wheel delta=%d", mouseWheelDelta);
        ++g_mouseWheelEventsThisSecond;
    }

    struct KeyTrace {
        ysKey::Code code;
        const char *name;
    };
    static const std::array<KeyTrace, 17> tracedKeys = {{
        {ysKey::Code::A, "A"},
        {ysKey::Code::S, "S"},
        {ysKey::Code::D, "D"},
        {ysKey::Code::H, "H"},
        {ysKey::Code::G, "G"},
        {ysKey::Code::F, "F"},
        {ysKey::Code::I, "I"},
        {ysKey::Code::Up, "Up"},
        {ysKey::Code::Down, "Down"},
        {ysKey::Code::Z, "Z"},
        {ysKey::Code::X, "X"},
        {ysKey::Code::C, "C"},
        {ysKey::Code::V, "V"},
        {ysKey::Code::B, "B"},
        {ysKey::Code::N, "N"},
        {ysKey::Code::M, "M"},
        {ysKey::Code::Space, "Space"}
    }};
    static std::array<bool, tracedKeys.size()> previousStates = {};
    auto logScriptWrite = [&](const char *ns, const char *key, double value, const char *source) {
        DebugTrace::Log(
            "script",
            "script_var_write ns=%s key=%s value=%.6f source=%s",
            ns,
            key,
            value,
            source);
    };
    int dispatchDepthProxy = 0;
    for (size_t i = 0; i < tracedKeys.size(); ++i) {
        const bool down = m_engine.IsKeyDown(tracedKeys[i].code);
        if (down != previousStates[i]) {
            DebugTrace::Log("input", "key_%s %s", tracedKeys[i].name, down ? "down" : "up");
            previousStates[i] = down;
            ++dispatchDepthProxy;
        }
    }
    DebugTrace::Log("input", "input_dispatch_queue_depth_proxy=%d", dispatchDepthProxy);

    bool fineControlInUse = false;
    static auto s_nextAnalogLog = std::chrono::steady_clock::now();
    static double s_lastLoggedThrottleEffective = -1.0;
    static double s_lastLoggedClutchEffective = -1.0;
    auto logWheelBinding = [&](const char *bindingName) {
        if (mouseWheelDelta != 0) {
            DebugTrace::Log("input", "mouse wheel routed binding=%s delta=%d", bindingName, mouseWheelDelta);
        }
    };
    if (m_engine.IsKeyDown(ysKey::Code::Z)) {
        const double rate = fineControlMode
            ? 0.001
            : 0.01;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.volume = clamp(audioParams.volume + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;
        logWheelBinding("volume");

        m_infoCluster->setLogMessage("[Z] - Set volume to " + std::to_string(audioParams.volume));
    }
    else if (m_engine.IsKeyDown(ysKey::Code::X)) {
        const double rate = fineControlMode
            ? 0.001
            : 0.01;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.convolution = clamp(audioParams.convolution + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;
        logWheelBinding("convolution");

        m_infoCluster->setLogMessage("[X] - Set convolution level to " + std::to_string(audioParams.convolution));
    }
    else if (m_engine.IsKeyDown(ysKey::Code::C)) {
        const double rate = fineControlMode
            ? 0.00001
            : 0.001;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.dF_F_mix = clamp(audioParams.dF_F_mix + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;
        logWheelBinding("high_frequency_gain");

        m_infoCluster->setLogMessage("[C] - Set high freq. gain to " + std::to_string(audioParams.dF_F_mix));
    }
    else if (m_engine.IsKeyDown(ysKey::Code::V)) {
        const double rate = fineControlMode
            ? 0.001
            : 0.01;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.airNoise = clamp(audioParams.airNoise + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;
        logWheelBinding("low_frequency_noise");

        m_infoCluster->setLogMessage("[V] - Set low freq. noise to " + std::to_string(audioParams.airNoise));
    }
    else if (m_engine.IsKeyDown(ysKey::Code::B)) {
        const double rate = fineControlMode
            ? 0.001
            : 0.01;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.inputSampleNoise = clamp(audioParams.inputSampleNoise + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;
        logWheelBinding("high_frequency_noise");

        m_infoCluster->setLogMessage("[B] - Set high freq. noise to " + std::to_string(audioParams.inputSampleNoise));
    }
    else if (m_engine.IsKeyDown(ysKey::Code::N)) {
        const double rate = fineControlMode
            ? 10.0
            : 100.0;

        const double newSimulationFrequency = clamp(
            m_simulator->getSimulationFrequency() + mouseWheelDelta * rate * dt,
            400.0, 400000.0);

        const double previousSimulationFrequency = m_simulator->getSimulationFrequency();
        m_simulator->setSimulationFrequency(newSimulationFrequency);
        if (previousSimulationFrequency != m_simulator->getSimulationFrequency()) {
            DebugTrace::Log(
                "simulator",
                "simulation_frequency changed source=wheel old=%.3f new=%.3f",
                previousSimulationFrequency,
                m_simulator->getSimulationFrequency());
            logScriptWrite("sim.control", "simulation_frequency", m_simulator->getSimulationFrequency(), "mouse_wheel");
        }
        fineControlInUse = true;
        logWheelBinding("simulation_frequency");

        m_infoCluster->setLogMessage("[N] - Set simulation freq to " + std::to_string(m_simulator->getSimulationFrequency()));
    }
    else if (m_engine.IsKeyDown(ysKey::Code::G) && m_simulator->m_dyno.m_hold) {
        if (mouseWheelDelta > 0) {
            m_dynoSpeed += m_iceEngine->getDynoHoldStep();
        }
        else if (mouseWheelDelta < 0) {
            m_dynoSpeed -= m_iceEngine->getDynoHoldStep();
        }

        m_dynoSpeed = clamp(m_dynoSpeed, m_iceEngine->getDynoMinSpeed(), m_iceEngine->getDynoMaxSpeed());

        m_infoCluster->setLogMessage("[G] - Set dyno speed to " + std::to_string(units::toRpm(m_dynoSpeed)));
        fineControlInUse = true;
        logWheelBinding("dyno_speed");
    }

    const double prevTargetThrottle = m_targetSpeedSetting;
    m_targetSpeedSetting = fineControlMode ? m_targetSpeedSetting : 0.0;
    if (m_engine.IsKeyDown(ysKey::Code::Q)) {
        m_targetSpeedSetting = 0.01;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::W)) {
        m_targetSpeedSetting = 0.1;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::E)) {
        m_targetSpeedSetting = 0.2;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::R)) {
        m_targetSpeedSetting = 1.0;
    }
    else if (fineControlMode && !fineControlInUse) {
        m_targetSpeedSetting = clamp(m_targetSpeedSetting + mouseWheelDelta * 0.0001);
        logWheelBinding("throttle_fine_adjust");
    }

    if (prevTargetThrottle != m_targetSpeedSetting) {
        m_infoCluster->setLogMessage("Speed control set to " + std::to_string(m_targetSpeedSetting));
        DebugTrace::Log(
            "simulator",
            "throttle_target changed old=%.5f new=%.5f",
            prevTargetThrottle,
            m_targetSpeedSetting);
        logScriptWrite("sim.control", "throttle_target", m_targetSpeedSetting, "input");
    }

    m_speedSetting = m_targetSpeedSetting * 0.5 + 0.5 * m_speedSetting;

    m_iceEngine->setSpeedControl(m_speedSetting);
    if (m_engine.ProcessKeyDown(ysKey::Code::M)) {
        const int currentLayer = getViewParameters().Layer0;
        if (currentLayer + 1 < m_iceEngine->getMaxDepth()) {
            setViewLayer(currentLayer + 1);
        }

        m_infoCluster->setLogMessage("[M] - Set render layer to " + std::to_string(getViewParameters().Layer0));
    }

    if (m_engine.ProcessKeyDown(ysKey::Code::OEM_Comma)) {
        if (getViewParameters().Layer0 - 1 >= 0)
            setViewLayer(getViewParameters().Layer0 - 1);

        m_infoCluster->setLogMessage("[,] - Set render layer to " + std::to_string(getViewParameters().Layer0));
    }

    if (m_engine.ProcessKeyDown(ysKey::Code::D)) {
        m_simulator->m_dyno.m_enabled = !m_simulator->m_dyno.m_enabled;

        const std::string msg = m_simulator->m_dyno.m_enabled
            ? "DYNOMOMETER ENABLED"
            : "DYNOMOMETER DISABLED";
        m_infoCluster->setLogMessage(msg);
        DebugTrace::Log(
            "simulator",
            "dyno_enabled toggled source=key_D state=%d",
            m_simulator->m_dyno.m_enabled ? 1 : 0);
        logScriptWrite("sim.dyno", "enabled", m_simulator->m_dyno.m_enabled ? 1.0 : 0.0, "key_D");
        DebugTrace::Log(
            "ui",
            "user_mode_transition dyno_panel enabled=%d hold=%d",
            m_simulator->m_dyno.m_enabled ? 1 : 0,
            m_simulator->m_dyno.m_hold ? 1 : 0);
    }

    if (m_engine.ProcessKeyDown(ysKey::Code::H)) {
        m_simulator->m_dyno.m_hold = !m_simulator->m_dyno.m_hold;

        const std::string msg = m_simulator->m_dyno.m_hold
            ? m_simulator->m_dyno.m_enabled ? "HOLD ENABLED" : "HOLD ON STANDBY [ENABLE DYNO. FOR HOLD]"
            : "HOLD DISABLED";
        m_infoCluster->setLogMessage(msg);
        DebugTrace::Log(
            "simulator",
            "dyno_hold toggled source=key_H state=%d dyno_enabled=%d",
            m_simulator->m_dyno.m_hold ? 1 : 0,
            m_simulator->m_dyno.m_enabled ? 1 : 0);
        logScriptWrite("sim.dyno", "hold", m_simulator->m_dyno.m_hold ? 1.0 : 0.0, "key_H");
        DebugTrace::Log(
            "ui",
            "user_mode_transition dyno_hold enabled=%d hold=%d",
            m_simulator->m_dyno.m_enabled ? 1 : 0,
            m_simulator->m_dyno.m_hold ? 1 : 0);
    }

    if (m_simulator->m_dyno.m_enabled) {
        if (!m_simulator->m_dyno.m_hold) {
            if (m_simulator->getFilteredDynoTorque() > units::torque(1.0, units::ft_lb)) {
                m_dynoSpeed += units::rpm(500) * dt;
            }
            else {
                m_dynoSpeed *= (1 / (1 + dt));
            }

            if (m_dynoSpeed > m_iceEngine->getRedline()) {
                m_simulator->m_dyno.m_enabled = false;
                m_dynoSpeed = units::rpm(0);
            }
        }
    }
    else {
        if (!m_simulator->m_dyno.m_hold) {
            m_dynoSpeed = units::rpm(0);
        }
    }

    m_dynoSpeed = clamp(m_dynoSpeed, m_iceEngine->getDynoMinSpeed(), m_iceEngine->getDynoMaxSpeed());
    m_simulator->m_dyno.m_rotationSpeed = m_dynoSpeed;
    static double s_lastLoggedDynoSpeed = -1.0;
    if (s_lastLoggedDynoSpeed < 0.0 || std::abs(m_dynoSpeed - s_lastLoggedDynoSpeed) >= units::rpm(50.0)) {
        logScriptWrite("sim.dyno", "rotation_speed", m_dynoSpeed, "update");
        s_lastLoggedDynoSpeed = m_dynoSpeed;
    }

    const bool prevStarterEnabled = m_simulator->m_starterMotor.m_enabled;
    if (m_engine.IsKeyDown(ysKey::Code::S)) {
        m_simulator->m_starterMotor.m_enabled = true;
    }
    else {
        m_simulator->m_starterMotor.m_enabled = false;
    }

    if (prevStarterEnabled != m_simulator->m_starterMotor.m_enabled) {
        const std::string msg = m_simulator->m_starterMotor.m_enabled
            ? "STARTER ENABLED"
            : "STARTER DISABLED";
        m_infoCluster->setLogMessage(msg);
        DebugTrace::Log(
            "simulator",
            "starter toggled source=key_S state=%d",
            m_simulator->m_starterMotor.m_enabled ? 1 : 0);
        logScriptWrite("sim.ignition", "starter_enabled", m_simulator->m_starterMotor.m_enabled ? 1.0 : 0.0, "key_S");
    }

    if (m_engine.ProcessKeyDown(ysKey::Code::A)) {
        m_simulator->getEngine()->getIgnitionModule()->m_enabled =
            !m_simulator->getEngine()->getIgnitionModule()->m_enabled;

        const std::string msg = m_simulator->getEngine()->getIgnitionModule()->m_enabled
            ? "IGNITION ENABLED"
            : "IGNITION DISABLED";
        m_infoCluster->setLogMessage(msg);
        DebugTrace::Log(
            "simulator",
            "ignition toggled source=key_A state=%d",
            m_simulator->getEngine()->getIgnitionModule()->m_enabled ? 1 : 0);
        logScriptWrite(
            "sim.ignition",
            "ignition_enabled",
            m_simulator->getEngine()->getIgnitionModule()->m_enabled ? 1.0 : 0.0,
            "key_A");
    }

    if (m_engine.ProcessKeyDown(ysKey::Code::Up)) {
        const int oldGear = m_simulator->getTransmission()->getGear();
        m_simulator->getTransmission()->changeGear(oldGear + 1);
        const int newGear = m_simulator->getTransmission()->getGear();

        m_infoCluster->setLogMessage(
            "UPSHIFTED TO " + std::to_string(m_simulator->getTransmission()->getGear() + 1));
        DebugTrace::Log(
            "simulator",
            "gear_changed source=key_Up old=%d new=%d",
            oldGear,
            newGear);
        logScriptWrite("sim.transmission", "gear_index", static_cast<double>(newGear), "key_Up");
    }
    else if (m_engine.ProcessKeyDown(ysKey::Code::Down)) {
        const int oldGear = m_simulator->getTransmission()->getGear();
        m_simulator->getTransmission()->changeGear(oldGear - 1);
        const int newGear = m_simulator->getTransmission()->getGear();

        if (m_simulator->getTransmission()->getGear() != -1) {
            m_infoCluster->setLogMessage(
                "DOWNSHIFTED TO " + std::to_string(m_simulator->getTransmission()->getGear() + 1));
        }
        else {
            m_infoCluster->setLogMessage("SHIFTED TO NEUTRAL");
        }
        DebugTrace::Log(
            "simulator",
            "gear_changed source=key_Down old=%d new=%d",
            oldGear,
            newGear);
        logScriptWrite("sim.transmission", "gear_index", static_cast<double>(newGear), "key_Down");
    }

    if (m_engine.IsKeyDown(ysKey::Code::T)) {
        m_targetClutchPressure -= 0.2 * dt;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::U)) {
        m_targetClutchPressure += 0.2 * dt;
    }
    else if (m_engine.IsKeyDown(ysKey::Code::Shift)) {
        m_targetClutchPressure = 0.0;
        m_infoCluster->setLogMessage("CLUTCH DEPRESSED");
    }
    else if (!m_engine.IsKeyDown(ysKey::Code::Y)) {
        m_targetClutchPressure = 1.0;
    }

    m_targetClutchPressure = clamp(m_targetClutchPressure);

    double clutchRC = 0.001;
    if (m_engine.IsKeyDown(ysKey::Code::Space)) {
        clutchRC = 1.0;
    }

    const double clutch_s = dt / (dt + clutchRC);
    m_clutchPressure = m_clutchPressure * (1 - clutch_s) + m_targetClutchPressure * clutch_s;
    m_simulator->getTransmission()->setClutchPressure(m_clutchPressure);

    const auto now = std::chrono::steady_clock::now();
    const bool throttleMoved = (s_lastLoggedThrottleEffective < 0.0)
        || std::abs(m_speedSetting - s_lastLoggedThrottleEffective) >= 0.01;
    const bool clutchMoved = (s_lastLoggedClutchEffective < 0.0)
        || std::abs(m_clutchPressure - s_lastLoggedClutchEffective) >= 0.01;
    if ((throttleMoved || clutchMoved) && now >= s_nextAnalogLog) {
        DebugTrace::Log(
            "simulator",
            "controls effective throttle=%.5f clutch=%.5f throttle_target=%.5f clutch_target=%.5f",
            m_speedSetting,
            m_clutchPressure,
            m_targetSpeedSetting,
            m_targetClutchPressure);
        logScriptWrite("sim.control", "throttle_effective", m_speedSetting, "smoothed");
        logScriptWrite("sim.control", "clutch_pressure", m_clutchPressure, "smoothed");
        s_lastLoggedThrottleEffective = m_speedSetting;
        s_lastLoggedClutchEffective = m_clutchPressure;
        s_nextAnalogLog = now + std::chrono::seconds(1);
    }

}

void EngineSimApplication::renderScene() {
    const auto layoutStart = std::chrono::steady_clock::now();
    DebugTrace::Log("ui", "layout recompute begin screen=%d", m_screen);
    getShaders()->ResetBaseColor();
    getShaders()->SetObjectTransform(ysMath::LoadIdentity());

    m_textRenderer.SetColor(ysColor::linearToSrgb(m_foreground));
    m_shaders.SetClearColor(ysColor::linearToSrgb(m_shadow));

    const int screenWidth = m_engine.GetGameWindow()->GetGameWidth();
    const int screenHeight = m_engine.GetGameWindow()->GetGameHeight();
    const float aspectRatio = screenWidth / (float)screenHeight;

    const Point cameraPos = m_engineView->getCameraPosition();
    static Point s_lastCameraPos = { 0.0f, 0.0f };
    static bool s_cameraInitialized = false;
    if (!s_cameraInitialized || cameraPos.x != s_lastCameraPos.x || cameraPos.y != s_lastCameraPos.y) {
        DebugTrace::Log(
            "ui",
            "camera transform update x=%.3f y=%.3f",
            cameraPos.x,
            cameraPos.y);
        s_lastCameraPos = cameraPos;
        s_cameraInitialized = true;
    }
    m_shaders.m_cameraPosition = ysMath::LoadVector(cameraPos.x, cameraPos.y);

    m_shaders.CalculateUiCamera(screenWidth, screenHeight);

    if (m_screen == 0) {
        Bounds windowBounds((float)screenWidth, (float)screenHeight, { 0, (float)screenHeight });
        Grid grid;
        grid.v_cells = 2;
        grid.h_cells = 3;
        Grid grid3x3;
        grid3x3.v_cells = 3;
        grid3x3.h_cells = 3;
        m_engineView->setDrawFrame(true);
        m_engineView->setBounds(grid.get(windowBounds, 1, 0, 1, 1));
        m_engineView->setLocalPosition({ 0, 0 });

        m_rightGaugeCluster->m_bounds = grid.get(windowBounds, 2, 0, 1, 2);
        m_oscCluster->m_bounds = grid.get(windowBounds, 1, 1);
        m_performanceCluster->m_bounds = grid3x3.get(windowBounds, 0, 1);
        m_loadSimulationCluster->m_bounds = grid3x3.get(windowBounds, 0, 2);

        Grid grid1x3;
        grid1x3.v_cells = 3;
        grid1x3.h_cells = 1;
        m_mixerCluster->m_bounds = grid1x3.get(grid3x3.get(windowBounds, 0, 0), 0, 2);
        m_infoCluster->m_bounds = grid1x3.get(grid3x3.get(windowBounds, 0, 0), 0, 0, 1, 2);

        m_engineView->setVisible(true);
        m_rightGaugeCluster->setVisible(true);
        m_oscCluster->setVisible(true);
        m_performanceCluster->setVisible(true);
        m_loadSimulationCluster->setVisible(true);
        m_mixerCluster->setVisible(true);
        m_infoCluster->setVisible(true);

        m_oscCluster->activate();
    }
    else if (m_screen == 1) {
        Bounds windowBounds((float)screenWidth, (float)screenHeight, { 0, (float)screenHeight });
        m_engineView->setDrawFrame(false);
        m_engineView->setBounds(windowBounds);
        m_engineView->setLocalPosition({ 0, 0 });
        m_engineView->activate();

        m_engineView->setVisible(true);
        m_rightGaugeCluster->setVisible(false);
        m_oscCluster->setVisible(false);
        m_performanceCluster->setVisible(false);
        m_loadSimulationCluster->setVisible(false);
        m_mixerCluster->setVisible(false);
        m_infoCluster->setVisible(false);
    }
    else if (m_screen == 2) {
        Bounds windowBounds((float)screenWidth, (float)screenHeight, { 0, (float)screenHeight });
        Grid grid;
        grid.v_cells = 1;
        grid.h_cells = 3;
        m_engineView->setDrawFrame(true);
        m_engineView->setBounds(grid.get(windowBounds, 0, 0, 2, 1));
        m_engineView->setLocalPosition({ 0, 0 });
        m_engineView->activate();

        m_rightGaugeCluster->m_bounds = grid.get(windowBounds, 2, 0, 1, 1);

        m_engineView->setVisible(true);
        m_rightGaugeCluster->setVisible(true);
        m_oscCluster->setVisible(false);
        m_performanceCluster->setVisible(false);
        m_loadSimulationCluster->setVisible(false);
        m_mixerCluster->setVisible(false);
        m_infoCluster->setVisible(false);
    }

    static int s_lastScreen = -1;
    if (s_lastScreen != m_screen) {
        DebugTrace::Log("ui", "user_mode_transition screen old=%d new=%d", s_lastScreen, m_screen);
        s_lastScreen = m_screen;
    }

    const float cameraAspectRatio =
        m_engineView->m_bounds.width() / m_engineView->m_bounds.height();
    m_engine.GetDevice()->ResizeRenderTarget(
        m_mainRenderTarget,
        m_engineView->m_bounds.width(),
        m_engineView->m_bounds.height(),
        m_engineView->m_bounds.width(),
        m_engineView->m_bounds.height()
    );
    m_engine.GetDevice()->RepositionRenderTarget(
        m_mainRenderTarget,
        m_engineView->m_bounds.getPosition(Bounds::tl).x,
        screenHeight - m_engineView->m_bounds.getPosition(Bounds::tl).y
    );
    m_shaders.CalculateCamera(
        cameraAspectRatio * m_displayHeight / m_engineView->m_zoom,
        m_displayHeight / m_engineView->m_zoom,
        m_engineView->m_bounds,
        m_screenWidth,
        m_screenHeight,
        m_displayAngle);

    m_geometryGenerator.reset();

    render();

    m_engine.GetDevice()->EditBufferDataRange(
        m_geometryVertexBuffer,
        (char *)m_geometryGenerator.getVertexData(),
        sizeof(dbasic::Vertex) * m_geometryGenerator.getCurrentVertexCount(),
        0);

    m_engine.GetDevice()->EditBufferDataRange(
        m_geometryIndexBuffer,
        (char *)m_geometryGenerator.getIndexData(),
        sizeof(unsigned short) * m_geometryGenerator.getCurrentIndexCount(),
        0);

    DebugTrace::Log(
        "mainloop",
        "render_queue_cpu_proxies vertices=%d indices=%d",
        m_geometryGenerator.getCurrentVertexCount(),
        m_geometryGenerator.getCurrentIndexCount());
    const auto layoutEnd = std::chrono::steady_clock::now();
    DebugTrace::Log(
        "ui",
        "layout recompute end duration_us=%lld",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(layoutEnd - layoutStart).count()));
}

void EngineSimApplication::refreshUserInterface() {
    m_uiManager.destroy();
    m_uiManager.initialize(this);

    m_engineView = m_uiManager.getRoot()->addElement<EngineView>();
    m_rightGaugeCluster = m_uiManager.getRoot()->addElement<RightGaugeCluster>();
    m_oscCluster = m_uiManager.getRoot()->addElement<OscilloscopeCluster>();
    m_performanceCluster = m_uiManager.getRoot()->addElement<PerformanceCluster>();
    m_loadSimulationCluster = m_uiManager.getRoot()->addElement<LoadSimulationCluster>();
    m_mixerCluster = m_uiManager.getRoot()->addElement<MixerCluster>();
    m_infoCluster = m_uiManager.getRoot()->addElement<InfoCluster>();

    m_infoCluster->setEngine(m_iceEngine);
    m_rightGaugeCluster->m_simulator = m_simulator;
    m_rightGaugeCluster->setEngine(m_iceEngine);
    m_oscCluster->setSimulator(m_simulator);
    if (m_iceEngine != nullptr) {
        m_oscCluster->setDynoMaxRange(units::toRpm(m_iceEngine->getRedline()));
    }
    m_performanceCluster->setSimulator(m_simulator);
    m_loadSimulationCluster->setSimulator(m_simulator);
    m_mixerCluster->setSimulator(m_simulator);
}

void EngineSimApplication::startRecording() {
    m_recording = true;

#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
    atg_dtv::Encoder::VideoSettings settings{};

    // Output filename
    settings.fname = "../workspace/video_capture/engine_sim_video_capture.mp4";
    settings.inputWidth = m_engine.GetScreenWidth();
    settings.inputHeight = m_engine.GetScreenHeight();
    settings.width = settings.inputWidth;
    settings.height = settings.inputHeight;
    settings.hardwareEncoding = true;
    settings.inputAlpha = true;
    settings.bitRate = 40000000;

    m_encoder.run(settings, 2);
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
}

void EngineSimApplication::updateScreenSizeStability() {
    m_screenResolution[m_screenResolutionIndex][0] = m_engine.GetScreenWidth();
    m_screenResolution[m_screenResolutionIndex][1] = m_engine.GetScreenHeight();

    m_screenResolutionIndex = (m_screenResolutionIndex + 1) % ScreenResolutionHistoryLength;
}

bool EngineSimApplication::readyToRecord() {
    const int w = m_screenResolution[0][0];
    const int h = m_screenResolution[0][1];

    if (w <= 0 && h <= 0) return false;
    if ((w % 2) != 0 || (h % 2) != 0) return false;

    for (int i = 1; i < ScreenResolutionHistoryLength; ++i) {
        if (m_screenResolution[i][0] != w) return false;
        if (m_screenResolution[i][1] != h) return false;
    }

    return true;
}

void EngineSimApplication::stopRecording() {
    m_recording = false;

#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
    m_encoder.commit();
    m_encoder.stop();
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
}

void EngineSimApplication::recordFrame() {
#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
    atg_dtv::Frame *frame = m_encoder.newFrame(false);
    if (frame != nullptr && m_encoder.getError() == atg_dtv::Encoder::Error::None) {
        m_engine.GetDevice()->ReadRenderTarget(m_engine.GetScreenRenderTarget(), frame->m_rgb);
    }

    m_encoder.submitFrame();
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
}
