// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC mod: everything the mod asks the operating system. bbsc_platform_win32.cpp answers it for
// bbsc.dll, bbsc_platform_posix.cpp for libbbsc.so (Linux hosts such as bbport). The rest of the
// mod is the same code on both.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "common/types.h"

namespace BbscMod::Platform {

// Protection bits, the same as the emulator's MemoryProt.
inline constexpr u32 ProtRead = 1;
inline constexpr u32 ProtWrite = 2;
inline constexpr u32 ProtExec = 4;

// A stretch of the address space: [start, end), mapped or not, with one protection (0 = no access).
struct Region {
    u64 start = 0;
    u64 end = 0;
    bool mapped = false;
    u32 prot = 0;
};

// The stretch holding `address`, as the OS sees it. False when the OS cannot say.
bool QueryRegion(u64 address, Region& out);

// The start of every executable mapping in the process, lowest first.
std::vector<u64> ExecutableRegions();

// Changes the protection of the pages holding [address, address + size). `restore` (when not null)
// receives what Restore needs to put the old protection back.
bool Protect(u64 address, u64 size, u32 prot, u32* restore);
void Restore(u64 address, u64 size, u32 restore);

// Makes code written at [address, address + size) visible to instruction fetch.
void FlushCode(u64 address, u64 size);

// Maps `size` bytes readable, writable and executable at exactly `address`. Returns `address`, or 0
// when that range is not free.
u64 MapExecutable(u64 address, u64 size);

// This DLL or shared library, and the program it was loaded into.
std::filesystem::path ModulePath();
std::filesystem::path ExecutablePath();
u32 ProcessId();

void SetEnvironment(const char* name, const char* value);

// Tells the player something went wrong before the log could say it (a message box on Windows).
void ShowError(const std::string& title, const std::string& text);

// shadPS4's user folder when it is not portable: %APPDATA%\shadPS4, or $XDG_DATA_HOME/shadPS4.
std::filesystem::path InstalledUserFolder();

// One HTTP POST with second-long timeouts. `headers` is "Name: value\r\n" lines. Returns the status
// code, 0 when there was no answer.
u32 HttpPost(const std::string& url, const std::string& body, const std::string& headers);

// The Discord app's local IPC endpoint number `index` (0-9). A handle, or -1 when it is not there.
s64 DiscordIpcOpen(int index);
bool DiscordIpcWrite(s64 handle, const void* data, size_t size);
bool DiscordIpcRead(s64 handle, void* data, size_t size); // blocks until all `size` bytes are read
bool DiscordIpcAvailable(s64 handle, size_t& bytes);      // false when the connection is gone
void DiscordIpcClose(s64 handle);

// The keys of the free camera and F8 unstuck.
enum class Key : u8 {
    F1,
    F8,
    W,
    A,
    S,
    D,
    E,
    Q,
    C,
    Z,
    X,
    V,
    Space,
    Shift,
    Alt,
    Comma,
    Period,
    Slash,
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    LeftBracket,
    RightBracket,
    Count
};

// Whether `key` is held while this process's window has the keyboard.
bool KeyDown(Key key);

// Mouse look for the free camera: the movement and wheel notches since the last call. While it is
// taken, the mouse belongs to the camera (cursor hidden, movement kept from the game where the
// platform allows it). `active` is false when the game window is not in front.
struct MouseLook {
    float dx = 0.0f;
    float dy = 0.0f;
    int wheel = 0;
    bool active = false;
};
MouseLook TakeMouseLook();

// Gives the mouse back to the game.
void ReleaseMouse();

} // namespace BbscMod::Platform
