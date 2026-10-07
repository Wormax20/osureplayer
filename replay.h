#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace osu {

struct Frame {
    std::int64_t delta_ms = 0;
    std::int64_t time_ms = 0;
    double x = 0, y = 0;
    std::uint32_t keys = 0;
};

struct Replay {
    std::uint8_t mode = 0;
    std::int32_t version = 0;
    std::string beatmap_hash, player, replay_hash, life_graph;
    std::array<std::uint16_t, 6> hits{};
    std::uint32_t score = 0, mods = 0;
    std::uint16_t max_combo = 0;
    bool perfect = false;
    std::int64_t timestamp = 0;
    std::optional<std::int64_t> online_id;
    std::optional<std::int32_t> seed;
    std::optional<double> target_accuracy;
    std::vector<Frame> frames;
    std::size_t compressed_bytes = 0, trailing_bytes = 0;
};

 
Replay parse_replay(std::span<const std::uint8_t> data);
Replay read_replay(const std::filesystem::path& path);
std::vector<Frame> parse_frames(std::string_view text, std::optional<std::int32_t>& seed);
double playback_rate(std::uint32_t mods);

}  
