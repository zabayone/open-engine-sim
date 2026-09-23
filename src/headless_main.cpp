#include "../include/engine.h"
#include "../include/impulse_response.h"
#include "../include/simulator.h"
#include "../include/transmission.h"
#include "../include/units.h"
#include "../include/vehicle.h"
#include "../include/wav_postprocess.h"
#include "../scripting/include/compiler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Options {
    std::filesystem::path assets = ENGINE_SIM_SOURCE_ASSET_DIRECTORY;
    std::filesystem::path script = "engines/atg-video-2/07_gm_ls.mr";
    std::filesystem::path output = "engine-sim-render.wav";
    std::filesystem::path rpmReport;
    double rpm = 3000.0;
    double duration = 2.0;
    double warmup = 1.0;
    double speedControl = 0.1;
    double volume = 0.25;
    int sampleRate = 44100;
    bool edgeFade = true;
};

std::uint16_t readU16(std::istream &stream) {
    std::uint8_t bytes[2]{};
    stream.read(reinterpret_cast<char *>(bytes), 2);
    return static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t readU32(std::istream &stream) {
    std::uint8_t bytes[4]{};
    stream.read(reinterpret_cast<char *>(bytes), 4);
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void writeU16(std::ostream &stream, std::uint16_t value) {
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
    };
    stream.write(bytes, 2);
}

void writeU32(std::ostream &stream, std::uint32_t value) {
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    stream.write(bytes, 4);
}

std::vector<std::int16_t> readMonoPcm16Wav(const std::filesystem::path &path, int sampleRate) {
    std::ifstream input(path, std::ios::binary);
    char riff[4]{};
    char wave[4]{};
    input.read(riff, 4);
    readU32(input);
    input.read(wave, 4);
    if (!input || std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE") {
        throw std::runtime_error("Unsupported impulse response WAV: " + path.string());
    }

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t rate = 0;
    std::vector<std::int16_t> samples;
    while (input) {
        char id[4]{};
        input.read(id, 4);
        if (!input) break;
        const std::uint32_t size = readU32(input);
        const std::string chunk(id, 4);
        if (chunk == "fmt ") {
            format = readU16(input);
            channels = readU16(input);
            rate = readU32(input);
            readU32(input);
            readU16(input);
            bits = readU16(input);
            if (size > 16) input.seekg(size - 16, std::ios::cur);
        } else if (chunk == "data") {
            samples.resize(size / sizeof(std::int16_t));
            input.read(reinterpret_cast<char *>(samples.data()), size);
        } else {
            input.seekg(size, std::ios::cur);
        }
        if ((size & 1U) != 0) input.seekg(1, std::ios::cur);
    }
    if (format != 1 || channels != 1 || bits != 16 || rate != static_cast<std::uint32_t>(sampleRate)
        || samples.empty()) {
        throw std::runtime_error("Impulse response must be mono PCM16 at the output sample rate: "
            + path.string());
    }
    return samples;
}

void writeMonoPcm16Wav(
    const std::filesystem::path &path,
    const std::vector<std::int16_t> &samples,
    int sampleRate)
{
    std::filesystem::create_directories(path.parent_path().empty() ? "." : path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Cannot open output WAV: " + path.string());
    const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    output.write("RIFF", 4);
    writeU32(output, 36 + dataSize);
    output.write("WAVEfmt ", 8);
    writeU32(output, 16);
    writeU16(output, 1);
    writeU16(output, 1);
    writeU32(output, static_cast<std::uint32_t>(sampleRate));
    writeU32(output, static_cast<std::uint32_t>(sampleRate * sizeof(std::int16_t)));
    writeU16(output, sizeof(std::int16_t));
    writeU16(output, 16);
    output.write("data", 4);
    writeU32(output, dataSize);
    output.write(reinterpret_cast<const char *>(samples.data()), dataSize);
}

Options parseOptions(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc) throw std::runtime_error("Missing value after " + argument);
            return argv[i];
        };
        if (argument == "--assets") options.assets = value();
        else if (argument == "--script") options.script = value();
        else if (argument == "--output") options.output = value();
        else if (argument == "--rpm-report") options.rpmReport = value();
        else if (argument == "--rpm") options.rpm = std::stod(value());
        else if (argument == "--duration") options.duration = std::stod(value());
        else if (argument == "--warmup") options.warmup = std::stod(value());
        else if (argument == "--speed-control") options.speedControl = std::stod(value());
        else if (argument == "--volume") options.volume = std::stod(value());
        else if (argument == "--sample-rate") options.sampleRate = std::stoi(value());
        else if (argument == "--no-edge-fade") options.edgeFade = false;
        else if (argument == "--help") {
            std::cout
                << "Usage: engine-sim-headless [options]\n"
                << "  --assets PATH          Asset directory\n"
                << "  --script PATH          Engine script relative to assets\n"
                << "  --output PATH          Output mono PCM16 WAV\n"
                << "  --rpm-report PATH      Write measured RPM/audio timing JSON\n"
                << "  --rpm VALUE            Dynamometer hold RPM\n"
                << "  --duration SECONDS     Captured duration\n"
                << "  --warmup SECONDS       Uncaptured settling time\n"
                << "  --speed-control VALUE  Engine speed-control input in [0, 1]\n"
                << "  --volume VALUE         Linear output gain in [0, 1]\n"
                << "  --sample-rate VALUE    Output sample rate\n"
                << "  --no-edge-fade        Keep unfaded PCM for loop construction\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + argument);
        }
    }
    if (!std::isfinite(options.rpm) || !std::isfinite(options.duration)
        || !std::isfinite(options.warmup) || !std::isfinite(options.speedControl)
        || !std::isfinite(options.volume) || options.duration * options.sampleRate < 1
        || options.rpm <= 0 || options.duration <= 0 || options.warmup < 0
        || options.sampleRate <= 0 || options.speedControl < 0 || options.speedControl > 1
        || options.volume < 0 || options.volume > 1) {
        throw std::runtime_error("Invalid numeric option");
    }
    return options;
}

