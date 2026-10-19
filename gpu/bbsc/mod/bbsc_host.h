/* SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* BBSC mod: the entry point of libbbsc.so, for a host that loads the mod itself (bbport). The
 * Windows DLL needs none of this: it starts on its own when it is loaded (DllMain) and finds the
 * game itself. Plain C, so a C host can include it.
 *
 * Besides the player's settings (mod.json), a host may set:
 *   BBSC_USER_DIR  its user folder (savedata/<user>/CUSA00207/...) when it is not shadPS4's;
 *   BBSC_DATA_DIR  a writable folder for BBSC's logs, captures and save backups when the mod's own
 *                  folder is read-only (LICENSE.txt, NOTICE.txt and SUPPORTERS.txt stay next to
 *                  the library). */

#ifndef BBSC_HOST_H
#define BBSC_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBSC_HOST_VERSION 1

typedef struct BbscHost {
    uint32_t version;    /* BBSC_HOST_VERSION */
    uint64_t image_base; /* where the CUSA03173 01.09 eboot is loaded; 0 = BBSC looks for it */
    /* The game's own view of an address: 1 with the mapping [*start, *end) and its protection
     * (1 read, 2 write, 4 execute; 0 no access), or 0 when the host does not know the address and
     * BBSC should ask the OS. May be null. Called from any thread, many times per frame. */
    int (*query_memory)(uint64_t address, uint64_t* start, uint64_t* end, uint32_t* prot);
} BbscHost;

/* Installs BBSC into the game before the game's code runs: checks every patch site, writes the
 * patches, installs the hooks and starts the relay, the P2P channel and the patch watcher.
 * Settings come from the environment, as for the DLL. Call it once, while no game thread runs.
 * Returns 0 when BBSC is installed; otherwise nothing was changed (the BBSC log says why). */
int bbsc_attach(const BbscHost* host);
typedef int (*BbscAttachFn)(const BbscHost* host);

#ifdef __cplusplus
}
#endif

#endif
