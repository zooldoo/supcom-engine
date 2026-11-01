#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"
#include "sim/prop.hpp"
#include "sim/sim_state.hpp"
#include "sim/synclog.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using osc::sim::EntityId;
using osc::sim::Prop;
using osc::sim::SimState;
using osc::sim::SyncLog;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("osc_synclog_test_" + std::to_string(rd()) + std::to_string(rd()));
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
};

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool lists(const std::string& beat, EntityId id) {
    return beat.find(fmt::format("\n  0x{:08x}\n", id.moho())) != std::string::npos;
}

EntityId add_prop(SimState& sim, osc::f32 x) {
    auto p = std::make_unique<Prop>();
    p->set_position({x, 0.0f, x});
    return sim.entity_registry().register_entity(std::move(p));
}

} // namespace

TEST_CASE("The synclog writes every period-th beat: its units, and what else was made or moved "
          "on it",
          "[synclog]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_victory_condition("sandbox");
    sim.add_army("ARMY_1", "Player");
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_blueprint_id("uel0001");
    const EntityId unit = sim.entity_registry().register_entity(std::move(u));
    const EntityId still = add_prop(sim, 10.0f);
    const EntityId moved = add_prop(sim, 20.0f);

    TempDir dir;
    SyncLog log(dir.path, 10);
    sim.set_synclog(&log);
    for (int i = 0; i < 20; ++i) {
        sim.tick();
    }
    sim.entity_registry().find(moved)->set_position({25.0f, 0.0f, 20.0f});
    sim.tick();
    sim.tick();

    std::vector<std::string> files;
    for (const auto& f : fs::directory_iterator(dir.path)) {
        files.push_back(f.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    CHECK(files == std::vector<std::string>{"beat00000.log", "beat00010.log", "beat00020.log"});

    const std::string first = read(dir.path / "beat00000.log");
    CHECK(first.rfind("********** beat 0 **********\n", 0) == 0);
    CHECK(first.find("Armies\n  \"Player\" [ARMY_1]\n") != std::string::npos);
    CHECK(lists(first, unit));
    CHECK(lists(first, still));
    CHECK(lists(first, moved));

    const std::string tenth = read(dir.path / "beat00010.log");
    CHECK(lists(tenth, unit));
    CHECK_FALSE(lists(tenth, still));
    CHECK_FALSE(lists(tenth, moved));

    const std::string twentieth = read(dir.path / "beat00020.log");
    CHECK(lists(twentieth, unit));
    CHECK_FALSE(lists(twentieth, still));
    CHECK(lists(twentieth, moved));
    CHECK(twentieth.find("    bp:uel0001\n") != std::string::npos);
    CHECK(
        twentieth.find("   vel: <   5.00,   0.00,   0.00> [0x40a00000 0x00000000 0x00000000]\n") !=
        std::string::npos);
}
