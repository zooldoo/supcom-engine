#include <catch2/catch_test_macros.hpp>

#include "scfa_convert/convert.hpp"
#include "sim/sim_callback_queue.hpp"

#include <cstring>
#include <string>
#include <variant>
#include <vector>

using osc::u8;
using osc::scfa::MohoOp;

namespace {

struct Bytes {
    std::vector<u8> b;
    void u8v(u8 v) { b.push_back(v); }
    void u16v(osc::u16 v) {
        u8v(static_cast<u8>(v & 0xFF));
        u8v(static_cast<u8>(v >> 8));
    }
    void u32v(osc::u32 v) {
        for (int i = 0; i < 4; ++i) {
            u8v(static_cast<u8>((v >> (8 * i)) & 0xFF));
        }
    }
    void f32v(float f) {
        osc::u32 bits;
        std::memcpy(&bits, &f, sizeof(bits));
        u32v(bits);
    }
    void raw(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
    void cstr(const std::string& s) {
        raw(s);
        u8v(0);
    }
    void scr_string(const std::string& s) {
        u8v(1);
        cstr(s);
    }
    void scr_number(float f) {
        u8v(0);
        f32v(f);
    }
    void scr_bool(bool v) {
        u8v(3);
        u8v(v ? 1 : 0);
    }
    void sized(const Bytes& inner) {
        u32v(static_cast<osc::u32>(inner.b.size()));
        b.insert(b.end(), inner.b.begin(), inner.b.end());
    }
    void message(MohoOp op, const Bytes& data) {
        u8v(static_cast<u8>(op));
        u16v(static_cast<osc::u16>(data.b.size() + 3));
        b.insert(b.end(), data.b.begin(), data.b.end());
    }
};

Bytes player(const std::string& army, bool human, float faction, const std::string& ai) {
    Bytes p;
    p.u8v(4);
    p.scr_string("ArmyName");
    p.scr_string(army);
    p.scr_string("Human");
    p.scr_bool(human);
    p.scr_string("Faction");
    p.scr_number(faction);
    p.scr_string("Team");
    p.scr_number(1);
    p.scr_string("AIPersonality");
    p.scr_string(ai);
    p.u8v(5);
    return p;
}

/// A replay of ARMY_1 (AI, rush) and ARMY_7 (the player, source 0) on X1MP_012.
Bytes header() {
    Bytes h;
    h.cstr("Supreme Commander v1.50.3599");
    h.cstr("\r\n");
    h.raw("Replay v1.9\r\n");
    h.cstr("/maps/X1MP_012/X1MP_012.scmap");
    h.cstr("\r\n\x1a");
    Bytes mods;
    mods.u8v(4);
    mods.u8v(5);
    h.sized(mods);
    Bytes scenario;
    scenario.u8v(4);
    scenario.scr_string("Options");
    scenario.u8v(4);
    scenario.scr_string("Victory");
    scenario.scr_string("demoralization");
    scenario.scr_string("UnitCap");
    scenario.scr_string("1000");
    scenario.u8v(5);
    scenario.u8v(5);
    h.sized(scenario);
    h.u8v(1);
    h.cstr("zooldoo");
    h.u32v(0xFFFFFFFF);
    h.u8v(0);
    h.u8v(2);
    h.sized(player("ARMY_1", false, 3, "rush"));
    h.u8v(0xFF);
    h.sized(player("ARMY_7", true, 4, ""));
    h.u8v(0);
    h.u8v(0xFF);
    h.u32v(175608674);
    return h;
}

Bytes advance() {
    Bytes a;
    a.u32v(1);
    return a;
}

std::optional<osc::scfa::MohoReplay> read(const Bytes& bytes) {
    std::string error;
    return osc::scfa::read_moho_replay(bytes.b, error);
}

} // namespace

TEST_CASE("A Moho replay's header and stream are read", "[scfa]") {
    Bytes r = header();
    Bytes source;
    source.u8v(0);
    r.message(MohoOp::SetCommandSource, source);
    r.message(MohoOp::Advance, advance());
    Bytes sum;
    for (u8 i = 0; i < 16; ++i) {
        sum.u8v(i);
    }
    sum.u32v(0);
    r.message(MohoOp::VerifyChecksum, sum);
    r.message(MohoOp::Advance, advance());
    r.message(MohoOp::EndGame, Bytes{});

    const auto moho = read(r);
    REQUIRE(moho);
    CHECK(moho->build == "Supreme Commander v1.50.3599");
    CHECK(moho->map == "/maps/X1MP_012/X1MP_012.scmap");
    CHECK(moho->seed == 175608674u);
    REQUIRE(moho->sources.size() == 1);
    CHECK(moho->sources[0].name == "zooldoo");
    CHECK(moho->sources[0].timeouts == -1);
    REQUIRE(moho->armies.size() == 2);
    CHECK(moho->armies[0].sources.empty());
    CHECK(moho->armies[1].sources == std::vector<u8>{0});
    CHECK(moho->armies[1].options.text("ArmyName") == "ARMY_7");
    CHECK(moho->beats == 2);
    REQUIRE(moho->messages.size() == 5);
    CHECK(moho->messages[2].op == MohoOp::VerifyChecksum);
    CHECK(moho->messages[2].beat == 1);
    CHECK(moho->messages[2].source == 0);
}

TEST_CASE("A Moho replay's setup is the engine's", "[scfa]") {
    const auto moho = read(header());
    REQUIRE(moho);
    const auto c = osc::scfa::convert(*moho);
    const auto& s = c.replay.setup;
    CHECK(c.replay.has_setup);
    CHECK(s.scenario == "/maps/X1MP_012/X1MP_012_scenario.lua");
    CHECK(s.seed == 175608674u);
    CHECK(c.replay.seed == 175608674u);
    CHECK(s.moho_random);
    CHECK(s.moho_pathing);
    CHECK(c.replay.victory_condition == "demoralization");
    REQUIRE(s.slots.size() == 2);
    CHECK(s.army_count == 2);
    CHECK(s.slots[0].slot == 1);
    CHECK_FALSE(s.slots[0].human);
    CHECK(s.slots[0].faction == 3);
    CHECK(s.slots[0].ai_personality == "rush");
    CHECK(s.slots[1].slot == 7);
    CHECK(s.slots[1].human);
    CHECK(s.slots[1].faction == 4);
    CHECK(s.ai_armies == std::vector<int>{0});
    REQUIRE(s.options.values.size() == 2);
    CHECK(s.options.values[1].first == "UnitCap");
    CHECK(s.options.values[1].second.string_value == "1000");
}

TEST_CASE("A Moho order becomes the engine's command in its beat", "[scfa]") {
    Bytes r = header();
    Bytes source;
    source.u8v(0);
    r.message(MohoOp::SetCommandSource, source);
    r.message(MohoOp::Advance, advance());
    r.message(MohoOp::Advance, advance());
    Bytes issue;
    issue.u32v(2);
    issue.u32v(0x00100005);
    issue.u32v(0x00100006);
    issue.u32v(0x12);
    issue.u32v(0xFFFFFFFF);
    issue.u8v(2);
    issue.u32v(0xFFFFFFFF);
    issue.u8v(2);
    issue.f32v(100.0f);
    issue.f32v(20.0f);
    issue.f32v(300.0f);
    issue.u8v(0);
    issue.u32v(1);
    issue.f32v(1.0f);
    issue.f32v(0.0f);
    issue.f32v(0.0f);
    issue.f32v(0.0f);
    issue.f32v(1.0f);
    issue.cstr("");
    issue.u32v(0);
    issue.u32v(1);
    issue.u32v(1);
    issue.u8v(2);
    issue.u8v(0);
    r.message(MohoOp::IssueCommand, issue);
    Bytes info;
    info.u32v(0x00100005);
    info.cstr("SetPaused");
    info.cstr("true");
    r.message(MohoOp::ProcessInfoPair, info);

    const auto moho = read(r);
    REQUIRE(moho);
    const auto c = osc::scfa::convert(*moho);
    REQUIRE(c.replay.commands.size() == 2);
    CHECK(c.replay.final_beat == 2);

    const auto& order = c.replay.commands[0];
    CHECK(order.exec_beat == 3);
    CHECK(order.source == 0);
    CHECK_FALSE(order.clear_existing);
    CHECK(order.command.type == osc::sim::CommandType::Move);
    CHECK(order.command.target_pos.x == 100.0f);
    CHECK(order.command.target_pos.z == 300.0f);
    CHECK(order.command.formation == "GrowthFormation");
    CHECK(order.command.has_facing);
    CHECK_FALSE(order.command.form_move);
    CHECK(order.command.command_id == ((1u << 24) | 0x12u));
    const auto first = osc::sim::EntityId::moho_only(0x00100005);
    const auto second = osc::sim::EntityId::moho_only(0x00100006);
    CHECK(order.unit_ids == std::vector<osc::sim::EntityId>{first, second});

    const auto& setting = c.replay.commands[1];
    REQUIRE(setting.callback);
    CHECK(setting.callback->func_name == osc::sim::kProcessInfoCallback);
    CHECK(std::get<std::string>(setting.callback->args.at("Action")) == "SetPaused");
    CHECK(setting.callback->unit_ids == std::vector<osc::sim::EntityId>{first});
}

TEST_CASE("A malformed Moho replay is refused", "[scfa]") {
    std::string error;
    Bytes junk;
    junk.cstr("Supreme Commander v1.50.3599");
    junk.cstr("\r\n");
    junk.raw("Replay v2.0\r\n");
    CHECK_FALSE(osc::scfa::read_moho_replay(junk.b, error));

    Bytes cut = header();
    cut.u8v(static_cast<u8>(MohoOp::Advance));
    cut.u16v(7);
    cut.u8v(1);
    CHECK_FALSE(osc::scfa::read_moho_replay(cut.b, error));

    Bytes unknown = header();
    unknown.message(static_cast<MohoOp>(40), Bytes{});
    CHECK_FALSE(osc::scfa::read_moho_replay(unknown.b, error));
}
