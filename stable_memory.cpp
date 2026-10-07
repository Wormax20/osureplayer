#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include "stable_memory.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace osu {
namespace {
 
 
const std::array<std::vector<int>, 5> patterns = {{
    {0xf8, 0x01, 0x74, 0x04, 0x83, 0x65},
    {0x48, 0x83, 0xf8, 0x04, 0x73, 0x1e},
    {0x5e, 0x5f, 0x5d, 0xc3, 0xa1, -1, -1, -1, -1, 0x89, -1, 0x04},
    {0xc8, 0xff, -1, -1, -1, -1, -1, 0x81, 0x0d, -1, -1, -1, -1, -1, 0x08, 0x00, 0x00},
     
     
    {0x8b, 0xfa, 0xb8, 0x01, 0x00, 0x00, 0x00,
     0xeb, 0x04, 0x8b, 0xfa, 0x33, 0xc0, 0x0b, 0xf8, 0x8b, 0xc7,
     0xa2, -1, -1, -1, -1,
     0x80, 0x3d, -1, -1, -1, -1, 0x00, 0x75, -1,
     0x80, 0x3d, -1, -1, -1, -1, 0x00, 0x74, -1,
     0x80, 0x3d, -1, -1, -1, -1, 0x00}
}};
constexpr std::size_t scan_chunk_size = 1024 * 1024;
const std::size_t scan_overlap = std::max_element(patterns.begin(), patterns.end(),
    [](const auto& left, const auto& right) { return left.size() < right.size(); })->size() - 1;
const char* names[] = {"base", "status", "song clock", "mods", "replay flag"};
bool valid_status(int status) {
    for (int known : {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 17, 18, 19, 22})
        if (status == known) return true;
    return false;
}
}

StableMemory::StableMemory() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate processes");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned count = 0;
    if (Process32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExeFile, L"osu!.exe") == 0) { pid_ = entry.th32ProcessID; ++count; }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (count != 1) throw std::runtime_error("Auto-sync requires exactly one running osu!.exe; start osu!stable first");
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid_);
    if (!process_) throw std::runtime_error("Cannot read osu! process; use the same privilege level");
    try {
        USHORT machine = 0, native = 0;
        if (!IsWow64Process2(process_, &machine, &native) ||
            (machine != IMAGE_FILE_MACHINE_I386 && !(machine == IMAGE_FILE_MACHINE_UNKNOWN && native == IMAGE_FILE_MACHINE_I386)))
            throw std::runtime_error("Auto-sync supports the 32-bit osu!stable client only");
        discover();
        read();
    } catch (...) {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

StableMemory::~StableMemory() { if (process_) CloseHandle(process_); }

void StableMemory::read_bytes(std::uintptr_t address, void* destination, std::size_t size) const {
    SIZE_T read_size = 0;
    if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(address), destination, size, &read_size) || read_size != size)
        throw std::runtime_error("osu! memory read failed; the client closed, changed version, or changed state");
}

void StableMemory::discover() {
    std::array<std::vector<std::uintptr_t>, 5> candidates;
    std::vector<std::uint8_t> buffer(scan_chunk_size + scan_overlap);
    std::uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION region{};
    while (address < 0xffff0000u && VirtualQueryEx(process_, reinterpret_cast<const void*>(address), &region, sizeof(region))) {
        auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        auto end = begin + region.RegionSize;
        if (end <= address) break;
         
        if (region.State == MEM_COMMIT && (region.Protect & 0xf0) && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            for (auto chunk = begin; chunk < end; chunk += scan_chunk_size) {
                SIZE_T size = 0;
                auto requested = std::min<std::size_t>(buffer.size(), end - chunk);
                if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(chunk), buffer.data(), requested, &size)) continue;
                for (std::size_t i = 0; i < patterns.size(); ++i)
                    for (auto offset : find_pattern(std::span(buffer.data(), size), patterns[i]))
                        candidates[i].push_back(chunk + offset);
            }
        }
        address = end;
    }
    std::array<std::uintptr_t*, 5> destinations = {&base_, &status_, &time_, &mods_, &replay_};
    const int offsets[] = {0, -4, 5, 9, 42};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        std::vector<std::uintptr_t> valid;
        for (auto match : candidates[i]) {
            try {
                auto location = i == 0 ? match : std::uintptr_t(value<std::uint32_t>(match + offsets[i]));
                if (i == 0) {
                    auto mode_address = value<std::uint32_t>(location - 51);
                    auto mode = value<int>(mode_address);
                    auto retries = value<int>(mode_address + 8);
                    if (mode < 0 || mode > 3 || retries < 0 || retries > 1000000) continue;
                    value<std::uint32_t>(value<std::uint32_t>(location - 12));
                } else if (i == 1 && !valid_status(value<int>(location))) continue;
                else if (i == 2 && std::abs(std::int64_t(value<int>(location))) > 86400000) continue;
                else if (i == 3 && (value<std::uint32_t>(location) & 0x80000000)) continue;
                else if (i == 4 && value<std::uint8_t>(location) > 1) continue;
                valid.push_back(location);
            } catch (const std::exception&) {   }
        }
        std::sort(valid.begin(), valid.end());
        valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
        if (valid.size() != 1)
            throw std::runtime_error(std::string("Cannot uniquely locate osu! ") + names[i] + " (" +
                std::to_string(valid.size()) + " valid matches). Open song selection and relaunch the player; client signatures may need updating");
        *destinations[i] = valid[0];
    }
}

GameState StableMemory::read() const {
    for (int attempt = 0; attempt < 3; ++attempt) {
        GameState result;
        result.status = value<int>(status_);
        result.time_ms = value<int>(time_);
        auto mode_address = value<std::uint32_t>(base_ - 51);
        result.mode = value<int>(mode_address);
        result.retries = value<int>(mode_address + 8);
        result.mods = value<std::uint32_t>(mods_);
        auto replay_flag = value<std::uint8_t>(replay_);
        result.watching_replay = replay_flag != 0;
        if (!valid_status(result.status) || result.mode < 0 || result.mode > 3 || replay_flag > 1 ||
            result.retries < 0 || result.retries > 1000000 || std::abs(std::int64_t(result.time_ms)) > 86400000)
            throw std::runtime_error("Invalid osu! game state; input has been stopped");
        auto beatmap = value<std::uint32_t>(value<std::uint32_t>(base_ - 12));
        if (beatmap) {
            auto text = value<std::uint32_t>(beatmap + 0x6c);
            if (text) {
                auto length = value<int>(text + 4);
                if (length != 0 && length != 32) throw std::runtime_error("Invalid beatmap MD5 in osu! memory");
                std::array<char16_t, 32> hash{};
                if (length) read_bytes(text + 8, hash.data(), hash.size() * sizeof(char16_t));
                for (int i = 0; i < length; ++i) {
                    char16_t c = hash[i];
                    if (c >= u'A' && c <= u'F') c += u'a' - u'A';
                    if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
                        throw std::runtime_error("Invalid beatmap hash characters in osu! memory");
                    result.beatmap_hash += static_cast<char>(c);
                }
            }
        }
         
        if (result.status != value<int>(status_) || beatmap != value<std::uint32_t>(value<std::uint32_t>(base_ - 12)))
            continue;
        return result;
    }
    throw std::runtime_error("Cannot obtain a consistent osu! game state; input has been stopped");
}
}  
