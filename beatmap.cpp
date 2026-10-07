#include "beatmap.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace osu {
namespace {
class DbReader {
public:
    explicit DbReader(std::span<const std::uint8_t> data) : data_(data) {}
    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > data_.size()) throw std::runtime_error("Truncated osu!.db");
        auto result = data_.first(count);
        data_ = data_.subspan(count);
        return result;
    }
    template<class T> T number() {
        auto bytes = take(sizeof(T));
        std::uint64_t bits = 0;
        for (std::size_t i = 0; i < bytes.size(); ++i) bits |= std::uint64_t(bytes[i]) << (8 * i);
        if constexpr (sizeof(T) == 8) return std::bit_cast<T>(bits);
        else if constexpr (sizeof(T) == 4) return std::bit_cast<T>(static_cast<std::uint32_t>(bits));
        else return static_cast<T>(bits);
    }
    std::string string() {
        auto marker = number<std::uint8_t>();
        if (marker == 0) return {};
        if (marker != 11) throw std::runtime_error("Invalid osu!.db string marker");
        std::uint32_t size = 0;
        for (unsigned i = 0; i < 5; ++i) {
            auto byte = number<std::uint8_t>();
            if (i == 4 && (byte & 0xf0)) throw std::runtime_error("osu!.db string length overflow");
            size |= std::uint32_t(byte & 127) << (7 * i);
            if (!(byte & 128)) {
                if (size > 4 * 1024 * 1024) throw std::runtime_error("osu!.db string too large");
                auto bytes = take(size);
                return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
            }
        }
        throw std::runtime_error("Invalid osu!.db string length");
    }
    int count(int maximum) {
        auto count = number<std::int32_t>();
        if (count < 0 || count > maximum) throw std::runtime_error("Invalid osu!.db item count");
        return count;
    }
private:
    std::span<const std::uint8_t> data_;
};

std::uint32_t star_mods(std::uint32_t mods) {
    return (mods & 512) ? (mods & ~512u) | 64u : mods;
}
}

