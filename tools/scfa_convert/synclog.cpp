#include "scfa_convert/synclog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace osc::scfa {

namespace {

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) {
        s.remove_suffix(1);
    }
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

/// The hex words after the '[' of "pos: <...> [0x.. 0x.. 0x..]".
std::vector<u32> bracket_hex(std::string_view s) {
    std::vector<u32> words;
    const size_t open = s.find('[');
    if (open == std::string_view::npos) {
        return words;
    }
    std::string rest(s.substr(open + 1));
    const char* p = rest.c_str();
    unsigned value = 0;
    int used = 0;
    while (std::sscanf(p, " 0x%x%n", &value, &used) == 1) {
        words.push_back(value);
        p += used;
    }
    return words;
}

std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (;;) {
        const size_t space = s.find(' ', start);
        out.push_back(s.substr(start, space == std::string_view::npos ? s.npos : space - start));
        if (space == std::string_view::npos) {
            return out;
        }
        start = space + 1;
    }
}

std::optional<u32> number(std::string_view s, int base) {
    std::string text(s);
    char* end = nullptr;
    const unsigned long v = std::strtoul(text.c_str(), &end, base);
    if (text.empty() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<u32>(v);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

std::string hex(u32 v) {
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08x", v);
    return text;
}

} // namespace

std::optional<SynclogBeat> read_synclog_beat(std::istream& in) {
    SynclogBeat out;
    bool have_beat = false;
    bool dirty = false;
    EntityState* entity = nullptr;
    for (std::string line; std::getline(in, line);) {
        if (!have_beat) {
            unsigned beat = 0;
            if (std::sscanf(line.c_str(), "********** beat %u", &beat) == 1) {
                out.beat = beat;
                have_beat = true;
            }
            continue;
        }
        const std::string_view text = trimmed(line);
        if (text == "Dirty Entities") {
            dirty = true;
            out.dumped = true;
            continue;
        }
        if (!dirty) {
            continue;
        }
        if (starts_with(text, "beat ")) {
            break;
        }
        if (starts_with(line, "  0x") && text.size() == 10) {
            if (const auto id = number(text.substr(2), 16)) {
                entity = &out.entities.emplace_back();
                entity->id = *id;
            }
            continue;
        }
        if (!entity) {
            continue;
        }
        if (starts_with(text, "health:")) {
            const size_t at = text.rfind("0x");
            if (at != std::string_view::npos) {
                entity->health = number(text.substr(at + 2), 16).value_or(0);
            }
        } else if (starts_with(text, "bp:")) {
            entity->blueprint = std::string(text.substr(3));
        } else if (starts_with(text, "pos:")) {
            const auto words = bracket_hex(text);
            for (size_t i = 0; i < words.size() && i < 3; ++i) {
                entity->pos[i] = words[i];
            }
        } else if (starts_with(text, "rot:")) {
            // Moho's quaternion is w, x, y, z
            const auto words = bracket_hex(text);
            if (words.size() == 4) {
                entity->rot = {words[1], words[2], words[3], words[0]};
            }
        }
    }
    if (!have_beat) {
        return std::nullopt;
    }
    return out;
}

BeatStates read_entity_trace(std::istream& in) {
    BeatStates out;
    std::map<u32, EntityState>* tick = nullptr;
    for (std::string line; std::getline(in, line);) {
        const std::string_view text = trimmed(line);
        if (starts_with(text, "T ")) {
            const auto fields = split(text);
            const auto at = fields.size() > 1 ? number(fields[1], 10) : std::nullopt;
            tick = at ? &out[*at] : nullptr;
            continue;
        }
        if (!tick) {
            continue;
        }
        const auto f = split(text);
        if (f.size() < 16 || f[5] != "p" || f[9] != "q" || f[14] != "h") {
            continue;
        }
        EntityState e;
        const auto id = number(f[0], 10);
        if (!id) {
            continue;
        }
        e.id = *id;
        e.blueprint = std::string(f[2]);
        for (size_t i = 0; i < 3; ++i) {
            e.pos[i] = number(f[6 + i], 16).value_or(0);
        }
        for (size_t i = 0; i < 4; ++i) {
            e.rot[i] = number(f[10 + i], 16).value_or(0);
        }
        e.health = number(f[15], 16).value_or(0);
        (*tick)[e.id] = std::move(e);
    }
    return out;
}

u64 ulp_distance(u32 a, u32 b) {
    const auto ordered = [](u32 bits) {
        const i64 magnitude = static_cast<i64>(bits & 0x7FFFFFFFu);
        return (bits & 0x80000000u) ? -magnitude : magnitude;
    };
    const i64 d = ordered(a) - ordered(b);
    return static_cast<u64>(d < 0 ? -d : d);
}

std::vector<BeatComparison> compare(const BeatStates& retail, const BeatStates& engine,
                                    size_t keep) {
    std::vector<BeatComparison> out;
    for (const auto& [beat, entities] : retail) {
        const auto traced = engine.find(beat);
        if (traced == engine.end()) {
            continue;
        }
        BeatComparison c;
        c.beat = beat;
        c.entities = entities.size();
        const auto note = [&](const EntityState& r, std::string field, std::string a, std::string b,
                              u64 ulps) {
            if (c.differences.size() < keep) {
                c.differences.push_back({beat, r.id, r.blueprint, std::move(field), std::move(a),
                                         std::move(b), ulps});
            }
        };
        for (const auto& [id, r] : entities) {
            const auto it = traced->second.find(id);
            if (it == traced->second.end()) {
                ++c.missing;
                note(r, "missing", hex(id), "-", 0);
                continue;
            }
            const EntityState& e = it->second;
            bool differs = false;
            if (lower(r.blueprint) != lower(e.blueprint)) {
                differs = true;
                note(r, "bp", r.blueprint, e.blueprint, 0);
            }
            const auto field = [&](const char* name, u32 a, u32 b) {
                if (a != b) {
                    differs = true;
                    note(r, name, hex(a), hex(b), ulp_distance(a, b));
                }
            };
            field("health", r.health, e.health);
            field("pos.x", r.pos[0], e.pos[0]);
            field("pos.y", r.pos[1], e.pos[1]);
            field("pos.z", r.pos[2], e.pos[2]);
            field("rot.x", r.rot[0], e.rot[0]);
            field("rot.y", r.rot[1], e.rot[1]);
            field("rot.z", r.rot[2], e.rot[2]);
            field("rot.w", r.rot[3], e.rot[3]);
            if (differs) {
                ++c.differing;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace osc::scfa
