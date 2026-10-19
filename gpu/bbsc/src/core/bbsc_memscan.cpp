// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bbsc_memscan.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <string>
#endif

#include <cstring>

namespace Core::Bbsc {

namespace {

#ifdef _WIN32
// One region. The emulator changes page protection all the time (GPU tracking), so a page that was
// readable a moment ago may fault now: the search runs under SEH and a fault just ends the region.
void SearchRegion(const std::uint8_t* start, std::size_t size, const std::uint8_t* needle,
                  std::size_t length, std::vector<std::uint64_t>& hits, std::size_t max_hits) {
    __try {
        const std::uint8_t* end = start + size;
        const std::uint8_t* at = start;
        while (hits.size() < max_hits && end - at >= static_cast<std::ptrdiff_t>(length)) {
            at = static_cast<const std::uint8_t*>(
                std::memchr(at, needle[0], static_cast<std::size_t>(end - at) - length + 1));
            if (at == nullptr) {
                break;
            }
            if (std::memcmp(at, needle, length) == 0) {
                hits.push_back(reinterpret_cast<std::uint64_t>(at));
            }
            ++at;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
#else
// The readable mappings of this process overlapping [lo, hi), clipped to it.
std::vector<std::pair<std::uint64_t, std::uint64_t>> ReadableRanges(std::uint64_t lo,
                                                                    std::uint64_t hi) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        char perms[5]{};
        if (std::sscanf(line.c_str(), "%" SCNx64 "-%" SCNx64 " %4s", &start, &end, perms) == 3 &&
            perms[0] == 'r' && end > lo && start < hi) {
            ranges.emplace_back(std::max(start, lo), std::min(end, hi));
        }
    }
    return ranges;
}

// One range, read in chunks through process_vm_readv: the emulator changes and drops mappings all
// the time, and a page gone since /proc/self/maps was read just ends the chunk instead of faulting.
void SearchRange(std::uint64_t start, std::uint64_t end, std::span<const std::uint8_t> needle,
                 std::vector<std::uint64_t>& hits, std::size_t max_hits) {
    constexpr std::uint64_t Chunk = 1 << 20;
    std::vector<std::uint8_t> buffer(Chunk + needle.size() - 1);
    for (std::uint64_t at = start; at < end && hits.size() < max_hits; at += Chunk) {
        const auto want =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), end - at));
        iovec local{buffer.data(), want};
        iovec remote{reinterpret_cast<void*>(at), want};
        const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        if (got < static_cast<ssize_t>(needle.size())) {
            continue;
        }
        // Matches starting in the overlap are found again by the next chunk.
        const std::size_t last =
            std::min<std::size_t>(static_cast<std::size_t>(got) - needle.size(), Chunk - 1);
        for (std::size_t i = 0; i <= last && hits.size() < max_hits; ++i) {
            const auto* found = static_cast<const std::uint8_t*>(
                std::memchr(buffer.data() + i, needle[0], last - i + 1));
            if (found == nullptr) {
                break;
            }
            i = static_cast<std::size_t>(found - buffer.data());
            if (std::memcmp(found, needle.data(), needle.size()) == 0) {
                hits.push_back(at + i);
            }
        }
    }
}
#endif

} // namespace

std::vector<std::uint64_t> FindInMemory(std::span<const std::uint8_t> needle, std::uint64_t lo,
                                        std::uint64_t hi, std::size_t max_hits) {
    std::vector<std::uint64_t> hits;
#ifdef _WIN32
    if (needle.empty()) {
        return hits;
    }
    constexpr DWORD Readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    std::uint64_t at = lo;
    while (at < hi && hits.size() < max_hits) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0) {
            break;
        }
        const auto start = reinterpret_cast<std::uint64_t>(info.BaseAddress);
        const std::uint64_t end = start + info.RegionSize;
        if (info.State == MEM_COMMIT && (info.Protect & Readable) != 0 &&
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) {
            SearchRegion(reinterpret_cast<const std::uint8_t*>(start), info.RegionSize,
                         needle.data(), needle.size(), hits, max_hits);
        }
        if (end <= at) {
            break;
        }
        at = end;
    }
#else
    if (needle.empty()) {
        return hits;
    }
    for (const auto& [start, end] : ReadableRanges(lo, hi)) {
        if (hits.size() >= max_hits) {
            break;
        }
        SearchRange(start, end, needle, hits, max_hits);
    }
#endif
    return hits;
}

} // namespace Core::Bbsc