std::optional<BeatmapInfo> read_beatmap_database(const std::filesystem::path& path,
    std::string_view hash, std::uint32_t mods) {
    if (hash.size() != 32) return std::nullopt;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open osu!.db");
    auto size = file.tellg();
    if (size <= 0 || size > 512 * 1024 * 1024) throw std::runtime_error("Invalid osu!.db size (limit: 512 MiB)");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read complete osu!.db");
    DbReader reader(bytes);
    int version = reader.number<std::int32_t>();
    if (version < 20070101 || version >= 30000000) throw std::runtime_error("Unsupported osu!.db version");
    reader.take(4 + 1 + 8);
    reader.string();
    int count = reader.count(1000000);
    for (int entry = 0; entry < count; ++entry) {
        if (version < 20191106) reader.take(4);
        BeatmapInfo info;
        info.database_version = version;
        info.artist = reader.string();
        auto unicode = reader.string();
        if (!unicode.empty()) info.artist = unicode;
        info.title = reader.string();
        unicode = reader.string();
        if (!unicode.empty()) info.title = unicode;
        info.creator = reader.string();
        info.difficulty = reader.string();
        reader.string();  
        bool match = reader.string() == hash;
        reader.string();  
        reader.take(1);  
        info.circles = reader.number<std::uint16_t>();
        info.sliders = reader.number<std::uint16_t>();
        info.spinners = reader.number<std::uint16_t>();
        reader.take(8);
        auto difficulty_value = [&]() -> double {
            return version < 20140609 ? reader.number<std::uint8_t>() : reader.number<float>();
        };
        info.base.ar = difficulty_value();
        info.base.cs = difficulty_value();
        info.base.hp = difficulty_value();
        info.base.od = difficulty_value();
        info.slider_multiplier = reader.number<double>();
        if (version >= 20140609) {
            for (int mode = 0; mode < 4; ++mode) {
                int pairs = reader.count(100000);
                for (int i = 0; i < pairs; ++i) {
                    if (reader.number<std::uint8_t>() != 8) throw std::runtime_error("Invalid star-rating mod marker");
                    auto combination = reader.number<std::uint32_t>();
                    int marker = reader.number<std::uint8_t>();
                    if (marker != (version >= 20250107 ? 12 : 13)) throw std::runtime_error("Invalid star-rating value marker");
                    double stars = version >= 20250107 ? reader.number<float>() : reader.number<double>();
                     
                    if (mode != 0 || !std::isfinite(stars) || stars < 0) continue;
                    if (combination == 0) info.stars_base = stars;
                    if (star_mods(combination) == star_mods(mods)) info.stars_mods = stars;
                }
            }
        }
        info.drain_seconds = reader.number<std::int32_t>();
        info.length_ms = reader.number<std::int32_t>();
        reader.take(4);  
        int timings = reader.count(1000000);
        for (int i = 0; i < timings; ++i) {
             
            double beat_length = reader.number<double>();
            reader.take(8);  
            bool red_line = reader.number<std::uint8_t>() != 0;
            if (red_line && beat_length > 0 && std::isfinite(beat_length)) {
                double bpm = 60000 / beat_length;
                if (!std::isfinite(bpm)) continue;
                info.bpm_min = std::min(info.bpm_min.value_or(bpm), bpm);
                info.bpm_max = std::max(info.bpm_max.value_or(bpm), bpm);
            }
        }
        info.beatmap_id = reader.number<std::int32_t>();
        info.set_id = reader.number<std::int32_t>();
        reader.take(4 + 4 + 2 + 4);  
        info.mode = reader.number<std::uint8_t>();
        info.source = reader.string();
        reader.string();  
        reader.take(2);
        reader.string();  
        reader.take(1 + 8 + 1);
        reader.string();  
        reader.take(8 + 5);
        if (version < 20140609) reader.take(2);
        reader.take(4 + 1);
        if (match) {
            for (double value : {info.base.ar, info.base.od, info.base.cs, info.base.hp, info.slider_multiplier})
                if (!std::isfinite(value)) throw std::runtime_error("Non-finite cached beatmap difficulty");
            if (info.mode != 0) throw std::runtime_error("Cached beatmap is not osu!standard");
            if (info.length_ms < 0 || info.drain_seconds < 0) throw std::runtime_error("Invalid cached beatmap length");
            return info;
        }
    }
    return std::nullopt;
}

Difficulty modded_difficulty(Difficulty value, std::uint32_t mods) {
    if ((mods & 2) && (mods & 16)) throw std::runtime_error("Conflicting EZ and HR mods");
    if (mods & 2) { value.ar *= 0.5; value.od *= 0.5; value.cs *= 0.5; value.hp *= 0.5; }
    if (mods & 16) {
        value.ar = std::min(value.ar * 1.4, 10.0);
        value.od = std::min(value.od * 1.4, 10.0);
        value.cs = std::min(value.cs * 1.3, 10.0);
        value.hp = std::min(value.hp * 1.4, 10.0);
    }
    double rate = playback_rate(mods);
    double preempt = (value.ar <= 5 ? 1800 - 120 * value.ar : 1200 - 150 * (value.ar - 5)) / rate;
    value.ar = preempt >= 1200 ? (1800 - preempt) / 120 : 5 + (1200 - preempt) / 150;
    value.od = (80 - (80 - 6 * value.od) / rate) / 6;
    return value;
}

std::string mod_names(std::uint32_t mods) {
    if (!mods) return "NM";
    const char* names[] = {"NF", "EZ", "TD", "HD", "HR", "SD", "DT", "RX", "HT", "NC", "FL", "AT", "SO",
        "AP", "PF", "4K", "5K", "6K", "7K", "8K", "FI", "RD", "CN", "TP", "9K", "CO", "1K", "3K", "2K", "V2", "MR"};
    std::string result;
    for (unsigned i = 0; i < std::size(names); ++i) {
        if (!(mods & (1u << i)) || (i == 6 && (mods & 512)) || (i == 5 && (mods & 16384))) continue;
        if (!result.empty()) result += '+';
        result += names[i];
    }
    if (mods & 0x80000000) result += result.empty() ? "UNKNOWN" : "+UNKNOWN";
    return result;
}
}  
