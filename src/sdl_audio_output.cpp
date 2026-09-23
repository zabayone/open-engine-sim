#include "../include/sdl_audio_output.h"
#include "../include/sdl_audio_util.h"
#include "../include/simulator.h"
#include "../include/wav_postprocess.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
void writeU16(std::ostream &stream, std::uint16_t value) {
    const char bytes[] = { static_cast<char>(value), static_cast<char>(value >> 8) };
    stream.write(bytes, 2);
}

void writeU32(std::ostream &stream, std::uint32_t value) {
    const char bytes[] = {
        static_cast<char>(value), static_cast<char>(value >> 8),
        static_cast<char>(value >> 16), static_cast<char>(value >> 24),
    };
    stream.write(bytes, 4);
}

void writeWavHeader(std::ostream &output, std::uint32_t dataSize) {
    output.write("RIFF", 4);
    writeU32(output, 36 + dataSize);
    output.write("WAVEfmt ", 8);
    writeU32(output, 16);
    writeU16(output, 1);
    writeU16(output, 1);
    writeU32(output, 44100);
    writeU32(output, 44100 * sizeof(std::int16_t));
    writeU16(output, sizeof(std::int16_t));
    writeU16(output, 16);
    output.write("data", 4);
    writeU32(output, dataSize);
}
} // namespace

bool SdlAudioOutput::start(Simulator *simulator) {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    stopLocked(false);
    if (simulator == nullptr) return false;
    // The synthesizer produces 44.1 kHz PCM. Keep this stream in that native
    // clock domain; SDL handles only the final conversion to the device rate.
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, 44100 };
    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (m_stream == nullptr) return false;
    m_simulator = simulator;
    m_diagnostics = SDL_GetHintBoolean("ENGINE_SIM_AUDIO_DIAGNOSTICS", false);
    m_lastDiagnosticTick = SDL_GetTicks();
    m_pcmFrames = 0;
    m_silenceFrames = 0;
    m_peakQueuedBytes = 0;
    if (!m_recordingPath.empty() && !m_recordingThread.joinable()) {
        const std::filesystem::path outputPath(m_recordingPath);
        if (!outputPath.parent_path().empty()) {
            std::filesystem::create_directories(outputPath.parent_path());
        }
        constexpr std::size_t recordingBufferSeconds = 5;
        m_recordingBuffer.resize(44100 * recordingBufferSeconds);
        m_recordingRead = 0;
        m_recordingWrite = 0;
        m_recordingRunning = true;
        m_recordingThread = std::thread(&SdlAudioOutput::recordingThread, this);
    }
    if (m_diagnostics) {
        SDL_AudioSpec source = {}, destination = {};
        if (SDL_GetAudioStreamFormat(m_stream, &source, &destination)) {
            std::fprintf(stderr, "audio: stream=%dHz/%dch -> device=%dHz/%dch\n",
                source.freq, source.channels, destination.freq, destination.channels);
        }
    }
    if (!SDL_ResumeAudioStreamDevice(m_stream)) {
        stopLocked(true);
        return false;
    }
    m_running = true;
    m_thread = std::thread(&SdlAudioOutput::audioThread, this);
    return true;
}

