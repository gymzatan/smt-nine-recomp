#include "kernel.h"
#include "xbox_memory_layout.h"
#include "recomp_continuation.h"
#include "apu/apu.h"
#include "usb/ohci.h"
#include "video/video_player.h"
#include <stdlib.h>
#include <string.h>
extern MCPXAPUState *g_apu_state;

enum { MACHINE_CREATION=1,MACHINE_MEMORY,MACHINE_THREADS,MACHINE_OBJECTS,
       MACHINE_DEVICES,MACHINE_SCHEDULER,MACHINE_USB,MACHINE_GPU,MACHINE_APU,MACHINE_FRONTEND,
       MACHINE_SECTIONS=MACHINE_FRONTEND };
typedef struct MachineHeader {
    char magic[8];uint32_t sections,pointer_bytes,devices;
    uint8_t executable[20],image[20];
} MachineHeader;
typedef struct MachineSection {uint32_t id;uint64_t bytes;uint8_t digest[20];} MachineSection;
static uint8_t s_executable_digest[20],s_image_digest[20];
static int s_machine_identity;
static int machine_hash_file(FILE *file,uint8_t *digest,uint64_t *bytes) {
    XBOX_SHA_CONTEXT hash;unsigned char buffer[65536];size_t count;
    if(_fseeki64(file,0,SEEK_SET))return 0;xbox_XcSHAInit(&hash);*bytes=0;
    while((count=fread(buffer,1,sizeof buffer,file))!=0) {
        xbox_XcSHAUpdate(&hash,buffer,(ULONG)count);*bytes+=count;
    }
    if(ferror(file))return 0;xbox_XcSHAFinal(&hash,digest);return _fseeki64(file,0,SEEK_SET)==0;
}
int xbox_CheckpointSetGameImage(const void *data,size_t bytes) {
    if(!data || !bytes || bytes>UINT32_MAX)return 0;
    XBOX_SHA_CONTEXT hash;xbox_XcSHAInit(&hash);xbox_XcSHAUpdate(&hash,data,(ULONG)bytes);
    xbox_XcSHAFinal(&hash,s_image_digest);
    wchar_t path[MAX_PATH];DWORD length=GetModuleFileNameW(NULL,path,MAX_PATH);uint64_t executable_bytes;
    if(!length || length>=MAX_PATH)return 0;FILE *file=_wfopen(path,L"rb");if(!file)return 0;
    int ok=machine_hash_file(file,s_executable_digest,&executable_bytes);fclose(file);
    s_machine_identity=ok;return ok;
}
static int machine_write_section(FILE *out,unsigned id) {
    FILE *payload=tmpfile();if(!payload)return 0;int ok=0;
    switch(id) {
    case MACHINE_CREATION: {
        XboxThreadCreationState state;xbox_CaptureThreadCreationState(&state);
        ok=fwrite(&state,sizeof state,1,payload)==1;break;
    }
    case MACHINE_MEMORY:ok=xbox_SavePhysicalMemory(payload);break;
    case MACHINE_THREADS:ok=xbox_CheckpointWriteThreads(payload);break;
    case MACHINE_OBJECTS:ok=xbox_CheckpointWriteObjects(payload);break;
    case MACHINE_DEVICES:ok=xbox_CheckpointWriteDevices(payload);break;
    case MACHINE_SCHEDULER:ok=xbox_SaveKernelScheduler(payload);break;
    case MACHINE_USB:ok=xbox_OhciSaveCheckpoint(payload);break;
    case MACHINE_GPU:ok=nv2a_gpu_save_checkpoint(payload);break;
    case MACHINE_APU:ok=mcpx_apu_save_checkpoint(g_apu_state,payload);break;
    case MACHINE_FRONTEND:ok=xbox_FramebufferSaveCheckpoint(payload);break;
    }
    MachineSection section={0};section.id=id;
    if(ok)ok=machine_hash_file(payload,section.digest,&section.bytes) && section.bytes &&
        fwrite(&section,sizeof section,1,out)==1;
    unsigned char buffer[65536];uint64_t remaining=section.bytes;
    while(ok && remaining) {
        size_t count=remaining<sizeof buffer?(size_t)remaining:sizeof buffer;
        ok=fread(buffer,count,1,payload)==1 && fwrite(buffer,count,1,out)==1;remaining-=count;
    }
    fclose(payload);return ok;
}
int xbox_CheckpointSaveMachine(FILE *out,DWORD timeout) {
    if(!out || !s_machine_identity || !xbox_CheckpointBegin(timeout))return 0;
    MachineHeader header={0};memcpy(header.magic,"XBMACH",6);header.sections=MACHINE_SECTIONS;
    header.pointer_bytes=sizeof(void *);header.devices=xbox_CheckpointDeviceMask();
    memcpy(header.executable,s_executable_digest,20);memcpy(header.image,s_image_digest,20);
    int ok=fwrite(&header,sizeof header,1,out)==1;
    for(unsigned id=1;ok && id<=MACHINE_SECTIONS;id++)ok=machine_write_section(out,id);
    xbox_CheckpointEnd();return ok;
}
static int machine_read_sections(FILE *in,const MachineHeader *header,FILE **payloads) {
    unsigned char buffer[65536];
    for(unsigned id=1;id<=MACHINE_SECTIONS;id++) {
        MachineSection section;
        if(fread(&section,sizeof section,1,in)!=1 || section.id!=id || !section.bytes)return 0;
        FILE *payload=payloads[id]=tmpfile();if(!payload)return 0;
        XBOX_SHA_CONTEXT hash;xbox_XcSHAInit(&hash);uint64_t remaining=section.bytes;
        while(remaining) {
            size_t count=remaining<sizeof buffer?(size_t)remaining:sizeof buffer;
            if(fread(buffer,count,1,in)!=1 || fwrite(buffer,count,1,payload)!=1)return 0;
            xbox_XcSHAUpdate(&hash,buffer,(ULONG)count);remaining-=count;
        }
        uint8_t digest[20];xbox_XcSHAFinal(&hash,digest);
        if(memcmp(digest,section.digest,20) || _fseeki64(payload,0,SEEK_SET))return 0;
    }
    (void)header;return fgetc(in)==EOF && !ferror(in);
}
static int machine_payload_done(FILE *file) {return fgetc(file)==EOF && !ferror(file);}
int xbox_CheckpointLoadMachine(FILE *in,int cold,DWORD timeout) {
    if(!in || !s_machine_identity || (cold!=0 && cold!=1))return 0;
    MachineHeader header;char magic[8]={"XBMACH"};FILE *payloads[MACHINE_SECTIONS+1]={0};
    int ok=fread(&header,sizeof header,1,in)==1 && !memcmp(header.magic,magic,8) &&
        header.sections==MACHINE_SECTIONS && header.pointer_bytes==sizeof(void *) &&
        !memcmp(header.executable,s_executable_digest,20) && !memcmp(header.image,s_image_digest,20) &&
        !(header.devices&~((1u<<XBOX_CHECKPOINT_DEVICE_COUNT)-2)) && machine_read_sections(in,&header,payloads);
    int frozen=0,committed=0;void *threads=NULL;XboxCheckpointRoot roots[64];unsigned count=0;
    if(ok && cold) {
        if(header.devices&(1u<<XBOX_CHECKPOINT_TIMER))xbox_CheckpointStartKernelDevices();
        if(header.devices&(1u<<XBOX_CHECKPOINT_USB))xbox_OhciInit();
        if(header.devices&(1u<<XBOX_CHECKPOINT_FRAMEBUFFER))xbox_FramebufferWindowStart();
        ULONGLONG began=GetTickCount64();
        while(xbox_CheckpointDeviceMask()!=header.devices && GetTickCount64()-began<timeout)Sleep(1);
    }
    if(ok)ok=xbox_CheckpointDeviceMask()==header.devices;
    if(ok){frozen=cold?xbox_CheckpointBootstrapBegin():xbox_CheckpointBegin(timeout);ok=frozen;}
    XboxThreadCreationState creation;
    if(ok)ok=fread(&creation,sizeof creation,1,payloads[MACHINE_CREATION])==1 &&
        machine_payload_done(payloads[MACHINE_CREATION]) && creation.call_count>=0 && creation.mode>=0 && creation.mode<=1;
    if(ok)ok=(threads=xbox_CheckpointPrepareThreads(payloads[MACHINE_THREADS],64))!=NULL &&
        machine_payload_done(payloads[MACHINE_THREADS]);
    if(ok)ok=xbox_CheckpointQuiesceThreads(threads);
    if(ok) {
        committed=1;
        ok=xbox_LoadPhysicalMemory(payloads[MACHINE_MEMORY]) && machine_payload_done(payloads[MACHINE_MEMORY]) &&
            xbox_RestoreThreadCreationState(&creation) && xbox_CheckpointApplyThreads(threads,roots,&count);
    }
    if(ok)ok=xbox_CheckpointLoadObjects(payloads[MACHINE_OBJECTS],roots,count) && machine_payload_done(payloads[MACHINE_OBJECTS]);
    if(ok)ok=xbox_LoadKernelScheduler(payloads[MACHINE_SCHEDULER]) && machine_payload_done(payloads[MACHINE_SCHEDULER]);
    if(ok)ok=xbox_CheckpointLoadDevices(payloads[MACHINE_DEVICES]) && machine_payload_done(payloads[MACHINE_DEVICES]);
    if(ok)ok=xbox_OhciLoadCheckpoint(payloads[MACHINE_USB]) && machine_payload_done(payloads[MACHINE_USB]);
    if(ok)ok=nv2a_gpu_load_checkpoint(payloads[MACHINE_GPU]) && machine_payload_done(payloads[MACHINE_GPU]);
    if(ok)ok=mcpx_apu_load_checkpoint(g_apu_state,payloads[MACHINE_APU]) && machine_payload_done(payloads[MACHINE_APU]);
    if(ok)ok=xbox_FramebufferLoadCheckpoint(payloads[MACHINE_FRONTEND]) && machine_payload_done(payloads[MACHINE_FRONTEND]);
    if(committed && !ok)recomp_cont_invalid();
    if(threads)xbox_CheckpointFreeThreads(threads);
    if(committed)for(unsigned i=0;i<count;i++)CloseHandle(roots[i].handle);
    if(frozen)xbox_CheckpointEnd();
    for(unsigned id=1;id<=MACHINE_SECTIONS;id++)if(payloads[id])fclose(payloads[id]);return ok;
}
