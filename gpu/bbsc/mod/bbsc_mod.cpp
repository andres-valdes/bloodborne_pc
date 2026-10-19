// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC as a mod: a DLL that bbsc-launch.exe injects into shadPS4.exe before it starts, or a shared
// library (libbbsc.so) that a Linux host such as bbport loads itself and starts with bbsc_attach
// (bbsc_host.h) before the game runs - the same steps below, without the waiting.
//
// The DLL runs in the emulator's process, where the game's memory sits at its guest addresses
// (eboot at 0x800000000 in the shadp2p fork; the official shadPS4 nightly of 2026-10-09 loads it
// at 0x9B00000 - FindEbootBase looks for it), so it needs nothing from the emulator:
//   1. wait until the eboot is in memory and every byte patch site reads back as the bytes that
//      must be there (all of them before writing any - another game or version is left alone);
//   2. write the byte patches (core/bbsc_patch_tables.h, the same list as the emulator build);
//   3. install every hook with the emulator build's own code, core/bloodborne_re.cpp, compiled
//      into this DLL with BBSC_MOD; its emulator calls are answered here and in bbsc_hooks.cpp
//      (hook trampolines) and bbsc_relay.cpp (BBSC's packets, through the game server);
//   4. keep watching: a patch site the emulator wrote back is written again.
// Session suspensions and the lamp role toggles come from bloodborne_re.cpp as in the emulator
// build (SetPersistentSessionPatches / SetHostLampPatches below).
//
// Log: logs\bbsc-mod-<date>-<time>-<pid>.log next to the DLL (one per game window, so two
// windows on one PC do not overwrite each other); capture files in captures\ next to it. A host
// whose mod folder is read-only (bbport's AppImage) points BBSC_DATA_DIR at a writable folder for
// these, the journals and the save backups; LICENSE.txt and the notices stay next to the mod.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#include "bbsc_host.h"
#include "bbsc_platform.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/bbsc_cinematic.h"
#include "core/bbsc_patch_tables.h"
#include "core/bloodborne_re.h"
#include "core/debugger.h"
#include "core/libraries/np/np_matching2/np_matching2_mm.h"
#include "core/memory.h"

using namespace Core::Bbsc::PatchTables;

// Public version (2026-10-11): 0.MINOR.PATCH while testing - PATCH for bug fixes, MINOR for new
// features, 1.0.0 the finished release. Keep it equal to "version" in mod.json.
#define BBSC_VERSION "0.0.1"

namespace BbscMod {
void LogPlain(const char* text);
u64 EbootAddress();
void StartRelay();
void StartP2PChannel(); // bbsc_channel.cpp
bool GameServerIsPublic(std::string* host_out); // bbsc_relay.cpp
void InstallHttpWrappers();
void StartDiscordStatus();
} // namespace BbscMod
namespace Core::Bbsc {
void InstallPadWrapper();
} // namespace Core::Bbsc

