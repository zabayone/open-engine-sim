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
    motor.sizeForDisplacement(units::torque(500, units::Nm), units::volume(5, units::L), 8);
    EXPECT_DOUBLE_EQ(motor.m_maxTorque, units::torque(500, units::Nm));
    motor.sizeForDisplacement(units::torque(100, units::Nm), units::volume(16, units::L), 8);
    EXPECT_DOUBLE_EQ(motor.m_maxTorque, units::torque(960, units::Nm));
    motor.sizeForDisplacement(units::torque(100, units::Nm), units::volume(16, units::L), 4);
    EXPECT_GT(motor.m_maxTorque, units::torque(4800, units::Nm));
}

TEST(EngineStartup, LargeEnginesKeepFiringAfterTwoSecondCrank) {
    const std::filesystem::path assets(ENGINE_SIM_TEST_ASSET_DIRECTORY);
    struct Template {
        const char *path;
        const char *boreText, *strokeText, *chamberText, *idleText;
        int cylinders;
        const char *unit;
        bool fixedCrankInertia;
    };
    const std::vector<Template> templates = {
        {"engines/atg-video-2/07_gm_ls.mr", "3.78", "3.622", "90", "0.996", 8, "inch", false},
        {"engines/atg-video-2/01_subaru_ej25_eh.mr", "99.5", "79", "67", "0.9978", 4, "mm", false},
        {"engines/atg-video-1/05_honda_vtec.mr", "81", "87.2", "41.6", "0.9989", 4, "mm", true},
        {"engines/atg-video-2/03_2jz.mr", "86.0", "86.0", "50", "0.9965", 6, "mm", false},
    };
    std::vector<std::filesystem::path> scripts = {
        assets / "engines/atg-video-2/11_merlin_v12.mr",
        assets / "engines/atg-video-2/09_radial_9.mr",
    };
    for (const auto &engine : templates) {
        std::ifstream original(assets / engine.path);
        ASSERT_TRUE(original.good());
        std::string source((std::istreambuf_iterator<char>(original)), {});
        const double pi = 3.141592653589793;
        const double millimetres = std::string(engine.unit) == "inch" ? 25.4 : 1.0;
        const double bore = std::stod(engine.boreText), stroke = std::stod(engine.strokeText);
        const double baseCc = pi * std::pow(bore * millimetres, 2)
            * (stroke * millimetres) * engine.cylinders / 4000.0;
        const double ratio = 16000.0 / baseCc;
        const double scale = std::cbrt(ratio);
        const auto replace = [&](const std::string &oldValue, const std::string &newValue) {
            size_t offset = source.find(oldValue);
            EXPECT_NE(offset, std::string::npos);
            while (offset != std::string::npos) {
                source.replace(offset, oldValue.size(), newValue);
                offset = source.find(oldValue, offset + newValue.size());
            }
        };
        replace("label bore(" + std::string(engine.boreText) + " * units." + engine.unit + ")",
            "label bore(" + std::to_string(bore * scale) + " * units." + engine.unit + ")");
        replace("label stroke(" + std::string(engine.strokeText) + " * units." + engine.unit + ")",
            "label stroke(" + std::to_string(stroke * scale) + " * units." + engine.unit + ")");
        replace("chamber_volume: " + std::string(engine.chamberText) + " * units.cc",
            "chamber_volume: " + std::to_string(std::stod(engine.chamberText) * ratio) + " * units.cc");
        const double idle = std::acos(std::min(1.0, std::cos(std::stod(engine.idleText) * pi / 2) * ratio)) * 2 / pi;
        replace("idle_throttle_plate_position: " + std::string(engine.idleText),
            "idle_throttle_plate_position: " + std::to_string(idle));
        if (engine.fixedCrankInertia) {
            replace("moment_of_inertia: 0.22986844776863666 * 0.5",
                "moment_of_inertia: " + std::to_string(0.22986844776863666 * std::pow(ratio, 5.0 / 3.0)) + " * 0.5");
        }
        const auto resized = std::filesystem::temp_directory_path()
            / ("engine-startup-resized-" + std::to_string(std::random_device{}()) + ".mr");
        std::ofstream output(resized);
        output << source;
        output.close();
        scripts.push_back(resized);
    }
    for (const auto &script : scripts) {
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
        const double starterRpm = std::abs(units::toRpm(engine->getStarterSpeed()));
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
            if (block == 199) EXPECT_GT(engine->getRpm(), starterRpm * 0.9);
            if (block >= 600) {
                finalMinimum = std::min(finalMinimum, engine->getRpm());
                finalMaximum = std::max(finalMaximum, engine->getRpm());
            }
        }
        EXPECT_GT(finalMinimum, starterRpm * 1.2);
        EXPECT_LT(finalMaximum / finalMinimum, 1.4);
        simulator->releaseSimulation(); delete simulator;
        engine->destroy(); delete engine;
        delete output.vehicle; delete output.transmission;
    }
    for (size_t index = 2; index < scripts.size(); ++index) std::filesystem::remove(scripts[index]);
}
