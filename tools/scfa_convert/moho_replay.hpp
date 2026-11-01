#pragma once

#include "core/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osc::scfa {

/// A value of Moho's SCR byte stream (SCR_ToByteStream), its numbers floats.
struct ScrValue {
    enum class Kind : u8 { Nil, Number, String, Bool, Table };
    Kind kind = Kind::Nil;
    f32 number = 0;
    bool boolean = false;
    std::string string;
    std::vector<std::pair<ScrValue, ScrValue>> table;

    const ScrValue* field(std::string_view key) const;
    std::string text(std::string_view key) const;
    std::optional<f32> number_at(std::string_view key) const;
    /// A boolean, or the string "true".
    bool flag(std::string_view key) const;
};

/// Moho's command stream operations (ECmdStreamOp).
enum class MohoOp : u8 {
    Advance = 0,
    SetCommandSource = 1,
    CommandSourceTerminated = 2,
    VerifyChecksum = 3,
    RequestPause = 4,
    Resume = 5,
    SingleStep = 6,
    CreateUnit = 7,
    CreateProp = 8,
    DestroyEntity = 9,
    WarpEntity = 10,
    ProcessInfoPair = 11,
    IssueCommand = 12,
    IssueFactoryCommand = 13,
    IncreaseCommandCount = 14,
    DecreaseCommandCount = 15,
    SetCommandTarget = 16,
    SetCommandType = 17,
    SetCommandCells = 18,
    RemoveCommandFromQueue = 19,
    DebugCommand = 20,
    ExecuteLuaInSim = 21,
    LuaSimCallback = 22,
    EndGame = 23,
};

struct MohoSource {
    std::string name;
    i32 timeouts = -1;
};

struct MohoArmy {
    ScrValue options; // its PlayerOptions
    std::vector<u8> sources;
};

/// One message of the stream. `beat`: the beats advanced before it, so it
/// runs in beat `beat + 1`.
struct MohoMessage {
    MohoOp op = MohoOp::Advance;
    u32 beat = 0;
    u8 source = 0xFF;
    std::vector<u8> data;
};

struct MohoReplay {
    std::string build; // "Supreme Commander v1.50.3599"
    std::string map;   // "/maps/X/X.scmap"
    ScrValue mods;
    ScrValue scenario;
    std::vector<MohoSource> sources;
    bool cheats = false;
    std::vector<MohoArmy> armies;
    u32 seed = 0;
    std::vector<MohoMessage> messages;
    u32 beats = 0;
};

/// A .SCFAReplay (FAF's .fafreplay once unpacked). Nullopt, with `error`,
/// on malformed input.
std::optional<MohoReplay> read_moho_replay(std::span<const u8> bytes, std::string& error);

/// Reads a message's data as CDecoder does; a read past its end sets ok() false.
class MohoReader {
public:
    explicit MohoReader(std::span<const u8> bytes) : b_(bytes) {}
    u8 u8v();
    u16 u16v();
    u32 u32v();
    i32 i32v() { return static_cast<i32>(u32v()); }
    f32 f32v();
    std::string cstr();
    std::optional<ScrValue> scr();
    bool ok() const { return ok_; }
    size_t position() const { return pos_; }
    bool at_end() const { return pos_ == b_.size(); }

private:
    bool need(size_t n);
    std::optional<ScrValue> scr_value(int depth);
    std::span<const u8> b_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace osc::scfa
