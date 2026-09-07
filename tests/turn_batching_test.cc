#include "bridge/resdb_conflict_matrix.h"
#include <cassert>
int main() {
    using namespace resdb::omnet;
    // Opposing lefts and compatible left/right pairs may cross together.
    assert(IsSafeToBatch(0,1,1,1,1,1));
    assert(IsSafeToBatch(0,1,1,2,2,0));
    // A crossing straight movement must still wait.
    assert(!IsSafeToBatch(0,1,1,2,0,0));
    // Preserve physical queues, independent lanes, and unknown-intent safety.
    assert(!IsSafeToBatch(0,1,1,0,1,1));
    assert(IsSafeToBatch(0,1,1,0,0,0));
    assert(!IsSafeToBatch(0,3,1,0,0,0));
    assert(!IsSafeToBatch(0,1,kNoPhysicalLane,0,0,kNoPhysicalLane));
    for (unsigned a=0;a<4;++a) for(unsigned b=0;b<4;++b)
        for(unsigned d=0;d<4;++d) for(unsigned e=0;e<4;++e)
            assert(IsSafeToBatch(a,d,0,b,e,1)==IsSafeToBatch(b,e,1,a,d,0));
}
