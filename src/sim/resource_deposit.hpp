#pragma once

#include "core/types.hpp"

#include <cmath>
#include <vector>

namespace osc::sim {

/// Resource deposit (mass/hydrocarbon point on map).
struct ResourceDeposit {
    f32 x = 0, y = 0, z = 0;
    f32 size = 1.0f;
    enum Type : u8 { Mass = 0, Hydrocarbon = 1 } type = Mass;
};

/// A deposit's square on the map grid, as Moho keeps it (CSimResources
/// BuildRectFromPointAndSize; 3599's fistp at 0x545DF9 rounds): from its
/// centre less half its whole size, to that plus the size.
struct DepositRect {
    i32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
};
inline DepositRect deposit_rect(const ResourceDeposit& d) {
    const i32 size = static_cast<i32>(d.size);
    const auto x0 = static_cast<i32>(std::nearbyint(d.x - static_cast<f32>(size) * 0.5f));
    const auto z0 = static_cast<i32>(std::nearbyint(d.z - static_cast<f32>(size) * 0.5f));
    return {x0, z0, x0 + size, z0 + size};
}

/// Moho's CSimResources::DepositIsInArea (3599 0x546590): a deposit of `type`
/// whose square holds the cells `area`, or lies within them.
inline bool deposit_in_area(const std::vector<ResourceDeposit>& deposits,
                            ResourceDeposit::Type type, const DepositRect& area) {
    for (const ResourceDeposit& d : deposits) {
        if (d.type != type) {
            continue;
        }
        const DepositRect r = deposit_rect(d);
        if (area.x0 <= r.x0) {
            if (area.x1 >= r.x1 && r.z0 >= area.z0 && area.z1 >= r.z1) {
                return true;
            }
            if (area.x0 < r.x0) {
                continue;
            }
        }
        if (r.x1 >= area.x1 && area.z0 >= r.z0 && r.z1 >= area.z1) {
            return true;
        }
    }
    return false;
}

} // namespace osc::sim
