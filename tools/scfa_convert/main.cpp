// osc_scfa_convert <in.SCFAReplay> <out.oscreplay> [--sums <file>]
// A Moho replay as the engine's, which `opensupcom --replay` plays. The
// checksums the players recorded go to --sums, a line a checksum:
// "<beat> <source> <md5>".

#include "scfa_convert/convert.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <in.SCFAReplay> <out.oscreplay> [--sums <file>]\n",
                     argv[0]);
        return 2;
    }
    std::string sums_path;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--sums") {
            sums_path = argv[i + 1];
        }
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<osc::u8> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    if (!in && !in.eof()) {
        std::fprintf(stderr, "can't read %s\n", argv[1]);
        return 1;
    }
    std::string error;
    const auto moho = osc::scfa::read_moho_replay(bytes, error);
    if (!moho) {
        std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
        return 1;
    }
    const osc::scfa::Conversion conversion = osc::scfa::convert(*moho);
    const std::vector<osc::u8> out = conversion.replay.serialize();
    std::ofstream file(argv[2], std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) {
        std::fprintf(stderr, "can't write %s\n", argv[2]);
        return 1;
    }
    if (!sums_path.empty()) {
        std::ofstream sums(sums_path);
        for (const auto& c : conversion.checksums) {
            char hex[33];
            for (size_t i = 0; i < c.digest.size(); ++i) {
                std::snprintf(hex + 2 * i, 3, "%02x", c.digest[i]);
            }
            sums << c.beat << ' ' << static_cast<int>(c.source) << ' ' << hex << '\n';
        }
    }
    std::printf("%s: %s, %s, seed %u, %zu armies, %u beats, %zu commands, %zu checksums\n", argv[1],
                moho->build.c_str(), conversion.replay.setup.scenario.c_str(), moho->seed,
                conversion.replay.setup.slots.size(), conversion.replay.final_beat,
                conversion.replay.commands.size(), conversion.checksums.size());
    for (const auto& [what, count] : conversion.skipped) {
        std::printf("  skipped %s: %d\n", what.c_str(), count);
    }
    for (const auto& note : conversion.notes) {
        std::printf("  note: %s\n", note.c_str());
    }
    return 0;
}
