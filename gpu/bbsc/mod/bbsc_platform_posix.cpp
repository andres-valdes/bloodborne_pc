// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC mod: the POSIX side of bbsc_platform.h, for libbbsc.so in a Linux host (bbport). Mappings
// come from /proc/self/maps, code protection from mprotect, and keys and the mouse from the host's
// own SDL3, found at run time (the library does not link SDL; without it the camera keeps the pad).

#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>

#include "bbsc_platform.h"
#include "common/logging/log.h"

namespace BbscMod::Platform {
namespace {

u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

u64 PageSize() {
    static const u64 size = static_cast<u64>(sysconf(_SC_PAGESIZE));
    return size;
}

int MmapProt(u32 prot) {
    return ((prot & ProtRead) != 0 ? PROT_READ : 0) | ((prot & ProtWrite) != 0 ? PROT_WRITE : 0) |
           ((prot & ProtExec) != 0 ? PROT_EXEC : 0);
}

// ---- /proc/self/maps
// ----------------------------------------------------------------------------- One snapshot of the
// whole map, shared by every thread. Reading the file costs about as much as a VirtualQuery storm
// did on Windows, so it is read again only when the snapshot is old, or when an address falls
// between mappings and the snapshot is old enough to have missed a new one.

struct Mapping {
    u64 start = 0;
    u64 end = 0;
    u32 prot = 0;
};

constexpr u64 MapsMaxAgeMs = 250;
constexpr u64 GapRecheckMs = 20;
constexpr u64 AddressSpaceEnd = 0x0000'8000'0000'0000ULL;

std::shared_mutex maps_mutex;
std::vector<Mapping> maps; // sorted by start
u64 maps_taken_ms = 0;

std::vector<Mapping> ReadMaps() {
    std::vector<Mapping> out;
    std::ifstream file("/proc/self/maps");
    std::string line;
    while (std::getline(file, line)) {
        Mapping m;
        char perms[5]{};
        if (std::sscanf(line.c_str(), "%" SCNx64 "-%" SCNx64 " %4s", &m.start, &m.end, perms) !=
            3) {
            continue;
        }
        m.prot = (perms[0] == 'r' ? ProtRead : 0) | (perms[1] == 'w' ? ProtWrite : 0) |
                 (perms[2] == 'x' ? ProtExec : 0);
        out.push_back(m);
    }
    return out;
}

// Re-reads the map unless another thread did within `fresh_ms`.
void Refresh(u64 fresh_ms) {
    std::unique_lock lock{maps_mutex};
    if (!maps.empty() && NowMs() - maps_taken_ms <= fresh_ms) {
        return;
    }
    maps = ReadMaps();
    maps_taken_ms = NowMs();
}

// The stretch holding `address` in the current snapshot (caller holds maps_mutex).
Region Find(u64 address) {
    const auto next = std::upper_bound(maps.begin(), maps.end(), address,
                                       [](u64 a, const Mapping& m) { return a < m.start; });
    if (next != maps.begin() && address < std::prev(next)->end) {
        const Mapping& m = *std::prev(next);
        return {m.start, m.end, true, m.prot};
    }
    const u64 start = next == maps.begin() ? 0 : std::prev(next)->end;
    const u64 end = next == maps.end() ? AddressSpaceEnd : next->start;
    return {start, end, false, 0};
}

// ---- Sockets
// -------------------------------------------------------------------------------------

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    int get() const {
        return fd_;
    }
    int release() {
        return std::exchange(fd_, -1);
    }

private:
    int fd_;
};

bool SendAll(int fd, const void* data, size_t size) {
    const auto* at = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t sent = send(fd, at, size, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        at += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool RecvAll(int fd, void* data, size_t size) {
    auto* at = static_cast<char*>(data);
    while (size > 0) {
        const ssize_t got = recv(fd, at, size, 0);
        if (got <= 0) {
            return false;
        }
        at += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

// A TCP connection to host:port within `timeout_ms`, with the same limit on every send and recv.
int ConnectTcp(const std::string& host, const std::string& port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0) {
        return -1;
    }
    Fd fd{socket(found->ai_family, found->ai_socktype | SOCK_NONBLOCK, found->ai_protocol)};
    bool connected = false;
    if (fd.get() >= 0) {
        connected = connect(fd.get(), found->ai_addr, found->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) {
            pollfd wait{fd.get(), POLLOUT, 0};
            int error = 0;
            socklen_t length = sizeof(error);
            connected = poll(&wait, 1, timeout_ms) == 1 &&
                        getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                        error == 0;
        }
    }
    freeaddrinfo(found);
    if (!connected) {
        return -1;
    }
    fcntl(fd.get(), F_SETFL, fcntl(fd.get(), F_GETFL) & ~O_NONBLOCK);
    const timeval timeout{timeout_ms / 1000, timeout_ms % 1000 * 1000};
    setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd.release();
}

// ---- SDL3 of the host
// ----------------------------------------------------------------------------

struct Sdl {
    decltype(&SDL_GetKeyboardState) get_keyboard_state = nullptr;
    decltype(&SDL_GetKeyboardFocus) get_keyboard_focus = nullptr;
    decltype(&SDL_AddEventWatch) add_event_watch = nullptr;
    bool found = false;
};

std::mutex mouse_mutex;
bool mouse_taken = false;
float mouse_dx = 0.0f;
float mouse_dy = 0.0f;
int mouse_wheel = 0;

// The host's window thread, for every event it pumps: only counts, never changes, the input.
bool SDLCALL WatchMouse(void*, SDL_Event* event) {
    std::scoped_lock lock{mouse_mutex};
    if (!mouse_taken) {
        return true;
    }
    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        mouse_dx += event->motion.xrel;
        mouse_dy += event->motion.yrel;
    } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        const float y =
            event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y;
        mouse_wheel += y > 0.0f ? 1 : (y < 0.0f ? -1 : 0);
    }
    return true;
}

const Sdl& HostSdl() {
    static const Sdl sdl = [] {
        Sdl s;
        s.get_keyboard_state = reinterpret_cast<decltype(&SDL_GetKeyboardState)>(
            dlsym(RTLD_DEFAULT, "SDL_GetKeyboardState"));
        s.get_keyboard_focus = reinterpret_cast<decltype(&SDL_GetKeyboardFocus)>(
            dlsym(RTLD_DEFAULT, "SDL_GetKeyboardFocus"));
        s.add_event_watch = reinterpret_cast<decltype(&SDL_AddEventWatch)>(
            dlsym(RTLD_DEFAULT, "SDL_AddEventWatch"));
        s.found = s.get_keyboard_state && s.get_keyboard_focus && s.add_event_watch;
        if (s.found) {
            s.add_event_watch(&WatchMouse, nullptr);
        } else {
            LOG_WARNING(Mod,
                        "Input: the host's SDL3 was not found - the F1 camera and F8 work with "
                        "the controller only");
        }
        return s;
    }();
    return sdl;
}

SDL_Scancode Scancode(Key key) {
    switch (key) {
    case Key::F1:
        return SDL_SCANCODE_F1;
    case Key::F8:
        return SDL_SCANCODE_F8;
    case Key::W:
        return SDL_SCANCODE_W;
    case Key::A:
        return SDL_SCANCODE_A;
    case Key::S:
        return SDL_SCANCODE_S;
    case Key::D:
        return SDL_SCANCODE_D;
    case Key::E:
        return SDL_SCANCODE_E;
    case Key::Q:
        return SDL_SCANCODE_Q;
    case Key::C:
        return SDL_SCANCODE_C;
    case Key::Z:
        return SDL_SCANCODE_Z;
    case Key::X:
        return SDL_SCANCODE_X;
    case Key::V:
        return SDL_SCANCODE_V;
    case Key::Space:
        return SDL_SCANCODE_SPACE;
    case Key::Shift:
        return SDL_SCANCODE_LSHIFT;
    case Key::Alt:
        return SDL_SCANCODE_LALT;
    case Key::Comma:
        return SDL_SCANCODE_COMMA;
    case Key::Period:
        return SDL_SCANCODE_PERIOD;
    case Key::Slash:
        return SDL_SCANCODE_SLASH;
    case Key::Up:
        return SDL_SCANCODE_UP;
    case Key::Down:
        return SDL_SCANCODE_DOWN;
    case Key::Left:
        return SDL_SCANCODE_LEFT;
    case Key::Right:
        return SDL_SCANCODE_RIGHT;
    case Key::PageUp:
        return SDL_SCANCODE_PAGEUP;
    case Key::PageDown:
        return SDL_SCANCODE_PAGEDOWN;
    case Key::LeftBracket:
        return SDL_SCANCODE_LEFTBRACKET;
    case Key::RightBracket:
        return SDL_SCANCODE_RIGHTBRACKET;
    case Key::Count:
        break;
    }
    return SDL_SCANCODE_UNKNOWN;
}

} // namespace

bool QueryRegion(u64 address, Region& out) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {
            Refresh(GapRecheckMs);
        }
        std::shared_lock lock{maps_mutex};
        const u64 age = NowMs() - maps_taken_ms;
        if (maps.empty() || (attempt == 0 && age > MapsMaxAgeMs)) {
            continue;
        }
        out = Find(address);
        if (out.mapped || attempt == 1 || age <= GapRecheckMs) {
            return true;
        }
    }
    return false;
}