namespace {

namespace Platform = BbscMod::Platform;
using namespace std::chrono_literals;

constexpr u64 ForkEbootBase = 0x800000000ULL; // where the shadp2p fork puts it
u64 EbootBase = 0; // found by FindEbootBase before anything else uses it
constexpr u64 EbootSize = 0x056D4000ULL; // CUSA03173 01.09 image size
constexpr u64 WaitForImageMs = 180'000;
constexpr auto SettleTime = 20ms;         // let the emulator finish its own load-time patching
constexpr auto WatchTime = 500ms;

std::filesystem::path mod_dir;
std::filesystem::path data_dir; // BBSC_DATA_DIR, else mod_dir
std::ofstream log_file;
std::mutex log_mutex;
// The host's own view of memory (bbsc_attach), asked before the OS. Null: the OS only.
int (*host_query_memory)(uint64_t address, uint64_t* start, uint64_t* end,
                         uint32_t* prot) = nullptr;

u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

void OpenLog() {
    mod_dir = Platform::ModulePath().parent_path();
    const char* data = std::getenv("BBSC_DATA_DIR");
    data_dir = data != nullptr && data[0] != '\0' ? std::filesystem::path{data} : mod_dir;
    const std::time_t now = std::time(nullptr);
    char name[96]{};
    std::strftime(name, sizeof(name), "bbsc-mod-%Y%m%d-%H%M%S", std::localtime(&now));
    const std::string file =
        std::string{name} + "-" + std::to_string(Platform::ProcessId()) + ".log";
    std::error_code ignored;
    std::filesystem::create_directories(data_dir / "logs", ignored);
    log_file.open(data_dir / "logs" / file);
}

// ---- Byte patches --------------------------------------------------------------------------------

// Asking the OS (VirtualQuery) is a kernel call that shares the address-space lock with the
// emulator's constant page-protection changes (GPU memory tracking). bloodborne_re.cpp checks every
// memory read, so thousands of these per frame took the game to ~1 fps (2026-09-30, two testers).
// The emulator build answers the same checks from its own memory map; here each game thread
// remembers the last few regions it asked about for a moment.
struct CachedRegion {
    Platform::Region region;
    u64 stamp_ms = 0;
};
constexpr u64 RegionCacheMs = 250;
constexpr size_t RegionCacheSize = 16;

bool CachedQueryRegion(u64 address, Platform::Region& out) {
    if (host_query_memory != nullptr &&
        host_query_memory(address, &out.start, &out.end, &out.prot) != 0) {
        out.mapped = true;
        return true;
    }
    thread_local std::array<CachedRegion, RegionCacheSize> cache{};
    thread_local size_t next = 0;
    const u64 now = NowMs();
    for (const CachedRegion& cached : cache) {
        if (address >= cached.region.start && address < cached.region.end &&
            now - cached.stamp_ms <= RegionCacheMs) {
            out = cached.region;
            return true;
        }
    }
    if (!Platform::QueryRegion(address, out)) {
        return false;
    }
    cache[next] = {out, now};
    next = (next + 1) % RegionCacheSize;
    return true;
}

bool Readable(u64 address, size_t size) {
    u64 at = address;
    const u64 end = address + size;
    while (at < end) {
        Platform::Region region;
        if (!CachedQueryRegion(at, region) || !region.mapped || region.prot == 0) {
            return false;
        }
        at = region.end;
    }
    return true;
}

bool MatchesAt(u64 base, u64 offset, const u8* bytes, size_t size) {
    const u64 address = base + offset;
    return Readable(address, size) &&
           std::memcmp(reinterpret_cast<const void*>(address), bytes, size) == 0;
}

bool Matches(u64 offset, const u8* bytes, size_t size) {
    const u64 address = EbootBase + offset;
    return Readable(address, size) &&
           std::memcmp(reinterpret_cast<const void*>(address), bytes, size) == 0;
}

void Write(u64 offset, const u8* bytes, size_t size) {
    const u64 address = EbootBase + offset;
    u32 restore = 0;
    if (!Platform::Protect(address, size,
                           Platform::ProtRead | Platform::ProtWrite | Platform::ProtExec,
                           &restore)) {
        return;
    }
    std::memcpy(reinterpret_cast<void*>(address), bytes, size);
    Platform::Restore(address, size, restore);
    Platform::FlushCode(address, size);
}

struct Site {
    const char* name;
    u64 offset;
    std::array<u8, 6> original{};
    std::array<u8, 6> patched{};
    size_t size;
    Group group;
};

template <size_t N>
std::array<u8, 6> Pad(const std::array<u8, N>& bytes) {
    std::array<u8, 6> out{};
    std::memcpy(out.data(), bytes.data(), N);
    return out;
}

constexpr size_t SiteCount =
    Retargets.size() + EntryReturns.size() + ByteEdits.size() + WideEdits.size() + EntryJumps.size();
std::array<Site, SiteCount> sites{};
std::mutex patch_mutex;
bool patches_ready = false;
bool session_on = true; // session groups: off while a suspension runs
bool role_on = false;   // role groups (lamp): on while hosting

void BuildSites() {
    size_t i = 0;
    for (const auto& e : Retargets) {
        sites[i++] = {e.name, e.site, Pad(EncodeRel32(e.opcode, e.site, e.original)),
                      Pad(EncodeRel32(e.opcode, e.site, e.target)), 5, e.group};
    }
    for (const auto& e : EntryReturns) {
        std::array<u8, 6> ret{};
        ret[0] = 0xC3;
        sites[i++] = {e.name, e.site, e.prologue, ret, 1, e.group};
    }
    for (const auto& e : ByteEdits) {
        sites[i++] = {e.name, e.site, Pad(e.original), Pad(e.replacement), 2, e.group};
    }
    for (const auto& e : WideEdits) {
        sites[i++] = {e.name, e.site, Pad(e.original), Pad(e.replacement), 4, e.group};
    }
    for (const auto& e : EntryJumps) {
        sites[i++] = {e.name, e.site, Pad(e.prologue), Pad(EncodeRel32(0xE9, e.site, e.target)), 5,
                      e.group};
    }
}

// The game image: where the fork puts it, else the start of any executable region whose first few
// patch sites read as Bloodborne's (original or patched). AllSitesKnown then checks every site.
u64 FindEbootBase() {
    const auto fits = [](u64 base) {
        int checked = 0;
        for (const Site& s : sites) {
            if (s.offset + s.size > EbootSize) {
                continue;
            }
            if (!MatchesAt(base, s.offset, s.original.data(), s.size) &&
                !MatchesAt(base, s.offset, s.patched.data(), s.size)) {
                return false;
            }
            if (++checked == 4) {
                return true;
            }
        }
        return checked != 0;
    };
    if (fits(ForkEbootBase)) {
        return ForkEbootBase;
    }
    for (const u64 base : Platform::ExecutableRegions()) {
        if (fits(base)) {
            return base;
        }
    }
    return 0;
}

bool AllSitesKnown(const Site** first_bad) {
    for (const Site& s : sites) {
        if (!GroupEnabled(s.group)) {
            continue;
        }
        if (s.offset + s.size > EbootSize ||
            (!Matches(s.offset, s.original.data(), s.size) &&
             !Matches(s.offset, s.patched.data(), s.size))) {
            *first_bad = &s;
            return false;
        }
    }
    return true;
}

// Whether a site should be patched right now.
bool Wanted(const Site& s) {
    if (!GroupEnabled(s.group)) {
        return false;
    }
    if (IsRoleGroup(s.group)) {
        return role_on;
    }
    if (IsSessionGroup(s.group)) {
        return session_on;
    }
    return true;
}

// Brings every enabled site to its wanted form. Returns how many were written.
int SyncSites() {
    int count = 0;
    for (const Site& s : sites) {
        if (!GroupEnabled(s.group)) {
            continue;
        }
        const auto& want = Wanted(s) ? s.patched : s.original;
        if (!Matches(s.offset, want.data(), s.size)) {
            Write(s.offset, want.data(), s.size);
            ++count;
        }
    }
    return count;
}

// ---- Start-up ------------------------------------------------------------------------------------

// LICENSE.txt (the GPL) and NOTICE.txt (copyright + source link) ship in the mod folder so a copy
// passed on keeps them. Missing or emptied -> BBSC stays off and says why.
bool NoticesPresent() {
    auto holds = [](const char* name, const char* text) {
        std::ifstream file(mod_dir / name, std::ios::binary);
        std::string content(64 * 1024, '\0');
        file.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<size_t>(file.gcount()));
        return content.find(text) != std::string::npos;
    };
    // SUPPORTERS.txt (2026-10-11, the user): the supporters list travels with BBSC like the
    // license; its names change freely, only the file and its header are required.
    return holds("LICENSE.txt", "GNU GENERAL PUBLIC LICENSE") &&
           holds("NOTICE.txt", "github.com/Classmg22/BBSC") &&
           holds("SUPPORTERS.txt", "BBSC SUPPORTERS");
}

