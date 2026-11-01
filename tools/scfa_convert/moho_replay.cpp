#include "scfa_convert/moho_replay.hpp"

#include <cstring>

namespace osc::scfa {

namespace {

constexpr int kMaxScrDepth = 32;
constexpr std::string_view kReplayBanner = "Replay v1.9\r\n";

} // namespace

const ScrValue* ScrValue::field(std::string_view key) const {
    for (const auto& [k, v] : table) {
        if (k.kind == Kind::String && k.string == key) {
            return &v;
        }
    }
    return nullptr;
}

std::string ScrValue::text(std::string_view key) const {
    const ScrValue* v = field(key);
    return v && v->kind == Kind::String ? v->string : std::string();
}

std::optional<f32> ScrValue::number_at(std::string_view key) const {
    const ScrValue* v = field(key);
    if (v && v->kind == Kind::Number) {
        return v->number;
    }
    return std::nullopt;
}

bool ScrValue::flag(std::string_view key) const {
    const ScrValue* v = field(key);
    if (!v) {
        return false;
    }
    return v->kind == Kind::Bool ? v->boolean : (v->kind == Kind::String && v->string == "true");
}

bool MohoReader::need(size_t n) {
    if (!ok_ || n > b_.size() - pos_) {
        ok_ = false;
    }
    return ok_;
}

u8 MohoReader::u8v() {
    return need(1) ? b_[pos_++] : 0;
}

u16 MohoReader::u16v() {
    if (!need(2)) {
        return 0;
    }
    const u16 v = static_cast<u16>(b_[pos_] | (b_[pos_ + 1] << 8));
    pos_ += 2;
    return v;
}

u32 MohoReader::u32v() {
    if (!need(4)) {
        return 0;
    }
    u32 v = 0;
    for (size_t i = 0; i < 4; ++i) {
        v |= static_cast<u32>(b_[pos_ + i]) << (8 * i);
    }
    pos_ += 4;
    return v;
}

f32 MohoReader::f32v() {
    const u32 bits = u32v();
    f32 v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::string MohoReader::cstr() {
    std::string s;
    while (ok_) {
        const u8 c = u8v();
        if (!ok_ || c == 0) {
            break;
        }
        s.push_back(static_cast<char>(c));
    }
    return s;
}

std::optional<ScrValue> MohoReader::scr() {
    return scr_value(0);
}

std::optional<ScrValue> MohoReader::scr_value(int depth) {
    ScrValue v;
    switch (u8v()) {
    case 0:
        v.kind = ScrValue::Kind::Number;
        v.number = f32v();
        break;
    case 1:
        v.kind = ScrValue::Kind::String;
        v.string = cstr();
        break;
    case 2: v.kind = ScrValue::Kind::Nil; break;
    case 3:
        v.kind = ScrValue::Kind::Bool;
        v.boolean = u8v() != 0;
        break;
    case 4:
        if (depth >= kMaxScrDepth) {
            return std::nullopt;
        }
        v.kind = ScrValue::Kind::Table;
        while (ok_) {
            if (!need(1)) {
                return std::nullopt;
            }
            if (b_[pos_] == 5) {
                ++pos_;
                break;
            }
            auto key = scr_value(depth + 1);
            auto value = key ? scr_value(depth + 1) : std::nullopt;
            if (!value) {
                return std::nullopt;
            }
            v.table.emplace_back(std::move(*key), std::move(*value));
        }
        break;
    default: return std::nullopt;
    }
    if (!ok_) {
        return std::nullopt;
    }
    return v;
}

std::optional<MohoReplay> read_moho_replay(std::span<const u8> bytes, std::string& error) {
    MohoReplay out;
    MohoReader r(bytes);
    out.build = r.cstr();
    r.cstr();
    if (!r.ok() || bytes.size() - r.position() < kReplayBanner.size() ||
        std::memcmp(bytes.data() + r.position(), kReplayBanner.data(), kReplayBanner.size()) !=
            0) {
        error = "not a Moho replay (no \"Replay v1.9\" banner)";
        return std::nullopt;
    }
    for (size_t i = 0; i < kReplayBanner.size(); ++i) {
        r.u8v();
    }
    out.map = r.cstr();
    r.cstr();

    const auto blob = [&](ScrValue& into, const char* what) {
        const u32 length = r.u32v();
        const size_t start = r.position();
        auto value = r.scr();
        if (!value || r.position() - start != length) {
            error = std::string("malformed ") + what;
            return false;
        }
        into = std::move(*value);
        return true;
    };
    if (!blob(out.mods, "GameMods") || !blob(out.scenario, "ScenarioInfo")) {
        return std::nullopt;
    }
    const u8 sources = r.u8v();
    for (u8 i = 0; i < sources && r.ok(); ++i) {
        MohoSource s;
        s.name = r.cstr();
        s.timeouts = r.i32v();
        out.sources.push_back(std::move(s));
    }
    out.cheats = r.u8v() != 0;
    const u8 armies = r.u8v();
    for (u8 i = 0; i < armies && r.ok(); ++i) {
        MohoArmy a;
        if (!blob(a.options, "PlayerOptions")) {
            return std::nullopt;
        }
        for (u8 s = r.u8v(); r.ok() && s != 0xFF; s = r.u8v()) {
            a.sources.push_back(s);
        }
        out.armies.push_back(std::move(a));
    }
    out.seed = r.u32v();
    if (!r.ok()) {
        error = "truncated header";
        return std::nullopt;
    }

    u8 source = 0xFF;
    while (!r.at_end()) {
        const size_t start = r.position();
        const u8 op = r.u8v();
        const u16 size = r.u16v();
        if (!r.ok() || size < 3 || bytes.size() - start < size) {
            error = "truncated message at byte " + std::to_string(start);
            return std::nullopt;
        }
        if (op > static_cast<u8>(MohoOp::EndGame)) {
            error = "unknown operation " + std::to_string(op) + " at byte " + std::to_string(start);
            return std::nullopt;
        }
        MohoMessage m;
        m.op = static_cast<MohoOp>(op);
        m.beat = out.beats;
        m.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(start + 3),
                      bytes.begin() + static_cast<std::ptrdiff_t>(start + size));
        for (size_t i = 3; i < size; ++i) {
            r.u8v();
        }
        if (m.op == MohoOp::SetCommandSource && !m.data.empty()) {
            source = m.data[0];
        }
        m.source = source;
        if (m.op == MohoOp::Advance) {
            MohoReader adv(m.data);
            out.beats += adv.u32v();
        }
        out.messages.push_back(std::move(m));
    }
    return out;
}

} // namespace osc::scfa
