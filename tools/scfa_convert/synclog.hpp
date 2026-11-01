#pragma once

#include "core/types.hpp"

#include <array>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace osc::scfa {

/// An entity as Moho's checksum dump or the engine's --entity-trace gives it, floats as bits.
struct EntityState {
    u32 id = 0;
    std::string blueprint;
    u32 health = 0;
    std::array<u32, 3> pos{};
    std::array<u32, 4> rot{}; // x, y, z, w
};

/// Beat (or tick) -> entity id -> its state.
using BeatStates = std::map<u32, std::map<u32, EntityState>>;

/// One /synclog beat%05d.log: its beat and, on a checksum beat, its dirty entities.
struct SynclogBeat {
    u32 beat = 0;
    bool dumped = false;
    std::vector<EntityState> entities;
};
std::optional<SynclogBeat> read_synclog_beat(std::istream& in);

/// The engine's --entity-trace, by tick.
BeatStates read_entity_trace(std::istream& in);

struct Difference {
    u32 beat = 0;
    u32 id = 0;
    std::string blueprint;
    std::string field; // "missing", "bp", "health", "pos.x", "rot.w", ...
    std::string retail;
    std::string engine;
    u64 ulps = 0;
};

struct BeatComparison {
    u32 beat = 0;
    size_t entities = 0; // in retail's dump
    size_t missing = 0;  // of them, not in the engine's trace
    size_t differing = 0;
    std::vector<Difference> differences; // the first `keep`
};

/// Each beat retail dumped against the engine's trace of that tick. A beat the
/// engine didn't trace is left out.
std::vector<BeatComparison> compare(const BeatStates& retail, const BeatStates& engine, size_t keep);

/// How many floats lie between two, by their bits.
u64 ulp_distance(u32 a, u32 b);

} // namespace osc::scfa
