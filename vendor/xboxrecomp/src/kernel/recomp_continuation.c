#include "recomp_continuation.h"
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define CONT_TLS __declspec(thread)
#else
#define CONT_TLS __thread
#endif

typedef struct SavedLocal {
    char name[64];
    uint32_t size;
    unsigned char data[8];
} SavedLocal;
typedef struct SavedJump {
    uint32_t buffer, count;
    uint64_t site;
    SavedLocal *locals;
    struct SavedJump *next;
} SavedJump;
typedef struct SavedFrame {
    uint32_t function, count;
    uint64_t site;
    SavedLocal *locals;
    SavedJump *jumps;
} SavedFrame;
static CONT_TLS RecompContFrame *head;
static CONT_TLS SavedFrame *recovery;
static CONT_TLS uint32_t recovery_count, recovery_next;
static CONT_TLS uint32_t returned_jump_buffer;

int recomp_cont_active(void) { return head != 0; }
int recomp_cont_recovering(uint32_t function) {
    return recovery && recovery_next<recovery_count && recovery[recovery_next].function==function;
}
RecompContFrame *recomp_cont_current(void) { return head; }

void recomp_cont_invalid(void) {
    fputs("[STOP] invalid generated guest continuation\n", stderr);
    exit(5);
}
static void discard_jumps(SavedJump *jump) {
    while (jump) {
        SavedJump *next=jump->next;
        free(jump->locals); free(jump); jump=next;
    }
}
static void discard_recovery(SavedFrame *frames, uint32_t count) {
    for (uint32_t i=0; i<count; ++i) {
        free(frames[i].locals); discard_jumps(frames[i].jumps);
    }
    free(frames);
}
uint64_t recomp_cont_enter(RecompContFrame *f, uint32_t function,
                          const RecompContLocal *locals, size_t count) {
    f->function=function; f->site=0;
    f->locals=locals; f->local_count=count; f->parent=head; f->jumps=0; f->on_leave=0; f->on_unwind=0; f->retained=0; head=f;
    if (recovery) {
        SavedFrame *saved=&recovery[recovery_next];
        if (saved->function!=function || saved->count!=count)
            recomp_cont_invalid();
        for (size_t i=0; i<count; ++i) {
            if (saved->locals[i].size!=locals[i].size ||
                strcmp(saved->locals[i].name,locals[i].name))
                recomp_cont_invalid();
            memcpy(locals[i].address,saved->locals[i].data,locals[i].size);
        }
        f->site=saved->site;
        f->jumps=saved->jumps; saved->jumps=0;
        for (SavedJump *j=f->jumps; j; j=j->next) {
            if (j->count!=count) recomp_cont_invalid();
            for (size_t i=0; i<count; ++i)
                if (j->locals[i].size!=locals[i].size ||
                    strcmp(j->locals[i].name,locals[i].name)) recomp_cont_invalid();
        }
        if (++recovery_next==recovery_count) {
            discard_recovery(recovery,recovery_count);
            recovery=0; recovery_next=recovery_count=0;
        }
    }
    return f->site;
}
void recomp_cont_leave(RecompContFrame *f) {
    if (head!=f) recomp_cont_invalid();
    discard_jumps(f->jumps); f->jumps=0;
    head=f->parent;
    if (f->on_leave) f->on_leave(f);
}
static SavedJump *find_jump(const RecompContFrame *f, uint32_t buffer) {
    for (SavedJump *j=f->jumps; j; j=j->next)
        if (j->buffer==buffer) return j;
    return 0;
}
void recomp_cont_jump_bind(RecompContFrame *f, uint32_t buffer, uint64_t site) {
    if (head!=f || !buffer || (site>>32)!=4 || f->local_count>UINT32_MAX)
        recomp_cont_invalid();
    SavedJump *j=find_jump(f,buffer);
    if (!j) {
        if (recomp_cont_jump_count(f)>=32) recomp_cont_invalid();
        j=calloc(1,sizeof *j);
        if (!j) recomp_cont_invalid();
        j->locals=calloc(f->local_count,sizeof *j->locals);
        if (f->local_count && !j->locals) recomp_cont_invalid();
        j->buffer=buffer; j->count=(uint32_t)f->local_count;
        SavedJump **end=(SavedJump **)&f->jumps;
        while (*end) end=&(*end)->next;
        *end=j;
    }
    j->site=site;
    for (size_t i=0;i<f->local_count;++i) {
        const RecompContLocal *v=&f->locals[i];
        size_t len=strlen(v->name);
        if (!len || len>=sizeof j->locals[i].name || !v->size || v->size>8)
            recomp_cont_invalid();
        memcpy(j->locals[i].name,v->name,len+1);
        j->locals[i].size=(uint32_t)v->size;
        memcpy(j->locals[i].data,v->address,v->size);
    }
}
size_t recomp_cont_jump_count(const RecompContFrame *f) {
    size_t count=0;
    for (SavedJump *j=f->jumps;j;j=j->next) ++count;
    return count;
}
uint32_t recomp_cont_jump_buffer(const RecompContFrame *f,size_t index) {
    for (SavedJump *j=f->jumps;j;j=j->next)
        if (!index--) return j->buffer;
    recomp_cont_invalid(); return 0;
}
int recomp_cont_jump_known(const RecompContFrame *f,uint32_t buffer) {
    for (RecompContFrame *p=head;p;p=p->parent)
        if (p==f) return find_jump(f,buffer)!=0;
    return 0;
}
/* Preserve the original owners until cleanup finishes: callbacks can enter
 * guest drivers and wait, so dropping their frame before cleanup loses the
 * saved call path. The retained target itself is not being abandoned. */
