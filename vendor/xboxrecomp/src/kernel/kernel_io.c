/*
 * kernel_io.c - Exported I/O type objects and the existing path-link index.
 * Device bodies, references, IRPs and callbacks share kernel_bridge.c's
 * guest object table, dispatcher objects and continuation engine.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include <string.h>
#include <stdlib.h>
extern ptrdiff_t g_xbox_mem_offset;

/* ============================================================================
 * Type Object Pointers
 *
 * Exported type objects for device and completion port types.
 * Game code rarely uses these directly - they're mainly for the
 * XDK libraries we're replacing.
 * ============================================================================ */

static ULONG g_device_object_type_data     = 0x44455643; /* 'DEVC' */
static ULONG g_completion_object_type_data = 0x434F4D50; /* 'COMP' */
static ULONG g_file_object_type_data       = 0x46494C45; /* 'FILE' */

PVOID xbox_IoDeviceObjectType     = &g_device_object_type_data;
PVOID xbox_IoCompletionObjectType = &g_completion_object_type_data;
PVOID xbox_IoFileObjectType       = &g_file_object_type_data;

/* ============================================================================
 * Symbolic Links
 *
 * The object table owns names, target references and lifetime. This existing
 * path index connects drive spellings to those same guest objects.
 * ============================================================================ */


/* The existing path map now joins to the object table by guest identity.
 * Counted names and targets live in guest allocations, carried by the same
 * RAM snapshot as the object. Slots are reused and grow only with live links. */
typedef struct XBOX_SYMLINK {uint32_t object,name,length,used;} XBOX_SYMLINK;
static XBOX_SYMLINK *g_symlinks;
static uint32_t g_symlink_count;
static char *link_pointer(uint32_t address) {return address?(char *)((uintptr_t)address+g_xbox_mem_offset):NULL;}
static XBOX_SYMLINK *xbox_find_symlink(const char *name) {
    if(!name || strlen(name)>UINT16_MAX)return NULL;
    XBOX_ANSI_STRING wanted={(USHORT)strlen(name),(USHORT)strlen(name),(PCHAR)name};
    for(uint32_t i=0;i<g_symlink_count;i++)if(g_symlinks[i].used) {
        XBOX_ANSI_STRING stored={(USHORT)g_symlinks[i].length,(USHORT)g_symlinks[i].length,link_pointer(g_symlinks[i].name)};
        if(xbox_RtlEqualString(&wanted,&stored,TRUE))return &g_symlinks[i];
    }
    return NULL;
}
const char *xbox_LookupSymbolicLink(const char *name) {
    XBOX_SYMLINK *entry=xbox_find_symlink(name);
    return entry?link_pointer(*(uint32_t *)(link_pointer(entry->object)+8)):NULL;
}
uint32_t xbox_LookupSymbolicLinkTarget(const char *name) {
    XBOX_SYMLINK *entry=xbox_find_symlink(name);
    return entry?*(uint32_t *)link_pointer(entry->object):0;
}
NTSTATUS xbox_RegisterSymbolicLink(uint32_t object,PXBOX_ANSI_STRING name) {
    uint32_t index;
    for(index=0;index<g_symlink_count;index++)if(!g_symlinks[index].object)break;
    if(index==g_symlink_count) {
        if(g_symlink_count==UINT32_MAX/sizeof *g_symlinks)return STATUS_INSUFFICIENT_RESOURCES;
        XBOX_SYMLINK *grown=realloc(g_symlinks,((size_t)g_symlink_count+1)*sizeof *grown);
        if(!grown)return STATUS_INSUFFICIENT_RESOURCES;
        g_symlinks=grown;memset(&g_symlinks[g_symlink_count++],0,sizeof *g_symlinks);
    }
    uint32_t storage=xbox_HeapAlloc((uint32_t)name->Length+1,1);if(!storage)return STATUS_INSUFFICIENT_RESOURCES;
    memcpy(link_pointer(storage),name->Buffer,name->Length);link_pointer(storage)[name->Length]=0;
    g_symlinks[index]=(XBOX_SYMLINK){object,storage,name->Length,1};return 0;
}
void xbox_UnlinkSymbolicLink(uint32_t object) {
    for(uint32_t i=0;i<g_symlink_count;i++)if(g_symlinks[i].object==object)g_symlinks[i].used=0;
}
void xbox_ReleaseSymbolicLink(uint32_t object) {
    for(uint32_t i=0;i<g_symlink_count;i++)if(g_symlinks[i].object==object) {
        xbox_HeapFree(g_symlinks[i].name);memset(&g_symlinks[i],0,sizeof g_symlinks[i]);break;
    }
}
typedef struct SavedIoNamespace {uint32_t count;XBOX_SYMLINK *links;} SavedIoNamespace;
void xbox_DiscardIoNamespace(void *state) {
    SavedIoNamespace *saved=state;if(!saved)return;
    free(saved->links);free(saved);
}
int xbox_SaveIoNamespace(FILE *out) {
    return out && xbox_CheckpointFrozen() && fwrite("XBNAMES",7,1,out)==1 && fwrite(&g_symlink_count,4,1,out)==1 &&
        (!g_symlink_count || fwrite(g_symlinks,sizeof *g_symlinks,g_symlink_count,out)==g_symlink_count);
}
void *xbox_PrepareIoNamespace(FILE *in) {
    if(!in || !xbox_CheckpointFrozen())return NULL;
    char magic[7];uint32_t count;
    if(fread(magic,7,1,in)!=1 || memcmp(magic,"XBNAMES",7) || fread(&count,4,1,in)!=1 ||
       count>xbox_GetMappedSize()/sizeof(XBOX_SYMLINK))return NULL;
    SavedIoNamespace *saved=calloc(1,sizeof *saved);if(!saved)return NULL;
    saved->count=count;saved->links=count?malloc((size_t)count*sizeof *saved->links):NULL;
    int ok=!count || (saved->links && fread(saved->links,sizeof *saved->links,count,in)==count);
    for(uint32_t i=0;ok && i<count;i++) {
        XBOX_SYMLINK *entry=&saved->links[i];ok=entry->used<=1;
        if(entry->object) {
            ok=ok && entry->object>=0x1000 && (uint64_t)entry->object+12<=xbox_GetMappedSize() &&
                entry->name>=0x1000 && entry->length && entry->length<=UINT16_MAX &&
                (uint64_t)entry->name+entry->length+1<=xbox_GetMappedSize() && !link_pointer(entry->name)[entry->length];
        } else ok=ok && !entry->name && !entry->length && !entry->used;
    }
    if(!ok){xbox_DiscardIoNamespace(saved);return NULL;}return saved;
}
void xbox_ApplyIoNamespace(void *state) {
    SavedIoNamespace *saved=state;
    free(g_symlinks);g_symlinks=saved->links;g_symlink_count=saved->count;saved->links=NULL;
}