// The log, the license files and the public-server rule: what both start-ups do before they look
// at the game. False = BBSC stays off.
bool Prepare() {
    OpenLog();
    BbscMod::LogPlain("BBSC mod loaded - version " BBSC_VERSION);
    if (!NoticesPresent()) {
        LOG_ERROR(Mod, "LICENSE.txt / NOTICE.txt / SUPPORTERS.txt missing or changed in {} - BBSC "
                       "is off",
                  Common::FS::PathToUTF8String(mod_dir));
        Platform::ShowError("BBSC", "LICENSE.txt, NOTICE.txt or SUPPORTERS.txt is missing from the "
                                    "BBSC mod folder (or was changed), so BBSC is off.\n\nThese "
                                    "files have to travel with BBSC.\nDownload the full package "
                                    "again from:\nhttps://github.com/Classmg22/BBSC");
        return false;
    }
    LOG_INFO(Mod, "BBSC mod in pid {} ({})", Platform::ProcessId(),
             Common::FS::PathToUTF8String(Platform::ExecutablePath()));
    // Public servers (2026-10-11, shadNet's author: The Hunter's Dream is exact to the official
    // server, custom summon behaviour does not work there). Wozzardman's seamless summon patches
    // (cross-map sign search, host-placement summon build) and the 5-hunter search patches change
    // what the game asks the summon server for, so they are off there whatever the switches say
    // (review 2026-10-11: the Mods tab writes its defaults, BBSC_MOD_SEAMLESS=1 among them, at
    // every start, so "a switch the player set wins" would have kept them on).
    {
        std::string server;
        if (BbscMod::GameServerIsPublic(&server)) {
            Platform::SetEnvironment("BBSC_MOD_SEAMLESS", "0");
            Platform::SetEnvironment("SHADPS4_BBSC_FIVE_HUNTERS", "0");
            LOG_INFO(Mod, "Public game server ({}): vanilla summons - seamless summon patches and the "
                          "5-hunter search off (the session itself stays seamless)",
                     server);
        }
    }
    BuildSites();
    return true;
}

