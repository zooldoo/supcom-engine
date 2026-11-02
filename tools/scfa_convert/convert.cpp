#include "scfa_convert/convert.hpp"

#include "sim/build_info.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/entity.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <utility>

namespace osc::scfa {

namespace {

using sim::CommandType;
using sim::EntityId;

// retail /lua/formations.lua: SurfaceFormations, AirFormations and ComboFormations alike
constexpr std::array<const char*, 2> kRetailFormations = {"AttackFormation", "GrowthFormation"};

struct OrderKind {
    CommandType type = CommandType::Stop;
    bool form_move = false;
    bool form_attack = false;
};

/// Moho's EUnitCommandType as the engine's order.
std::optional<OrderKind> order_kind(u8 moho, std::string& name) {
    static constexpr std::array<const char*, 40> kNames = {
        "None",
        "Stop",
        "Move",
        "Dive",
        "FormMove",
        "BuildSiloTactical",
        "BuildSiloNuke",
        "BuildFactory",
        "BuildMobile",
        "BuildAssist",
        "Attack",
        "FormAttack",
        "Nuke",
        "Tactical",
        "Teleport",
        "Guard",
        "Patrol",
        "Ferry",
        "FormPatrol",
        "Reclaim",
        "Repair",
        "Capture",
        "TransportLoadUnits",
        "TransportReverseLoadUnits",
        "TransportUnloadUnits",
        "TransportUnloadSpecificUnits",
        "DetachFromTransport",
        "Upgrade",
        "Script",
        "AssistCommander",
        "KillSelf",
        "DestroySelf",
        "Sacrifice",
        "Pause",
        "OverCharge",
        "AggressiveMove",
        "FormAggressiveMove",
        "AssistMove",
        "SpecialAction",
        "Dock",
    };
    name = moho < kNames.size() ? kNames[moho] : "unknown";
    switch (moho) {
    case 1: return OrderKind{CommandType::Stop};
    case 2: return OrderKind{CommandType::Move};
    case 3: return OrderKind{CommandType::Dive};
    case 4: return OrderKind{CommandType::Move, true};
    case 5: return OrderKind{CommandType::SiloBuildTactical};
    case 6: return OrderKind{CommandType::SiloBuildNuke};
    case 7: return OrderKind{CommandType::BuildFactory};
    case 8: return OrderKind{CommandType::BuildMobile};
    case 10: return OrderKind{CommandType::Attack};
    case 11: return OrderKind{CommandType::Attack, false, true};
    case 12: return OrderKind{CommandType::Nuke};
    case 13: return OrderKind{CommandType::Tactical};
    case 14: return OrderKind{CommandType::Teleport};
    case 15: return OrderKind{CommandType::Guard};
    case 16: return OrderKind{CommandType::Patrol};
    case 17: return OrderKind{CommandType::Ferry};
    case 18: return OrderKind{CommandType::Patrol};
    case 19: return OrderKind{CommandType::Reclaim};
    case 20: return OrderKind{CommandType::Repair};
    case 21: return OrderKind{CommandType::Capture};
    case 22: return OrderKind{CommandType::TransportLoad};
    case 24: return OrderKind{CommandType::TransportUnload};
    case 27: return OrderKind{CommandType::Upgrade};
    case 28: return OrderKind{CommandType::Script};
    case 31: return OrderKind{CommandType::DestroySelf};
    case 32: return OrderKind{CommandType::Sacrifice};
    case 34: return OrderKind{CommandType::Overcharge};
    case 35: return OrderKind{CommandType::AggressiveMove};
    case 36: return OrderKind{CommandType::AggressiveMove};
    case 39: return OrderKind{CommandType::Dock};
    default: return std::nullopt;
    }
}

const char* op_name(MohoOp op) {
    static constexpr std::array<const char*, 24> kNames = {
        "Advance",
        "SetCommandSource",
        "CommandSourceTerminated",
        "VerifyChecksum",
        "RequestPause",
        "Resume",
        "SingleStep",
        "CreateUnit",
        "CreateProp",
        "DestroyEntity",
        "WarpEntity",
        "ProcessInfoPair",
        "IssueCommand",
        "IssueFactoryCommand",
        "IncreaseCommandCount",
        "DecreaseCommandCount",
        "SetCommandTarget",
        "SetCommandType",
        "SetCommandCells",
        "RemoveCommandFromQueue",
        "DebugCommand",
        "ExecuteLuaInSim",
        "LuaSimCallback",
        "EndGame",
    };
    return kNames[static_cast<size_t>(op)];
}

EntityId moho_entity(u32 id) {
    return EntityId::moho_only(id);
}

f64 moho_entity_arg(u32 id) {
    return static_cast<f64>(moho_entity(id).key());
}

std::vector<EntityId> entity_set(MohoReader& r) {
    std::vector<EntityId> ids;
    const u32 count = r.u32v();
    for (u32 i = 0; i < count && r.ok(); ++i) {
        ids.push_back(moho_entity(r.u32v()));
    }
    return ids;
}

void push_scr(lua_State* L, const ScrValue& v) {
    switch (v.kind) {
    case ScrValue::Kind::Nil: lua_pushnil(L); break;
    case ScrValue::Kind::Number: lua_pushnumber(L, static_cast<lua_Number>(v.number)); break;
    case ScrValue::Kind::String: lua_pushlstring(L, v.string.data(), v.string.size()); break;
    case ScrValue::Kind::Bool: lua_pushboolean(L, v.boolean ? 1 : 0); break;
    case ScrValue::Kind::Table:
        lua_newtable(L);
        for (const auto& [key, value] : v.table) {
            push_scr(L, key);
            push_scr(L, value);
            lua_rawset(L, -3);
        }
        break;
    }
}

class Converter {
public:
    explicit Converter(const MohoReplay& moho) : moho_(moho), L_(lua_open()) {}
    ~Converter() { lua_close(L_); }
    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;

