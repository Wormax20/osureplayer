#pragma once

#include "game_sync.h"

namespace osu {

class StableMemory {
public:
    StableMemory();
    ~StableMemory();
    StableMemory(const StableMemory&) = delete;
    StableMemory& operator=(const StableMemory&) = delete;
    std::uint32_t process_id() const { return pid_; }
    GameState read() const;
private:
    void read_bytes(std::uintptr_t address, void* destination, std::size_t size) const;
    template<class T> T value(std::uintptr_t address) const {
        T result{};
        read_bytes(address, &result, sizeof(result));
        return result;
    }
    void discover();
    void* process_ = nullptr;
    std::uint32_t pid_ = 0;
    std::uintptr_t base_ = 0, status_ = 0, time_ = 0, mods_ = 0, replay_ = 0;
};

}  