void loadImpulseResponses(
    Engine &engine,
    Simulator &simulator,
    const std::filesystem::path &assets,
    int sampleRate)
{
    for (int index = 0; index < engine.getExhaustSystemCount(); ++index) {
        ImpulseResponse *response = engine.getExhaustSystem(index)->getImpulseResponse();
        if (response == nullptr) continue;
        const auto samples = readMonoPcm16Wav(response->getFilename(), sampleRate);
        simulator.synthesizer().initializeImpulseResponse(
            samples.data(), static_cast<unsigned int>(samples.size()),
            static_cast<float>(response->getVolume()), index);
    }
}

std::vector<std::int16_t> render(const Options &options) {
    es_script::Compiler compiler;
    compiler.initialize(options.assets.string());
    const std::filesystem::path script = options.assets / options.script;
    const std::filesystem::path entry = std::filesystem::temp_directory_path()
        / ("engine-sim-headless-entry-" + std::to_string(std::random_device{}()) + ".mr");
    {
        std::ofstream file(entry);
        file << "import \"" << script.generic_string() << "\"\n\nmain()\n";
    }
    const bool compiledSuccessfully = compiler.compile(entry.string());
    std::error_code ignored;
    std::filesystem::remove(entry, ignored);
    if (!compiledSuccessfully) {
        compiler.destroy();
        throw std::runtime_error("Engine script failed to compile; see error_log.log");
    }
    const es_script::Compiler::Output compiled = compiler.execute();
    compiler.destroy();
    if (compiled.engine == nullptr || compiled.vehicle == nullptr || compiled.transmission == nullptr) {
        throw std::runtime_error("Engine script did not produce a complete simulation");
    }

    Engine *engine = compiled.engine;
    Vehicle *vehicle = compiled.vehicle;
    Transmission *transmission = compiled.transmission;
    Simulator *simulator = engine->createSimulator(vehicle, transmission, options.sampleRate);
    engine->calculateDisplacement();
    simulator->setSimulationFrequency(static_cast<int>(engine->getSimulationFrequency()));
    simulator->setSynthesizerLatencyCorrectionEnabled(false);
    simulator->setMaximumSynthesizerInputLatency(-1.0);
    loadImpulseResponses(*engine, *simulator, options.assets, options.sampleRate);
    Synthesizer::AudioParameters audioParameters = simulator->synthesizer().getAudioParameters();
    audioParameters.volume = static_cast<float>(options.volume);
    simulator->synthesizer().setAudioParameters(audioParameters);

    std::srand(0);
    engine->setSpeedControl(options.speedControl);
    engine->getIgnitionModule()->m_enabled = true;
    for (int i = 0; i < engine->getCrankshaftCount(); ++i) {
        engine->getCrankshaft(i)->m_body.v_theta = -units::rpm(options.rpm);
    }
    simulator->m_dyno.m_enabled = true;
    simulator->m_dyno.m_hold = true;
    simulator->m_dyno.m_rotationSpeed = units::rpm(options.rpm);
    simulator->synthesizer().discardAudioOutput();

    // Use integer physics steps and retain only PCM actually produced.
    // Per-block zero padding would change the audio clock and apparent pitch.
    const double frequency = simulator->getSimulationFrequency();
    const double blockDuration = std::max(1.0, std::round(frequency * 0.01)) / frequency;
    const size_t requestedSamples = static_cast<size_t>(std::llround(options.duration * options.sampleRate));
    const size_t warmupSamples = static_cast<size_t>(std::llround(options.warmup * options.sampleRate));
    size_t discardedSamples = 0;
    std::vector<std::int16_t> audio;
    audio.reserve(requestedSamples);
    std::vector<std::int16_t> block(1024);
    double rpmSum = 0.0;
    double rpmMinimum = std::numeric_limits<double>::max();
    double rpmMaximum = 0.0;
    size_t measuredSamples = 0;
    while (audio.size() < requestedSamples) {
        simulator->startFrame(blockDuration);
        while (simulator->simulateStep()) { }
        simulator->endFrame();
        simulator->synthesizer().pumpAudioRendering();
        const int produced = simulator->readAudioOutput(static_cast<int>(block.size()), block.data());
        if (produced <= 0) throw std::runtime_error("Synthesizer produced no PCM samples");
        const size_t skip = std::min(static_cast<size_t>(produced), warmupSamples - discardedSamples);
        discardedSamples += skip;
        const size_t count = std::min(static_cast<size_t>(produced) - skip, requestedSamples - audio.size());
        if (count) {
            audio.insert(audio.end(), block.begin() + skip, block.begin() + skip + count);
            const double rpm = engine->getRpm();
            rpmSum += rpm * count;
            rpmMinimum = std::min(rpmMinimum, rpm);
            rpmMaximum = std::max(rpmMaximum, rpm);
            measuredSamples += count;
        }
    }
    const double meanRpm = rpmSum / measuredSamples;
    std::cout << "Engine RPM: mean=" << meanRpm << ", min=" << rpmMinimum << ", max=" << rpmMaximum << "\n";
    if (!options.rpmReport.empty()) {
        std::ofstream report(options.rpmReport);
        if (!report) throw std::runtime_error("Cannot write RPM report");
        report << std::setprecision(12)
            << "{\"requested_rpm\":" << options.rpm
            << ",\"mean_rpm\":" << meanRpm
            << ",\"minimum_rpm\":" << rpmMinimum
            << ",\"maximum_rpm\":" << rpmMaximum
            << ",\"cylinders\":" << engine->getCylinderCount()
            << ",\"firing_frequency_hz\":" << meanRpm * engine->getCylinderCount() / 120.0
            << ",\"sample_rate\":" << options.sampleRate
            << ",\"sample_count\":" << audio.size()
            << ",\"duration\":" << static_cast<double>(audio.size()) / options.sampleRate
            << ",\"speed_control\":" << options.speedControl << "}\n";
    }

    simulator->releaseSimulation();
    delete simulator;
    engine->destroy();
    delete engine;
    delete vehicle;
    delete transmission;
    return audio;
}
} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseOptions(argc, argv);
        auto samples = render(options);
        wav_postprocess::Bandpass bandpass(options.sampleRate);
        const std::size_t fade = wav_postprocess::edgeSamples(options.sampleRate);
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const double filtered = bandpass.process(samples[index]);
            const double gain = options.edgeFade
                ? wav_postprocess::edgeGain(index, samples.size(), fade) : 1.0;
            samples[index] = wav_postprocess::pcm16(filtered * gain);
        }
        writeMonoPcm16Wav(options.output, samples, options.sampleRate);
        std::cout << "Rendered " << samples.size() << " samples to " << options.output << "\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "engine-sim-headless: " << error.what() << "\n";
        return 1;
    }
}
