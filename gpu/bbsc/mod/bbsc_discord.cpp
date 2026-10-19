// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC Discord status: "Playing BBSC - In Central Yharnam - Hosting (2 of 5)", or the boss during a
// boss fight. Talks to the Discord app's local IPC endpoint (Platform::DiscordIpcOpen) itself - no
// Discord library: a frame is {u32 opcode, u32 length, JSON}; opcode 0 = handshake with the
// application ID, opcode 1 = a command (SET_ACTIVITY). The game state comes from
// bloodborne_re.cpp's game tick (GetBbscPresence). Off with the Mods tab switch "Discord status"
// (SHADPS4_BBSC_DISCORD=0). No Discord running = nothing happens; it tries again every 20 s.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fmt/format.h>

#include "bbsc_platform.h"
#include "core/bloodborne_re.h"

namespace BbscMod {
void LogPlain(const char* text);
}

namespace {

namespace Platform = BbscMod::Platform;

constexpr const char* DiscordApplicationId = "1554935855674691645"; // "BBSC" (CLS_AMG)

bool DiscordStatusEnabled() {
    const char* value = std::getenv("SHADPS4_BBSC_DISCORD");
    return value == nullptr || (std::string{value} != "0" && std::string{value} != "false");
}

std::string Json(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out + '"';
}

class DiscordPipe {
public:
    ~DiscordPipe() {
        Close();
    }

    bool Connect() {
        for (int i = 0; i < 10 && pipe < 0; ++i) {
            pipe = Platform::DiscordIpcOpen(i);
        }
        if (pipe < 0) {
            return false;
        }
        if (!Send(0, fmt::format("{{\"v\":1,\"client_id\":\"{}\"}}", DiscordApplicationId))) {
            Close();
            return false;
        }
        return Drain(true);
    }

    bool Send(std::uint32_t opcode, const std::string& json) {
        if (pipe < 0) {
            return false;
        }
        std::string frame(8, '\0');
        const auto length = static_cast<std::uint32_t>(json.size());
        std::memcpy(frame.data(), &opcode, 4);
        std::memcpy(frame.data() + 4, &length, 4);
        frame += json;
        return Platform::DiscordIpcWrite(pipe, frame.data(), frame.size());
    }

    // Reads whatever Discord sent (answers, READY) so the pipe never fills. wait = block for one
    // frame (the handshake answer). False = the pipe is gone or Discord closed it (opcode 2).
    bool Drain(bool wait) {
        for (;;) {
            size_t available = 0;
            if (!Platform::DiscordIpcAvailable(pipe, available)) {
                Close();
                return false;
            }
            if (available < 8 && !wait) {
                return true;
            }
            std::uint32_t header[2]{};
            if (!Platform::DiscordIpcRead(pipe, header, 8)) {
                Close();
                return false;
            }
            std::string body(header[1], '\0');
            if (!Platform::DiscordIpcRead(pipe, body.data(), body.size())) {
                Close();
                return false;
            }
            if (header[0] == 2) { // CLOSE: wrong application ID or Discord shutting down
                BbscMod::LogPlain(("Discord status: Discord closed the connection: " + body).c_str());
                Close();
                return false;
            }
            wait = false;
        }
    }

    bool Connected() const {
        return pipe >= 0;
    }

    void Close() {
        if (pipe >= 0) {
            Platform::DiscordIpcClose(pipe);
            pipe = -1;
        }
    }

private:
    s64 pipe = -1;
};

std::string ActivityJson(const Core::Bloodborne::BbscPresence& p, std::int64_t game_start_s) {
    std::string details;
    std::string state;
    std::string large_text = "Bloodborne Seamless Co-op";
    std::int64_t start = game_start_s;
    if (!p.in_game) {
        details = "At the title screen";
    } else if (!p.boss.empty()) {
        details = "Boss fight: " + p.boss;
        large_text = "In " + p.area;
        start = p.boss_since_s;
    } else {
        details = "In " + p.area;
    }
    if (p.in_game) {
        state = p.role == 1 ? "Hosting" : p.role == 2 ? "Co-op guest" : "Solo hunter";
    }
    std::string activity = "{\"details\":" + Json(details);
    if (!state.empty()) {
        activity += ",\"state\":" + Json(state);
    }
    activity += fmt::format(",\"timestamps\":{{\"start\":{}}}", start);
    activity += ",\"assets\":{\"large_image\":\"bbsc\",\"large_text\":" + Json(large_text) + "}";
    if (p.in_game && p.role != 0) {
        activity += fmt::format(",\"party\":{{\"id\":\"bbsc-{}\",\"size\":[{},{}]}}",
                                Platform::ProcessId(), p.hunters, p.max_hunters);
    }
    activity += "}";
    return activity;
}

void DiscordThread() {
    const std::int64_t game_start_s = std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();
    DiscordPipe discord;
    std::string sent;
    std::uint64_t nonce = 0;
    auto next_try = std::chrono::steady_clock::now();
    bool logged_connect = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!discord.Connected()) {
            if (std::chrono::steady_clock::now() < next_try) {
                continue;
            }
            if (!discord.Connect()) {
                next_try = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                continue;
            }
            if (!logged_connect) {
                BbscMod::LogPlain("Discord status: connected to Discord");
                logged_connect = true;
            }
            sent.clear();
        }
        if (!discord.Drain(false)) {
            continue;
        }
        const std::string activity = ActivityJson(Core::Bloodborne::GetBbscPresence(), game_start_s);
        if (activity == sent) {
            continue;
        }
        const std::string command = fmt::format(
            "{{\"cmd\":\"SET_ACTIVITY\",\"args\":{{\"pid\":{},\"activity\":{}}},\"nonce\":\"{}\"}}",
            Platform::ProcessId(), activity, ++nonce);
        if (discord.Send(1, command)) {
            sent = activity;
        } else {
            discord.Close();
        }
    }
}

} // namespace

namespace BbscMod {

void StartDiscordStatus() {
    if (!DiscordStatusEnabled()) {
        LogPlain("Discord status: off (Mods tab)");
        return;
    }
    std::thread(DiscordThread).detach();
    LogPlain("Discord status: on - shows when the Discord app is running");
}

} // namespace BbscMod
