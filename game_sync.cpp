#include "game_sync.h"
#include "beatmap.h"

#include <algorithm>

namespace osu {
namespace {
std::uint32_t normalized_mods(std::uint32_t mods) {
    mods &= ~(1u << 2);  
    if (mods & (1u << 9)) mods |= 1u << 6;  
    if (mods & (1u << 14)) mods |= 1u << 5;  
    return mods;
}
}

AutoSync::AutoSync(std::string hash, std::uint32_t mods) : hash_(std::move(hash)), mods_(normalized_mods(mods)) {}

std::string AutoSync::blocking_reason(const GameState& state) const {
    if (state.mode != 0) return "select osu!standard mode";
    if (state.watching_replay) return "leave the built-in replay and start a local play";
    if (state.beatmap_hash != hash_) return "select the replay's matching beatmap (MD5 differs)";
    if (state.status != 2) return "start a new play of the matching beatmap";
    return {};
}

std::string AutoSync::mods_warning(std::uint32_t mods) const {
    auto current_mods = normalized_mods(mods);
    if (current_mods != mods_)
        return "Mods mismatch: game=" + mod_names(current_mods) + ", playback=" + mod_names(mods_) +
            "; playback will continue";
    return {};
}

SyncAction AutoSync::update(const GameState& state, double wall_ms) {
    auto old = previous_;
    previous_ = state;
    bool restart = old && (state.retries != old->retries || state.time_ms < old->time_ms - 20);
    bool new_play = old && state.status == 2 && (old->status != 2 || restart);
    if (new_play) {
        pending_start_ = true;
        clock_reset_ = state.time_ms <= 0 && (old->status != 2 || state.time_ms < old->time_ms - 20);
    }
    bool matching = blocking_reason(state).empty();
    if (!matching) {
        bool was_active = active_;
        active_ = stalled_ = false;
        if (was_active || state.status != 2 || (old && old->status == 2 && state.time_ms > old->time_ms)) pending_start_ = false;
        return was_active ? SyncAction::release : SyncAction::wait;
    }
     
    if (active_ && restart) {
        active_ = stalled_ = false;
        return SyncAction::release;
    }
    if (!active_) {
         
        if (!pending_start_ || !clock_reset_ || !old || old->status != 2 || state.time_ms <= old->time_ms) return SyncAction::wait;
        active_ = true;
        stalled_ = pending_start_ = false;
        last_change_ms_ = wall_ms;
        return SyncAction::start;
    }
    if (state.time_ms > old->time_ms) {
        bool seek = state.time_ms - old->time_ms >
            (wall_ms - last_change_ms_) * playback_rate(state.mods) + 250;
        stalled_ = false;
        last_change_ms_ = wall_ms;
        return seek ? SyncAction::seek : SyncAction::advance;
    }
    if (!stalled_ && wall_ms - last_change_ms_ >= 100) {
        stalled_ = true;
        return SyncAction::release;
    }
    return SyncAction::wait;
}

std::vector<std::size_t> find_pattern(std::span<const std::uint8_t> bytes, std::span<const int> pattern) {
    std::vector<std::size_t> result;
    if (pattern.empty() || bytes.size() < pattern.size()) return result;
    for (std::size_t i = 0; i <= bytes.size() - pattern.size(); ++i) {
        if (pattern[0] >= 0) {
            auto end = bytes.begin() + (bytes.size() - pattern.size() + 1);
            auto found = std::find(bytes.begin() + i, end, static_cast<std::uint8_t>(pattern[0]));
            if (found == end) break;
            i = static_cast<std::size_t>(found - bytes.begin());
        }
        bool match = true;
        for (std::size_t p = 1; p < pattern.size(); ++p)
            if (pattern[p] >= 0 && bytes[i + p] != pattern[p]) { match = false; break; }
        if (match) result.push_back(i);
    }
    return result;
}
}  
