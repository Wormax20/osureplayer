#pragma once

#include "replay.h"

namespace osu {

struct Difficulty {
    double ar = 0, od = 0, cs = 0, hp = 0;
};

struct BeatmapInfo {
    std::string artist, title, creator, difficulty, source;
    Difficulty base;
    int database_version = 0, mode = 0;
    int beatmap_id = 0, set_id = 0, length_ms = 0, drain_seconds = 0;
    int circles = 0, sliders = 0, spinners = 0;
    double slider_multiplier = 0;
    std::optional<double> bpm_min, bpm_max, stars_base, stars_mods;
};

std::optional<BeatmapInfo> read_beatmap_database(const std::filesystem::path& path,
    std::string_view hash, std::uint32_t mods);
Difficulty modded_difficulty(Difficulty base, std::uint32_t mods);
std::string mod_names(std::uint32_t mods);

}  
