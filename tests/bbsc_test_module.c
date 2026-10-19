/* A stand-in for libbbsc.so (tests/test_bbsc.c): records what the runtime gives bbsc_attach, and
 * what its memory callback answers for an address in each image segment, in a guest mapping and in
 * memory the game does not own. */
#include "gpu/bbsc/mod/bbsc_host.h"

/* Per query: whether the runtime knew the address, then start, end and protection. */
uint64_t bbsc_test_image;
uint64_t bbsc_test_queries[4][4];

int bbsc_attach(const BbscHost *host) {
    bbsc_test_image=host->image_base;
    const uint64_t addresses[4]={host->image_base+0x10, host->image_base+0x2008,
                                 UINT64_C(0x900000800), 0x1234};
    for (int i=0;i<4;++i) {
        uint64_t start=0, end=0;
        uint32_t prot=0;
        bbsc_test_queries[i][0]=(uint64_t)host->query_memory(addresses[i],&start,&end,&prot);
        bbsc_test_queries[i][1]=start;
        bbsc_test_queries[i][2]=end;
        bbsc_test_queries[i][3]=prot;
    }
    return host->version==BBSC_HOST_VERSION ? 0 : 1;
}