// shadPS4 maps the game's code writable; other hosts (bbhost) keep it read-only, and the shared
// hook code writes some code bytes directly. Make the image's code writable like shadPS4.
void UnlockCode() {
    int unlocked = 0;
    Platform::Region region;
    u64 at = EbootBase;
    while (at < EbootBase + EbootSize && Platform::QueryRegion(at, region)) {
        if (region.mapped && (region.prot & Platform::ProtExec) != 0 &&
            (region.prot & Platform::ProtWrite) == 0) {
            const u64 start = std::max(region.start, EbootBase);
            const u64 end = std::min<u64>(region.end, EbootBase + EbootSize);
            if (Platform::Protect(start, end - start,
                                  Platform::ProtRead | Platform::ProtWrite | Platform::ProtExec,
                                  nullptr)) {
                ++unlocked;
            }
        }
        at = region.end;
    }
    if (unlocked != 0) {
        LOG_INFO(Mod, "Game code made writable ({} region(s)) - this host maps it read-only",
                 unlocked);
    }
}

// The patches, the hooks and BBSC's own services, once every site of the image is known.
void Install(u64 started, u64 verified) {
    MemoryPatcher::g_eboot_address = EbootBase;
    MemoryPatcher::g_eboot_image_size = EbootSize;
    MemoryPatcher::g_game_serial = "CUSA03173";

    // Wozzardman's seamless patches first: one of them (0x019471B1) is also in BBSC's list, and it
    // checks for the original bytes.
    Core::Bloodborne::InstallSeamlessCoopPatches();

    int written = 0;
    {
        std::scoped_lock lock{patch_mutex};
        patches_ready = true;
        written = SyncSites();
    }
    LOG_INFO(Mod, "Image verified after {} ms: {} byte patch sites written", verified, written);

    Core::Bloodborne::InstallReverseEngineeringTrace();
    BbscMod::StartRelay();
    BbscMod::StartP2PChannel();
    BbscMod::InstallHttpWrappers();
    Core::Bbsc::InstallPadWrapper();
    BbscMod::StartDiscordStatus();
    LOG_INFO(Mod, "BBSC mod ready after {} ms", NowMs() - started);
}

// A patch site the host wrote back is written again.
void WatchSites() {
    for (;;) {
        std::this_thread::sleep_for(WatchTime);
        std::scoped_lock lock{patch_mutex};
        if (const int redone = SyncSites(); redone != 0) {
            LOG_WARNING(Mod, "{} patch site(s) had changed back - written again", redone);
        }
    }
}

