#include "sim/synclog.hpp"

#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>

#include <bit>
#include <fstream>
#include <utility>

namespace osc::sim {

namespace {

u32 bits(f32 v) {
    return std::bit_cast<u32>(v);
}

bool same(const Vector3& a, const Vector3& b) {
    return bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) && bits(a.z) == bits(b.z);
}

bool same(const Quaternion& a, const Quaternion& b) {
    return bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) && bits(a.z) == bits(b.z) &&
           bits(a.w) == bits(b.w);
}

// Moho's Unit::GetVelocity and Entity::GetVelocity.
Vector3 velocity(const Entity& e) {
    if (e.is_unit()) {
        const auto& unit = static_cast<const Unit&>(e);
        if (unit.is_mobile() && unit.layer() != "Air") {
            return unit.motion_velocity();
        }
    }
    const Vector3& p = e.position();
    const Vector3& was = e.last_position();
    return {p.x - was.x, p.y - was.y, p.z - was.z};
}

f32 per_tick(f64 per_second) {
    return static_cast<f32>(per_second * SimState::SECONDS_PER_TICK);
}

} // namespace

SyncLog::SyncLog(std::filesystem::path dir, u32 period)
    : dir_(std::move(dir)), period_(period > 0 ? period : 1) {
    std::filesystem::create_directories(dir_);
}

// Moho's beat 0 is the setup's (no AdvanceBeat); beat N is the one whose tick is N.
bool SyncLog::logged(const SimState& sim) const {
    return sim.beat_count() % period_ == 0;
}

void SyncLog::begin_beat(const SimState& sim) {
    poses_.clear();
    dying_.clear();
    if (!logged(sim)) {
        return;
    }
    sim.entity_registry().for_each([&](const Entity& e) {
        if (!e.is_unit() && !e.destroyed()) {
            poses_[e.entity_id()] = {e.position(), e.orientation(), e.health()};
        }
    });
}

void SyncLog::note_dying(const SimState& sim) {
    if (!logged(sim)) {
        return;
    }
    sim.entity_registry().for_each([&](const Entity& e) {
        if (e.destroyed()) {
            dying_ += entity(e, velocity(e));
        }
    });
}

std::string SyncLog::entity(const Entity& e, const Vector3& v) const {
    const Vector3& p = e.position();
    const Quaternion& q = e.orientation();
    const f32 health = e.health();
    return fmt::format("  0x{:08x}\n"
                       "    health: {:.1f} 0x{:08x}\n"
                       "    bp:{}\n"
                       "    pos: <{:7.2f},{:7.2f},{:7.2f}> [0x{:08x} 0x{:08x} 0x{:08x}]\n"
                       "    rot: <{:7.4f},{:7.4f},{:7.4f},{:7.4f}> [0x{:08x} 0x{:08x} 0x{:08x} "
                       "0x{:08x}]\n"
                       "   vel: <{:7.2f},{:7.2f},{:7.2f}> [0x{:08x} 0x{:08x} 0x{:08x}]\n",
                       e.entity_id().moho(), health, bits(health), e.blueprint_id(), p.x, p.y, p.z,
                       bits(p.x), bits(p.y), bits(p.z), q.w, q.x, q.y, q.z, bits(q.w), bits(q.x),
                       bits(q.y), bits(q.z), v.x, v.y, v.z, bits(v.x), bits(v.y), bits(v.z));
}

void SyncLog::end_beat(const SimState& sim) {
    if (!logged(sim)) {
        return;
    }
    const u32 beat = sim.beat_count();
    std::string out = fmt::format("********** beat {} **********\n  tick number {}\nArmies\n", beat,
                                  sim.tick_count());
    for (size_t i = 0; i < sim.army_count(); ++i) {
        const ArmyBrain& army = *sim.army_at(i);
        const EconomyState& ec = army.economy();
        const auto pair = [&](const char* name, f32 energy, f32 mass) {
            out += fmt::format("    {}={:.1f},{:.1f}\n", name, energy, mass);
        };
        out += fmt::format("  \"{}\" [{}]\n", army.nickname(), army.name());
        pair("mStored", ec.energy.stored, ec.mass.stored);
        pair("mIncome", per_tick(ec.energy.income), per_tick(ec.mass.income));
        pair("mReclaimed", static_cast<f32>(ec.energy.reclaimed),
             static_cast<f32>(ec.mass.reclaimed));
        pair("mLastUseRequested", per_tick(ec.energy.requested), per_tick(ec.mass.requested));
        pair("mLastUseActual", per_tick(ec.energy.requested * army.energy_efficiency()),
             per_tick(ec.mass.requested * army.mass_efficiency()));
        out += "    mMaxStorage[0]=\n    mMaxStorage[1]=\n";
    }
    out += "Dirty Entities\n";
    sim.entity_registry().for_each([&](const Entity& e) {
        if (e.destroyed()) {
            return;
        }
        if (!e.is_unit() && !e.parent_entity_id() && !e.beam_launcher_id()) {
            const auto it = poses_.find(e.entity_id());
            if (it != poses_.end() && same(it->second.position, e.position()) &&
                same(it->second.orientation, e.orientation()) &&
                bits(it->second.health) == bits(e.health())) {
                return;
            }
        }
        out += entity(e, velocity(e));
    });
    out += dying_;
    std::ofstream(dir_ / fmt::format("beat{:05}.log", beat), std::ios::binary) << out;
}

} // namespace osc::sim
