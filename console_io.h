#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace osu {

class ConsoleSession {
public:
    ConsoleSession();
    ~ConsoleSession();
private:
    unsigned output_cp_ = 0;
};
std::optional<std::wstring> read_console_line();
std::filesystem::path dropped_replay_path(std::wstring text);

}  
