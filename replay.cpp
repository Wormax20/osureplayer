#include "replay.h"
#include "third_party/lzma/LzmaDec.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace osu {
namespace {
constexpr std::size_t max_file = 64 * 1024 * 1024;
constexpr std::size_t max_decoded = 128 * 1024 * 1024;
constexpr std::int64_t max_time = 24 * 60 * 60 * 1000;

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > data_.size()) throw std::runtime_error("Truncated replay file");
        auto value = data_.first(count);
        data_ = data_.subspan(count);
        return value;
    }

    template<class T> T number() {
        using U = std::make_unsigned_t<T>;
        auto bytes = take(sizeof(T));
        U value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            value |= static_cast<U>(static_cast<U>(bytes[i]) << (8 * i));
        return std::bit_cast<T>(value);
    }

    std::string string() {
        auto marker = number<std::uint8_t>();
        if (marker == 0) return {};
        if (marker != 0x0b) throw std::runtime_error("Invalid osu! string marker");
        std::uint32_t length = 0;
        for (unsigned i = 0; i < 5; ++i) {
            auto byte = number<std::uint8_t>();
            if (i == 4 && (byte & 0xf0)) throw std::runtime_error("ULEB128 overflow");
            length |= std::uint32_t(byte & 0x7f) << (7 * i);
            if (!(byte & 0x80)) {
                if (length > 4 * 1024 * 1024) throw std::runtime_error("Replay string is too large");
                auto value = take(length);
                return {reinterpret_cast<const char*>(value.data()), value.size()};
            }
        }
        throw std::runtime_error("Invalid ULEB128");
    }
    std::size_t remaining() const { return data_.size(); }
private:
    std::span<const std::uint8_t> data_;
};

void* allocate(ISzAllocPtr, std::size_t size) { return std::malloc(size); }
void deallocate(ISzAllocPtr, void* ptr) { std::free(ptr); }
const ISzAlloc allocator = {allocate, deallocate};

struct Decoder {
    CLzmaDec state{};
    Decoder() { LzmaDec_Construct(&state); }
    ~Decoder() { LzmaDec_Free(&state, &allocator); }
};

std::string decompress(std::span<const std::uint8_t> data) {
    Reader reader(data);
    auto props = reader.take(5);
    auto expected = reader.number<std::uint64_t>();
    bool known_size = expected != std::numeric_limits<std::uint64_t>::max();
    if (known_size && expected > max_decoded) throw std::runtime_error("LZMA output exceeds 128 MiB");
    CLzmaProps decoded_props{};
    if (LzmaProps_Decode(&decoded_props, props.data(), 5) != SZ_OK || decoded_props.dicSize > max_decoded)
        throw std::runtime_error("Invalid or excessive LZMA dictionary");
    Decoder decoder;
    if (LzmaDec_Allocate(&decoder.state, props.data(), 5, &allocator) != SZ_OK)
        throw std::runtime_error("Cannot allocate LZMA decoder");
    LzmaDec_Init(&decoder.state);
    auto input = data.subspan(13);
    std::string output;
    std::array<Byte, 65536> buffer{};
    while (true) {
        SizeT in_size = input.size();
        SizeT out_size = std::min(buffer.size(), max_decoded - output.size());
        if (known_size) out_size = static_cast<SizeT>(std::min<std::uint64_t>(out_size, expected - output.size()));
        bool last = known_size ? output.size() + out_size == expected : output.size() + out_size == max_decoded;
        ELzmaStatus status{};
        auto result = LzmaDec_DecodeToBuf(&decoder.state, buffer.data(), &out_size,
            input.data(), &in_size, last ? LZMA_FINISH_END : LZMA_FINISH_ANY, &status);
        if (result != SZ_OK) throw std::runtime_error("Invalid or truncated LZMA replay stream");
        input = input.subspan(in_size);
        output.append(reinterpret_cast<const char*>(buffer.data()), out_size);
        if (status == LZMA_STATUS_FINISHED_WITH_MARK ||
            (known_size && output.size() == expected && status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)) {
            if (known_size && output.size() != expected) throw std::runtime_error("LZMA output length mismatch");
            return output;
        }
        if ((!in_size && !out_size) || output.size() == max_decoded)
            throw std::runtime_error("Incomplete or oversized LZMA replay stream");
    }
}

template<class T> T number(std::string_view text) {
    T value{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::runtime_error("Invalid replay frame number");
    return value;
}
}  

