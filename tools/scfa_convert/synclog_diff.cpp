// osc_synclog_diff <retail /synclog dir> <engine --entity-trace file> [--keep <n>]
// Each beat retail dumped its dirty entities on, against the engine's trace of
// that tick: per beat how many it dumped, how many the engine lacks and how
// many differ; then the first beat that differs, field by field.

#include "scfa_convert/synclog.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <retail synclog dir> <engine entity trace> [--keep <n>]\n",
                     argv[0]);
        return 2;
    }
    size_t keep = 20;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--keep") {
            keep = std::stoul(argv[i + 1]);
        }
    }
    osc::scfa::BeatStates retail;
    for (const auto& file : std::filesystem::recursive_directory_iterator(argv[1])) {
        const std::string name = file.path().filename().string();
        if (!file.is_regular_file() || name.rfind("beat", 0) != 0) {
            continue;
        }
        std::ifstream in(file.path());
        const auto beat = osc::scfa::read_synclog_beat(in);
        if (!beat || !beat->dumped) {
            continue;
        }
        auto& entities = retail[beat->beat];
        for (const auto& e : beat->entities) {
            entities[e.id] = e;
        }
    }
    std::ifstream trace(argv[2]);
    const osc::scfa::BeatStates engine = osc::scfa::read_entity_trace(trace);
    const auto beats = osc::scfa::compare(retail, engine, keep);

    const osc::scfa::BeatComparison* first = nullptr;
    for (const auto& b : beats) {
        std::printf("beat %u: %zu dumped, %zu missing, %zu differ\n", b.beat, b.entities, b.missing,
                    b.differing);
        if (!first && (b.missing > 0 || b.differing > 0)) {
            first = &b;
        }
    }
    std::printf("retail beats dumped %zu, engine ticks traced %zu, compared %zu\n", retail.size(),
                engine.size(), beats.size());
    if (!first) {
        std::printf("no difference\n");
        return 0;
    }
    std::printf("first difference: beat %u\n", first->beat);
    for (const auto& d : first->differences) {
        std::printf("  0x%08x %s %s: retail %s engine %s (%llu ulp)\n", d.id, d.blueprint.c_str(),
                    d.field.c_str(), d.retail.c_str(), d.engine.c_str(),
                    static_cast<unsigned long long>(d.ulps));
    }
    return 0;
}