void SdlAudioOutput::audioThread() {
    while (m_running) {
        fillStream();
        if (m_diagnostics) {
            const std::uint64_t now = SDL_GetTicks();
            if (now - m_lastDiagnosticTick >= 1000) {
                const int queuedBytes = SDL_GetAudioStreamQueued(m_stream);
                std::fprintf(stderr, "audio: pcm=%llu silence=%llu input=%.3fs output=%.3fs stream=%.3fs peak-queue=%dB\n",
                    static_cast<unsigned long long>(m_pcmFrames),
                    static_cast<unsigned long long>(m_silenceFrames),
                    m_simulator != nullptr ? m_simulator->getSynthesizerInputLatency() : 0.0,
                    m_simulator != nullptr ? m_simulator->getSynthesizerOutputLatency() : 0.0,
                    std::max(0, queuedBytes) / (44100.0 * sizeof(std::int16_t)),
                    m_peakQueuedBytes);
                m_pcmFrames = 0;
                m_silenceFrames = 0;
                m_peakQueuedBytes = 0;
                m_lastDiagnosticTick = now;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void SdlAudioOutput::recordingThread() {
    std::ofstream output(m_recordingPath, std::ios::binary);
    if (!output) return;
    writeWavHeader(output, 0);
    std::uint64_t writtenSamples = 0;
    std::array<std::int16_t, 1024> block{};
    wav_postprocess::Bandpass bandpass(44100);
    const std::size_t fade = wav_postprocess::edgeSamples(44100);
    std::vector<std::int16_t> tail(fade);
    while (m_recordingRunning || m_recordingRead.load() < m_recordingWrite.load()) {
        const std::uint64_t read = m_recordingRead.load(std::memory_order_relaxed);
        const std::uint64_t write = m_recordingWrite.load(std::memory_order_acquire);
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(write - read, block.size()));
        if (count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::int16_t filtered = bandpass.process(
                m_recordingBuffer[(read + i) % m_recordingBuffer.size()]);
            tail[(writtenSamples + i) % fade] = filtered;
            const double startGain = wav_postprocess::fadeInGain(writtenSamples + i, fade);
            block[i] = wav_postprocess::pcm16(filtered * startGain);
        }
        output.write(reinterpret_cast<const char *>(block.data()), count * sizeof(std::int16_t));
        writtenSamples += count;
        m_recordingRead.store(read + count, std::memory_order_release);
    }
    const std::size_t tailCount = static_cast<std::size_t>(
        std::min<std::uint64_t>(writtenSamples, fade));
    output.seekp(44 + (writtenSamples - tailCount) * sizeof(std::int16_t));
    for (std::size_t i = 0; i < tailCount; ++i) {
        const std::size_t index = writtenSamples - tailCount + i;
        const std::int16_t sample = wav_postprocess::pcm16(
            tail[index % fade] * wav_postprocess::edgeGain(index, writtenSamples, fade));
        output.write(reinterpret_cast<const char *>(&sample), sizeof(sample));
    }
    output.seekp(0);
    writeWavHeader(output, static_cast<std::uint32_t>(writtenSamples * sizeof(std::int16_t)));
}

void SdlAudioOutput::fillStream() {
    if (m_stream == nullptr || m_simulator == nullptr) return;
    constexpr int chunkFrames = 512;
    // Keep a small, stable device lead. Larger queues hide underruns but make
    // controls feel disconnected and turn a single discontinuity into a
    // conspicuous delayed clack.
    constexpr int targetFrames = 1024;
    constexpr int targetBytes = targetFrames * static_cast<int>(sizeof(std::int16_t));
    int queuedBytes = SDL_GetAudioStreamQueued(m_stream);
    if (queuedBytes < 0) return;
    while (queuedBytes < targetBytes) {
        std::array<std::int16_t, chunkFrames> samples{};
        const int frames = std::min(chunkFrames,
            (targetBytes - queuedBytes) / static_cast<int>(sizeof(std::int16_t)));
        // readAudioOutput zero-fills a short read. Queuing the complete chunk
        // preserves the fixed lead the DirectSound ring buffer provided at
        // startup and during a transient synthesizer underrun.
        const int pcmFrames = m_simulator->readAudioOutput(frames, samples.data());
        if (!m_recordingBuffer.empty()) {
            const std::uint64_t write = m_recordingWrite.load(std::memory_order_relaxed);
            const std::uint64_t read = m_recordingRead.load(std::memory_order_acquire);
            const std::size_t available = m_recordingBuffer.size()
                - static_cast<std::size_t>(std::min<std::uint64_t>(
                    write - read, m_recordingBuffer.size()));
            const std::size_t count = std::min<std::size_t>(frames, available);
            for (std::size_t i = 0; i < count; ++i) {
                m_recordingBuffer[(write + i) % m_recordingBuffer.size()] = samples[i];
            }
            m_recordingWrite.store(write + count, std::memory_order_release);
        }
        m_pcmFrames += std::max(0, pcmFrames);
        m_silenceFrames += frames - std::max(0, pcmFrames);
        const int bytes = frames * static_cast<int>(sizeof(std::int16_t));
        if (!SDL_PutAudioStreamData(m_stream, samples.data(), bytes)) return;
        queuedBytes += bytes;
        m_peakQueuedBytes = std::max(m_peakQueuedBytes, queuedBytes);
    }
}

bool SdlAudioOutput::loadImpulseResponse(Synthesizer &synthesizer, const std::string &path, float volume, int index) {
    return loadSdlImpulseResponse(synthesizer, path, volume, index);
}

void SdlAudioOutput::stop() {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    stopLocked(true);
}

void SdlAudioOutput::stopLocked(bool finalizeRecording) {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (finalizeRecording) {
        m_recordingRunning = false;
        if (m_recordingThread.joinable()) m_recordingThread.join();
        m_recordingBuffer.clear();
    }
    if (m_stream != nullptr) SDL_DestroyAudioStream(m_stream);
    m_stream = nullptr;
    m_simulator = nullptr;
}