// The injected DLL: the emulator is still loading the game, so wait for it.
void Worker() {
    if (!Prepare()) {
        return;
    }
    const u64 started = NowMs();
    while ((EbootBase = FindEbootBase()) == 0) {
        if (NowMs() - started > WaitForImageMs) {
            LOG_ERROR(Mod, "No Bloodborne CUSA03173 01.09 image in memory after {} s - not this "
                           "game or not this version; nothing was changed",
                      WaitForImageMs / 1000);
            return;
        }
        std::this_thread::sleep_for(50ms);
    }
    LOG_INFO(Mod, "Game image found at {:#x}{}", EbootBase,
             EbootBase == ForkEbootBase ? "" : " (not the shadp2p fork's address)");
    UnlockCode();
    const Site* bad = nullptr;
    while (!AllSitesKnown(&bad)) {
        if (NowMs() - started > WaitForImageMs) {
            LOG_ERROR(Mod,
                      "No Bloodborne CUSA03173 01.09 image after {} s (first unknown site: {} at "
                      "{:#x}) - not this game or not this version; nothing was changed",
                      WaitForImageMs / 1000, bad ? bad->name : "?", bad ? bad->offset : 0);
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(SettleTime);
    Install(started, NowMs() - started);
    WatchSites();
}

} // namespace

#ifndef _WIN32
// A host that loads the mod itself, before the game runs (bbsc_host.h): the image is complete, so
// it is checked once, and every site before anything is changed.
extern "C" __attribute__((visibility("default"))) int bbsc_attach(const BbscHost* host) {
    static std::atomic<bool> attached{false};
    if (host == nullptr || attached.exchange(true) || !Prepare()) {
        return -1;
    }
    if (host->version != BBSC_HOST_VERSION) {
        LOG_ERROR(Mod, "The host speaks BBSC host interface {}, this BBSC {} - BBSC is off",
                  host->version, BBSC_HOST_VERSION);
        return -1;
    }
    host_query_memory = host->query_memory;
    const u64 started = NowMs();
    EbootBase = host->image_base != 0 ? host->image_base : FindEbootBase();
    const Site* bad = nullptr;
    if (EbootBase == 0 || !AllSitesKnown(&bad)) {
        LOG_ERROR(Mod, "No Bloodborne CUSA03173 01.09 image at {:#x} (first unknown site: {} at "
                       "{:#x}) - not this game or not this version; nothing was changed",
                  EbootBase, bad ? bad->name : "?", bad ? bad->offset : 0);
        EbootBase = 0;
        return -1;
    }
    LOG_INFO(Mod, "Game image at {:#x}, given by the host", EbootBase);
    UnlockCode();
    Install(started, NowMs() - started);
    std::thread(WatchSites).detach();
    return 0;
}
#endif

// ---- Logging -------------------------------------------------------------------------------------

namespace BbscMod {

void LogLine(char level, const char* category, const std::string& text) {
    std::scoped_lock lock{log_mutex};
    if (!log_file.is_open()) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count() % 1000;
    char clock[16]{};
    std::strftime(clock, sizeof(clock), "%H:%M:%S", std::localtime(&seconds));
    log_file << fmt::format("[{}.{:03}] {} {}: {}\n", clock, millis, level, category, text)
             << std::flush;
}

void LogPlain(const char* text) {
    LogLine('I', "Mod", text);
}

u64 EbootAddress() {
    return EbootBase;
}

// The injected DLL's start-up (bbsc_platform_win32.cpp, DllMain): waits for the game, then
// installs.
void RunWorker() {
    Worker();
}

// Writes an import slot of the game (an aligned 8-byte store, so a game thread reading the slot
// sees the old or the new value, never half of each).
void WriteSlot(u64 address, u64 value) {
    u32 restore = 0;
    const bool unlocked =
        Platform::Protect(address, 8, Platform::ProtRead | Platform::ProtWrite, &restore);
    *reinterpret_cast<volatile u64*>(address) = value;
    if (unlocked) {
        Platform::Restore(address, 8, restore);
    }
}

// Import slots the DLL swapped for its wrappers (bbsc_http.cpp, bbsc_channel.cpp). The official
// shadPS4 links the eboot's imports again when the game loads a library later (2026-10-10: no
// wrapper ever ran on the nightly - the slots were written back to the emulator's functions), so a
// thread puts the wrapper back whenever a slot no longer points at it, taking the new value as the
// original.
namespace {
struct GuardedSlot {
    u64 slot_offset;
    u64 wrapper;
    void** original; // the wrapper's function pointer (an aligned 8-byte store)
    const char* name;
    int rewritten = 0;
};
std::mutex guarded_mutex;
std::vector<GuardedSlot> guarded_slots;

void SlotGuardThread() {
    for (;;) {
        std::this_thread::sleep_for(5ms);
        std::scoped_lock lock{guarded_mutex};
        for (auto& guarded : guarded_slots) {
            auto* slot = reinterpret_cast<volatile u64*>(EbootBase + guarded.slot_offset);
            const u64 value = *slot;
            if (value == guarded.wrapper || value == 0 ||
                (value >= EbootBase && value < EbootBase + EbootSize)) {
                continue;
            }
            *reinterpret_cast<void* volatile*>(guarded.original) = reinterpret_cast<void*>(value);
            WriteSlot(EbootBase + guarded.slot_offset, guarded.wrapper);
            if (++guarded.rewritten <= 3) {
                LOG_INFO(Mod, "Import {} was linked again by the emulator - wrapper put back ({}x)",
                         guarded.name, guarded.rewritten);
            }
        }
    }
}
} // namespace

void GuardSlot(u64 slot_offset, u64 wrapper, void** original, const char* name) {
    std::scoped_lock lock{guarded_mutex};
    if (guarded_slots.empty()) {
        std::thread(SlotGuardThread).detach();
    }
    guarded_slots.push_back({slot_offset, wrapper, original, name});
}

} // namespace BbscMod

// ---- What bloodborne_re.cpp asks the emulator for ----------------------------------------------

namespace Core {

bool MemoryManager::IsValidMapping(u64 address, u64 size) {
    return Readable(address, size == 0 ? 1 : size);
}

s32 MemoryManager::QueryProtection(u64 address, void** start, void** end, u32* prot) {
    Platform::Region region;
    if (!CachedQueryRegion(address, region) || !region.mapped) {
        return -1;
    }
    *start = reinterpret_cast<void*>(region.start);
    *end = reinterpret_cast<void*>(region.end);
    *prot = region.prot;
    return 0;
}

namespace Debugger {
int GetCurrentPid() {
    return static_cast<int>(Platform::ProcessId());
}
} // namespace Debugger

namespace Bbsc {

void InstallPersistentSessionPatches() {} // the DLL applies them itself (Worker)

void SetPersistentSessionPatches(bool enabled) {
    std::scoped_lock lock{patch_mutex};
    if (!patches_ready || session_on == enabled) {
        return;
    }
    session_on = enabled;
    const int count = SyncSites();
    LOG_INFO(Debug, "BBSC persistence {} ({} session sites; echoes, lamp, fog walls and deaths "
                    "untouched)",
             enabled ? "re-applied" : "suspended", count);
}

void SetHostLampPatches(bool hosting) {
    std::scoped_lock lock{patch_mutex};
    if (!patches_ready || role_on == hosting) {
        return;
    }
    role_on = hosting;
    const int count = SyncSites();
    LOG_INFO(Debug, "BBSC lamp: {} ({} sites)",
             hosting ? "prompt allowed (hosting a session)"
                     : "vanilla gates (solo or guest - a guest never gets the lamp)",
             count);
}

// The F1 camera. The emulator build feeds it from its SDL window events (bbsc_cinematic.cpp); a mod
// sees no window events, so the keys are polled here instead (Platform::KeyDown), only while this
// process's window has the keyboard (Nucleus runs two). bloodborne_re.cpp turns it on by itself
// when you die in co-op and off when you respawn. The mouse looks around without a button held and
// the wheel sets the speed (Platform::TakeMouseLook; PageUp / PageDown still work).
using Platform::Key;
std::mutex cine_mutex;
CinematicInput cine_state;
std::array<bool, static_cast<size_t>(Key::Count)> cine_was_down{};

// The controller: the game's scePadReadState import slot (0x053E4EA0, tools/gamedata/
// eboot_imports.py) is swapped for PadReadState below, which sees the pad data the emulator hands
// the game - any controller the emulator supports. L3 + R3 together toggles the camera; while it is
// on the sticks and buttons fly it and the game gets a pad at rest (the hunter does not move).
constexpr u64 PadReadStateSlot = 0x053E4EA0;
using PadReadStateFn = int PS4_SYSV_ABI (*)(s32 handle, u8* data);
PadReadStateFn original_pad_read = nullptr;
constexpr u32 PadL3 = 0x2, PadR3 = 0x4, PadUp = 0x10, PadRight = 0x20, PadDown = 0x40,
              PadLeft = 0x80, PadL1 = 0x400, PadR1 = 0x800, PadTriangle = 0x1000,
              PadCross = 0x4000, PadSquare = 0x8000;
struct CinePad {
    u32 buttons = 0;
    u32 last_buttons = 0;
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f, l2 = 0.0f, r2 = 0.0f;
    int speed_steps = 0, smooth_steps = 0;
    bool zoom_reset = false;
};
CinePad cine_pad;

float PadAxis(u8 value) {
    const float v = (static_cast<float>(value) - 128.0f) / 127.0f;
    constexpr float Dead = 0.15f;
    return std::abs(v) < Dead ? 0.0f : std::clamp((v - std::copysign(Dead, v)) / (1.0f - Dead), -1.0f, 1.0f);
}

// OrbisPadData: +0 buttons (u32), +4 left stick x/y, +6 right stick x/y, +8 L2, +9 R2 (u8).
int PS4_SYSV_ABI PadReadState(s32 handle, u8* data) {
    const int result = original_pad_read(handle, data);
    if (result != 0 || data == nullptr) {
        return result;
    }
    u32 buttons = 0;
    std::memcpy(&buttons, data, 4);
    std::scoped_lock lock{cine_mutex};
    const u32 pressed = buttons & ~cine_pad.last_buttons;
    cine_pad.last_buttons = buttons;
    if ((buttons & (PadL3 | PadR3)) == (PadL3 | PadR3) && (pressed & (PadL3 | PadR3)) != 0) {
        const bool enabled = !cine_state.enabled;
        cine_state = CinematicInput{};
        cine_state.enabled = enabled;
        LOG_INFO(Debug, "BBSC cinematic camera: {} (L3 + R3)", enabled ? "on" : "off");
    }
    if (!cine_state.enabled) {
        cine_pad = CinePad{.last_buttons = buttons};
        return result;
    }
    cine_pad.buttons = buttons;
    cine_pad.lx = PadAxis(data[4]);
    cine_pad.ly = PadAxis(data[5]);
    cine_pad.rx = PadAxis(data[6]);
    cine_pad.ry = PadAxis(data[7]);
    cine_pad.l2 = static_cast<float>(data[8]) / 255.0f;
    cine_pad.r2 = static_cast<float>(data[9]) / 255.0f;
    cine_pad.speed_steps += ((pressed & PadUp) ? 1 : 0) - ((pressed & PadDown) ? 1 : 0);
    cine_pad.smooth_steps += ((pressed & PadRight) ? 1 : 0) - ((pressed & PadLeft) ? 1 : 0);
    cine_pad.zoom_reset = cine_pad.zoom_reset || (pressed & PadTriangle) != 0;
    // The game sees a controller at rest (connected, nothing pressed).
    const u32 rest_buttons = buttons & 0x80000000u;
    std::memcpy(data, &rest_buttons, 4);
    data[4] = data[5] = data[6] = data[7] = 128;
    data[8] = data[9] = 0;
    return result;
}

void InstallPadWrapper() {
    const u64 slot = EbootBase + PadReadStateSlot;
    for (int i = 0; i < 60'000; ++i) {
        const u64 value = *reinterpret_cast<const volatile u64*>(slot);
        if (value != 0 && (value < EbootBase || value >= EbootBase + EbootSize)) {
            original_pad_read = reinterpret_cast<PadReadStateFn>(value);
            BbscMod::WriteSlot(slot, reinterpret_cast<u64>(&PadReadState));
            LOG_INFO(Mod, "Pad: controller works the F1 camera too (L3 + R3)");
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    LOG_WARNING(Mod, "Pad: scePadReadState slot never filled - F1 camera keyboard only");
}

bool CineKeyPressed(Key key, bool down) {
    bool& was_down = cine_was_down[static_cast<size_t>(key)];
    const bool pressed = down && !was_down;
    was_down = down;
    return pressed;
}

CinematicInput TakeCinematicInput() {
    std::scoped_lock lock{cine_mutex};
    const auto down = [](Key key) { return Platform::KeyDown(key); };
    if (CineKeyPressed(Key::F1, down(Key::F1))) {
        const bool enabled = !cine_state.enabled;
        cine_state = CinematicInput{};
        cine_state.enabled = enabled;
        LOG_INFO(Debug, "BBSC cinematic camera: {} (F1)", enabled ? "on" : "off");
    }
    if (!cine_state.enabled) {
        Platform::ReleaseMouse();
        return {};
    }
    CinematicInput& c = cine_state;
    const CinePad& p = cine_pad;
    const Platform::MouseLook mouse = Platform::TakeMouseLook();
    c.forward = down(Key::W);
    c.back = down(Key::S);
    c.left = down(Key::A);
    c.right = down(Key::D);
    c.up = down(Key::E) || down(Key::Space);
    c.down = down(Key::Q) || down(Key::C);
    c.fast = down(Key::Shift) || (p.buttons & PadCross) != 0;
    c.slow = down(Key::Alt) || (p.buttons & PadSquare) != 0;
    c.zoom_in = down(Key::Z);
    c.zoom_out = down(Key::X);
    c.zoom_reset = CineKeyPressed(Key::V, down(Key::V)) || p.zoom_reset;
    cine_pad.zoom_reset = false;
    c.roll_left = down(Key::Comma);
    c.roll_right = down(Key::Period);
    c.roll_reset = CineKeyPressed(Key::Slash, down(Key::Slash));
    c.look_up = down(Key::Up);
    c.look_down = down(Key::Down);
    c.look_left = down(Key::Left);
    c.look_right = down(Key::Right);
    c.wheel = (CineKeyPressed(Key::PageUp, down(Key::PageUp)) ? 1 : 0) -
              (CineKeyPressed(Key::PageDown, down(Key::PageDown)) ? 1 : 0) + p.speed_steps +
              mouse.wheel;
    c.smooth_steps = (CineKeyPressed(Key::RightBracket, down(Key::RightBracket)) ? 1 : 0) -
                     (CineKeyPressed(Key::LeftBracket, down(Key::LeftBracket)) ? 1 : 0) +
                     p.smooth_steps;
    cine_pad.speed_steps = 0;
    cine_pad.smooth_steps = 0;
    c.pad_move_x = p.lx;
    c.pad_move_y = -p.ly; // stick up = smaller y = forward
    c.pad_rise = ((p.buttons & PadR1) ? 1.0f : 0.0f) - ((p.buttons & PadL1) ? 1.0f : 0.0f);
    c.pad_look_x = p.rx;
    c.pad_look_y = -p.ry;
    c.pad_zoom = p.r2 - p.l2;
    c.mouse_dx = mouse.dx;
    c.mouse_dy = mouse.dy;
    return c;
}

// F8 (bloodborne_re.cpp, TickBbscUnstuckKey): true once per press in the game window.
bool TakeUnstuckKey() {
    std::scoped_lock lock{cine_mutex};
    return CineKeyPressed(Key::F8, Platform::KeyDown(Key::F8));
}

void CinematicForceOff() {
    std::scoped_lock lock{cine_mutex};
    cine_state = CinematicInput{};
    cine_pad = CinePad{.last_buttons = cine_pad.last_buttons};
    Platform::ReleaseMouse();
}

void CinematicForceOn() {
    std::scoped_lock lock{cine_mutex};
    if (cine_state.enabled) {
        return;
    }
    cine_state = CinematicInput{};
    cine_state.enabled = true;
    cine_pad = CinePad{.last_buttons = cine_pad.last_buttons};
    Platform::ReleaseMouse();
}

} // namespace Bbsc
} // namespace Core

namespace Common::FS {
std::filesystem::path GetUserPath(PathType type) {
    return type == PathType::CapturesDir ? data_dir / "captures" : data_dir;
}
} // namespace Common::FS

namespace Libraries::Np::NpMatching2 {
int BbscReplayRoomMembers(bool) {
    return 0; // the emulator's room cache is out of reach for a mod
}
} // namespace Libraries::Np::NpMatching2
