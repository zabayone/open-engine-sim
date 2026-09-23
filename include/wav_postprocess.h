#ifndef ENGINE_SIM_WAV_POSTPROCESS_H
#define ENGINE_SIM_WAV_POSTPROCESS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Offline WAV conditioning. Keep this out of the real-time audio callback.
namespace wav_postprocess {
constexpr double pi = 3.14159265358979323846;
constexpr double edgeSeconds = 0.01;

struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    static Biquad butterworth(double cutoff, int sampleRate, bool highPass) {
        const double omega = 2.0 * pi * cutoff / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / std::sqrt(2.0);
        const double a0 = 1.0 + alpha;
        Biquad filter;
        filter.b0 = (1.0 + (highPass ? cosine : -cosine)) / (2.0 * a0);
        filter.b1 = (highPass ? -(1.0 + cosine) : 1.0 - cosine) / a0;
        filter.b2 = filter.b0;
        filter.a1 = -2.0 * cosine / a0;
        filter.a2 = (1.0 - alpha) / a0;
        return filter;
    }

    double process(double input) {
        const double output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;
        return output;
    }
};

inline std::int16_t pcm16(double value) {
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -32768.0, 32767.0)));
}

class Bandpass {
public:
    explicit Bandpass(int sampleRate)
        : m_highPass(Biquad::butterworth(20.0, sampleRate, true)),
          m_lowPass(sampleRate > 40000
              ? Biquad::butterworth(20000.0, sampleRate, false) : Biquad{}),
          m_useLowPass(sampleRate > 40000) { }

    std::int16_t process(std::int16_t sample) {
        double output = m_highPass.process(sample);
        if (m_useLowPass) output = m_lowPass.process(output);
        return pcm16(output);
    }

private:
    Biquad m_highPass;
    Biquad m_lowPass;
    bool m_useLowPass;
};

inline std::size_t edgeSamples(int sampleRate) {
    return std::max<std::size_t>(2, static_cast<std::size_t>(std::lround(edgeSeconds * sampleRate)));
}

inline double fadeInGain(std::size_t index, std::size_t fadeSamples) {
    return index < fadeSamples
        ? 0.5 - 0.5 * std::cos(pi * index / (fadeSamples - 1)) : 1.0;
}

inline double edgeGain(std::size_t index, std::size_t count, std::size_t fadeSamples) {
    if (count <= 1) return 0.0;
    const double start = fadeInGain(index, fadeSamples);
    const std::size_t remaining = count - 1 - index;
    const double end = fadeInGain(remaining, fadeSamples);
    return std::min(start, end);
}
} // namespace wav_postprocess

#endif
