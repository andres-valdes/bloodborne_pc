/* Seamless co-op loader (runtime_bbsc.c): the module is started only with online play, on the game
 * image, and its memory checks get the game's view (tests/bbsc_test_module.c stands in for BBSC).
 * The real module (out/gpu/bbsc/libbbsc.so, when built) refuses an image that is not Bloodborne
 * 1.09 and leaves every byte of it as it was. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbsc/mod/bbsc_host.h"
#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* The runtime around the loader: online play on or off, and one guest mapping (CPU read/write). */
#define GUEST_START UINT64_C(0x900000000)
#define GUEST_END (GUEST_START+0x1000)
static int online;
int runtime_net_enabled(void) { return online; }
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    if (address>=GUEST_END) return 0;
    *mapped=address>=GUEST_START;
    *start=*mapped ? GUEST_START : 0;
    *end=*mapped ? GUEST_END : GUEST_START;
    return 1;
}
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end) {
    if (address<GUEST_START || address>=GUEST_END) return 0;
    *prot=2; *type=-1; *end=GUEST_END; /* SCE_KERNEL_PROT_CPU_RW */
    return 1;
}

static void loader(void) {
    const char *path="out/bbsc-test-module.so";
    static unsigned char image[0x3000];
    static const RuntimeSegment segments[]={{0,0x1000,5},{0x2000,0x1000,6}}; /* r-x, rw- */
    setenv("BB_BBSC_MODULE",path,1);
    runtime_bbsc_start(image,segments,2);
    assert(!dlopen(path,RTLD_NOW|RTLD_NOLOAD)); /* no module without online play */
    online=1;
    runtime_bbsc_start(image,segments,2);
    void *module=dlopen(path,RTLD_NOW|RTLD_NOLOAD);
    assert(module);
    assert(*(const uint64_t *)dlsym(module,"bbsc_test_image")==(uintptr_t)image);
    const uint64_t (*q)[4]=dlsym(module,"bbsc_test_queries");
    const uint64_t base=(uintptr_t)image;
    assert(q[0][0]==1 && q[0][1]==base && q[0][2]==base+0x1000 && q[0][3]==5);
    assert(q[1][0]==1 && q[1][1]==base+0x2000 && q[1][2]==base+0x3000 && q[1][3]==3);
    assert(q[2][0]==1 && q[2][1]==GUEST_START && q[2][2]==GUEST_END && q[2][3]==3);
    assert(q[3][0]==0);
    puts("PASS: the module starts with online play, on the image, with the game's view of memory");
}

/* The BBSC log written to `dir`/logs ("" when there is none); the folder is removed. */
static void take_log(const char *dir, char *out, size_t size) {
    char logs[256];
    snprintf(logs,sizeof(logs),"%s/logs",dir);
    out[0]=0;
    DIR *d=opendir(logs);
    for (struct dirent *e; d && (e=readdir(d));) {
        if (e->d_name[0]=='.') continue;
        char file[512];
        snprintf(file,sizeof(file),"%s/%s",logs,e->d_name);
        FILE *f=fopen(file,"r");
        if (f) { out[fread(out,1,size-1,f)]=0; fclose(f); }
        unlink(file);
    }
    if (d) closedir(d);
    rmdir(logs);
    rmdir(dir);
}

static void real_module(void) {
    const char *path="out/gpu/bbsc/libbbsc.so";
    if (access(path,R_OK)) {
        printf("SKIP: no seamless co-op module (%s)\n",path);
        return;
    }
    char data[]="/tmp/bbport-bbsc-test-XXXXXX";
    assert(mkdtemp(data));
    const pid_t child=fork();
    if (!child) {
        setenv("BBSC_DATA_DIR",data,1);
        void *module=dlopen(path,RTLD_NOW|RTLD_LOCAL);
        assert(module);
        const BbscAttachFn attach=(BbscAttachFn)dlsym(module,"bbsc_attach");
        assert(attach);
        /* Another game: as large as Bloodborne's image (bbsc_mod.cpp, EbootSize), all zero. */
        const size_t size=0x056D4000;
        unsigned char *image=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(image!=MAP_FAILED);
        static const unsigned char zero[4096];
        const BbscHost host={.version=BBSC_HOST_VERSION, .image_base=(uintptr_t)image};
        assert(attach(&host)!=0);
        for (size_t at=0;at<size;at+=sizeof(zero)) assert(!memcmp(image+at,zero,sizeof(zero)));
        assert(attach(&host)!=0); /* once per process */
        _exit(0);
    }
    int status=0;
    assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    static char log[1<<16];
    take_log(data,log,sizeof(log));
    assert(strstr(log,"No Bloodborne CUSA03173 01.09 image") && strstr(log,"nothing was changed"));
    puts("PASS: the module refuses another game's image and leaves it unchanged");
}

int main(void) {
    loader();
    real_module();
    return 0;
}
