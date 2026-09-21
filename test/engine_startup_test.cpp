#include "compiler.h"
#include "engine.h"
#include "simulator.h"
#include "transmission.h"
#include "vehicle.h"
#include "starter_motor.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

TEST(StarterSizing, PreservesStrongerAuthoredMotors) {
    StarterMotor motor;
    motor.sizeForDisplacement(units::torque(200, units::Nm), units::volume(5, units::L));
    EXPECT_DOUBLE_EQ(motor.m_maxTorque, units::torque(200, units::Nm));
    motor.sizeForDisplacement(units::torque(100, units::Nm), units::volume(27, units::L));
    EXPECT_DOUBLE_EQ(motor.m_maxTorque, units::torque(540, units::Nm));
}

TEST(EngineStartup, LargeEnginesKeepFiringAfterTwoSecondCrank) {
    const std::filesystem::path assets(ENGINE_SIM_TEST_ASSET_DIRECTORY);
    for (const char *script : {"engines/atg-video-2/11_merlin_v12.mr", "engines/atg-video-2/09_radial_9.mr"}) {
        SCOPED_TRACE(script);
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
}