void recomp_cont_prepare_unwind(RecompContFrame *stop) {
    if(!head) {if(stop)recomp_cont_invalid();return;}
    RecompContFrame *owner=head;
    while(owner && owner!=stop)owner=owner->parent;
    if(stop && !owner)recomp_cont_invalid();
    const uint32_t function=0xfe00074cu;
    uint32_t depth=0,finishing=0;RecompContFrame frame;
    const RecompContLocal locals[]={{"depth",&depth,4},{"finishing",&finishing,4}};
    uint64_t resume=recomp_cont_enter(&frame,function,locals,2);
    if(resume && resume!=((6ULL<<32)|function))recomp_cont_invalid();
    recomp_cont_site(&frame,(6ULL<<32)|function);
    owner=frame.parent;
    for(uint32_t i=0;i<depth;i++) {
        if(!owner || owner==stop)recomp_cont_invalid();owner=owner->parent;
    }
    if(resume) {
        if(!finishing || !owner || owner==stop || !owner->on_unwind)recomp_cont_invalid();
        goto finish;
    }
    for(;owner!=stop;owner=owner->parent,++depth) {
        if(!owner)recomp_cont_invalid();
        if(owner->on_unwind) {
            finishing=1;
finish:
            owner->on_unwind(owner);finishing=0;
        }
    }
    recomp_cont_leave(&frame);
}

void recomp_cont_unwind(RecompContFrame *f,uint32_t buffer) {
    RecompContFrame *p=head;
    while (p && p!=f) p=p->parent;
    if (!p) recomp_cont_invalid();
    while (head!=f) recomp_cont_leave(head);
    returned_jump_buffer=buffer;
}
uint64_t recomp_cont_jump_restore(RecompContFrame *f,uint32_t buffer) {
    if (head!=f) recomp_cont_invalid();
    SavedJump *j=find_jump(f,buffer);
    if (!j) recomp_cont_invalid();
    for (size_t i=0;i<f->local_count;++i)
        memcpy(f->locals[i].address,j->locals[i].data,j->locals[i].size);
    f->site=0; return j->site;
}
uint64_t recomp_cont_jump_return(RecompContFrame *f) {
    return recomp_cont_jump_restore(f,returned_jump_buffer);
}
/* The original guest EH runtime has already selected and unwound the catch.
 * Retain the same suspended callers and rebuild only their native C frames. */
