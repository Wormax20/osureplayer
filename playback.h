#pragma once

#include "replay.h"

namespace osu {

enum class Button { mouse1, mouse2, key1, key2, smoke };
using Buttons = std::array<bool, 5>;
struct Change { Button button; bool down; };
Buttons button_state(std::uint32_t keys);
std::vector<Change> button_changes(const Buttons& before, const Buttons& after);

struct Sample {
    double time_ms = 0, x = 0, y = 0;
    std::uint32_t keys = 0;
};
struct Schedule {
    std::vector<Sample> frames;
    std::size_t startup_frames = 0, regressed_frames = 0;
    double rate = 1;
    std::uint32_t mods = 0;
};
Schedule make_timeline(const Replay& replay);
Sample interpolate(const Sample& left, const Sample& right, double time_ms);

struct Rect { double left = 0, top = 0, width = 0, height = 0; };
struct Point { double x, y; };
Rect stable_playfield(const Rect& client);
Point map_position(const Rect& playfield, double x, double y);

struct PlayOptions {
    wchar_t key1 = L'Z', key2 = L'X', smoke = L'C';
    std::optional<Rect> rect;  
};
void play_windows(const Replay& replay, const Schedule& schedule, const PlayOptions& options);

}  
