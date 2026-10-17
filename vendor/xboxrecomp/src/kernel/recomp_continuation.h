#ifndef RECOMP_CONTINUATION_H
#define RECOMP_CONTINUATION_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef struct RecompContLocal {
    const char *name;
    void *address;
    size_t size;
} RecompContLocal;

typedef struct RecompContFrame {
    uint32_t function;
    uint64_t site;
    const RecompContLocal *locals;
    size_t local_count;
    struct RecompContFrame *parent;
    void *jumps;
    void (*on_leave)(struct RecompContFrame *);
} RecompContFrame;

uint64_t recomp_cont_enter(RecompContFrame *, uint32_t,
                          const RecompContLocal *, size_t);
void recomp_cont_leave(RecompContFrame *);
void recomp_cont_site(RecompContFrame *, uint64_t);
void recomp_cont_invalid(void);
int recomp_cont_active(void);
int recomp_cont_snapshot_ready(void);
uint32_t recomp_cont_root_function(void);
RecompContFrame *recomp_cont_current(void);
void recomp_cont_jump_bind(RecompContFrame *, uint32_t, uint64_t);
size_t recomp_cont_jump_count(const RecompContFrame *);
uint32_t recomp_cont_jump_buffer(const RecompContFrame *, size_t);
int recomp_cont_jump_known(const RecompContFrame *, uint32_t);
void recomp_cont_unwind(RecompContFrame *, uint32_t);
uint64_t recomp_cont_jump_restore(RecompContFrame *, uint32_t);
uint64_t recomp_cont_jump_return(RecompContFrame *);
int recomp_cont_save(FILE *);
int recomp_cont_load(FILE *);
int recomp_cont_validate(FILE *, uint32_t *);
void recomp_cont_discard_root(void);
#endif
