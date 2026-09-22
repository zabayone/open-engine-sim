#include "compiler.h"
#include "engine.h"
#include "simulator.h"
#include "transmission.h"
#include "vehicle.h"
#include "starter_motor.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

TEST(StarterSizing, PreservesStrongerAuthoredMotors) {
    StarterMotor motor;
    motor.sizeForDisplacement(units::torque(500, units::Nm), units::volume(5, units::L));
    EXPECT_DOUBLE_EQ(motor.m_maxTorque, units::torque(500, units::Nm));
    motor.sizeForDisplacement(units::torque(100, units::Nm), units::volume(27, units::L));
    EXPECT_DOUBLE_EQ(motor.m_maxTorque, units::torque(1620, units::Nm));
}

TEST(EngineStartup, LargeEnginesKeepFiringAfterTwoSecondCrank) {
    const std::filesystem::path assets(ENGINE_SIM_TEST_ASSET_DIRECTORY);
    const auto resized = std::filesystem::temp_directory_path()
        / ("engine-startup-resized-" + std::to_string(std::random_device{}()) + ".mr");
    {
        std::ifstream original(assets / "engines/atg-video-2/07_gm_ls.mr");
        ASSERT_TRUE(original.good());
        std::string source((std::istreambuf_iterator<char>(original)), {});
        const double pi = 3.141592653589793;
        const double baseCc = pi * std::pow(3.78 * 25.4, 2) * (3.622 * 25.4) * 8 / 4000.0;
        const double ratio = 16000.0 / baseCc;
        const double scale = std::cbrt(ratio);
        const auto replace = [&](const std::string &oldValue, const std::string &newValue) {
            const size_t offset = source.find(oldValue);
            EXPECT_NE(offset, std::string::npos);
            if (offset != std::string::npos) source.replace(offset, oldValue.size(), newValue);
        };
        replace("label bore(3.78 * units.inch)",
            "label bore(" + std::to_string(3.78 * scale) + " * units.inch)");
        replace("label stroke(3.622 * units.inch)",
            "label stroke(" + std::to_string(3.622 * scale) + " * units.inch)");
        const double idle = std::acos(std::cos(0.996 * pi / 2) * ratio) * 2 / pi;
        replace("idle_throttle_plate_position: 0.996",
            "idle_throttle_plate_position: " + std::to_string(idle));
        std::ofstream output(resized);
        output << source;
    }
    for (const auto &script : std::vector<std::filesystem::path>{
        "engines/atg-video-2/11_merlin_v12.mr",
        "engines/atg-video-2/09_radial_9.mr", resized}) {
        SCOPED_TRACE(script.string());
        const auto entry = std::filesystem::temp_directory_path()
            / ("engine-startup-" + std::to_string(std::random_device{}()) + ".mr");
        { std::ofstream file(entry); file << "import \"" << (assets / script).generic_string() << "\"\nmain()\n"; }
        es_script::Compiler compiler;
        compiler.initialize(assets.string());
        ASSERT_TRUE(compiler.compile(entry.string()));
        const auto output = compiler.execute();
        compiler.destroy();
        std::filesystem::remove(entry);
        ASSERT_NE(output.engine, nullptr);
        Engine *engine = output.engine;
        Simulator *simulator = engine->createSimulator(output.vehicle, output.transmission, 44100);
        simulator->setSimulationFrequency(engine->getSimulationFrequency());
        simulator->setSynthesizerLatencyCorrectionEnabled(false);
        engine->setSpeedControl(0);
        engine->getIgnitionModule()->m_enabled = true;
        double finalMinimum = 1e9, finalMaximum = 0;
        int16_t audio[1024];
        std::srand(0);
        for (int block = 0; block < 800; ++block) {
            simulator->m_starterMotor.m_enabled = block < 200;
            simulator->startFrame(0.01);
            while (simulator->simulateStep()) {}
            simulator->endFrame();
            simulator->synthesizer().pumpAudioRendering();
            simulator->readAudioOutput(1024, audio);
            if (block == 199) EXPECT_GT(engine->getRpm(), units::toRpm(engine->getStarterSpeed()) * 0.9);
            if (block >= 600) {
                finalMinimum = std::min(finalMinimum, engine->getRpm());
                finalMaximum = std::max(finalMaximum, engine->getRpm());
            }
        }
        EXPECT_GT(finalMinimum, units::toRpm(engine->getStarterSpeed()) * 1.2);
        EXPECT_LT(finalMaximum / finalMinimum, 1.4);
        simulator->releaseSimulation(); delete simulator;
        engine->destroy(); delete engine;
        delete output.vehicle; delete output.transmission;
    }
    std::filesystem::remove(resized);
}
