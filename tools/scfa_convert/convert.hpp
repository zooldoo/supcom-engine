#pragma once

#include "scfa_convert/moho_replay.hpp"
#include "sim/replay.hpp"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace osc::scfa {

/// A checksum a player recorded: its MD5 of the sim after `beat`.
struct MohoChecksum {
    u32 beat = 0;
    u8 source = 0xFF;
    std::array<u8, 16> digest{};
};

struct Conversion {
    sim::Replay replay;
    std::vector<MohoChecksum> checksums;
    /// Messages left out, by kind ("op:SetCommandCells", "order:BuildAssist"...).
    std::map<std::string, int> skipped;
    std::vector<std::string> notes;
};

/// The engine's replay of a Moho one: its setup from the header (Moho's
/// random stream and pathing), its stream as the engine's commands, each
/// entity named by its Moho id alone (EntityId::moho_only).
Conversion convert(const MohoReplay& moho);

/// The retail scenario file of a map ("/maps/X/X.scmap" -> "/maps/X/X_scenario.lua").
std::string scenario_of_map(const std::string& map);

} // namespace osc::scfa
