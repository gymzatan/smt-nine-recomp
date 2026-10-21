#include "kernel/recomp_continuation.h"
#include <assert.h>
#include <string.h>
static int leaves;
static void left(RecompContFrame *f) { (void)f; ++leaves; }
int main(int argc,char **argv) {
    RecompContFrame root,owner,fragment,helper;
    uint32_t outer_ebp=0x9000,owner_ebp=0x8000,tail_ebp=0x8000;
    uint32_t value=0x12345678,inner_ebp=0x8000;
    const RecompContLocal r[]={{"ebp",&outer_ebp,4}};
    const RecompContLocal o[]={{"ebp",&owner_ebp,4},{"value",&value,4}};
    const RecompContLocal t[]={{"ebp",&tail_ebp,4}};
    const RecompContLocal h[]={{"ebp",&inner_ebp,4}};
    assert(!recomp_cont_enter(&root,0x10000,r,1));root.on_leave=left;
    recomp_cont_site(&root,0x100010010ULL);
    assert(!recomp_cont_enter(&owner,0x11000,o,2));owner.on_leave=left;
    recomp_cont_jump_bind(&owner,0x7000,0x400011020ULL);
    recomp_cont_site(&owner,0x300011030ULL);
    assert(!recomp_cont_enter(&fragment,0x12000,t,1));fragment.on_leave=left;
    recomp_cont_site(&fragment,0x100012020ULL);
    assert(!recomp_cont_enter(&helper,0x13000,h,1));helper.on_leave=left;
    recomp_cont_site(&helper,0x300013040ULL);
    recomp_cont_prepare_transfer(argc>1 ? 0x1234 : 0x8000,0x14000);
    assert(leaves==4 && !recomp_cont_active());
    outer_ebp=owner_ebp=value=0;
    assert(recomp_cont_enter(&root,0x10000,r,1)==0x100010010ULL);
    assert(outer_ebp==0x9000);
    assert(recomp_cont_enter(&owner,0x11000,o,2)==0x700014000ULL);
    assert(owner_ebp==0x8000 && value==0x12345678);
    assert(recomp_cont_jump_known(&owner,0x7000));
    assert(recomp_cont_jump_count(&owner)==1);
    /* A checkpoint taken in the resumed original continuation must retain
     * the catch parent and restore its existing jump-buffer snapshots. */
    assert(!recomp_cont_enter(&fragment,0x14000,t,1));
    recomp_cont_site(&fragment,0x200014010ULL);
    FILE *packet=tmpfile();assert(packet && recomp_cont_snapshot_ready());
    assert(recomp_cont_save(packet));rewind(packet);
    recomp_cont_discard_root();
    assert(recomp_cont_load(packet));fclose(packet);
    assert(recomp_cont_enter(&root,0x10000,r,1)==0x100010010ULL);
    assert(recomp_cont_enter(&owner,0x11000,o,2)==0x700014000ULL);
    assert(recomp_cont_jump_known(&owner,0x7000));
    assert(recomp_cont_enter(&fragment,0x14000,t,1)==0x200014010ULL);
    recomp_cont_leave(&fragment);recomp_cont_leave(&owner);recomp_cont_leave(&root);
    assert(!recomp_cont_active());return 0;
}
