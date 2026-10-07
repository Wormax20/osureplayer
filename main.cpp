#include <iostream>

#include "playback.h"
#include "beatmap.h"
#include "console_io.h"
#include <cmath>
#include <cwctype>
#include <stdexcept>

namespace {
void usage() {
    std::cout << "osu!stable standard replay player\n"
        << "  osureplayer                   Drop a replay into the console, then Enter\n"
        << "  osureplayer --interactive [options]\n"
        << "  osureplayer --help\n"
        << "  osureplayer play <file.osr> [options]\n\n"
        << "Options:\n"
        << "  --keys <ZX>               K1/K2 bindings: two distinct letters/digits\n"
        << "  --smoke-key <C>           Smoke binding (default: C)\n"
        << "  --rect <left> <top> <width> <height>   Playfield in client pixels\n\n"
        << "Auto mode waits for a new play with matching beatmap MD5; different mods only warn.\n"
        << "F8/Escape/Ctrl+C stops playback.\n";
}
std::string printable(std::string value) {
    for (char& c : value) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
    return value;
}
void inspect(const osu::Replay& replay) {
    std::cout << "Player: " << printable(replay.player)
        << "\nMods: " << osu::mod_names(replay.mods) << " | Score: " << replay.score << " | Combo: " << replay.max_combo << 'x'
        << "\n300/100/50/Miss: " << replay.hits[0] << '/' << replay.hits[1] << '/' << replay.hits[2] << '/' << replay.hits[5] << '\n';
}
double numeric(const std::wstring& value) {
    std::size_t end = 0;
    double result = std::stod(value, &end);
    if (end != value.size() || !std::isfinite(result) || std::abs(result) > 86400000)
        throw std::runtime_error("Invalid numeric argument");
    return result;
}
wchar_t binding(wchar_t value) {
    value = static_cast<wchar_t>(std::towupper(value));
    if (!(value >= L'A' && value <= L'Z') && !(value >= L'0' && value <= L'9'))
        throw std::runtime_error("Bindings must be letters A-Z or digits 0-9");
    return value;
}
osu::PlayOptions play_options(int argc, wchar_t* argv[], int start) {
    osu::PlayOptions options;
    for (int i = start; i < argc; ++i) {
        std::wstring option = argv[i];
        auto argument = [&]() -> std::wstring {
            if (++i >= argc) throw std::runtime_error("Missing option value");
            return argv[i];
        };
        if (option == L"--keys") {
            auto keys = argument();
            if (keys.size() != 2) throw std::runtime_error("--keys requires exactly two characters");
            options.key1 = binding(keys[0]); options.key2 = binding(keys[1]);
        } else if (option == L"--smoke-key") {
            auto key = argument();
            if (key.size() != 1) throw std::runtime_error("--smoke-key requires one character");
            options.smoke = binding(key[0]);
        } else if (option == L"--rect") {
            osu::Rect rect;
            rect.left = numeric(argument()); rect.top = numeric(argument());
            rect.width = numeric(argument()); rect.height = numeric(argument());
            if (rect.left < 0 || rect.top < 0 || rect.width <= 0 || rect.height <= 0)
                throw std::runtime_error("Invalid playfield rectangle");
            options.rect = rect;
        } else throw std::runtime_error("Unknown play option");
    }
    if (options.key1 == options.key2 || options.key1 == options.smoke || options.key2 == options.smoke)
        throw std::runtime_error("All three bindings must be distinct");
    return options;
}
void play(const osu::Replay& replay, const osu::PlayOptions& options) {
    auto schedule = osu::make_timeline(replay);
    inspect(replay);
    osu::play_windows(replay, schedule, options);
}
void interactive(osu::PlayOptions options) {
    std::cout << "Drop .osr and press Enter; q exits.\n";
    for (;;) {
        std::cout << "\nReplay (.osr)> " << std::flush;
        auto line = osu::read_console_line();
        if (!line) return;
        try {
            auto first = line->find_first_not_of(L" \t\r\n");
            if (first == line->npos) continue;
            auto text = line->substr(first, line->find_last_not_of(L" \t\r\n") - first + 1);
            if (text == L"q" || text == L"Q" || text == L"exit") return;
            auto path = osu::dropped_replay_path(text);
            play(osu::read_replay(path), options);
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << '\n';
        }
    }
}
}

int wmain(int argc, wchar_t* argv[]) {
    osu::ConsoleSession console;
    try {
        if (argc == 1 || std::wstring(argv[1]) == L"--interactive") {
            auto options = play_options(argc, argv, 2);
            interactive(options);
            return 0;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--help") { usage(); return 0; }
        if (std::wstring(argv[1]) != L"play") throw std::runtime_error("Unknown command");
        if (argc < 3) throw std::runtime_error("play expects a replay path");
        auto options = play_options(argc, argv, 3);
        play(osu::read_replay(argv[2]), options);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
