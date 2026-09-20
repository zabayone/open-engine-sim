#include "../include/desktop_platform_sdl.h"
#include "../include/engine_sim_application.h"
#include "../include/runtime_paths.h"
#include "../include/sdl_gpu_renderer.h"
#include "../include/sdl_audio_output.h"

#include <cstdio>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <cctype>
#include <filesystem>
#include <string>

int main(int argc, char **argv) {
    std::string initialScript;
    std::string recordingPath;
    std::string trainerSession;
    std::string windowTitle = "Open Engine Simulator";
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--script" && i + 1 < argc) initialScript = argv[++i];
        else if (argument == "--record-wav" && i + 1 < argc) recordingPath = argv[++i];
        else if (argument == "--trainer-session" && i + 1 < argc) trainerSession = argv[++i];
        else if (argument == "--window-title" && i + 1 < argc) windowTitle = argv[++i];
        else if (argument == "--help") {
            std::printf(
                "Usage: engine-sim-desktop [--script SCRIPT_PATH] [--record-wav OUTPUT_PATH] "
                "[--window-title TITLE] [--trainer-session DIRECTORY]\n");
            return 0;
        }
        else {
            std::fprintf(stderr, "Unknown or incomplete argument: %s\n", argument.c_str());
            return 2;
        }
    }

    DesktopPlatformSdl platform;
    if (!platform.initialize(windowTitle, 1920, 1080)) {
        std::fprintf(stderr, "SDL initialization failed: %s\n", platform.lastError().c_str());
        return 1;
    }

    const RuntimePaths paths = RuntimePaths::discover(
        platform.applicationDirectory(), ENGINE_SIM_SOURCE_ASSET_DIRECTORY);
    const std::filesystem::path packagedShaderDirectory = paths.assetDirectory / "shaders";
    const std::string shaderDirectory = std::filesystem::exists(
            packagedShaderDirectory / "engine_sim.vertex.msl")
        ? packagedShaderDirectory.string()
        : ENGINE_SIM_SHADER_DIRECTORY;

    SdlGpuRenderer renderer;
    if (!renderer.initialize(platform.nativeWindowHandle(), shaderDirectory)) {
        std::fprintf(stderr, "SDL GPU initialization failed: %s\n", renderer.lastError());
        return 1;
    }

    EngineSimApplication application;
    SdlAudioOutput audioOutput;
    if (!recordingPath.empty()) audioOutput.setRecordingPath(recordingPath);
    if (!initialScript.empty()) application.setInitialEngineScript(initialScript);
    application.setTrainerSession(trainerSession);
    SDL_AudioStream *preview = nullptr;
    application.trainerCommand = [&](const std::string &operation, const std::string &path) {
        if (operation == "record") {
            audioOutput.stop();
            audioOutput.setRecordingPath(path);
            if (!audioOutput.start(application.getSimulator()))
                throw std::runtime_error("Unable to start audio recording/output");
        } else if (operation == "folder") {
            // Encode file paths for SDL_OpenURL, including spaces and non-ASCII bytes.
            std::string url = "file://";
#ifdef _WIN32
            url += '/';
#endif
            const char *digits = "0123456789ABCDEF";
            for (unsigned char c : std::filesystem::path(path).generic_string()) {
                if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '_' || c == '.') url += c;
                else { url += '%'; url += digits[c >> 4]; url += digits[c & 15]; }
            }
            if (!platform.openUrl(url)) throw std::runtime_error("Unable to open saved runs folder");
        } else if (operation == "stopplay") {
            if (preview) { SDL_DestroyAudioStream(preview); preview = nullptr; }
        } else if (operation == "play") {
            if (preview) { SDL_DestroyAudioStream(preview); preview = nullptr; }
            SDL_AudioSpec spec;
            Uint8 *data = nullptr;
            Uint32 length = 0;
            if (!SDL_LoadWAV(path.c_str(), &spec, &data, &length)) throw std::runtime_error(SDL_GetError());
            preview = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            const bool queued = preview && SDL_PutAudioStreamData(preview, data, length);
            SDL_free(data);
            if (!queued || !SDL_ResumeAudioStreamDevice(preview)) throw std::runtime_error(SDL_GetError());
        }
    };
    application.initialize(&platform, &renderer, &audioOutput, paths);
    if (!trainerSession.empty()) application.showTrainerOverlay();
    application.run();
    application.destroy();

    if (preview) SDL_DestroyAudioStream(preview);
    renderer.shutdown();
    platform.shutdown();
    return 0;
}
