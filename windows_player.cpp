#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>

#include "playback.h"
#include "stable_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace osu {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic_bool interrupted = false;
BOOL WINAPI console_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupted.store(true);
    return TRUE;
}
bool pressed(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }
bool stopped() { return interrupted.load() || pressed(VK_F8) || pressed(VK_ESCAPE); }

struct Runtime {
    bool timer = false;
    HANDLE poll_timer = nullptr;
    Runtime() {
        interrupted.store(false);
        if (!SetConsoleCtrlHandler(console_handler, TRUE)) throw std::runtime_error("Cannot install stop handler");
        timer = timeBeginPeriod(1) == TIMERR_NOERROR;
        poll_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    ~Runtime() {
        if (poll_timer) CloseHandle(poll_timer);
        if (timer) timeEndPeriod(1);
        SetConsoleCtrlHandler(console_handler, FALSE);
    }
    void wait(bool active) const {
        LARGE_INTEGER due;
        due.QuadPart = -5000;
        if (active && poll_timer && SetWaitableTimerEx(poll_timer, &due, 0, nullptr, nullptr, nullptr, 0) &&
            WaitForSingleObject(poll_timer, 100) == WAIT_OBJECT_0) return;
        Sleep(1);
    }
};

Rect client_rect(HWND window) {
    RECT rect{};
    POINT origin{};
    if (!IsWindow(window) || IsIconic(window) || !GetClientRect(window, &rect) ||
        !ClientToScreen(window, &origin) || rect.right <= 0 || rect.bottom <= 0)
        throw std::runtime_error("Target window is unavailable");
    return {double(origin.x), double(origin.y), double(rect.right), double(rect.bottom)};
}
bool same_rect(const Rect& a, const Rect& b) {
    return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}
class InputSink {
public:
    explicit InputSink(const PlayOptions& options) {
        const wchar_t keys[] = {options.key1, options.key2, options.smoke};
        for (std::size_t i = 0; i < scans_.size(); ++i) {
            scans_[i] = static_cast<WORD>(MapVirtualKeyW(keys[i], MAPVK_VK_TO_VSC));
            if (!scans_[i]) throw std::runtime_error("Cannot map keyboard binding");
        }
        desktop_ = {double(GetSystemMetrics(SM_XVIRTUALSCREEN)), double(GetSystemMetrics(SM_YVIRTUALSCREEN)),
            double(GetSystemMetrics(SM_CXVIRTUALSCREEN)), double(GetSystemMetrics(SM_CYVIRTUALSCREEN))};
        if (desktop_.width <= 1 || desktop_.height <= 1) throw std::runtime_error("Invalid desktop bounds");
    }
    ~InputSink() { release(); }

    void send(Point point, std::uint32_t keys, const Rect& client) {
         
        const double left = std::max(client.left, desktop_.left);
        const double top = std::max(client.top, desktop_.top);
        const double right = std::min(client.left + client.width - 1, desktop_.left + desktop_.width - 1);
        const double bottom = std::min(client.top + client.height - 1, desktop_.top + desktop_.height - 1);
        if (left > right || top > bottom) throw std::runtime_error("Target is outside the desktop");
        point.x = std::clamp(point.x, left, right);
        point.y = std::clamp(point.y, top, bottom);
        std::vector<INPUT> events(1);
        events[0].type = INPUT_MOUSE;
        events[0].mi.dx = static_cast<LONG>(std::lround((point.x - desktop_.left) * 65535 / (desktop_.width - 1)));
        events[0].mi.dy = static_cast<LONG>(std::lround((point.y - desktop_.top) * 65535 / (desktop_.height - 1)));
        events[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
        auto desired = button_state(keys);
        for (auto change : button_changes(held_, desired)) events.push_back(event_for(change));
         
        for (std::size_t i = 0; i < held_.size(); ++i) held_[i] = held_[i] || desired[i];
        if (SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT)) != events.size())
            throw std::runtime_error("SendInput failed; check that both applications have the same privilege level");
        held_ = desired;
    }

    bool release() noexcept {
        std::array<INPUT, 5> events{};
        UINT count = 0;
        for (std::size_t i = 0; i < held_.size(); ++i)
            if (held_[i]) events[count++] = event_for({static_cast<Button>(i), false});
        if (!count) return true;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (SendInput(count, events.data(), sizeof(INPUT)) == count) {
                held_ = {};
                return true;
            }
        }
        std::cerr << "Could not release all injected buttons; press and release your configured play keys and mouse buttons manually.\n";
        return false;
    }
