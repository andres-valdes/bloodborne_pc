/* Seamless co-op (BB_BBSC=1, with online play): BBSC (gpu/bbsc: bbsc/libbbsc.so beside libbbgpu.so),
 * started by probe.c on the relocated, patched image before its code is made read-only and run, so
 * its patches and hooks are in place before any game thread exists. BBSC checks guest memory before
 * every access; it gets the game's own view here (the image's segments, then the guest mappings),
 * not the host's page protections, which the GPU's page tracking changes all the time. Without the
 * module, or when it refuses the image, the game runs as before. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbsc/mod/bbsc_host.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const unsigned char *game_image;
static const RuntimeSegment *game_segments;
static uint64_t game_segment_count;

int runtime_bbsc_enabled(void) {
    static int enabled=-1;
    if (enabled<0) { const char *v=getenv("BB_BBSC"); enabled=v && v[0]=='1'; }
    return enabled;
}

/* bbsc_host.h: 1 with the mapping around address and its protection (1 read, 2 write, 4 execute),
 * 0 for memory the game does not own (BBSC then asks the OS). */
static int query_memory(uint64_t address, uint64_t *start, uint64_t *end, uint32_t *prot) {
    for (uint64_t i=0;i<game_segment_count;++i) {
        const uint64_t base=(uint64_t)(uintptr_t)game_image+game_segments[i].address;
        if (address>=base && address-base<game_segments[i].size) {
            const uint64_t flags=game_segments[i].flags;
            *start=base; *end=base+game_segments[i].size;
            *prot=((flags&4) ? 1u : 0)|((flags&2) ? 2u : 0)|((flags&1) ? 4u : 0);
            return 1;
        }
    }
    uintptr_t region_start, region_end, vma_end;
    int mapped, guest_prot, type;
    if (!runtime_memory_region(address,&region_start,&region_end,&mapped) || !mapped ||
        !runtime_memory_vma_info(address,&guest_prot,&type,&vma_end)) return 0;
    *start=region_start; *end=region_end;
    *prot=((guest_prot&3) ? 1u : 0)|((guest_prot&2) ? 2u : 0)|((guest_prot&4) ? 4u : 0);
    return 1;
}

/* The module: BB_BBSC_MODULE, else gpu/bbsc/libbbsc.so beside bb-probe (where libbbgpu.so is). */
void runtime_bbsc_start(const unsigned char *image, const RuntimeSegment *segments, uint64_t count) {
    if (!runtime_net_enabled()) {
        printf("Co-op: seamless co-op needs online play (BB_ONLINE=1): playing without it\n");
        return;
    }
    char path[PATH_MAX];
    const char *forced=getenv("BB_BBSC_MODULE");
    if (forced && forced[0]) {
        snprintf(path,sizeof(path),"%s",forced);
    } else {
        char exe[PATH_MAX-32];
        const ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
        if (n<=0) return;
        exe[n]=0;
        char *slash=strrchr(exe,'/');
        if (slash) *slash=0;
        snprintf(path,sizeof(path),"%s/gpu/bbsc/libbbsc.so",exe);
    }
    /* RTLD_LOCAL: BBSC's C++ stays out of the process's symbol scope. */
    void *module=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (!module) {
        printf("Co-op: the seamless co-op module is not available (%s): playing without it\n",dlerror());
        return;
    }
    const BbscAttachFn attach=(BbscAttachFn)dlsym(module,"bbsc_attach");
    game_image=image; game_segments=segments; game_segment_count=count;
    const BbscHost host={
        .version=BBSC_HOST_VERSION, .image_base=(uint64_t)(uintptr_t)image, .query_memory=query_memory,
    };
    if (!attach || attach(&host)!=0) {
        printf("Co-op: %s did not start (its log says why): playing without seamless co-op\n",path);
        return;
    }
    printf("Co-op: seamless co-op module %s\n",path);
}
