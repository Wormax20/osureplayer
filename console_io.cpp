#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "console_io.h"

#include <cwctype>
#include <iostream>
#include <stdexcept>

namespace osu {
ConsoleSession::ConsoleSession() {
    output_cp_ = GetConsoleOutputCP();
    if (output_cp_) SetConsoleOutputCP(CP_UTF8);
}
ConsoleSession::~ConsoleSession() { if (output_cp_) SetConsoleOutputCP(output_cp_); }

std::optional<std::wstring> read_console_line() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) {
        std::wstring line;
        wchar_t buffer[1024];
        do {
            DWORD count = 0;
            if (!ReadConsoleW(input, buffer, static_cast<DWORD>(std::size(buffer)), &count, nullptr))
                throw std::runtime_error("Cannot read console input");
            if (!count) return std::nullopt;
            line.append(buffer, count);
            if (line.size() > 32768) throw std::runtime_error("Dropped path is too long");
        } while (line.find(L'\n') == line.npos);
        return line;
    }
     
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    if (line.empty()) return std::wstring{};
    if (line.size() > 32768) throw std::runtime_error("Dropped path is too long");
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0);
    if (!length) throw std::runtime_error("Invalid UTF-8 console input");
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), wide.data(), length);
    return wide;
}

std::filesystem::path dropped_replay_path(std::wstring text) {
    auto first = text.find_first_not_of(L" \t\r\n");
    if (first == text.npos) throw std::runtime_error("Drop one .osr file, then press Enter");
    text = text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
    if (text.front() == L'"' || text.front() == L'\'') {
        wchar_t quote = text.front();
        if (text.size() < 2 || text.back() != quote) throw std::runtime_error("Unclosed quoted path");
        text = text.substr(1, text.size() - 2);
        if (text.find(quote) != text.npos) throw std::runtime_error("Drop only one replay at a time");
    }
    if (text.empty() || text.find_first_of(L"\"\r\n") != text.npos || text.find(L'\0') != text.npos)
        throw std::runtime_error("Invalid replay path");
    std::filesystem::path path(text);
    auto extension = path.extension().wstring();
    for (auto& c : extension) c = static_cast<wchar_t>(std::towlower(c));
    if (extension != L".osr") throw std::runtime_error("Please drop an .osr replay file");
    return path;
}

}  