std::vector<u64> ExecutableRegions() {
    Refresh(0);
    std::shared_lock lock{maps_mutex};
    std::vector<u64> starts;
    for (const Mapping& m : maps) {
        if ((m.prot & ProtExec) != 0) {
            starts.push_back(m.start);
        }
    }
    return starts;
}

bool Protect(u64 address, u64 size, u32 prot, u32* restore) {
    const u64 start = address & ~(PageSize() - 1);
    const u64 end = (address + size + PageSize() - 1) & ~(PageSize() - 1);
    if (restore != nullptr) {
        Refresh(0);
        std::shared_lock lock{maps_mutex};
        const Region region = Find(address);
        if (!region.mapped) {
            return false;
        }
        *restore = region.prot;
    }
    return mprotect(reinterpret_cast<void*>(start), end - start, MmapProt(prot)) == 0;
}

void Restore(u64 address, u64 size, u32 restore) {
    Protect(address, size, restore, nullptr);
}

void FlushCode(u64 address, u64 size) {
    __builtin___clear_cache(reinterpret_cast<char*>(address),
                            reinterpret_cast<char*>(address + size));
}

u64 MapExecutable(u64 address, u64 size) {
    void* got = mmap(reinterpret_cast<void*>(address), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED) {
        return 0;
    }
    if (reinterpret_cast<u64>(got) != address) { // a kernel older than 4.17 took it as a hint
        munmap(got, size);
        return 0;
    }
    return address;
}

