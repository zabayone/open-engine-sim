#include "wav_postprocess.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>

namespace {
double outputAmplitude(double frequency) {
    constexpr int sampleRate = 44100;
    wav_postprocess::Bandpass bandpass(sampleRate);
    double inputEnergy = 0.0;
    double outputEnergy = 0.0;
    for (int i = 0; i < sampleRate; ++i) {
        const double input = 12000.0 * std::sin(
            2.0 * wav_postprocess::pi * frequency * i / sampleRate);
        const double output = bandpass.process(wav_postprocess::pcm16(input));
        if (i > sampleRate / 4) {
            inputEnergy += input * input;
            outputEnergy += output * output;
        }
    }
    return std::sqrt(outputEnergy / inputEnergy);
}
} // namespace

TEST(WavPostprocess, BandpassRejectsRumbleAndUltrasonicContent) {
    EXPECT_LT(outputAmplitude(5.0), 0.1);
    EXPECT_GT(outputAmplitude(1000.0), 0.95);
    EXPECT_LT(outputAmplitude(21000.0), 0.5);
}

TEST(WavPostprocess, FadeTouchesOnlyTheEdges) {
    constexpr std::size_t count = 44100;
    const auto fade = wav_postprocess::edgeSamples(44100);
    EXPECT_DOUBLE_EQ(wav_postprocess::edgeGain(0, count, fade), 0.0);
    EXPECT_DOUBLE_EQ(wav_postprocess::edgeGain(count - 1, count, fade), 0.0);
    EXPECT_DOUBLE_EQ(wav_postprocess::edgeGain(count / 2, count, fade), 1.0);
}