RecompContFrame *recomp_cont_transfer_owner(uint32_t frame_ebp) {
    if (recovery || !frame_ebp) recomp_cont_invalid();
    RecompContFrame *owner=0;
    for (RecompContFrame *f=head;f;f=f->parent) {
        for (size_t i=0;i<f->local_count;++i) {
            const RecompContLocal *v=&f->locals[i];
            if (v->size==4 && !strcmp(v->name,"ebp") &&
                *(const uint32_t *)v->address==frame_ebp) owner=f;
        }
    }
    /* Tail fragments inherit their caller's EBP. The oldest matching frame
     * owns that guest activation; none of its fragments may be replayed. */
    if (!owner || ((owner->site>>32)!=1 && (owner->site>>32)!=3 &&
                   (owner->site>>32)!=7)) recomp_cont_invalid();
    return owner;
}
void recomp_cont_prepare_transfer(uint32_t frame_ebp, uint32_t target) {
    if (!target) recomp_cont_invalid();
    RecompContFrame *owner=recomp_cont_transfer_owner(frame_ebp);
    uint32_t count=0;
    for (RecompContFrame *f=owner;f;f=f->parent) {
        if (count==UINT32_MAX || f->local_count>UINT32_MAX) recomp_cont_invalid();
        ++count;
    }
    SavedFrame *frames=calloc(count,sizeof *frames);
    if (!frames) recomp_cont_invalid();
    recomp_cont_unwind(owner,0);
    uint32_t index=count;
    for (RecompContFrame *f=owner;f;f=f->parent) {
        SavedFrame *saved=&frames[--index];
        saved->function=f->function;
        saved->site=f==owner ? (7ULL<<32)|target : f->site;
        saved->count=(uint32_t)f->local_count;
        saved->locals=calloc(saved->count,sizeof *saved->locals);
        if (saved->count && !saved->locals) recomp_cont_invalid();
        for (size_t i=0;i<f->local_count;++i) {
            const RecompContLocal *v=&f->locals[i];
            size_t len=strlen(v->name);
            if (!len || len>=64 || !v->size || v->size>8) recomp_cont_invalid();
            memcpy(saved->locals[i].name,v->name,len+1);
            saved->locals[i].size=(uint32_t)v->size;
            memcpy(saved->locals[i].data,v->address,v->size);
        }
        saved->jumps=f->jumps;f->jumps=0;
    }
    while (head) {head->retained=1;recomp_cont_leave(head);}
    recovery=frames;recovery_count=count;recovery_next=0;
}