std::filesystem::path ModulePath() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&ModulePath), &info) == 0 || info.dli_fname == nullptr) {
        return {};
    }
    std::error_code ignored;
    return std::filesystem::canonical(info.dli_fname, ignored);
}

std::filesystem::path ExecutablePath() {
    std::error_code ignored;
    return std::filesystem::read_symlink("/proc/self/exe", ignored);
}

u32 ProcessId() {
    return static_cast<u32>(getpid());
}

void SetEnvironment(const char* name, const char* value) {
    setenv(name, value, 1);
}

void ShowError(const std::string& title, const std::string& text) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
}

std::filesystem::path InstalledUserFolder() {
    if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && data[0] != '\0') {
        return std::filesystem::path{data} / "shadPS4";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home} / ".local" / "share" / "shadPS4";
    }
    return {};
}

// Plain http:// only: the stale-sign delete goes to a BBSC host's own broker, never to a public
// (https) server (bbsc_http.cpp).
u32 HttpPost(const std::string& url, const std::string& body, const std::string& headers) {
    constexpr std::string_view Scheme = "http://";
    if (!url.starts_with(Scheme)) {
        return 0;
    }
    const size_t path_at = url.find('/', Scheme.size());
    const std::string authority = url.substr(Scheme.size(), path_at - Scheme.size());
    const std::string path = path_at == std::string::npos ? "/" : url.substr(path_at);
    const size_t colon = authority.rfind(':');
    const std::string host = authority.substr(0, colon);
    const std::string port = colon == std::string::npos ? "80" : authority.substr(colon + 1);
    Fd fd{ConnectTcp(host, port, 1500)};
    if (fd.get() < 0) {
        return 0;
    }
    const std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + authority +
                                "\r\nContent-Length: " + std::to_string(body.size()) +
                                "\r\nConnection: close\r\n" + headers + "\r\n" + body;
    if (!SendAll(fd.get(), request.data(), request.size())) {
        return 0;
    }
    // "HTTP/1.1 200 OK": the status is all the caller needs.
    char status_line[16]{};
    if (!RecvAll(fd.get(), status_line, 12) || std::strncmp(status_line, "HTTP/1.", 7) != 0) {
        return 0;
    }
    return static_cast<u32>(std::atoi(status_line + 9));
}

