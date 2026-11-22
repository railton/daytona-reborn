// Diagnostic only: bounded gearbox stimulus, never a production start path.
// Reuses PcSolo input timing; changes steering only during gearbox selection.
// Records a replayable input script and guest observations without RAM writes.
#include "runtime/game_loop.h"
#include "runtime/pc_solo.h"
#include "common/nvram.h"
#include "common/probe_evidence.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

std::ofstream output(const fs::path &path, bool binary = false) {
    std::ofstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(path, std::ios::out | (binary ? std::ios::binary : std::ios::openmode(0)));
    return stream;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: m2selectionprobe IMAGES NEW_OUTPUT_DIR STEER(0..255)\n";
        return 2;
    }
    try {
        size_t consumed = 0;
        const unsigned steer = std::stoul(argv[3], &consumed, 0);
        if (consumed != std::string(argv[3]).size() || steer > 255)
            throw std::runtime_error("steering must be a byte");
        const fs::path dir(argv[2]);
        if (!fs::create_directory(dir)) throw std::runtime_error("output directory already exists");
        rt::GameLoop game(argv[1]);
        rt::PcSolo::configure(game.board());
        tools::save_nvram(game, dir.string());
        if (fs::file_size(dir / "ioboard_eeprom.bin") != game.board().io().eeprom.size() ||
            fs::file_size(dir / "backup_ram.bin") != game.board().backup_ram().size())
            throw std::runtime_error("initial NVRAM was not saved completely");
        rt::PcSolo adapter(rt::RaceCourse::Beginner);
        auto csv = output(dir / "selection.csv");
        auto script = output(dir / "inputs.txt");
        csv << "tick_before,frame_before,mode_before,step_before,steer,accel,brake,mode_after,step_after,frame_after\n";
        tools::SelectionEvidence evidence;
        for (uint64_t tick_before = 0; tick_before < 1400; ++tick_before) {
            const auto frame_before = game.frames();
            if (frame_before != tick_before)
                throw std::runtime_error("fresh probe tick/frame counters disagree");
            const auto mode = game.board().read_byte(0x5010a0);
            const auto step = game.board().read_byte(0x5010f8);
            rt::Inputs input{};
            if (!evidence.entered_frame) input = adapter.input(game.board());
            else input.accel = 0xe0;
            if (!evidence.entered_frame && mode == tools::SelectionEvidence::kMode && step == tools::SelectionEvidence::kStep)
                input.steer = uint8_t(steer);
            tools::require_selection_ports(input.in0, input.in1, input.in2);
            evidence.note(mode, step, input.steer, uint8_t(steer));
            const unsigned start = (input.in0 & 0x10) ? 0u : 1u;
            script << tick_before << " steer=" << unsigned(input.steer) << '\n'
                   << tick_before << " accel=" << unsigned(input.accel) << '\n'
                   << tick_before << " brake=" << unsigned(input.brake) << '\n'
                   << tick_before << " start=" << start << '\n';
            game.run_frame(input);
            const uint64_t frame_after = game.frames();
            tools::require_frame_step(frame_before, frame_after);
            const auto after = game.board().read_byte(0x5010a0);
            csv << tick_before << ',' << frame_before << ',' << unsigned(mode) << ',' << unsigned(step) << ','
                << unsigned(input.steer) << ',' << unsigned(input.accel) << ','
                << unsigned(input.brake) << ',' << unsigned(after) << ','
                << unsigned(game.board().read_byte(0x5010f8)) << ',' << frame_after << '\n';
            const bool just_entered = evidence.observe_entry(mode, after, frame_after);
            if (frame_after % 50 == 0 || just_entered) {
                auto frame = output(dir / ("run_" + std::to_string(frame_after) + ".rgb"), true);
                frame.write(reinterpret_cast<const char *>(game.screen().data()),
                            std::streamsize(game.screen().size() * sizeof(uint32_t)));
            }
        }
        csv.flush();
        script.flush();
        std::cout << "{\"frames\":" << game.frames() << ",\"entered_race\":" << evidence.entered_frame
                  << ",\"i960\":" << game.instructions() << ",\"steer\":" << steer
                  << ",\"selection_samples\":" << evidence.samples << "}\n";
        return evidence.success() ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
