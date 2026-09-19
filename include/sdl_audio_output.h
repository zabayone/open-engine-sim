#ifndef ATG_ENGINE_SIM_SDL_AUDIO_OUTPUT_H
#define ATG_ENGINE_SIM_SDL_AUDIO_OUTPUT_H

#include "audio_output.h"

#include <SDL3/SDL_audio.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SdlAudioOutput final : public AudioOutput {
public:
    ~SdlAudioOutput() override { stop(); }
    bool start(Simulator *simulator) override;
    bool loadImpulseResponse(Synthesizer &synthesizer, const std::string &path, float volume, int index) override;
    void stop() override;
    void setRecordingPath(const std::string &path) { m_recordingPath = path; }

private:
    void audioThread();
    void recordingThread();
    void fillStream();
    void stopLocked(bool finalizeRecording);

    SDL_AudioStream *m_stream = nullptr;
    Simulator *m_simulator = nullptr;
    std::atomic<bool> m_running = false;
    std::mutex m_lifecycleMutex;
    std::thread m_thread;
    bool m_diagnostics = false;
    std::uint64_t m_lastDiagnosticTick = 0;
    std::uint64_t m_pcmFrames = 0;
    std::uint64_t m_silenceFrames = 0;
    int m_peakQueuedBytes = 0;
    std::string m_recordingPath;
    std::vector<std::int16_t> m_recordingBuffer;
    std::atomic<std::uint64_t> m_recordingRead = 0;
    std::atomic<std::uint64_t> m_recordingWrite = 0;
    std::atomic<bool> m_recordingRunning = false;
    std::thread m_recordingThread;
};

#endif
