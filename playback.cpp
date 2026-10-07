#include "playback.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace osu {
Buttons button_state(std::uint32_t keys) {
     
    return {bool((keys & 1) && !(keys & 4)), bool((keys & 2) && !(keys & 8)),
        bool(keys & 4), bool(keys & 8), bool(keys & 16)};
}

std::vector<Change> button_changes(const Buttons& before, const Buttons& after) {
    std::vector<Change> result;
     
    for (bool down : {false, true})
        for (std::size_t i = 0; i < before.size(); ++i)
            if (before[i] != after[i] && after[i] == down)
                result.push_back({static_cast<Button>(i), down});
    return result;
}

Sample interpolate(const Sample& left, const Sample& right, double time_ms) {
    double amount = right.time_ms > left.time_ms
        ? std::clamp((time_ms - left.time_ms) / (right.time_ms - left.time_ms), 0.0, 1.0) : 0;
    return {time_ms, left.x + amount * (right.x - left.x), left.y + amount * (right.y - left.y), left.keys};
}

Schedule make_timeline(const Replay& replay) {
    Schedule schedule;
    schedule.mods = replay.mods;
    schedule.rate = playback_rate(schedule.mods);
    std::vector<Sample> normalized;
    for (std::size_t i = 0; i < replay.frames.size(); ++i) {
        const auto& frame = replay.frames[i];
        if (i < 2 && frame.x == 256 && frame.y == -500 && frame.keys == 0) {
            ++schedule.startup_frames;
            continue;
        }
        double time = static_cast<double>(frame.time_ms);
        if (!normalized.empty() && time < normalized.back().time_ms) {
             
            time = normalized.back().time_ms;
            ++schedule.regressed_frames;
        }
        normalized.push_back({time, frame.x, frame.y, frame.keys});
    }
    if (normalized.empty()) throw std::runtime_error("No playable replay frames");
    schedule.frames = std::move(normalized);
    return schedule;
}

Rect stable_playfield(const Rect& client) {
    double scale = std::min(client.width / 640.0, client.height / 480.0);
    double width = 512 * scale, height = 384 * scale;
    return {client.left + (client.width - width) / 2,
        client.top + (client.height - height) / 2 + 8 * scale, width, height};
}

Point map_position(const Rect& rect, double x, double y) {
    return {rect.left + x * rect.width / 512, rect.top + y * rect.height / 384};
}
}  
