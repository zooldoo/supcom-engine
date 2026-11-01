#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp"
#include "sim/entity_id.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace osc::sim {

class SimState;

/// Moho's /synclog of its checksum beats (Sim::UpdateChecksum, every sim_ChecksumPeriod-th
/// beat): each army's economy totals and the entities it hashes, in its format, a file a beat.
/// A unit is hashed every time; any other entity when made or moved on that beat.
class SyncLog {
public:
    SyncLog(std::filesystem::path dir, u32 period);

    void begin_beat(const SimState& sim);
    void end_beat(const SimState& sim);

private:
    struct Pose {
        Vector3 position;
        Quaternion orientation;
        f32 health = 0.0f;
    };

    bool logged(const SimState& sim) const;
    std::string entity(const Entity& e, const Vector3& velocity) const;

    std::filesystem::path dir_;
    u32 period_;
    std::unordered_map<EntityId, Pose> poses_;
};

} // namespace osc::sim