// The Discord app's socket: discord-ipc-N in $XDG_RUNTIME_DIR (or the temp folder), directly or
// inside the Flatpak / Snap app folders - where Discord's own RPC library looks.
s64 DiscordIpcOpen(int index) {
    std::vector<std::string> bases;
    for (const char* name : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
            bases.emplace_back(value);
        }
    }
    bases.emplace_back("/tmp");
    for (const std::string& base : bases) {
        for (const char* app : {"", "/app/com.discordapp.Discord", "/snap.discord"}) {
            const std::string path = base + app + "/discord-ipc-" + std::to_string(index);
            sockaddr_un address{};
            if (path.size() >= sizeof(address.sun_path)) {
                continue;
            }
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
            Fd fd{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
            if (fd.get() >= 0 && connect(fd.get(), reinterpret_cast<const sockaddr*>(&address),
                                         sizeof(address)) == 0) {
                return fd.release();
            }
        }
    }
    return -1;
}

bool DiscordIpcWrite(s64 handle, const void* data, size_t size) {
    return SendAll(static_cast<int>(handle), data, size);
}

bool DiscordIpcRead(s64 handle, void* data, size_t size) {
    return RecvAll(static_cast<int>(handle), data, size);
}

bool DiscordIpcAvailable(s64 handle, size_t& bytes) {
    pollfd wait{static_cast<int>(handle), POLLIN, 0};
    if (poll(&wait, 1, 0) < 0 || (wait.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return false;
    }
    int available = 0;
    if (ioctl(static_cast<int>(handle), FIONREAD, &available) != 0) {
        return false;
    }
    if ((wait.revents & POLLIN) != 0 && available == 0) {
        return false; // readable with nothing to read: Discord closed the socket
    }
    bytes = static_cast<size_t>(available);
    return true;
}

void DiscordIpcClose(s64 handle) {
    close(static_cast<int>(handle));
}

bool KeyDown(Key key) {
    const Sdl& sdl = HostSdl();
    if (!sdl.found || sdl.get_keyboard_focus() == nullptr) {
        return false;
    }
    int count = 0;
    const bool* state = sdl.get_keyboard_state(&count);
    const SDL_Scancode code = Scancode(key);
    return state != nullptr && static_cast<int>(code) < count && state[code];
}

// The movement SDL reported since the last call. The host keeps its own mouse handling (bbport:
// F1 frees the mouse, a click takes it again); the game does not see the movement anyway while
// the camera is on, because the pad wrapper hands it a controller at rest.
MouseLook TakeMouseLook() {
    const Sdl& sdl = HostSdl();
    MouseLook look;
    std::scoped_lock lock{mouse_mutex};
    look.active = sdl.found && sdl.get_keyboard_focus() != nullptr;
    if (look.active && mouse_taken) {
        look.dx = mouse_dx;
        look.dy = mouse_dy;
        look.wheel = mouse_wheel;
    }
    mouse_taken = look.active;
    mouse_dx = mouse_dy = 0.0f;
    mouse_wheel = 0;
    return look;
}

void ReleaseMouse() {
    std::scoped_lock lock{mouse_mutex};
    mouse_taken = false;
    mouse_dx = mouse_dy = 0.0f;
    mouse_wheel = 0;
}

} // namespace BbscMod::Platform