    Conversion run() {
        setup();
        for (const MohoMessage& m : moho_.messages) {
            message(m);
        }
        out_.replay.final_beat = moho_.beats;
        return std::move(out_);
    }

private:
    void skip(const std::string& what) { ++out_.skipped[what]; }

    std::optional<std::string> lua_bytes(const ScrValue& v) {
        push_scr(L_, v);
        auto bytes = sim::lua_to_bytes(L_, lua_gettop(L_));
        lua_pop(L_, 1);
        return bytes;
    }

    void setup() {
        sim::Replay& r = out_.replay;
        sim::GameSetup& s = r.setup;
        r.has_setup = true;
        r.build = sim::build_id();
        r.seed = moho_.seed;
        s.scenario = scenario_of_map(moho_.map);
        s.seed = moho_.seed;
        s.moho_random = true;
        s.moho_pathing = true;

        if (const ScrValue* options = moho_.scenario.field("Options")) {
            s.options.configured = true;
            for (const auto& [key, value] : options->table) {
                if (key.kind != ScrValue::Kind::String) {
                    continue;
                }
                if (key.string == "RestrictedCategories") {
                    for (const auto& [i, category] : value.table) {
                        if (category.kind == ScrValue::Kind::String) {
                            s.options.restricted_categories.push_back(category.string);
                        }
                    }
                    continue;
                }
                switch (value.kind) {
                case ScrValue::Kind::String: s.options.set_string(key.string, value.string); break;
                case ScrValue::Kind::Number: s.options.set_number(key.string, value.number); break;
                case ScrValue::Kind::Bool: s.options.set_bool(key.string, value.boolean); break;
                default: break;
                }
            }
            r.victory_condition = options->text("Victory");
        }
        if (!moho_.mods.table.empty()) {
            out_.notes.push_back("the replay's game mods are not carried over");
        }

        u32 next_source = 0;
        for (size_t i = 0; i < moho_.armies.size(); ++i) {
            const ScrValue& o = moho_.armies[i].options;
            if (o.flag("Civilian")) {
                s.civilians = true;
                continue;
            }
            sim::ArmySlotConfig slot;
            slot.configured = true;
            const std::string army = o.text("ArmyName");
            if (army.rfind("ARMY_", 0) == 0) {
                slot.slot = std::atoi(army.c_str() + 5);
            }
            slot.human = o.flag("Human");
            slot.faction = static_cast<int>(o.number_at("Faction").value_or(1));
            slot.team = static_cast<int>(o.number_at("Team").value_or(static_cast<f32>(i + 1)));
            slot.start_spot = static_cast<int>(o.number_at("StartSpot").value_or(0));
            slot.player_color = static_cast<int>(o.number_at("PlayerColor").value_or(-1));
            slot.army_color = static_cast<int>(o.number_at("ArmyColor").value_or(-1));
            if (!slot.human) {
                slot.ai_personality = o.text("AIPersonality");
                s.ai_armies.push_back(static_cast<int>(s.slots.size()));
                if (!slot.ai_personality.empty()) {
                    s.ai_personality = slot.ai_personality;
                }
            } else {
                for (const u8 source : moho_.armies[i].sources) {
                    sources_.try_emplace(source, next_source);
                }
                ++next_source;
            }
            s.slots.push_back(slot);
        }
        s.army_count = static_cast<int>(s.slots.size());
    }

