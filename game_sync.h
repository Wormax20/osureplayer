#pragma once

#include "playback.h"

namespace osu {

struct GameState {
    int status = 0, time_ms = 0, retries = 0, mode = 0;
    std::uint32_t mods = 0;
    bool watching_replay = false;
    std::string beatmap_hash;
};

enum class SyncAction { wait, start, advance, seek, release };

 
class AutoSync {
public:
    AutoSync(std::string hash, std::uint32_t mods);
    SyncAction update(const GameState& state, double wall_ms);
    std::string blocking_reason(const GameState& state) const;
    std::string mods_warning(std::uint32_t mods) const;
    bool active() const { return active_; }
    bool awaiting_start() const { return pending_start_; }
private:
    std::string hash_;
    std::uint32_t mods_;
    std::optional<GameState> previous_;
    bool active_ = false, stalled_ = false, pending_start_ = false, clock_reset_ = false;
    double last_change_ms_ = 0;
};

 
std::vector<std::size_t> find_pattern(std::span<const std::uint8_t> bytes, std::span<const int> pattern);

}  