private:
    INPUT event_for(Change change) const noexcept {
        INPUT event{};
        auto index = static_cast<std::size_t>(change.button);
        if (index < 2) {
            event.type = INPUT_MOUSE;
            event.mi.dwFlags = index == 0 ? (change.down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
                : (change.down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        } else {
            event.type = INPUT_KEYBOARD;
            event.ki.wScan = scans_[index - 2];
            event.ki.dwFlags = KEYEVENTF_SCANCODE | (change.down ? 0 : KEYEVENTF_KEYUP);
        }
        return event;
    }
    Buttons held_{};
    std::array<WORD, 3> scans_{};
    Rect desktop_;
};

Rect screen_playfield(HWND target, const PlayOptions& options, Rect& client) {
    client = client_rect(target);
    Rect field = options.rect.value_or(stable_playfield({0, 0, client.width, client.height}));
    if (field.left < 0 || field.top < 0 || field.width <= 0 || field.height <= 0 ||
        field.left + field.width > client.width || field.top + field.height > client.height)
        throw std::runtime_error("Playfield rectangle must be inside the target client area");
    field.left += client.left;
    field.top += client.top;
    return field;
}

void play_automatic(const Replay& replay, const Schedule& schedule, const PlayOptions& options, const Runtime& runtime) {
    StableMemory memory;
    AutoSync sync(replay.beatmap_hash, schedule.mods);
    std::cout << "Ready. Start/retry song; F8 / Esc stops.\n" << std::flush;
    InputSink input(options);
    HWND target = nullptr;
    Rect client, field;
    std::size_t next = 0;
    double last_time = -86400000;
    std::string last_wait_reason;
    std::string last_mods_warning;
    auto origin = Clock::now();
    while (!stopped()) {
        auto state = memory.read();
        auto wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - origin).count();
        auto action = sync.update(state, wall_ms);
        auto warning = sync.mods_warning(state.mods);
        if (warning != last_mods_warning) {
            if (!warning.empty()) std::cout << "Warning: " << warning << ".\n" << std::flush;
            last_mods_warning = std::move(warning);
        }
        if (!sync.active() && (state.mode != 0 || state.watching_replay || state.beatmap_hash != replay.beatmap_hash)) {
            auto reason = sync.blocking_reason(state);
            if (reason != last_wait_reason) {
                std::cout << "Waiting: " << reason << ".\n" << std::flush;
                last_wait_reason = std::move(reason);
            }
        } else last_wait_reason.clear();
        if (target && (GetForegroundWindow() != target || !same_rect(client, client_rect(target))))
            throw std::runtime_error("Playback stopped: target lost focus, moved, or resized");
        if (action == SyncAction::release) {
            if (!input.release()) throw std::runtime_error("Input cleanup failed");
            if (!sync.active() && !sync.awaiting_start()) {
                std::cout << "Stopped: gameplay/map/mode changed.\n";
                return;
            }
        }
        if (action == SyncAction::wait || action == SyncAction::release) { runtime.wait(sync.active()); continue; }
        if (action == SyncAction::start) {
            target = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(target, &pid);
            if (pid != memory.process_id()) throw std::runtime_error("osu! must be foreground when the song starts");
            for (int key : {int(options.key1), int(options.key2), int(options.smoke), VK_LBUTTON, VK_RBUTTON})
                if (pressed(key)) throw std::runtime_error("Release play keys/buttons when starting the song");
            field = screen_playfield(target, options, client);
            std::cout << "Playing.\n" << std::flush;
        }
        double song_time = state.time_ms;
        if (action == SyncAction::start || action == SyncAction::seek) {
            if (!input.release()) throw std::runtime_error("Input cleanup failed");
            auto first = std::lower_bound(schedule.frames.begin(), schedule.frames.end(), song_time,
                [](const Sample& frame, double time) { return frame.time_ms < time; });
            next = static_cast<std::size_t>(first - schedule.frames.begin());
            last_time = song_time;
        }
         
        song_time = std::max(song_time, last_time);
        last_time = song_time;
        while (next < schedule.frames.size() && schedule.frames[next].time_ms <= song_time) {
            if (stopped() || GetForegroundWindow() != target) break;
            const auto& frame = schedule.frames[next++];
            input.send(map_position(field, frame.x, frame.y), frame.keys, client);
        }
        if (stopped() || GetForegroundWindow() != target) break;
        if (next == schedule.frames.size()) {
            if (!input.release()) throw std::runtime_error("Input cleanup failed");
            std::cout << "Finished.\n";
            return;
        }
        if (next > 0) {
            auto frame = interpolate(schedule.frames[next - 1], schedule.frames[next], song_time);
            input.send(map_position(field, frame.x, frame.y), frame.keys, client);
        }
        runtime.wait(true);
    }
    if (!input.release()) throw std::runtime_error("Input cleanup failed");
    std::cout << "Stopped.\n";
}
}  

void play_windows(const Replay& replay, const Schedule& schedule, const PlayOptions& options) {
    if (schedule.frames.empty()) throw std::runtime_error("Empty playback schedule");
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        throw std::runtime_error("Cannot enable per-monitor DPI awareness");
    Runtime runtime;
    play_automatic(replay, schedule, options, runtime);
}
}  