    void schedule(const MohoMessage& m, sim::ScheduledCommand c) {
        c.exec_beat = m.beat + 1;
        out_.replay.commands.push_back(std::move(c));
    }

    std::optional<u32> engine_source(const MohoMessage& m) const {
        const auto it = sources_.find(m.source);
        if (it == sources_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    u32 engine_command_id(u32 source, u32 moho) const {
        return ((source + 1) << 24) | (moho & 0xFFFFFFu);
    }

    void callback(const MohoMessage& m, u32 source, sim::SimCallbackEntry entry) {
        sim::ScheduledCommand c;
        c.source = source;
        c.callback = std::move(entry);
        schedule(m, std::move(c));
    }

    void message(const MohoMessage& m) {
        MohoReader r(m.data);
        switch (m.op) {
        case MohoOp::Advance:
        case MohoOp::SetCommandSource:
        case MohoOp::EndGame: return;
        case MohoOp::VerifyChecksum: {
            MohoChecksum c;
            for (u8& b : c.digest) {
                b = r.u8v();
            }
            c.beat = r.u32v();
            c.source = m.source;
            if (r.ok()) {
                out_.checksums.push_back(c);
            }
            return;
        }
        default: break;
        }
        const auto source = engine_source(m);
        if (!source) {
            skip(std::string("observer:") + op_name(m.op));
            return;
        }
        switch (m.op) {
        case MohoOp::RequestPause:
        case MohoOp::Resume:
        case MohoOp::SingleStep: {
            sim::SimCallbackEntry e;
            e.func_name = m.op == MohoOp::RequestPause ? sim::kRequestPauseCallback
                          : m.op == MohoOp::Resume     ? sim::kResumeCallback
                                                       : sim::kSingleStepCallback;
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::CommandSourceTerminated: {
            sim::SimCallbackEntry e;
            e.func_name = sim::kCommandSourceTerminatedCallback;
            e.args["Source"] = static_cast<f64>(*source);
            callback(m, sim::kEngineSource, std::move(e));
            return;
        }
        case MohoOp::ProcessInfoPair: {
            sim::SimCallbackEntry e;
            e.func_name = sim::kProcessInfoCallback;
            e.unit_ids.push_back(moho_entity(r.u32v()));
            e.args["Action"] = r.cstr();
            e.args["Value"] = r.cstr();
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::LuaSimCallback: {
            sim::SimCallbackEntry e;
            e.func_name = r.cstr();
            const auto args = r.scr();
            e.unit_ids = entity_set(r);
            if (!args || !r.ok()) {
                skip("malformed:LuaSimCallback");
                return;
            }
            e.lua_args = lua_bytes(*args);
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::IssueCommand:
        case MohoOp::IssueFactoryCommand:
            issue(m, *source, r, m.op == MohoOp::IssueFactoryCommand);
            return;
        case MohoOp::SetCommandTarget: {
            const u32 moho_id = r.u32v();
            sim::SimCallbackEntry e;
            e.func_name = sim::kSetCommandTargetCallback;
            e.args["Command"] = static_cast<f64>(engine_command_id(*source, moho_id));
            const u8 kind = r.u8v();
            if (kind == 1) {
                e.args["Target"] = moho_entity_arg(r.u32v());
            } else if (kind == 2) {
                e.args["X"] = static_cast<f64>(r.f32v());
                e.args["Y"] = static_cast<f64>(r.f32v());
                e.args["Z"] = static_cast<f64>(r.f32v());
            }
            e.unit_ids = units_of(*source, moho_id);
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::SetCommandType: {
            const u32 moho_id = r.u32v();
            std::string name;
            const auto kind = order_kind(static_cast<u8>(r.i32v()), name);
            if (!kind) {
                skip("type:" + name);
                return;
            }
            sim::SimCallbackEntry e;
            e.func_name = sim::kSetCommandTypeCallback;
            e.args["Command"] = static_cast<f64>(engine_command_id(*source, moho_id));
            e.args["Type"] = static_cast<f64>(static_cast<u8>(kind->type));
            e.unit_ids = units_of(*source, moho_id);
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::RemoveCommandFromQueue: {
            const u32 moho_id = r.u32v();
            sim::SimCallbackEntry e;
            e.func_name = sim::kRemoveCommandCallback;
            e.args["Command"] = static_cast<f64>(engine_command_id(*source, moho_id));
            e.unit_ids.push_back(moho_entity(r.u32v()));
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::DecreaseCommandCount: {
            const u32 moho_id = r.u32v();
            sim::SimCallbackEntry e;
            e.func_name = sim::kDecreaseCommandCountCallback;
            e.args["Command"] = static_cast<f64>(engine_command_id(*source, moho_id));
            e.args["Count"] = static_cast<f64>(r.i32v());
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::DebugCommand: {
            sim::SimCallbackEntry e;
            e.func_name = sim::kDebugCommandCallback;
            e.args["Command"] = r.cstr();
            e.args["X"] = static_cast<f64>(r.f32v());
            e.args["Y"] = static_cast<f64>(r.f32v());
            e.args["Z"] = static_cast<f64>(r.f32v());
            const u8 focus = r.u8v();
            e.args["FocusArmy"] = focus == 0xFF ? -1.0 : static_cast<f64>(focus);
            e.unit_ids = entity_set(r);
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::ExecuteLuaInSim: {
            sim::SimCallbackEntry e;
            e.func_name = sim::kExecLuaInSimCallback;
            e.args["Func"] = r.cstr();
            const auto args = r.scr();
            if (!args) {
                skip("malformed:ExecuteLuaInSim");
                return;
            }
            e.lua_args = lua_bytes(*args);
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::CreateUnit: {
            sim::SimCallbackEntry e;
            e.func_name = sim::kCreateUnitCallback;
            e.args["Army"] = static_cast<f64>(r.u8v());
            e.args["Blueprint"] = r.cstr();
            e.args["X"] = static_cast<f64>(r.f32v());
            e.args["Z"] = static_cast<f64>(r.f32v());
            e.args["Heading"] = static_cast<f64>(r.f32v());
            callback(m, *source, std::move(e));
            return;
        }
        case MohoOp::WarpEntity: {
            sim::SimCallbackEntry e;
            e.func_name = sim::kWarpEntityCallback;
            e.args["Entity"] = moho_entity_arg(r.u32v());
            for (int i = 0; i < 4; ++i) {
                r.f32v();
            }
            e.args["X"] = static_cast<f64>(r.f32v());
            e.args["Y"] = static_cast<f64>(r.f32v());
            e.args["Z"] = static_cast<f64>(r.f32v());
            callback(m, *source, std::move(e));
            return;
        }
        default: skip(std::string("op:") + op_name(m.op)); return;
        }
    }

    void issue(const MohoMessage& m, u32 source, MohoReader& r, bool factory) {
        sim::ScheduledCommand c;
        c.source = source;
        c.unit_ids = entity_set(r);
        const u32 moho_id = r.u32v();
        r.i32v();
        std::string name;
        const auto kind = order_kind(r.u8v(), name);
        r.i32v();
        sim::UnitCommand& cmd = c.command;
        const auto target = [&](bool primary) {
            const u8 type = r.u8v();
            if (type == 1) {
                const EntityId id = moho_entity(r.u32v());
                if (primary) {
                    cmd.target_id = id;
                }
            } else if (type == 2) {
                const sim::Vector3 p{r.f32v(), r.f32v(), r.f32v()};
                if (primary) {
                    cmd.target_pos = p;
                }
            }
        };
        target(true);
        target(false);
        const i32 formation = r.i32v();
        sim::Quaternion orientation{0, 0, 0, 1};
        if (formation != -1) {
            orientation.w = r.f32v();
            orientation.x = r.f32v();
            orientation.y = r.f32v();
            orientation.z = r.f32v();
            r.f32v();
        }
        cmd.blueprint_id = r.cstr();
        std::transform(cmd.blueprint_id.begin(), cmd.blueprint_id.end(), cmd.blueprint_id.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const i32 cells = r.i32v();
        for (i32 i = 0; i < cells && r.ok(); ++i) {
            r.u32v();
        }
        cmd.max_count = r.i32v();
        cmd.count = r.i32v();
        const auto object = r.scr();
        c.clear_existing = r.u8v() != 0;
        if (!r.ok() || !object) {
            skip("malformed:" + std::string(op_name(m.op)));
            return;
        }
        if (!kind) {
            skip("order:" + name);
            return;
        }
        cmd.type = kind->type;
        cmd.factory = factory;
        cmd.form_attack = kind->form_attack;
        if (formation >= 0 && c.unit_ids.size() > 1 &&
            (cmd.type == CommandType::Move || cmd.type == CommandType::Attack ||
             cmd.type == CommandType::Patrol || cmd.type == CommandType::AggressiveMove)) {
            const size_t index = static_cast<size_t>(formation);
            cmd.formation = kRetailFormations[index < kRetailFormations.size() ? index : 0];
            cmd.has_facing = true;
            cmd.facing = sim::quat_yaw(orientation);
            cmd.form_move = kind->form_move;
        }
        if (cmd.type == CommandType::Script) {
            auto bytes = lua_bytes(*object);
            if (!bytes) {
                skip("malformed:Script");
                return;
            }
            cmd.script_args = std::move(*bytes);
        }
        if (!factory && cmd.type != CommandType::Stop) {
            cmd.command_id = engine_command_id(source, moho_id);
            units_[cmd.command_id] = c.unit_ids;
        }
        schedule(m, std::move(c));
    }

    std::vector<EntityId> units_of(u32 source, u32 moho_id) const {
        const auto it = units_.find(engine_command_id(source, moho_id));
        return it == units_.end() ? std::vector<EntityId>{} : it->second;
    }

    const MohoReplay& moho_;
    lua_State* L_;
    Conversion out_;
    std::unordered_map<u8, u32> sources_;
    std::unordered_map<u32, std::vector<EntityId>> units_;
};

} // namespace

std::string scenario_of_map(const std::string& map) {
    constexpr std::string_view kSuffix = ".scmap";
    std::string base = map;
    if (base.size() >= kSuffix.size()) {
        std::string tail = base.substr(base.size() - kSuffix.size());
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (tail == kSuffix) {
            base.resize(base.size() - kSuffix.size());
        }
    }
    return base + "_scenario.lua";
}

Conversion convert(const MohoReplay& moho) {
    return Converter(moho).run();
}

} // namespace osc::scfa