void recomp_cont_site(RecompContFrame *f, uint64_t site) {
    if (head!=f) recomp_cont_invalid();
    f->site=site;
}
static int write_u32(FILE *out, uint32_t v) {
    return fwrite(&v,sizeof v,1,out)==1;
}
static int read_u32(FILE *in, uint32_t *v) {
    return fread(v,sizeof *v,1,in)==1;
}
static int write_u64(FILE *out, uint64_t v) {
    return fwrite(&v,sizeof v,1,out)==1;
}
static int read_u64(FILE *in, uint64_t *v) {
    return fread(v,sizeof *v,1,in)==1;
}
static size_t snapshot_depth(void) {
    /* A call site records its suspended caller, not the native callee's
     * execution. The leaf must be a loop or described kernel-wait boundary. */
    if (!head || ((head->site >> 32) != 2 &&
                  !((head->site >> 32) == 5 && (uint32_t)head->site == head->function))) return 0;
    size_t depth=0;
    for (RecompContFrame *f=head; f; f=f->parent) {
        if (!f->site || depth==UINT32_MAX || f->local_count>UINT32_MAX) return 0;
        if (f!=head && (f->site>>32)!=1 && (f->site>>32)!=10 && (f->site>>32)!=3 && (f->site>>32)!=7 &&
            !(((f->site>>32)==6 || (f->site>>32)==8 || (f->site>>32)==9 || (f->site>>32)==11) && (uint32_t)f->site==f->function)) return 0;
        ++depth;
    }
    if (depth>SIZE_MAX/sizeof(RecompContFrame *)) return 0;
    return depth;
}
int recomp_cont_snapshot_ready(void) { return snapshot_depth()!=0; }
uint32_t recomp_cont_root_function(void) {
    RecompContFrame *f=head;
    if (!f) return 0;
    while (f->parent) f=f->parent;
    return f->function;
}
int recomp_cont_save(FILE *out) {
    size_t depth=snapshot_depth();
    if (!depth) return 0;
    RecompContFrame **frames=malloc(depth*sizeof *frames);
    if (!frames) return 0;
    size_t i=depth;
    for (RecompContFrame *f=head; f; f=f->parent) frames[--i]=f;
    int ok=fwrite("XBCONJ",6,1,out)==1 && write_u32(out,(uint32_t)depth);
    for (i=0; ok && i<depth; ++i) {
        RecompContFrame *f=frames[i];
        ok=write_u32(out,f->function) && write_u64(out,f->site) &&
           write_u32(out,(uint32_t)f->local_count);
        for (size_t j=0; ok && j<f->local_count; ++j) {
            const RecompContLocal *v=&f->locals[j];
            size_t len=strlen(v->name);
            if (!len || len>=64 || !v->size || v->size>8) { ok=0; break; }
            ok=write_u32(out,(uint32_t)len) && fwrite(v->name,len,1,out)==1 &&
               write_u32(out,(uint32_t)v->size) && fwrite(v->address,v->size,1,out)==1;
        }
        ok=ok && write_u32(out,(uint32_t)recomp_cont_jump_count(f));
        for (SavedJump *j=f->jumps;ok && j;j=j->next) {
            ok=write_u32(out,j->buffer) && write_u64(out,j->site) && write_u32(out,j->count);
            for (uint32_t k=0;ok && k<j->count;++k) {
                SavedLocal *v=&j->locals[k];
                uint32_t len=(uint32_t)strlen(v->name);
                ok=write_u32(out,len) && fwrite(v->name,len,1,out)==1 &&
                   write_u32(out,v->size) && fwrite(v->data,v->size,1,out)==1;
            }
        }
    }
    free(frames); return ok;
}
static int read_recovery(FILE *in,SavedFrame **result,uint32_t *result_count) {
    char magic[6]; uint32_t count;
    if (fread(magic,6,1,in)!=1 || memcmp(magic,"XBCONJ",6) ||
        !read_u32(in,&count) || !count || (size_t)count>SIZE_MAX/sizeof(SavedFrame))
        return 0;
    SavedFrame *frames=calloc(count,sizeof *frames);
    if (!frames) return 0;
    int ok=1;
    for (uint32_t i=0; ok && i<count; ++i) {
        SavedFrame *f=&frames[i];
        ok=read_u32(in,&f->function) && read_u64(in,&f->site) && f->site &&
           read_u32(in,&f->count) && (size_t)f->count<=SIZE_MAX/sizeof(SavedLocal);
        if (ok) {
            uint64_t kind=f->site>>32;
            ok=i+1<count ? (kind==1 || kind==3 || ((kind==7 || kind==10) && (uint32_t)f->site) || ((kind==6 || kind==8 || kind==9 || kind==11) && (uint32_t)f->site==f->function)) :
                           (kind==2 || (kind==5 && (uint32_t)f->site==f->function));
        }
        if (!ok) break;
        f->locals=calloc(f->count,sizeof *f->locals);
        if (f->count && !f->locals) { ok=0; break; }
        for (uint32_t j=0; ok && j<f->count; ++j) {
            SavedLocal *v=&f->locals[j]; uint32_t len;
            ok=read_u32(in,&len) && len && len<64 && fread(v->name,len,1,in)==1 &&
               read_u32(in,&v->size) && v->size && v->size<=8 &&
               fread(v->data,v->size,1,in)==1;
        }
        uint32_t jumps=0;
        ok=ok && read_u32(in,&jumps) && jumps<=32;
        SavedJump **next=&f->jumps;
        for (uint32_t k=0;ok && k<jumps;++k) {
            SavedJump *j=calloc(1,sizeof *j);
            if (!j) { ok=0; break; }
            *next=j;next=&j->next;
            ok=read_u32(in,&j->buffer) && j->buffer && read_u64(in,&j->site) &&
               (j->site>>32)==4 && read_u32(in,&j->count) && j->count==f->count;
            if (!ok) break;
            j->locals=calloc(j->count,sizeof *j->locals);
            if (j->count && !j->locals) { ok=0; break; }
            for (uint32_t n=0;ok && n<j->count;++n) {
                SavedLocal *v=&j->locals[n];uint32_t len;
                ok=read_u32(in,&len) && len && len<64 && fread(v->name,len,1,in)==1 &&
                   read_u32(in,&v->size) && v->size && v->size<=8 &&
                   fread(v->data,v->size,1,in)==1;
            }
        }
    }
    if (!ok) { discard_recovery(frames,count); return 0; }
    *result=frames; *result_count=count; return 1;
}
int recomp_cont_validate(FILE *in,uint32_t *root_function) {
    SavedFrame *frames;uint32_t count;
    if (!read_recovery(in,&frames,&count)) return 0;
    if (root_function) *root_function=frames[0].function;
    discard_recovery(frames,count);return 1;
}
int recomp_cont_load(FILE *in) {
    if (head || recovery || !read_recovery(in,&recovery,&recovery_count)) return 0;
    recovery_next=0;return 1;
}
void recomp_cont_discard_root(void) {
    while(head) recomp_cont_leave(head);
    if(recovery) discard_recovery(recovery,recovery_count);
    recovery=NULL;recovery_count=recovery_next=0;returned_jump_buffer=0;
}