std::vector<Frame> parse_frames(std::string_view text, std::optional<std::int32_t>& seed) {
    std::vector<Frame> frames;
    seed.reset();
    std::int64_t time = 0;
    while (!text.empty()) {
        auto comma = text.find(',');
        auto token = text.substr(0, comma);
        text = comma == text.npos ? std::string_view{} : text.substr(comma + 1);
        if (token.empty()) throw std::runtime_error("Empty replay frame");
        if (seed) throw std::runtime_error("Replay data follows RNG seed");
        std::array<std::string_view, 4> fields;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            auto pipe = token.find('|');
            if ((i < 3) == (pipe == token.npos)) throw std::runtime_error("Replay frame must have four fields");
            fields[i] = token.substr(0, pipe);
            token = pipe == token.npos ? std::string_view{} : token.substr(pipe + 1);
        }
        auto delta = number<std::int64_t>(fields[0]);
        auto x = number<double>(fields[1]), y = number<double>(fields[2]);
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 100000 || std::abs(y) > 100000)
            throw std::runtime_error("Invalid replay cursor coordinate");
        if (delta == -12345) {
            seed = number<std::int32_t>(fields[3]);
            continue;
        }
        if (delta < -max_time || delta > max_time || time + delta < -max_time || time + delta > max_time)
            throw std::runtime_error("Replay time exceeds 24 hours");
        auto keys = number<std::uint32_t>(fields[3]);
        if (keys & ~31u) throw std::runtime_error("Unsupported standard replay key bits");
        time += delta;
        if (frames.size() >= 2000000) throw std::runtime_error("Replay has too many frames");
        frames.push_back({delta, time, x, y, keys});
    }
    if (frames.empty()) throw std::runtime_error("Replay has no frames");
    return frames;
}

Replay parse_replay(std::span<const std::uint8_t> data) {
    if (data.size() > max_file) throw std::runtime_error("Replay exceeds 64 MiB");
    Reader reader(data);
    Replay replay;
    replay.mode = reader.number<std::uint8_t>();
    if (replay.mode != 0) throw std::runtime_error("Only osu!standard replays are supported");
    replay.version = reader.number<std::int32_t>();
    if (replay.version <= 0 || replay.version >= 30000000)
        throw std::runtime_error("Only osu!stable replay versions are supported");
    replay.beatmap_hash = reader.string();
    replay.player = reader.string();
    replay.replay_hash = reader.string();
    for (auto& hit : replay.hits) hit = reader.number<std::uint16_t>();
    replay.score = reader.number<std::uint32_t>();
    replay.max_combo = reader.number<std::uint16_t>();
    auto perfect = reader.number<std::uint8_t>();
    if (perfect > 1) throw std::runtime_error("Invalid perfect flag");
    replay.perfect = perfect != 0;
    replay.mods = reader.number<std::uint32_t>();
    replay.life_graph = reader.string();
    replay.timestamp = reader.number<std::int64_t>();
    auto compressed_size = reader.number<std::int32_t>();
    if (compressed_size <= 0) throw std::runtime_error("Replay has no embedded frame data");
    replay.compressed_bytes = static_cast<std::size_t>(compressed_size);
    auto compressed = reader.take(replay.compressed_bytes);
    if (replay.version >= 20140721) replay.online_id = reader.number<std::int64_t>();
    else if (replay.version >= 20121008) replay.online_id = reader.number<std::int32_t>();
    if (replay.mods & (1u << 23)) {
        replay.target_accuracy = std::bit_cast<double>(reader.number<std::uint64_t>());
        if (!std::isfinite(*replay.target_accuracy)) throw std::runtime_error("Invalid target accuracy");
    }
    replay.trailing_bytes = reader.remaining();
    replay.frames = parse_frames(decompress(compressed), replay.seed);
    return replay;
}

Replay read_replay(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open replay file");
    auto length = file.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(max_file))
        throw std::runtime_error("Invalid replay file size (limit: 64 MiB)");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Cannot read complete replay file");
    return parse_replay(data);
}

double playback_rate(std::uint32_t mods) {
    bool fast = (mods & ((1u << 6) | (1u << 9))) != 0;
    bool slow = (mods & (1u << 8)) != 0;
    if (fast && slow) throw std::runtime_error("Conflicting DT/NC and HT mods");
    return fast ? 1.5 : slow ? 0.75 : 1.0;
}
}  
