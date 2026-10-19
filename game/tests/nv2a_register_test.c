#include "nv2a_state.h"
#include "nv2a_mmio_hook.h"
#include <assert.h>
#include <stddef.h>
#include <math.h>
#include "nv2a_vsh_interp.h"
#include "xbox_memory_layout.h"
#include "../../vendor/xboxrecomp/src/d3d/d3d8_internal.h"
#include "../../vendor/xboxrecomp/src/d3d/d3d8_vsh.h"
#include <d3dcompiler.h>
/* Isolated host-memory/window fixtures; the GPU and pixel consumer below
 * are the same compiled sources as the game executable. */
static HANDLE s_fixture_section;
static uint64_t decoder_nv_read(void *gpu,uint32_t off,unsigned size) { return nv2a_mmio_read(gpu,off,size); }
static void decoder_nv_write(void *gpu,uint32_t off,uint64_t value,unsigned size) { nv2a_mmio_write(gpu,off,value,size); }
static void check_compiled_signed_mmio(NV2AState *gpu) {
    nv2a_mmio_write(gpu,0x6013D4,0x1F,1);
    nv2a_mmio_write(gpu,0x6013D5,0x91,1);
    const uint8_t code[]={0x0F,0xBE,0x14,0x01};
    CONTEXT ctx={0};ctx.Rip=(DWORD64)(uintptr_t)code;ctx.Rdx=~0ULL;ctx.EFlags=0x246;
    assert(xbox_mmio_decode(&ctx,0x6013D5,0,gpu,decoder_nv_read,decoder_nv_write));
    assert(ctx.Rdx==0xFFFFFF91ULL && ctx.Rip==(DWORD64)(uintptr_t)code+sizeof code && ctx.EFlags==0x246);
    const uint8_t wide[]={0x4C,0x0F,0xBE,0x0C,0x01};
    ctx.Rip=(DWORD64)(uintptr_t)wide;ctx.R9=0;
    assert(xbox_mmio_decode(&ctx,0x6013D5,0,gpu,decoder_nv_read,decoder_nv_write));
    assert(ctx.R9==0xFFFFFFFFFFFFFF91ULL && ctx.Rip==(DWORD64)(uintptr_t)wide+sizeof wide);
    fprintf(stderr,"[MMIO] original NV2A signed byte, SIB, destination width and flags passed\n");
}
static uint8_t *s_fixture_ram,*s_fixture_tiled;
static size_t s_fixture_size;
extern int nv2a_pb_handle_memory_fault(uintptr_t,int);
extern void nv2a_pb_release_memory_views(void);
static LONG CALLBACK fixture_memory_fault(PEXCEPTION_POINTERS exception)
{
    if(exception->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && exception->ExceptionRecord->ExceptionInformation[0]<=1 && nv2a_pb_handle_memory_fault(exception->ExceptionRecord->ExceptionInformation[1],(int)exception->ExceptionRecord->ExceptionInformation[0])) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
BOOL xbox_GetSharedMemoryViews(const void *address,size_t bytes,XboxSharedMemoryViews *views)
{
    uintptr_t p=(uintptr_t)address,b=(uintptr_t)s_fixture_ram;
    if(!s_fixture_section || p<b || p-b>s_fixture_size || bytes>s_fixture_size-(p-b)) return FALSE;
    memset(views,0,sizeof *views);views->section=s_fixture_section;views->view_size=s_fixture_size;views->offset=p-b;
    views->cpu_views[0]=s_fixture_ram;views->cpu_views[1]=s_fixture_tiled;views->view_count=2;return TRUE;
}
static uint8_t *fixture_shared_ram(size_t bytes)
{
    s_fixture_section=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,(DWORD)bytes,NULL);assert(s_fixture_section);
    s_fixture_ram=MapViewOfFile(s_fixture_section,FILE_MAP_ALL_ACCESS,0,0,bytes);
    s_fixture_tiled=MapViewOfFile(s_fixture_section,FILE_MAP_ALL_ACCESS,0,0,bytes);
    assert(s_fixture_ram && s_fixture_tiled);s_fixture_size=bytes;
    assert(AddVectoredExceptionHandler(1,fixture_memory_fault));return s_fixture_ram;
}
ptrdiff_t g_xbox_mem_offset;
int xbox_CheckpointFrozen(void) { return 0; }
static SRWLOCK s_fixture_devices=SRWLOCK_INIT;
void xbox_CheckpointDeviceEnter(unsigned device) { (void)device;AcquireSRWLockShared(&s_fixture_devices); }
void xbox_CheckpointDeviceLeave(unsigned device) { (void)device;ReleaseSRWLockShared(&s_fixture_devices); }
void xbox_CheckpointDeviceCreating(unsigned device) { (void)device; }
void xbox_CheckpointDeviceCreated(unsigned device) { (void)device; }
static int s_test_clock;
static LONGLONG s_test_counter;
void xbox_RuntimePerformanceCounter(LARGE_INTEGER *counter) {
    if(s_test_clock) counter->QuadPart=s_test_counter;
    else QueryPerformanceCounter(counter);
}
uint32_t g_xbox_image_lo, g_xbox_image_hi;
ptrdiff_t xbox_GetMemoryOffset(void) { return g_xbox_mem_offset; }
void xbox_FramebufferWindowSet(uint32_t address, uint32_t pitch) { (void)address; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t address, uint32_t pitch) { (void)address; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
extern int nv2a_pb_exec_method(uint32_t, uint32_t, uint32_t, uint8_t *);
extern void nv2a_pb_publish(void);
static uint32_t calls, methods[16], parameters[16];
static void observe_pci_irq(void *opaque, int level)
{
    unsigned *levels = opaque;
    ++levels[level != 0];
}
static int consume(uint32_t subchannel, uint32_t method, uint32_t parameter, uint8_t *output)
{
    assert(subchannel == 0 && calls < 16);
    (void)output;methods[calls] = method; parameters[calls++] = parameter;return 0;
}

static void crt_write(NV2AState *gpu, uint8_t index, uint8_t value)
{
    nv2a_mmio_write(gpu, 0x6013D4, index, 1);
    nv2a_mmio_write(gpu, 0x6013D5, value, 1);
}

static const uint32_t s_passthrough_program[8] = {
        0, (1u << 21) | 0x1B, 2u << 26, (0xFu << 12) | (1u << 11),
        0, (1u << 21) | (3u << 9) | 0x1B, 2u << 26,
        (0xFu << 12) | (1u << 11) | (3u << 3) | 1u
    };

static const uint32_t s_screen_program[12][4] = {
        {0,0x0020001B,0x0836106C,0x2F100FF8},
        {0,0x0420061B,0x083613FC,0x5011F818},
        {0,0x002008FF,0x0836106C,0x2070F828},
        {0,0x0240081B,0x1436186C,0x2F20F824},
        {0,0x0060201B,0x2436106C,0x3070F800},
        {0,0x00200200,0x0836106C,0x2070F830},
        {0,0x00200E1B,0x0836106C,0x2070F838},
        {0,0x0020101B,0x0836106C,0x2070F840},
        {0,0x0020121B,0x0836106C,0x2070F848},
        {0,0x0020141B,0x0836106C,0x2070F850},
        {0,0x0020161B,0x0836106C,0x2070F858},
        {0,0x0020181B,0x0836106C,0x2070F861}
    };

static void check_screen_program_w(void)
{
    /* Actual NINE transformed-screen program and captured glyph attributes.
     * NV2A clamps output W before interpolation, not the incoming attribute. */
    const float scale[4] = {1,1,16777215,1};
    const float offset[4] = {0.53125f,0.53125f,0,0};
    const float input_w[] = {0.0f,-0.0f,2.0f,-2.0f,1.0e-30f,-1.0e-30f,
                            1.0e30f,-1.0e30f,INFINITY,-INFINITY};
    const float expected_w[] = {18446744073709551616.0f,-18446744073709551616.0f,
                               0.5f,-0.5f,18446744073709551616.0f,-18446744073709551616.0f,
                               5.42101086242752217e-20f,-5.42101086242752217e-20f,
                               5.42101086242752217e-20f,-5.42101086242752217e-20f};
    float in[NV2A_VSH_INPUTS][4] = {{0}};
    Nv2aVshOutput out;
    nv2a_vsh_set_start_slot(0);
    nv2a_vsh_set_constant(0, scale);
    nv2a_vsh_set_constant(1, offset);
    for (unsigned i = 0; i < 12; i++) nv2a_vsh_set_instruction(i, s_screen_program[i]);
    for (unsigned i = 0; i < NV2A_VSH_INPUTS; i++) in[i][3] = 1;
    in[0][0]=176; in[0][1]=52; in[0][2]=3.9e-5f;
    in[9][0]=0.4921875f; in[9][1]=0.7734375f;
    /* A partial upload changes the live instruction immediately; decoding
     * cannot wait for the fourth word or retain an earlier whole program. */
    in[0][3] = 2.0f;
    nv2a_vsh_set_load_slot(0);
    nv2a_vsh_program_word(s_screen_program[0][0]);
    nv2a_vsh_program_word((s_screen_program[0][1] & ~(15u << 21)) | (2u << 21));
    assert(nv2a_vsh_run(in, &out));
    assert(out.pos[0] == 176.0f * 176.0f + 0.53125f);
    nv2a_vsh_program_word(s_screen_program[0][2]);
    nv2a_vsh_program_word(s_screen_program[0][3]);
    nv2a_vsh_set_instruction(0, s_screen_program[0]);
    in[15][0] = 42.0f; in[15][1] = 23.0f;
    nv2a_vsh_set_load_slot(0);
    nv2a_vsh_program_word(s_screen_program[0][0]);
    nv2a_vsh_program_word((s_screen_program[0][1] & ~(15u << 9)) | (15u << 9));
    assert(nv2a_vsh_inputs_used() & (1u << 15));
    assert(nv2a_vsh_run(in, &out) && out.pos[0] == 42.53125f);
    nv2a_vsh_set_instruction(0, s_screen_program[0]);
    assert(!(nv2a_vsh_inputs_used() & (1u << 15)));
    for (unsigned reciprocal = 2; reciprocal <= 3; reciprocal++) {
      uint32_t instruction[4];
      memcpy(instruction, s_screen_program[1], sizeof(instruction));
      instruction[1] = (instruction[1] & ~(7u << 25)) | (reciprocal << 25);
      nv2a_vsh_set_instruction(1, instruction);
      for (unsigned i = 0; i < sizeof(input_w)/sizeof(input_w[0]); i++) {
        in[0][3] = input_w[i];
        assert(nv2a_vsh_run(in, &out));
        assert(out.pos[0] == 176.53125f && out.pos[1] == 52.53125f);
        /* RCP(-inf) produces -0; the following ADD with +0 produces +0.
         * RCC bounds the reciprocal before that ADD, retaining its sign. */
        float wanted_w = (reciprocal == 2 && i == 9) ? -expected_w[i] : expected_w[i];
        assert(out.pos[3] == wanted_w);
        assert(out.tex[0][0] == in[9][0] && out.tex[0][1] == in[9][1]);
        float iw = 1.0f / out.pos[3];
        assert(isfinite(iw) && iw != 0.0f);
        assert((out.tex[0][0] * iw) / iw == in[9][0]);
      }
    }
}

static void check_hardware_vertex_program(void)
{
    ID3D11Device *device=d3d8_GetD3D11Device();
    ID3D11DeviceContext *context=d3d8_GetD3D11Context();
    assert(device && context);
    const float input_w[]={0.0f,-0.0f,2.0f,-2.0f,1e-30f,-1e-30f,1e30f,-1e30f,INFINITY,-INFINITY};
    float input[10][16][4]={0},constants[192][4];
    Nv2aVshOutput expected[10];
    for(unsigned vertex=0;vertex<10;vertex++) {
        for(unsigned attribute=0;attribute<16;attribute++) input[vertex][attribute][3]=1;
        input[vertex][0][0]=176;input[vertex][0][1]=52;input[vertex][0][2]=3.9e-5f;
        input[vertex][0][3]=input_w[vertex];
        input[vertex][9][0]=0.4921875f;input[vertex][9][1]=0.7734375f;
    }
    const float scale[4]={1,1,16777215,1},offset[4]={0.53125f,0.53125f,0,0};
    nv2a_vsh_set_start_slot(0);nv2a_vsh_set_cxt_write(0);
    nv2a_vsh_set_constant(0,scale);nv2a_vsh_set_constant(1,offset);
    for(unsigned ilu=2;ilu<=3;ilu++) {
        uint32_t words[12][4];memcpy(words,s_screen_program,sizeof words);
        words[1][1]=(words[1][1]&~(7u<<25))|(ilu<<25);
        for(unsigned instruction=0;instruction<12;instruction++) nv2a_vsh_set_instruction(instruction,words[instruction]);
        for(unsigned vertex=0;vertex<10;vertex++) assert(nv2a_vsh_run(input[vertex],&expected[vertex]));
        for(unsigned constant=0;constant<192;constant++) memcpy(constants[constant],nv2a_vsh_constant(constant),16);
        NV2AVshProgram program;char source[131072];
        d3d8_vsh_parse((const DWORD *)words,12,&program);
        assert(program.length==12 && program.insns[0].mac_op==NV2A_VSH_MAC_MOV);
        assert(program.insns[1].ilu_op==ilu && program.insns[1].ilu_dst.temp_reg==1);
        int length=d3d8_vsh_generate_hlsl(&program,source,sizeof source);assert(length>0);
        ID3DBlob *code=NULL,*errors=NULL;
        HRESULT hr=D3DCompile(source,length,"original_nv_vsh_fixture",NULL,NULL,"main","vs_5_0",D3DCOMPILE_IEEE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(FAILED(hr)) fprintf(stderr,"[VSH GPU] compile: %s\n",errors?(char *)ID3D10Blob_GetBufferPointer(errors):"no diagnostic");
        if(errors) ID3D10Blob_Release(errors);
        assert(SUCCEEDED(hr));
        ID3D11VertexShader *shader=NULL;ID3D11GeometryShader *stream_shader=NULL;ID3D11InputLayout *layout=NULL;
        assert(SUCCEEDED(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&shader)));
        D3D11_INPUT_ELEMENT_DESC inputs[16]={0};unsigned count=0;
        for(unsigned attribute=0;attribute<16;attribute++) if(program.inputs_read&(1u<<attribute)) {
            inputs[count++]=(D3D11_INPUT_ELEMENT_DESC){"ATTR",attribute,DXGI_FORMAT_R32G32B32A32_FLOAT,0,attribute*16,D3D11_INPUT_PER_VERTEX_DATA,0};
        }
        assert(SUCCEEDED(ID3D11Device_CreateInputLayout(device,inputs,count,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),&layout)));
        D3D11_SO_DECLARATION_ENTRY outputs[]={
            {0,"SV_POSITION",0,0,4,0},{0,"COLOR",0,0,4,0},{0,"COLOR",1,0,4,0},
            {0,"FOG",0,0,1,0},{0,NULL,0,0,3,0},
            {0,"TEXCOORD",0,0,4,0},{0,"TEXCOORD",1,0,4,0},{0,"TEXCOORD",2,0,4,0},{0,"TEXCOORD",3,0,4,0}};
        UINT stride=sizeof(Nv2aVshOutput),zero=0,input_stride=sizeof input[0];
        assert(SUCCEEDED(ID3D11Device_CreateGeometryShaderWithStreamOutput(device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),outputs,sizeof outputs/sizeof outputs[0],&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,NULL,&stream_shader)));
        ID3D11Buffer *vertices=NULL,*cb=NULL,*result=NULL,*staging=NULL;
        D3D11_BUFFER_DESC desc={0};D3D11_SUBRESOURCE_DATA data={input,0,0};
        desc.ByteWidth=sizeof input;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&desc,&data,&vertices)));
        desc.ByteWidth=sizeof constants;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;data.pSysMem=constants;
        assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&desc,&data,&cb)));
        desc.ByteWidth=sizeof expected;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_STREAM_OUTPUT;
        assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&desc,NULL,&result)));
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&desc,NULL,&staging)));
        ID3D11DeviceContext_IASetInputLayout(context,layout);
        ID3D11DeviceContext_IASetVertexBuffers(context,0,1,&vertices,&input_stride,&zero);
        ID3D11DeviceContext_IASetPrimitiveTopology(context,D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ID3D11DeviceContext_VSSetShader(context,shader,NULL,0);ID3D11DeviceContext_VSSetConstantBuffers(context,1,1,&cb);
        ID3D11DeviceContext_GSSetShader(context,stream_shader,NULL,0);ID3D11DeviceContext_SOSetTargets(context,1,&result,&zero);
        ID3D11DeviceContext_Draw(context,10,0);
        ID3D11DeviceContext_SOSetTargets(context,0,NULL,NULL);ID3D11DeviceContext_GSSetShader(context,NULL,NULL,0);
        ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)staging,(ID3D11Resource *)result);
        D3D11_MAPPED_SUBRESOURCE mapped;
        assert(SUCCEEDED(ID3D11DeviceContext_Map(context,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped)));
        const float *actual=(const float *)mapped.pData,*wanted=(const float *)expected;
        for(unsigned vertex=0;vertex<10;vertex++) for(unsigned component=0;component<32;component++) {
            if(component>=13 && component<=15) continue;
            unsigned at=vertex*32+component;
            if(actual[at]!=wanted[at]) fprintf(stderr,"[VSH GPU] ilu=%u vertex=%u component=%u GPU=%.9g original=%.9g\n",ilu,vertex,component,actual[at],wanted[at]);
            assert(actual[at]==wanted[at]);
        }
        ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)staging,0);
        ID3D11Buffer_Release(vertices);ID3D11Buffer_Release(cb);ID3D11Buffer_Release(result);ID3D11Buffer_Release(staging);
        ID3D11VertexShader_Release(shader);ID3D11GeometryShader_Release(stream_shader);ID3D11InputLayout_Release(layout);ID3D10Blob_Release(code);
        fprintf(stderr,"[VSH GPU] original NINE12-instruction program ILU%u:10vertices match originalCPU outputs and signedW bounds\n",ilu);
    }
}

static int check_dialogue_panel(const char *texture_path)
{
    const uint32_t bytes = 2u * 1024u * 1024u;
    uint8_t *ram = fixture_shared_ram(bytes), ramin[4096] = {0};
    assert(ram);
    NV2AState *gpu = nv2a_init_standalone(ram, bytes, ramin, sizeof(ramin));
    assert(gpu);
    g_xbox_mem_offset = (ptrdiff_t)ram - (ptrdiff_t)0x80000000u;
    FILE *texture = fopen(texture_path, "rb");
    assert(texture && fread(ram + 0x140000, 1, 512u * 256u, texture) == 512u * 256u);
    fclose(texture);
    uint32_t *dma = (uint32_t *)(ramin + 0x100);
    dma[0] = 2; dma[1] = bytes - 1; dma[2] = 0;
    ((uint32_t *)(ramin + 0x200))[0] = NV_KELVIN_PRIMITIVE;
    gpu->pfifo.method_consumer = nv2a_pb_exec_method;
    pgraph_method(gpu, 0, 0, 0x200);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_COLOR, 0x100);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_A, 0x100);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_B, 0x100);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_VERTEX_A, 0x100);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_VERTEX_B, 0x100);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_CLIP_HORIZONTAL, 640u << 16);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_CLIP_VERTICAL, 480u << 16);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_HORIZONTAL, (590u << 16) | 47u);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_VERTICAL, 400u << 16 | 392u);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_PITCH, 2560);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_FORMAT, 8);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_COLOR_OFFSET, 0x1000);
    pgraph_method(gpu, 0, NV097_SET_COLOR_MASK, 0x01010101);
    pgraph_method(gpu, 0, NV097_SET_BLEND_ENABLE, 1);
    pgraph_method(gpu, 0, NV097_SET_BLEND_FUNC_SFACTOR, 0x302);
    pgraph_method(gpu, 0, NV097_SET_BLEND_FUNC_DFACTOR, 0x303);
    pgraph_method(gpu, 0, NV097_SET_BLEND_EQUATION, 0x8006);
    pgraph_method(gpu, 0, NV097_SET_CLIP_MAX, 0x4B7FFFFF);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_OFFSET, 0x140000);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_FORMAT, 0x08910F29);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL0, 0x4003FFC4);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_ADDRESS, 0x00010303);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_FILTER, 0x01012000);
    check_screen_program_w();
    const uint32_t original_rcp[4] = {0,0x0420061B,0x083613FC,0x5011F818};
    nv2a_vsh_set_instruction(1, original_rcp);
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, 2);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_OFFSET, 0x180000);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 0x1842);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 9 * 4, 0x180010);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 9 * 4, 0x1822);
    for (unsigned y = 392; y <= 400; ++y)
        for (unsigned x = 47; x <= 590; ++x)
            stl_le_p(ram + 0x1000 + y * 2560 + x * 4, 0xFFC7CBD8);
    /* Original NINE panel records and captured DXT5 texture. The two atlas
     * rectangles meet at x319; u272 is transparent padding. */
    for (unsigned panel = 0; panel < 2; ++panel) {
        float x = panel ? 47.0f : 319.0f, y = panel ? 0.0f : 112.0f;
        float vertices[4][6] = {
            {x,340,0.000104f,1,0,y/255.0f},
            {x+272,340,0.000104f,1,272.0f/511.0f,y/255.0f},
            {x+272,444,0.000104f,1,272.0f/511.0f,(y+104)/255.0f},
            {x,444,0.000104f,1,0,(y+104)/255.0f}
        };
        memcpy(ram + 0x180000, vertices, sizeof(vertices));
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 7);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 3u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    }
    uint32_t *row = (uint32_t *)(ram + 0x1000 + 400 * 2560);
    fprintf(stderr, "[PANEL] x318=%08X x319=%08X x320=%08X background=FFC7CBD8\n", row[318],row[319],row[320]);
    assert(row[318] != 0xFFC7CBD8 && row[319] == row[318] && row[320] == row[318]);
    row = (uint32_t *)(ram + 0x1000 + 392 * 2560);
    fprintf(stderr, "[PANEL] diagonal x182=%08X x183=%08X x184=%08X\n",row[182],row[183],row[184]);
    assert(row[182] == row[183] && row[183] == row[184]);
    return 0;
}

static int check_movie_background(void)
{
    const uint32_t bytes = 2u * 1024u * 1024u;
    uint8_t *ram = fixture_shared_ram(bytes), ramin[4096] = {0};
    assert(ram);
    NV2AState *gpu = nv2a_init_standalone(ram, bytes, ramin, sizeof(ramin));
    assert(gpu);
    g_xbox_mem_offset = (ptrdiff_t)ram - (ptrdiff_t)0x80000000u;
    uint32_t *dma = (uint32_t *)(ramin + 0x100);
    dma[0] = 2; dma[1] = bytes - 1; dma[2] = 0;
    ((uint32_t *)(ramin + 0x200))[0] = NV_KELVIN_PRIMITIVE;
    gpu->pfifo.method_consumer = nv2a_pb_exec_method;
    pgraph_method(gpu, 0, 0, 0x200);
    pgraph_method(gpu, 0, NV097_SET_POINT_SMOOTH_ENABLE, 1);
    assert(gpu->pgraph.regs[NV_PGRAPH_SETUPRASTER] & NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_COLOR, 0x100);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_VERTEX_A, 0x100);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_CLIP_HORIZONTAL, 640u << 16);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_CLIP_VERTICAL, 480u << 16);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_HORIZONTAL, 639u << 16);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_VERTICAL, 479u << 16);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_PITCH, 2560);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_FORMAT, 8);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_COLOR_OFFSET, 0x1000);
    pgraph_method(gpu, 0, NV097_SET_COLOR_MASK, 0x01010101);
    pgraph_method(gpu, 0, NV097_SET_CLIP_MIN, 0);
    pgraph_method(gpu, 0, NV097_SET_CLIP_MAX, 0x4B7FFFFF);
    pgraph_method(gpu, 0, NV097_SET_ZMIN_MAX_CONTROL, 0);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_TEST_ENABLE, 0);
    pgraph_method(gpu, 0, NV097_SET_BLEND_ENABLE, 0);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_CONTROL, 1);
    pgraph_method(gpu, 0, NV097_SET_SHADER_STAGE_PROGRAM, 0);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_COLOR_ICW, 0x04200000);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_ALPHA_ICW, 0x14200000);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_COLOR_OCW, 0x00000C00);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_ALPHA_OCW, 0x00000C00);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x0000000C);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001C80);
    check_screen_program_w();
    const uint32_t original_rcp[4] = {0,0x0420061B,0x083613FC,0x5011F818};
    nv2a_vsh_set_instruction(1, original_rcp);
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, 2);
    for(unsigned stage=0;stage<4;stage++) {
        for(unsigned component=0;component<4;component++) {
            pgraph_method(gpu,0,NV097_SET_TEXGEN_S+stage*16+component*4,NV097_SET_TEXGEN_S_EYE_LINEAR);
            uint32_t mask=NV_PGRAPH_CSV1_A_T0_S<<(component*3+(stage%2)*16);
            assert(GET_MASK(gpu->pgraph.regs[stage<2?NV_PGRAPH_CSV1_A:NV_PGRAPH_CSV1_B],mask)==NV_PGRAPH_CSV1_A_T0_S_EYE_LINEAR);
        }
        pgraph_method(gpu,0,NV097_SET_TEXTURE_MATRIX_ENABLE+stage*4,1);
        assert(gpu->pgraph.texture_matrix_enable[stage]);
    }
    pgraph_method(gpu,0,NV097_SET_FOG_GEN_MODE,NV097_SET_FOG_GEN_MODE_V_PLANAR);
    assert(GET_MASK(gpu->pgraph.regs[NV_PGRAPH_CSV0_D],NV_PGRAPH_CSV0_D_FOGGENMODE)==NV_PGRAPH_CSV0_D_FOGGENMODE_PLANAR);
    struct vertex_fixture { float x,y,z,w; uint32_t colour; } vertices[4] = {
        {0,0,1,1,0xFF000000}, {640,0,1,1,0xFF000000},
        {0,480,1,1,0xFF000000}, {640,480,1,1,0xFF000000}
    };
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_OFFSET, 0x180000);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 0x1442);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, 0x180010);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, 0x1440);
    /* Captured NINE movie clear: strip at .53125, constant far depth
     * 16777215, no texture or depth test, original black combiner. */
    for (unsigned boundary = 0; boundary < 3; ++boundary) {
        float z = boundary == 0 ? 1.0f : boundary == 1 ? 0.0f : 1.25f;
        for (unsigned i = 0; i < 4; ++i) vertices[i].z = z;
        memcpy(ram + 0x180000, vertices, sizeof(vertices));
        memset(ram + 0x1000, 0xA5, 640u * 480u * 4u);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 6);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 3u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        nv2a_pb_publish();
        unsigned changed = 0;
        for (unsigned i = 0; i < 640u * 480u; ++i)
            changed += ldl_le_p(ram + 0x1000 + i * 4) == 0xFF000000;
        fprintf(stderr, "[MOVIE] boundary=%u black pixels=%u expected=%u\n",
                boundary, changed, boundary < 2 ? 640u * 480u : 0);
        assert(changed == (boundary < 2 ? 640u * 480u : 0));
    }
    /* Publishing a small GPU draw must not replace a later CPU write to a
     * pixel outside the original raster bounds. The next draw also consumes
     * that same physical colour storage. */
    for (unsigned i = 0; i < 4; ++i) {
        vertices[i].x = 20.0f + (i & 1) * 20.0f;
        vertices[i].y = 30.0f + (i >> 1) * 20.0f;
        vertices[i].z = 0.5f;
    }
    memset(ram + 0x1000, 0xA5, 640u * 480u * 4u);
    for (unsigned draw = 0; draw < 2; ++draw) {
        memcpy(ram + 0x180000, vertices, sizeof(vertices));
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 6);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 3u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        unsigned x = draw ? 222 : 22;
        stl_le_p(ram + 0x1000 + 400 * 2560 + 600 * 4, 0x12345678);
        stl_le_p(ram + 0x1000 + 32 * 2560 + x * 4, 0x87654321);
        nv2a_pb_publish();
        uint32_t inside=ldl_le_p(ram + 0x1000 + 32 * 2560 + x * 4);
        fprintf(stderr,"[SURFACE] laterCPUinside=%08X expected=87654321\n",inside);
        assert(inside == 0x87654321);
        assert(ldl_le_p(ram + 0x1000) == 0xA5A5A5A5);
        uint32_t outside = ldl_le_p(ram + 0x1000 + 400 * 2560 + 600 * 4);
        fprintf(stderr, "[SURFACE] draw=%u outside CPU pixel=%08X expected=12345678\n", draw, outside);
        assert(outside == 0x12345678);
        for (unsigned i = 0; i < 4; ++i) vertices[i].x += 200.0f;
    }
    /* Last CPU store remains a store when its value equals the older RAM
     * contents. GPU black was submitted between the two equal CPU values. */
    for(unsigned i=0;i<4;i++) { vertices[i].x=20.0f+(i&1)*20.0f;vertices[i].y=30.0f+(i>>1)*20.0f;vertices[i].z=0.5f; }
    memset(ram+0x1000,0xA5,640u*480u*4u);memcpy(ram+0x180000,vertices,sizeof vertices);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,6);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,3u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);
    stl_le_p(ram+0x1000+32*2560+22*4,0xA5A5A5A5);
    nv2a_pb_publish();
    uint32_t same_value=ldl_le_p(ram+0x1000+32*2560+22*4);
    fprintf(stderr,"[CPU STORE] same earlierRAMvalue=%08X expected=A5A5A5A5\n",same_value);
    assert(same_value==0xA5A5A5A5);
    /* The real completion semaphore orders both CPU aliases of the same
     * physical colour data. A later store through the tiled view is visible
     * through the original view, without a separate copy or guessed value. */
    memset(ram+0x1000,0xA5,640u*480u*4u);memcpy(ram+0x180000,vertices,sizeof vertices);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,6);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,3u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);
    pgraph_method(gpu,0,NV097_SET_CONTEXT_DMA_SEMAPHORE,0x100);
    pgraph_method(gpu,0,NV097_SET_SEMAPHORE_OFFSET,0x1F0000);
    pgraph_method(gpu,0,NV097_BACK_END_WRITE_SEMAPHORE_RELEASE,0x71A5C301);
    assert(ldl_le_p(ram+0x1F0000)==0x71A5C301);
    assert(ldl_le_p(s_fixture_tiled+0x1000+32*2560+22*4)==0xFF000000);
    assert(ldl_le_p(ram+0x1000+32*2560+22*4)==0xFF000000);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,6);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,3u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);
    stl_le_p(s_fixture_tiled+0x1000+32*2560+22*4,0x87654321);
    nv2a_pb_publish();
    assert(ldl_le_p(ram+0x1000+32*2560+22*4)==0x87654321);
    assert(!nv2a_pb_handle_memory_fault((uintptr_t)&same_value,0));
    fprintf(stderr,"[MEMORY] actualGPU semaphore and primary/tiled reads/writes preserve one backing\n");
    /* Retain pending draws across the original 4MB vertex stream wrap.
     * Each batch has different positions, so overwriting an in-flight range
     * loses earlier triangles rather than merely changing an upload count. */
    memset(ram + 0x1000, 0xA5, 640u * 480u * 4u);
    for (unsigned draw = 0; draw < 9; ++draw) {
        for (unsigned i = 0; i < 4095; ++i) {
            unsigned corner = i % 3;
            float position[4] = {20.0f + draw * 60.0f + (corner == 1 ? 20.0f : 0.0f),
                                 30.0f + (corner == 2 ? 20.0f : 0.0f),0.5f,1.0f};
            memcpy(ram + 0x180000 + i * 20, position, sizeof(position));
            stl_le_p(ram + 0x180000 + i * 20 + 16, 0xFF000000);
        }
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        for (unsigned first = 0; first < 4095; first += 256) {
            unsigned count = 4095 - first;
            if (count > 256) count = 256;
            pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, ((count - 1) << 24) | first);
        }
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    }
    stl_le_p(ram + 0x1000 + 400 * 2560 + 600 * 4, 0x12345678);
    nv2a_pb_publish();
    for (unsigned draw = 0; draw < 9; ++draw)
        assert(ldl_le_p(ram + 0x1000 + 32 * 2560 + (22 + draw * 60) * 4) == 0xFF000000);
    assert(ldl_le_p(ram + 0x1000 + 400 * 2560 + 600 * 4) == 0x12345678);
    fprintf(stderr,"[STREAM] nine pending 4095-vertex batches retain all positions across original stream wrap\n");
    for(unsigned stage=0;stage<4;stage++) {
        for(unsigned component=0;component<4;component++)
            pgraph_method(gpu,0,NV097_SET_TEXGEN_S+stage*16+component*4,NV097_SET_TEXGEN_S_DISABLE);
        pgraph_method(gpu,0,NV097_SET_TEXTURE_MATRIX_ENABLE+stage*4,0);
    }
    pgraph_method(gpu,0,NV097_SET_FOG_GEN_MODE,NV097_SET_FOG_GEN_MODE_V_SPEC_ALPHA);
    fprintf(stderr,"[VERTEX STATE] originalprogram outputs preserved withstored TexGen/texturematrix/foggen states; disabledbeforefixed consumer\n");
    /* One physical publication consumes both pending GPU program bounds and
     * the original fixed screen triangle bounds, including CPU later writes. */
    for(unsigned draw=0;draw<2;draw++) {
        for(unsigned i=0;i<4;i++) {
            vertices[i].x=20.0f+draw*200.0f+(i&1)*20.0f;
            vertices[i].y=30.0f+(i>>1)*20.0f;vertices[i].z=0.5f;vertices[i].w=1;vertices[i].colour=0xFF000000;
        }
        if(!draw) memset(ram+0x1000,0xA5,640u*480u*4u);
        memcpy(ram+0x180000,vertices,sizeof vertices);
        pgraph_method(gpu,0,NV097_SET_TRANSFORM_EXECUTION_MODE,draw?0:2);
        pgraph_method(gpu,0,NV097_SET_BEGIN_END,6);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,3u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);
    }
    stl_le_p(ram+0x1000+400*2560+600*4,0x12345678);
    nv2a_pb_publish();
    assert(ldl_le_p(ram+0x1000+32*2560+22*4)==0xFF000000);
    assert(ldl_le_p(ram+0x1000+32*2560+222*4)==0xFF000000);
    assert(ldl_le_p(ram+0x1000+400*2560+600*4)==0x12345678);
    pgraph_method(gpu,0,NV097_SET_TRANSFORM_EXECUTION_MODE,2);
    fprintf(stderr,"[BOUNDS] pending GPU program and original fixed triangles publish both regions\n");
    /* Existing passthrough program exercises native setup independently of
     * the transformed-screen epilogue: provoking identity, cull and near W. */
    for(unsigned i=0;i<2;i++) nv2a_vsh_set_instruction(i,s_passthrough_program+i*4);
    const float unit[4]={1,1,1,1},zero[4]={0};
    nv2a_vsh_set_constant(58,unit);nv2a_vsh_set_constant(59,zero);
    vertices[0]=(struct vertex_fixture){20,20,0.5f,1,0xFFFF0000};
    vertices[1]=(struct vertex_fixture){60,20,0.5f,1,0xFF00FF00};
    vertices[2]=(struct vertex_fixture){20,60,0.5f,1,0xFF0000FF};
    vertices[3]=(struct vertex_fixture){60,60,0.5f,1,0xFFFFFFFF};
    pgraph_method(gpu,0,NV097_SET_SHADE_MODE,NV097_SET_SHADE_MODE_V_FLAT);
    for(unsigned first=0;first<2;first++) {
        pgraph_method(gpu,0,NV097_SET_PROVOKING_VERTEX,first?NV097_SET_PROVOKING_VERTEX_FIRST:NV097_SET_PROVOKING_VERTEX_LAST);
        memset(ram+0x1000,0xA5,640u*480u*4u);memcpy(ram+0x180000,vertices,sizeof vertices);
        pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
        assert(ldl_le_p(ram+0x1000+22*2560+22*4)==(first?0xFFFF0000:0xFF0000FF));
    }
    vertices[2].x=60;vertices[3].x=20;memcpy(ram+0x180000,vertices,sizeof vertices);
    memset(ram+0x1000,0xA5,640u*480u*4u);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,8);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,3u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x1000+22*2560+22*4)==0xFFFFFFFF);
    assert(ldl_le_p(ram+0x1000+48*2560+48*4)==0xFFFFFFFF);
    vertices[2].x=20;memcpy(ram+0x180000,vertices,sizeof vertices);
    pgraph_method(gpu,0,NV097_SET_CULL_FACE_ENABLE,1);pgraph_method(gpu,0,NV097_SET_CULL_FACE,NV097_SET_CULL_FACE_V_BACK);
    for(unsigned cw=0;cw<2;cw++) {
        pgraph_method(gpu,0,NV097_SET_FRONT_FACE,cw?NV097_SET_FRONT_FACE_V_CW:NV097_SET_FRONT_FACE_V_CCW);
        memset(ram+0x1000,0xA5,640u*480u*4u);
        pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
        assert(ldl_le_p(ram+0x1000+22*2560+22*4)==(cw?0xFFFF0000:0xA5A5A5A5));
    }
    pgraph_method(gpu,0,NV097_SET_CULL_FACE_ENABLE,0);
    vertices[2].w=-1;memcpy(ram+0x180000,vertices,sizeof vertices);memset(ram+0x1000,0xA5,640u*480u*4u);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x1000+18*2560+22*4)==0xFFFF0000);
    assert(ldl_le_p(ram+0x1000+22*2560+22*4)==0xA5A5A5A5);
    fprintf(stderr,"[SETUP] existing program provoking first/last, quad fourth colour, both front faces and mixed near-W passed\n");
    /* Original logical depth identity is independent of colour viewport
     * size. Smaller colour targets retain depth written in the same region. */
    pgraph_method(gpu,0,NV097_SET_CONTEXT_DMA_ZETA,0x100);
    pgraph_method(gpu,0,NV097_SET_SURFACE_ZETA_OFFSET,0);
    pgraph_method(gpu,0,NV097_SET_SURFACE_FORMAT,0x28);
    pgraph_method(gpu,0,NV097_SET_DEPTH_TEST_ENABLE,1);
    pgraph_method(gpu,0,NV097_SET_DEPTH_FUNC,0x201);
    pgraph_method(gpu,0,NV097_SET_DEPTH_MASK,1);
    pgraph_method(gpu,0,NV097_SET_ZSTENCIL_CLEAR_VALUE,0xFFFFFFFF);
    pgraph_method(gpu,0,NV097_SET_CLEAR_RECT_HORIZONTAL,639u<<16);
    pgraph_method(gpu,0,NV097_SET_CLEAR_RECT_VERTICAL,479u<<16);
    pgraph_method(gpu,0,NV097_CLEAR_SURFACE,3);
    vertices[2].w=1;
    for(unsigned i=0;i<3;i++) vertices[i].z=0.5f;
    memcpy(ram+0x180000,vertices,sizeof vertices);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    pgraph_method(gpu,0,NV097_SET_SURFACE_CLIP_HORIZONTAL,256u<<16);
    pgraph_method(gpu,0,NV097_SET_SURFACE_CLIP_VERTICAL,256u<<16);
    pgraph_method(gpu,0,NV097_SET_SURFACE_PITCH,1024);
    pgraph_method(gpu,0,NV097_SET_SURFACE_COLOR_OFFSET,0x130000);
    memset(ram+0x130000,0xA5,256u*256u*4u);
    for(unsigned i=0;i<3;i++) vertices[i].z=0.25f;
    memcpy(ram+0x180000,vertices,sizeof vertices);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x130000+22*1024+22*4)==0xFFFF0000);
    memset(ram+0x130000,0xA5,256u*256u*4u);
    for(unsigned i=0;i<3;i++) vertices[i].z=0.75f;
    memcpy(ram+0x180000,vertices,sizeof vertices);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x130000+22*1024+22*4)==0xA5A5A5A5);
    pgraph_method(gpu,0,NV097_SET_CLEAR_RECT_HORIZONTAL,22u|(22u<<16));
    pgraph_method(gpu,0,NV097_SET_CLEAR_RECT_VERTICAL,22u|(22u<<16));
    pgraph_method(gpu,0,NV097_SET_ZSTENCIL_CLEAR_VALUE,0xFFFFFFFF);
    pgraph_method(gpu,0,NV097_CLEAR_SURFACE,1);
    pgraph_method(gpu,0,NV097_SET_STENCIL_TEST_ENABLE,1);
    pgraph_method(gpu,0,NV097_SET_STENCIL_FUNC,0x202);
    pgraph_method(gpu,0,NV097_SET_STENCIL_FUNC_REF,0xFF);
    pgraph_method(gpu,0,NV097_SET_STENCIL_FUNC_MASK,0xFF);
    pgraph_method(gpu,0,NV097_SET_STENCIL_OP_FAIL,0x1E00);
    pgraph_method(gpu,0,NV097_SET_STENCIL_OP_ZFAIL,0x1E00);
    pgraph_method(gpu,0,NV097_SET_STENCIL_OP_ZPASS,0x1E00);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x130000+22*1024+22*4)==0xFFFF0000);
    assert(ldl_le_p(ram+0x130000+22*1024+23*4)==0xA5A5A5A5);
    pgraph_method(gpu,0,NV097_SET_ZSTENCIL_CLEAR_VALUE,0x0000003C);
    pgraph_method(gpu,0,NV097_CLEAR_SURFACE,2);
    pgraph_method(gpu,0,NV097_SET_STENCIL_FUNC_REF,0x3C);
    pgraph_method(gpu,0,NV097_SET_DEPTH_TEST_ENABLE,0);
    memset(ram+0x130000,0xA5,256u*256u*4u);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x130000+22*1024+22*4)==0xFFFF0000);
    assert(ldl_le_p(ram+0x130000+22*1024+23*4)==0xA5A5A5A5);
    pgraph_method(gpu,0,NV097_SET_STENCIL_TEST_ENABLE,0);
    pgraph_method(gpu,0,NV097_SET_DEPTH_TEST_ENABLE,1);
    pgraph_method(gpu,0,NV097_SET_DEPTH_MASK,0);
    for(unsigned i=0;i<3;i++) vertices[i].z=0.70f;
    memcpy(ram+0x180000,vertices,sizeof vertices);
    memset(ram+0x130000,0xA5,256u*256u*4u);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x130000+22*1024+22*4)==0xFFFF0000);
    assert(ldl_le_p(ram+0x130000+22*1024+23*4)==0xA5A5A5A5);
    pgraph_method(gpu,0,NV097_SET_ZSTENCIL_CLEAR_VALUE,0xFFFFFF07);
    pgraph_method(gpu,0,NV097_CLEAR_SURFACE,3);
    pgraph_method(gpu,0,NV097_SET_CLEAR_RECT_HORIZONTAL,23u|(22u<<16));
    pgraph_method(gpu,0,NV097_SET_ZSTENCIL_CLEAR_VALUE,0);
    pgraph_method(gpu,0,NV097_CLEAR_SURFACE,3);
    pgraph_method(gpu,0,NV097_SET_CLEAR_RECT_HORIZONTAL,22u|(22u<<16));
    pgraph_method(gpu,0,NV097_SET_STENCIL_TEST_ENABLE,1);
    pgraph_method(gpu,0,NV097_SET_STENCIL_FUNC_REF,7);
    for(unsigned i=0;i<3;i++) vertices[i].z=0.80f;
    memcpy(ram+0x180000,vertices,sizeof vertices);
    memset(ram+0x130000,0xA5,256u*256u*4u);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x130000+22*1024+22*4)==0xFFFF0000);
    assert(ldl_le_p(ram+0x130000+22*1024+23*4)==0xA5A5A5A5);
    pgraph_method(gpu,0,NV097_SET_STENCIL_TEST_ENABLE,0);
    pgraph_method(gpu,0,NV097_SET_DEPTH_TEST_ENABLE,0);
    fprintf(stderr,"[DEPTH VIEW] existing640x480 depth retains256x256 nearer/farther ordering\n");
    fprintf(stderr,"[DEPTH RECT] onepixel Z/stencil/both clear preserves outside andunselected plane\n");
    /* Actual NINE first-battle target: 08080224, 256-square RGB8 swizzle.
     * Morton(22,22)=0x33c and Morton(23,22)=0x33d; linear22,22 is0x5858. */
    for(unsigned i=0;i<3;i++) vertices[i].colour=0x40994422;
    memcpy(ram+0x180000,vertices,sizeof vertices);
    pgraph_method(gpu,0,NV097_SET_SURFACE_FORMAT,0x08080224);
    memset(ram+0x130000,0xA5,256u*256u*4u);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    uint32_t swizzled_pixel=ldl_le_p(ram+0x130000+0xcf0);
    fprintf(stderr,"[RGB8 SWIZZLE] actualtarget pixel=%08X expected=40994422 linear=%08X\n",swizzled_pixel,ldl_le_p(ram+0x130000+0x5858));
    assert(swizzled_pixel==0x40994422);
    assert(ldl_le_p(ram+0x130000+0x5858)==0xA5A5A5A5);
    assert(ldl_le_p(ram+0x130000+0xcf4)==0x40994422);
    pgraph_method(gpu,0,NV097_SET_CLEAR_RECT_HORIZONTAL,22u|(22u<<16));
    pgraph_method(gpu,0,NV097_SET_CLEAR_RECT_VERTICAL,22u|(22u<<16));
    pgraph_method(gpu,0,NV097_SET_COLOR_CLEAR_VALUE,0x123456AA);
    pgraph_method(gpu,0,NV097_CLEAR_SURFACE,NV097_CLEAR_SURFACE_B);
    assert(ldl_le_p(ram+0x130000+0xcf0)==0x409944AA);
    assert(ldl_le_p(ram+0x130000+0xcf4)==0x40994422);
    pgraph_method(gpu,0,NV097_CLEAR_SURFACE,NV097_CLEAR_SURFACE_A);
    assert(ldl_le_p(ram+0x130000+0xcf0)==0xFF9944AA);
    assert(ldl_le_p(ram+0x130000+0xcf4)==0x40994422);
    /* A CPU write outside the next rendered region survives GPU publication. */
    stl_le_p(ram+0x130000+0x3fffc,0x12345678);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x130000+0xcf0)==0x40994422);
    assert(ldl_le_p(ram+0x130000+0x3fffc)==0x12345678);
    pgraph_method(gpu,0,NV097_SET_SURFACE_FORMAT,0x08080124);
    memset(ram+0x130000,0xA5,256u*256u*4u);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    assert(ldl_le_p(ram+0x130000+0x5858)==0x40994422);
    assert(ldl_le_p(ram+0x130000+0xcf0)==0xA5A5A5A5);
    pgraph_method(gpu,0,NV097_SET_SURFACE_FORMAT,0x08080128);
    assert(ldl_le_p(ram+0x130000+0x5858)==0x40994422);
    fprintf(stderr,"[RGB8 TARGET] originalRGBA8draw alpha, swizzlephysicaladdress, maskedclear, outsideCPUwrite andpitchedrebind passed\n");
    const char *hardware=getenv("RECOMP_GPU_D3D11");
    if(hardware && !strcmp(hardware,"1")) {
        check_hardware_vertex_program();
        for(unsigned i=0;i<2;i++) nv2a_vsh_set_instruction(i,s_passthrough_program+i*4);
        nv2a_vsh_set_constant(58,unit);nv2a_vsh_set_constant(59,zero);
        pgraph_method(gpu,0,NV097_SET_SHADER_STAGE_PROGRAM,0);
        pgraph_method(gpu,0,NV097_SET_SHADE_MODE,NV097_SET_SHADE_MODE_V_FLAT);
        for(unsigned mode=2;mode<=4;mode++) {
            vertices[0]=(struct vertex_fixture){20.5f,100.5f,0.5f,1,0xFFFF0000};
            vertices[1]=(struct vertex_fixture){60.5f,100.5f,0.5f,1,0xFF00FF00};
            vertices[2]=(struct vertex_fixture){60.5f,120.5f,0.5f,1,0xFF0000FF};
            vertices[3]=(struct vertex_fixture){20.5f,120.5f,0.5f,1,0xFFFFFFFF};
            memcpy(ram+0x180000,vertices,sizeof vertices);memset(ram+0x130000,0xA5,256u*256u*4u);
            pgraph_method(gpu,0,NV097_SET_PROVOKING_VERTEX,NV097_SET_PROVOKING_VERTEX_LAST);
            pgraph_method(gpu,0,NV097_SET_BEGIN_END,mode);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,3u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
            uint32_t ab=ldl_le_p(ram+0x130000+100*1024+30*4),bc=ldl_le_p(ram+0x130000+110*1024+60*4),da=ldl_le_p(ram+0x130000+110*1024+20*4);
            fprintf(stderr,"[LINES] mode=%u AB=%08X BC=%08X DA=%08X\n",mode,ab,bc,da);
            assert(ab==0xFF00FF00);assert(bc==(mode==2?0xA5A5A5A5:0xFF0000FF));assert(da==(mode==3?0xFFFF0000:0xA5A5A5A5));
            assert(ldl_le_p(ram+0x130000+99*1024+30*4)==0xA5A5A5A5);
        }
        for(unsigned first=0;first<2;first++) {
            memset(ram+0x130000,0xA5,256u*256u*4u);
            pgraph_method(gpu,0,NV097_SET_PROVOKING_VERTEX,first?NV097_SET_PROVOKING_VERTEX_FIRST:NV097_SET_PROVOKING_VERTEX_LAST);
            pgraph_method(gpu,0,NV097_SET_BEGIN_END,2);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,1u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
            assert(ldl_le_p(ram+0x130000+100*1024+30*4)==(first?0xFFFF0000:0xFF00FF00));
        }
        pgraph_method(gpu,0,NV097_SET_SHADE_MODE,NV097_SET_SHADE_MODE_V_SMOOTH);
        memset(ram+0x130000,0xA5,256u*256u*4u);
        pgraph_method(gpu,0,NV097_SET_BEGIN_END,2);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,1u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
        uint32_t gradient=ldl_le_p(ram+0x130000+100*1024+40*4);
        fprintf(stderr,"[LINES] smoothgradient actual=%08X expected~FF808000\n",gradient);
        assert(abs((int)((gradient>>16)&255)-128)<=1 && abs((int)((gradient>>8)&255)-128)<=1);
        fprintf(stderr,"[LINES] independentpairs, strip/loopclosing, provokingfirst/last andsmoothgradient=%08X passed\n",gradient);
        pgraph_method(gpu,0,NV097_SET_TRANSFORM_EXECUTION_MODE,0);
        pgraph_method(gpu,0,NV097_SET_SHADE_MODE,NV097_SET_SHADE_MODE_V_FLAT);
        pgraph_method(gpu,0,NV097_SET_PROVOKING_VERTEX,NV097_SET_PROVOKING_VERTEX_LAST);
        memset(ram+0x130000,0xA5,256u*256u*4u);
        pgraph_method(gpu,0,NV097_SET_BEGIN_END,2);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,1u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
        uint32_t fixed_line=ldl_le_p(ram+0x130000+100*1024+30*4);
        fprintf(stderr,"[LINES] originalfixed vertex outputs line=%08X expected=FF00FF00\n",fixed_line);
        assert(fixed_line==0xFF00FF00);
        assert(ldl_le_p(ram+0x130000+99*1024+30*4)==0xA5A5A5A5);
    }
    nv2a_pb_release_memory_views();
    return 0;
}

static int check_point_geometry(int rejection)
{
    const uint32_t bytes=2u*1024u*1024u;
    uint8_t *ram=fixture_shared_ram(bytes),ramin[4096]={0};
    NV2AState *gpu=nv2a_init_standalone(ram,bytes,ramin,sizeof ramin);assert(gpu);
    g_xbox_mem_offset=(ptrdiff_t)ram-(ptrdiff_t)0x80000000u;
    uint32_t *dma=(uint32_t *)(ramin+0x100);dma[0]=2;dma[1]=bytes-1;
    ((uint32_t *)(ramin+0x200))[0]=NV_KELVIN_PRIMITIVE;gpu->pfifo.method_consumer=nv2a_pb_exec_method;
    pgraph_method(gpu,0,0,0x200);
    pgraph_method(gpu,0,NV097_SET_CONTEXT_DMA_COLOR,0x100);
    pgraph_method(gpu,0,NV097_SET_CONTEXT_DMA_VERTEX_A,0x100);
    pgraph_method(gpu,0,NV097_SET_SURFACE_CLIP_HORIZONTAL,640u<<16);
    pgraph_method(gpu,0,NV097_SET_SURFACE_CLIP_VERTICAL,480u<<16);
    pgraph_method(gpu,0,NV097_SET_WINDOW_CLIP_HORIZONTAL,639u<<16);
    pgraph_method(gpu,0,NV097_SET_WINDOW_CLIP_VERTICAL,479u<<16);
    pgraph_method(gpu,0,NV097_SET_SURFACE_PITCH,2560);
    pgraph_method(gpu,0,NV097_SET_SURFACE_FORMAT,8);
    pgraph_method(gpu,0,NV097_SET_SURFACE_COLOR_OFFSET,0x1000);
    pgraph_method(gpu,0,NV097_SET_COLOR_MASK,0x01010101);
    pgraph_method(gpu,0,NV097_SET_CLIP_MAX,0x4B7FFFFF);
    pgraph_method(gpu,0,NV097_SET_COMBINER_CONTROL,1);
    pgraph_method(gpu,0,NV097_SET_COMBINER_COLOR_ICW,0x04200000);
    pgraph_method(gpu,0,NV097_SET_COMBINER_ALPHA_ICW,0x14200000);
    pgraph_method(gpu,0,NV097_SET_COMBINER_COLOR_OCW,0x00000C00);
    pgraph_method(gpu,0,NV097_SET_COMBINER_ALPHA_OCW,0x00000C00);
    pgraph_method(gpu,0,NV097_SET_COMBINER_SPECULAR_FOG_CW0,0x0000000C);
    pgraph_method(gpu,0,NV097_SET_COMBINER_SPECULAR_FOG_CW1,0x00001C80);
    struct { float position[4];uint32_t colour;float size; } vertex={{-141.33f,244.924f,247.407f,1},0xFFFFFFFF,6};
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_OFFSET,0x180000);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x1842);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_OFFSET+3*4,0x180010);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_FORMAT+3*4,0x1840);
    /* Actual first POINTS input and matrices captured at the original draw.
     * The original formula yields width10 at pixel center274,150. */
    const float matrices[12][4]={
        {607.669f,-37.5966f,259.697f,351628}, {16.623f,-603.596f,169.45f,283424},
        {1.62669e6f,-1.97121e6f,1.6582e7f,1.62933e10f}, {0.096955f,-0.117489f,0.98833f,972.125f},
        {0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},
        {0.995223f,-7.06332e-11f,-0.0976311f,69.9808f}, {0.0114706f,0.993074f,0.116928f,-86.4907f},
        {0.096955f,-0.117489f,0.98833f,972.125f}, {0,0,0,1}};
    for(unsigned i=0;i<12;i++) nv2a_vsh_set_constant(i,matrices[i]);
    const float offset[4]={0.53125f,0.53125f,0,0};nv2a_vsh_set_constant(59,offset);
    const float params[8]={0,0,3.08642e-5f,64,64,64,-0.0f,0};
    for(unsigned i=0;i<8;i++) { uint32_t word;memcpy(&word,params+i,4);pgraph_method(gpu,0,NV097_SET_POINT_PARAMS+i*4,word); }
    pgraph_method(gpu,0,NV097_SET_TRANSFORM_EXECUTION_MODE,0);
    pgraph_method(gpu,0,NV097_SET_POINT_SIZE,192);
    pgraph_method(gpu,0,NV097_SET_POINT_PARAMS_ENABLE,1);
    for(unsigned test=0;test<4;test++) {
        memset(ram+0x1000,0xA5,640u*480u*4u);memcpy(ram+0x180000,&vertex,sizeof vertex);
        if(test==1) {
            pgraph_method(gpu,0,NV097_SET_POINT_SMOOTH_ENABLE,1);
            pgraph_method(gpu,0,NV097_SET_SHADER_STAGE_PROGRAM,4u<<15);
            pgraph_method(gpu,0,NV097_SET_COMBINER_COLOR_ICW,0x0B200000);
        }
        if(test==2) {
            pgraph_method(gpu,0,NV097_SET_POINT_PARAMS_ENABLE,0);
            pgraph_method(gpu,0,NV097_SET_SHADER_STAGE_PROGRAM,0);
            pgraph_method(gpu,0,NV097_SET_COMBINER_COLOR_ICW,0x04200000);
        }
        if(test==3) {
            pgraph_method(gpu,0,NV097_SET_WINDOW_CLIP_HORIZONTAL,273u<<16|270u);
            pgraph_method(gpu,0,NV097_SET_WINDOW_CLIP_VERTICAL,149u<<16|146u);
        }
        pgraph_method(gpu,0,NV097_SET_BEGIN_END,1);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,0);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
        unsigned changed=0;for(unsigned i=0;i<640u*480u;i++) changed+=ldl_le_p(ram+0x1000+i*4)!=0xA5A5A5A5;
        unsigned expected=test<2?100:test==2?576:16;
        fprintf(stderr,"[POINTS] fixed captured test=%u pixels=%u expected=%u\n",test,changed,expected);assert(changed==expected);
        if(test==0) {
            assert(ldl_le_p(ram+0x1000+145*2560+269*4)==0xFFFFFFFF);
            assert(ldl_le_p(ram+0x1000+144*2560+269*4)==0xA5A5A5A5);
            assert(ldl_le_p(ram+0x1000+154*2560+278*4)==0xFFFFFFFF);
        }
        if(test==1) {
            uint32_t lo=ldl_le_p(ram+0x1000+145*2560+269*4),hi=ldl_le_p(ram+0x1000+154*2560+278*4);
            fprintf(stderr,"[POINTS] sprite stage3 corners=%08X,%08X expected~FF0D0DFF,FFF2F2FF\n",lo,hi);
            assert(abs((int)((lo>>16)&255)-13)<=1 && abs((int)((lo>>8)&255)-13)<=1 && (lo&255)==255);
            assert(abs((int)((hi>>16)&255)-242)<=1 && abs((int)((hi>>8)&255)-242)<=1 && (hi&255)==255);
        }
    }
    pgraph_method(gpu,0,NV097_SET_WINDOW_CLIP_HORIZONTAL,639u<<16);pgraph_method(gpu,0,NV097_SET_WINDOW_CLIP_VERTICAL,479u<<16);
    float specular[4]={1,0,0,0.25f};memcpy(ram+0x180100,specular,sizeof specular);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_OFFSET+4*4,0x180100);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_FORMAT+4*4,0x1042);
    pgraph_method(gpu,0,NV097_SET_WINDOW_CLIP_HORIZONTAL,639u<<16);pgraph_method(gpu,0,NV097_SET_WINDOW_CLIP_VERTICAL,479u<<16);
    pgraph_method(gpu,0,NV097_SET_COMBINER_COLOR_ICW,0x05200000);
    pgraph_method(gpu,0,NV097_SET_COMBINER_ALPHA_ICW,0x15200000);
    const uint32_t expected_specular[3]={0xFFFF0000,0x40FF0000,0xFF000000};
    for(unsigned test=0;test<3;test++) {
        pgraph_method(gpu,0,NV097_SET_LIGHT_CONTROL,test==1?NV097_SET_LIGHT_CONTROL_ALPHA_FROM_MATERIAL_SPECULAR:0);
        uint32_t before=gpu->pgraph.regs[NV_PGRAPH_CSV0_C];
        pgraph_method(gpu,0,NV097_SET_SPECULAR_ENABLE,test<2);
        assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_C]==((before&~NV_PGRAPH_CSV0_C_SPECULAR_ENABLE)|(test<2?NV_PGRAPH_CSV0_C_SPECULAR_ENABLE:0)));
        memset(ram+0x1000,0xA5,640u*480u*4u);memcpy(ram+0x180000,&vertex,sizeof vertex);
        pgraph_method(gpu,0,NV097_SET_BEGIN_END,1);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,0);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
        uint32_t actual=ldl_le_p(ram+0x1000+150*2560+274*4);
        fprintf(stderr,"[SPECULAR] existing fixed point D1 test=%u pixel=%08X expected=%08X\n",test,actual,expected_specular[test]);assert(actual==expected_specular[test]);
    }
    pgraph_method(gpu,0,NV097_SET_LIGHT_CONTROL,0);
    pgraph_method(gpu,0,NV097_SET_COMBINER_COLOR_ICW,0x04200000);
    pgraph_method(gpu,0,NV097_SET_COMBINER_ALPHA_ICW,0x14200000);
    pgraph_method(gpu,0,NV097_SET_POINT_PARAMS_ENABLE,1);pgraph_method(gpu,0,NV097_SET_TRANSFORM_EXECUTION_MODE,2);
    const uint32_t point_program[12]={
        0,(1u<<21)|0x1B,2u<<26,(0xFu<<12)|(1u<<11),
        0,(1u<<21)|(3u<<9)|0x1B,2u<<26,(0xFu<<12)|(1u<<11)|(3u<<3),
        0,(1u<<21)|(6u<<9)|0x1B,2u<<26,(0xFu<<12)|(1u<<11)|(6u<<3)|1u};
    nv2a_vsh_set_start_slot(0);nv2a_vsh_set_cxt_write(0);for(unsigned i=0;i<3;i++) nv2a_vsh_set_instruction(i,point_program+i*4);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_OFFSET+6*4,0x180014);pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_FORMAT+6*4,0x1812);
    vertex.position[0]=100.75f;vertex.position[1]=80.75f;vertex.position[2]=500;vertex.colour=0xFF00FF00;
    pgraph_method(gpu,0,NV097_SET_LIGHTING_ENABLE,1);
    pgraph_method(gpu,0,NV097_SET_COLOR_MATERIAL,0xa5);
    memset(ram+0x1000,0xA5,640u*480u*4u);memcpy(ram+0x180000,&vertex,sizeof vertex);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,1);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,0);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    unsigned changed=0;for(unsigned i=0;i<640u*480u;i++) changed+=ldl_le_p(ram+0x1000+i*4)==0xFF00FF00;
    fprintf(stderr,"[POINTS] original program oPts=6 pixels=%u expected=36\n",changed);assert(changed==36);
    pgraph_method(gpu,0,NV097_SET_LIGHTING_ENABLE,0);
    pgraph_method(gpu,0,NV097_SET_COLOR_MATERIAL,0);

    /* Original NV LT products are not API material/light values. Exercise the
     * production fixed VS, shared geometry and physical combiner surface. */
    pgraph_method(gpu,0,NV097_SET_TRANSFORM_EXECUTION_MODE,0);
    pgraph_method(gpu,0,NV097_SET_POINT_PARAMS_ENABLE,0);
    pgraph_method(gpu,0,NV097_SET_LIGHTING_ENABLE,1);
    pgraph_method(gpu,0,NV097_SET_SPECULAR_ENABLE,0);
    pgraph_method(gpu,0,NV097_SET_NORMALIZATION_ENABLE,1);
    const float identity[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    for(unsigned i=0;i<4;i++) { nv2a_vsh_set_constant(i,identity[i]);nv2a_vsh_set_constant(8+i,identity[i]);nv2a_vsh_set_constant(12+i,identity[i]); }
    const float scene[3]={0.125f,0.25f,0},dif[3]={0.375f,0.25f,0},direction[3]={0,0,1},zero[3]={0,0,0};
    for(unsigned k=0;k<3;k++) {
        uint32_t word;memcpy(&word,scene+k,4);pgraph_method(gpu,0,NV097_SET_SCENE_AMBIENT_COLOR+k*4,word);
        memcpy(&word,zero+k,4);pgraph_method(gpu,0,NV097_SET_MATERIAL_EMISSION+k*4,word);
        for(unsigned i=0;i<8;i++) {
            pgraph_method(gpu,0,NV097_SET_LIGHT_AMBIENT_COLOR+i*128+k*4,word);
            pgraph_method(gpu,0,NV097_SET_LIGHT_SPECULAR_COLOR+i*128+k*4,word);
            memcpy(&word,dif+k,4);pgraph_method(gpu,0,NV097_SET_LIGHT_DIFFUSE_COLOR+i*128+k*4,word);
            memcpy(&word,direction+k,4);pgraph_method(gpu,0,NV097_SET_LIGHT_INFINITE_DIRECTION+i*128+k*4,word);
            memcpy(&word,zero+k,4);
        }
    }
    pgraph_method(gpu,0,NV097_SET_MATERIAL_ALPHA,0x3F000000);
    struct FixedVertex { float position[4],normal[3];uint32_t colour; } fixed_vertices[3]={
        {{40,40,0,1},{0,0,2},0xFF00FF00},{{100,40,0,1},{0,0,2},0xFF00FF00},{{40,100,0,1},{0,0,2},0xFF00FF00}};
    memcpy(ram+0x180000,fixed_vertices,sizeof fixed_vertices);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_FORMAT,0x2042);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_OFFSET+2*4,0x180010);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_FORMAT+2*4,0x2032);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_OFFSET+3*4,0x18001C);
    pgraph_method(gpu,0,NV097_SET_VERTEX_DATA_ARRAY_FORMAT+3*4,0x2040);
    for(unsigned test=0;test<6;test++) {
        unsigned light=test==2 ? 7 : 0;
        pgraph_method(gpu,0,NV097_SET_COLOR_MATERIAL,test==4 ? 0x10 : 0);
        pgraph_method(gpu,0,NV097_SET_NORMALIZATION_ENABLE,test==5 ? 0 : 1);
        pgraph_method(gpu,0,NV097_SET_LIGHT_ENABLE_MASK,test==0 ? 0 : (test==3 ? 2u : 1u)<<(light*2));
        if(test==3) {
            const float local[3]={50.53125f,50.53125f,10},att[3]={1,0,0};
            for(unsigned k=0;k<3;k++) { uint32_t word;memcpy(&word,local+k,4);pgraph_method(gpu,0,NV097_SET_LIGHT_LOCAL_POSITION+k*4,word);memcpy(&word,att+k,4);pgraph_method(gpu,0,NV097_SET_LIGHT_LOCAL_ATTENUATION+k*4,word); }
            pgraph_method(gpu,0,NV097_SET_LIGHT_LOCAL_RANGE,0x3F800000); /* outside range -> scene only */
        }
        if(rejection==1) pgraph_method(gpu,0,NV097_SET_LIGHT_ENABLE_MASK,3);
        memset(ram+0x1000,0xA5,640u*480u*4u);
        pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
        uint32_t actual=ldl_le_p(ram+0x1000+50*2560+50*4),expected=(test==0 || test==3)?0x80204000:test==4?0xFF208000:test==5?0x80DFBF00:0x80808000;
        fprintf(stderr,"[LIGHTING] fixed triangle test=%u light=%u pixel=%08X expected=%08X\n",test,light,actual,expected);
        /* The UNORM surface and interpolated colour allow one quantization unit. */
        for(unsigned k=0;k<4;k++) assert(abs((int)((actual>>(k*8))&255)-(int)((expected>>(k*8))&255))<=1);
    }

    pgraph_method(gpu,0,NV097_SET_COLOR_MATERIAL,0);
    pgraph_method(gpu,0,NV097_SET_NORMALIZATION_ENABLE,1);
    pgraph_method(gpu,0,NV097_SET_LIGHT_ENABLE_MASK,1);
    pgraph_method(gpu,0,NV097_SET_SPECULAR_ENABLE,1);
    const float half[3]={0,0,0.25f},sp[3]={0.25f,0.5f,0},coeff[6]={-0.125f,0.25f,0.25f,0,0,0};
    for(unsigned k=0;k<3;k++) { uint32_t word;memcpy(&word,half+k,4);pgraph_method(gpu,0,NV097_SET_LIGHT_INFINITE_HALF_VECTOR+k*4,word);memcpy(&word,sp+k,4);pgraph_method(gpu,0,NV097_SET_LIGHT_SPECULAR_COLOR+k*4,word); }
    for(unsigned k=0;k<6;k++) { uint32_t word;memcpy(&word,coeff+k,4);pgraph_method(gpu,0,NV097_SET_SPECULAR_PARAMS+k*4,word); }
    if(rejection==2) pgraph_method(gpu,0,NV097_SET_SPECULAR_PARAMS+4,0);
    memset(ram+0x1000,0xA5,640u*480u*4u);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,1);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,0);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    uint32_t lit_point=ldl_le_p(ram+0x1000+40*2560+40*4);
    fprintf(stderr,"[LIGHTING] fixed point exact specular power=0.5 pixel=%08X expected~809FBF00\n",lit_point);
    for(unsigned k=0;k<4;k++) assert(abs((int)((lit_point>>(k*8))&255)-(int)((0x809FBF00u>>(k*8))&255))<=1);
    /* In-range point lamp at distance10 with constant attenuation2. */
    pgraph_method(gpu,0,NV097_SET_SPECULAR_ENABLE,0);
    pgraph_method(gpu,0,NV097_SET_LIGHT_ENABLE_MASK,2);
    const float local_point[3]={40,40,10},atten[3]={2,0,0};
    for(unsigned k=0;k<3;k++) { uint32_t word;memcpy(&word,local_point+k,4);pgraph_method(gpu,0,NV097_SET_LIGHT_LOCAL_POSITION+k*4,word);memcpy(&word,atten+k,4);pgraph_method(gpu,0,NV097_SET_LIGHT_LOCAL_ATTENUATION+k*4,word); }
    pgraph_method(gpu,0,NV097_SET_LIGHT_LOCAL_RANGE,0x41A00000);
    memset(ram+0x1000,0xA5,640u*480u*4u);
    pgraph_method(gpu,0,NV097_SET_BEGIN_END,1);pgraph_method(gpu,0,NV097_DRAW_ARRAYS,0);pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);nv2a_pb_publish();
    lit_point=ldl_le_p(ram+0x1000+40*2560+40*4);
    fprintf(stderr,"[LIGHTING] fixed local point attenuation2 pixel=%08X expected~80506000\n",lit_point);
    for(unsigned k=0;k<4;k++) assert(abs((int)((lit_point>>(k*8))&255)-(int)((0x80506000u>>(k*8))&255))<=1);
    pgraph_method(gpu,0,NV097_SET_LIGHTING_ENABLE,0);
    pgraph_method(gpu,0,NV097_SET_NORMALIZATION_ENABLE,0);
    nv2a_pb_release_memory_views();return 0;
}

static int check_fifo_boundaries(NV2AState *gpu,uint8_t *ramin,const char *mode)
{
    static const struct {uint32_t base,limit,get,put;} valid[]={
        {0x200,0x1ff,0x1fc,0x200},
        {0xf00,0x1ff,0xfc,0x100},
        {0xf00,0xffffffff,0xfc,0x100},
        {0,3,0,4}
    };
    uint32_t *object=(uint32_t *)(ramin+0x100);
    gpu->pfifo.regs[NV_PFIFO_MODE]=1;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_PUSH1]=0x100;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_PUSH0]=1;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUSH]=1;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_PULL0]=1;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_INSTANCE]=0x10;
    gpu->pgraph.regs[NV_PGRAPH_FIFO]=1;
    if(!strcmp(mode,"--fifo-boundaries")) {
        for(unsigned i=0;i<sizeof valid/sizeof valid[0];i++) {
            object[0]=2|(valid[i].base<<20);object[1]=valid[i].limit;object[2]=0;
            gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE]=0;
            gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]=valid[i].get;
            gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT]=valid[i].put;
            nv2a_fifo_run(gpu);
            assert(gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]==valid[i].put);
        }
        fprintf(stderr,"[FIFO] original nonzero DMA base, descriptor/VRAM last dword, max limit and four-byte span passed\n");
        return 0;
    }
    object[0]=2;object[1]=0xfff;object[2]=0;
    uint32_t get=0,put=4;
    if(!strcmp(mode,"--reject-fifo-limit")) {object[0]|=0x200u<<20;object[1]=0x1ff;get=0x200;put=0x204;}
    else if(!strcmp(mode,"--reject-fifo-vram")) {object[0]|=0xf00u<<20;object[1]=0x1ff;get=0x100;put=0x104;}
    else if(!strcmp(mode,"--reject-fifo-partial")) object[1]=2;
    else if(!strcmp(mode,"--reject-fifo-payload")) {
        object[1]=3;put=8;gpu->pgraph.regs[NV_PGRAPH_CTX_CACHE1]=NV_KELVIN_PRIMITIVE;
        gpu->pfifo.method_consumer=consume;
        gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE]=(2u<<18)|0xB80;
    }
    else if(!strcmp(mode,"--reject-fifo-align")) {get=1;put=5;}
    else {assert(!strcmp(mode,"--reject-fifo-max-get"));object[1]=0xffffffff;get=0xfffffffc;put=0;}
    gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]=get;gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT]=put;
    nv2a_fifo_run(gpu);assert(!"Original FIFO must reject the invalid word");return 1;
}

static uint32_t s_packet_seen, s_packet_total, s_packet_increment, s_packet_stop;
static int consume_packet(uint32_t subchannel,uint32_t method,uint32_t parameter,uint8_t *output)
{
    NV2AState *gpu=nv2a_get_state();uint32_t *r=gpu->pfifo.regs;(void)output;
    assert(subchannel==3 && method==0xB80+(s_packet_increment?s_packet_seen*4:0));
    assert(parameter==0x43210000+s_packet_seen);
    assert(r[NV_PFIFO_CACHE1_DMA_GET]==4+s_packet_seen*4);
    assert(r[NV_PFIFO_CACHE1_DMA_DATA_SHADOW]==parameter);
    assert(!(r[NV_PFIFO_CACHE1_STATUS]&NV_PFIFO_CACHE1_STATUS_LOW_MARK));
    assert(GET_MASK(r[NV_PFIFO_CACHE1_DMA_STATE],NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT)==s_packet_total-s_packet_seen);
    assert((r[NV_PFIFO_CACHE1_DMA_STATE]&NV_PFIFO_CACHE1_DMA_STATE_METHOD)==method);
    ++s_packet_seen;
    if(s_packet_seen==3 && s_packet_stop==1) gpu->pgraph.waiting_for_nop=true;
    if(s_packet_seen==3 && s_packet_stop==2) r[NV_PFIFO_CACHE1_PULL0]=0;
    return 0;
}
static int check_fifo_packets(NV2AState *gpu,uint8_t *ram,uint8_t *ramin)
{
    uint32_t *r=gpu->pfifo.regs,*commands=(uint32_t *)ram,*object=(uint32_t *)(ramin+0x100);
    object[0]=2;object[1]=4095;object[2]=0;
    ((uint32_t *)(ramin+0x200))[0]=NV_KELVIN_PRIMITIVE;pgraph_method(gpu,3,0,0x200);
    r[NV_PFIFO_MODE]=1;r[NV_PFIFO_CACHE1_PUSH1]=0x100;r[NV_PFIFO_CACHE1_PUSH0]=1;
    r[NV_PFIFO_CACHE1_DMA_PUSH]=1;r[NV_PFIFO_CACHE1_DMA_INSTANCE]=0x10;
    gpu->pgraph.regs[NV_PGRAPH_FIFO]=1;gpu->pfifo.method_consumer=consume_packet;
    const uint32_t counts[]={1,16,127,1023};
    for(unsigned trial=0;trial<8;trial++) {
        s_packet_seen=0;s_packet_increment=trial<2;s_packet_total=counts[trial%4];s_packet_stop=trial==6?1:trial==7?2:0;
        commands[0]=(s_packet_increment?0:0x40000000u)|(s_packet_total<<18)|(3<<13)|0xB80;
        for(unsigned i=0;i<s_packet_total;i++)commands[1+i]=0x43210000+i;
        r[NV_PFIFO_CACHE1_DMA_STATE]=0;r[NV_PFIFO_CACHE1_DMA_DCOUNT]=0;r[NV_PFIFO_CACHE1_DMA_GET]=0;
        r[NV_PFIFO_CACHE1_PULL0]=1;gpu->pgraph.waiting_for_nop=false;
        r[NV_PFIFO_CACHE1_DMA_PUT]=(s_packet_stop?1+s_packet_total:1+(s_packet_total+1)/2)*4;
        nv2a_fifo_run(gpu);
        unsigned first=s_packet_stop?3:(s_packet_total+1)/2;
        assert(s_packet_seen==first && r[NV_PFIFO_CACHE1_DMA_GET]==4+first*4);
        assert(GET_MASK(r[NV_PFIFO_CACHE1_DMA_STATE],NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT)==s_packet_total-first);
        if(s_packet_stop) {nv2a_fifo_run(gpu);assert(s_packet_seen==first);}
        r[NV_PFIFO_CACHE1_PULL0]=1;gpu->pgraph.waiting_for_nop=false;
        r[NV_PFIFO_CACHE1_DMA_PUT]=(1+s_packet_total)*4;nv2a_fifo_run(gpu);
        assert(s_packet_seen==s_packet_total && r[NV_PFIFO_CACHE1_DMA_DCOUNT]==s_packet_total);
        assert(r[NV_PFIFO_CACHE1_DMA_GET]==r[NV_PFIFO_CACHE1_DMA_PUT]);
        assert(GET_MASK(r[NV_PFIFO_CACHE1_DMA_STATE],NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT)==0);
    }
    fprintf(stderr,"[FIFO] packet per-word GET/state/shadow/status, increment/nonincrement, partial PUT, 1023 words, NOP and pull suspension passed\n");
    return 0;
}

static volatile uint32_t s_packet_checksum;
static int consume_packet_benchmark(uint32_t subchannel,uint32_t method,uint32_t parameter,uint8_t *output)
{
    (void)subchannel;(void)method;(void)output;s_packet_checksum+=parameter;return 0;
}
static int benchmark_fifo_packets(NV2AState *gpu,uint8_t *ram,uint8_t *ramin)
{
    check_fifo_packets(gpu,ram,ramin);uint32_t *r=gpu->pfifo.regs,*commands=(uint32_t *)ram;
    gpu->pfifo.method_consumer=consume_packet_benchmark;
    commands[0]=0x40000000u|(1023u<<18)|(3<<13)|0xB80;
    for(unsigned i=0;i<1023;i++)commands[1+i]=i;
    r[NV_PFIFO_CACHE1_DMA_PUT]=4096;
    LARGE_INTEGER start,end,frequency;assert(QueryPerformanceFrequency(&frequency));
    assert(QueryPerformanceCounter(&start));
    for(unsigned run=0;run<32768;run++) {
        r[NV_PFIFO_CACHE1_DMA_STATE]=0;r[NV_PFIFO_CACHE1_DMA_GET]=0;nv2a_fifo_run(gpu);
        assert(r[NV_PFIFO_CACHE1_DMA_GET]==4096);
    }
    assert(QueryPerformanceCounter(&end));
    uint32_t expected=(uint32_t)((uint64_t)32768*1023*1022/2);assert(s_packet_checksum==expected);
    fprintf(stderr,"[FIFO BENCH] words=33521664 ns_per_word=%.6f checksum=%08X\n",(end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/33521664,s_packet_checksum);
    return 0;
}

static int check_vblank_clock(NV2AState *gpu)
{
    LARGE_INTEGER frequency;assert(QueryPerformanceFrequency(&frequency));
    s_test_clock=1;s_test_counter=0;gpu->vblank_frame=0;
    gpu->pcrtc.pending_interrupts=0;gpu->pcrtc.enabled_interrupts=0;
    gpu->pmc.enabled_interrupts=1;gpu->pcrtc.raster=17;
    nv2a_vblank_tick(gpu);
    assert(!gpu->pcrtc.pending_interrupts && gpu->pcrtc.raster==17);
    for(unsigned i=1;i<=60;i++) {
        s_test_counter=(2*i+1)*frequency.QuadPart/120;
        nv2a_vblank_tick(gpu);
        assert(gpu->vblank_frame==i && gpu->pcrtc.raster==0);
        assert(gpu->pcrtc.pending_interrupts==NV_PCRTC_INTR_0_VBLANK);
        assert(!(gpu->pmc.pending_interrupts & NV_PMC_INTR_0_PCRTC));
        pcrtc_write(gpu,NV_PCRTC_INTR_EN_0,1,4);
        assert(gpu->pmc.pending_interrupts & NV_PMC_INTR_0_PCRTC);
        pcrtc_write(gpu,NV_PCRTC_INTR_0,NV_PCRTC_INTR_0_VBLANK,4);
        assert(!(gpu->pmc.pending_interrupts & NV_PMC_INTR_0_PCRTC));
        gpu->pcrtc.raster=23;nv2a_vblank_tick(gpu);
        assert(!gpu->pcrtc.pending_interrupts && gpu->pcrtc.raster==23);
        pcrtc_write(gpu,NV_PCRTC_INTR_EN_0,0,4);
    }
    s_test_counter+=frequency.QuadPart*3;nv2a_vblank_tick(gpu);
    assert(gpu->pcrtc.pending_interrupts==NV_PCRTC_INTR_0_VBLANK);
    fprintf(stderr,"[PCRTC] 60 virtual-clock fields, unchanged-clock pause, masked latch, original IRQ enable/ack and delayed polling passed\n");
    return 0;
}

static int check_register_masks(void)
{
    uint32_t random=0x12345678u;
    for(unsigned shift=0;shift<32;shift++) {
        for(unsigned width=1;width<=32-shift;width++) {
            uint32_t mask=(UINT32_MAX>>(32-width))<<shift;
            for(unsigned sample=0;sample<128;sample++) {
                random=random*1664525u+1013904223u;
                uint32_t value=random, field=random^0xa5a5a5a5u, actual=value;
                assert(GET_MASK(value,mask)==((value&mask)>>shift));
                SET_MASK(actual,mask,field);
                assert(actual==((value&~mask)|((field<<shift)&mask)));
                uint64_t wide=((uint64_t)value<<32)|field;
                assert(GET_MASK(wide,mask)==((wide&mask)>>shift));
            }
        }
    }
    for(unsigned sample=0;sample<10000;sample++) {
        random=random*1664525u+1013904223u;
        uint32_t mask=random?random:1u, value=random^0xdeadbeefu, actual=value;
        unsigned shift=0;
        while(!(mask&(1u<<shift)))++shift;
        assert(GET_MASK(value,mask)==((value&mask)>>shift));
        SET_MASK(actual,mask,value);
        assert(actual==((value&~mask)|((value<<shift)&mask)));
    }
    fprintf(stderr,"[MASK] every contiguous 32-bit field and 10000 sparse masks preserve extraction and insertion\n");
    return 0;
}

static HANDLE s_worker_entered,s_worker_continue;
static DWORD s_worker_consumer_thread;
static int consume_worker_packet(uint32_t subchannel,uint32_t method,uint32_t parameter,uint8_t *output)
{
    s_worker_consumer_thread=GetCurrentThreadId();
    if(!s_packet_seen && s_worker_entered) {
        SetEvent(s_worker_entered);
        assert(WaitForSingleObject(s_worker_continue,5000)==WAIT_OBJECT_0);
    }
    return consume_packet(subchannel,method,parameter,output);
}
static int check_fifo_worker(NV2AState *gpu,uint8_t *ram,uint8_t *ramin)
{
    check_fifo_packets(gpu,ram,ramin);
    uint32_t *r=gpu->pfifo.regs,*commands=(uint32_t *)ram;
    s_packet_seen=0;s_packet_total=16;s_packet_increment=0;s_packet_stop=0;
    commands[0]=0x40000000u|(16u<<18)|(3u<<13)|0xB80;
    for(unsigned i=0;i<16;i++)commands[i+1]=0x43210000+i;
    r[NV_PFIFO_CACHE1_DMA_GET]=r[NV_PFIFO_CACHE1_DMA_PUT]=r[NV_PFIFO_CACHE1_DMA_STATE]=0;
    r[NV_PFIFO_CACHE1_DMA_DCOUNT]=0;
    gpu->pfifo.method_consumer=consume_worker_packet;
    s_worker_entered=CreateEvent(NULL,FALSE,FALSE,NULL);
    s_worker_continue=CreateEvent(NULL,FALSE,FALSE,NULL);
    assert(s_worker_entered && s_worker_continue && nv2a_fifo_start(gpu, 1));
    AcquireSRWLockExclusive(&s_fixture_devices);
    /* A frozen worker cannot consume
     * or acknowledge these submitted bytes until the device barrier opens. */
    nv2a_mmio_write(gpu,0x800040,36,4);
    assert(nv2a_mmio_read(gpu,0x800044,4)==0 && s_packet_seen==0);
    ReleaseSRWLockExclusive(&s_fixture_devices);
    assert(WaitForSingleObject(s_worker_entered,5000)==WAIT_OBJECT_0);
    assert(s_worker_consumer_thread!=GetCurrentThreadId());
    assert(r[NV_PFIFO_CACHE1_DMA_GET]==4 && s_packet_seen==0);
    assert(nv2a_mmio_read(gpu,0x800044,4)==4);
    nv2a_mmio_write(gpu,0x100410,0x10000,4);
    assert(nv2a_mmio_read(gpu,0x100410,4)==0);
    nv2a_mmio_write(gpu,0x800040,36,4); /* PUT must not wait for the blocked consumer. */
    SetEvent(s_worker_continue);nv2a_fifo_wait_idle(gpu);
    assert(s_packet_seen==8 && r[NV_PFIFO_CACHE1_DMA_GET]==36);
    CloseHandle(s_worker_entered);s_worker_entered=NULL;CloseHandle(s_worker_continue);
    nv2a_mmio_write(gpu,0x800040,68,4);nv2a_fifo_wait_idle(gpu);
    assert(s_packet_seen==16 && r[NV_PFIFO_CACHE1_DMA_GET]==68);
    for(unsigned round=0;round<100;round++) {
        qemu_mutex_lock(&gpu->pfifo.lock);
        s_packet_seen=0;s_packet_stop=1;
        r[NV_PFIFO_CACHE1_DMA_GET]=r[NV_PFIFO_CACHE1_DMA_STATE]=0;
        nv2a_fifo_kick(gpu);qemu_mutex_unlock(&gpu->pfifo.lock);
        nv2a_fifo_wait_idle(gpu);
        assert(s_packet_seen==3 && r[NV_PFIFO_CACHE1_DMA_GET]==16);
        qemu_mutex_lock(&gpu->pfifo.lock);
        gpu->pgraph.waiting_for_nop=false;nv2a_fifo_kick(gpu);
        qemu_mutex_unlock(&gpu->pfifo.lock);nv2a_fifo_wait_idle(gpu);
        assert(s_packet_seen==16 && r[NV_PFIFO_CACHE1_DMA_GET]==68);
    }
    nv2a_fifo_stop(gpu);assert(!gpu->pfifo.thread.thread);
    fprintf(stderr,"[FIFO WORKER] independent consumption, freeze, partial PUT, per-word visibility, 100 pause/resume cycles and shutdown passed\n");
    return 0;
}

int main(int argc, char **argv)
{
    check_register_masks();
    if(argc==2 && !strcmp(argv[1],"--register-masks"))return 0;
    if (argc == 2 && !strcmp(argv[1], "--point-geometry"))
        return check_point_geometry(0);
    if(argc==2 && !strcmp(argv[1],"--reject-fixed-spot")) return check_point_geometry(1);
    if(argc==2 && !strcmp(argv[1],"--reject-fixed-specular-coefficients")) return check_point_geometry(2);
    if (argc == 2 && !strcmp(argv[1], "--movie-background"))
        return check_movie_background();
    if (argc == 3 && !strcmp(argv[1], "--dialogue-panel"))
        return check_dialogue_panel(argv[2]);
    uint8_t ram[4096] = {0}, ramin[4096] = {0};
    NV2AState *gpu = nv2a_init_standalone(ram, sizeof(ram), ramin, sizeof(ramin));
    assert(gpu && gpu->vram_ptr == ram && gpu->ramin_ptr == ramin);
    if(argc==2 && !strcmp(argv[1],"--fifo-worker")) return check_fifo_worker(gpu,ram,ramin);
    if(argc==2 && !strcmp(argv[1],"--fifo-packet-benchmark")) return benchmark_fifo_packets(gpu,ram,ramin);
    if(argc==2 && !strcmp(argv[1],"--fifo-packets")) return check_fifo_packets(gpu,ram,ramin);
    if(argc==2 && !strcmp(argv[1],"--vblank-clock")) return check_vblank_clock(gpu);
    if(argc==2 && !strcmp(argv[1],"--prmdio-palette")) {
        nv2a_mmio_write(gpu,0x6813C8,128,1);
        assert(nv2a_mmio_read(gpu,0x6813C8,1)==128);
        for(unsigned i=0;i<3;i++) nv2a_mmio_write(gpu,0x6813C9,0x31+i,1);
        assert(nv2a_mmio_read(gpu,0x6813C8,1)==129);
        assert(gpu->puserdac.palette[384]==0x31 && gpu->puserdac.palette[386]==0x33);
        nv2a_mmio_write(gpu,0x6813C8,255,1);
        for(unsigned i=0;i<4;i++) nv2a_mmio_write(gpu,0x6813C9,0x61+i,1);
        assert(gpu->puserdac.palette[765]==0x61 && gpu->puserdac.palette[767]==0x63 && gpu->puserdac.palette[0]==0x64);
        fprintf(stderr,"[PRMDIO] original palette index, RGB advance and wrap passed\n");return 0;
    }
    if(argc==2 && (!strcmp(argv[1],"--fifo-boundaries") || !strcmp(argv[1],"--reject-fifo-limit") ||
       !strcmp(argv[1],"--reject-fifo-vram") || !strcmp(argv[1],"--reject-fifo-partial") || !strcmp(argv[1],"--reject-fifo-payload") ||
       !strcmp(argv[1],"--reject-fifo-align") || !strcmp(argv[1],"--reject-fifo-max-get")))
        return check_fifo_boundaries(gpu,ramin,argv[1]);
    if (argc == 2) {
        uint32_t *object = (uint32_t *)(ramin + 0x100);
        object[0] = 2 | (0x200u << 20); object[1] = 0x1FF; object[2] = 0;
        if (!strcmp(argv[1], "--reject-limit")) nv_dma_span(gpu, 0x100, 0x1FF, 2);
        if (!strcmp(argv[1], "--reject-instance")) nv_dma_span(gpu, 4090, 0, 1);
        if (!strcmp(argv[1], "--reject-vram")) {
            object[0] = 2 | (0xFF0u << 20); object[1] = 0x1F;
            nv_dma_span(gpu, 0x100, 0, 17);
        }
        if (!strcmp(argv[1], "--reject-specular-value")) {
            object[0] = NV_KELVIN_PRIMITIVE;
            gpu->pfifo.method_consumer = nv2a_pb_exec_method;
            pgraph_method(gpu, 0, 0, 0x100);
            pgraph_method(gpu, 0, NV097_SET_SPECULAR_ENABLE, 2);
        }
        if (!strcmp(argv[1], "--reject-material-source") ||
                !strcmp(argv[1], "--reject-material-back")) {
            object[0] = NV_KELVIN_PRIMITIVE;
            gpu->pfifo.method_consumer = nv2a_pb_exec_method;
            pgraph_method(gpu, 0, 0, 0x100);
            pgraph_method(gpu, 0, NV097_SET_COLOR_MATERIAL,
                          !strcmp(argv[1], "--reject-material-source") ? 3 : 0x100);
        }
        if (!strcmp(argv[1], "--reject-lighting-value") ||
                !strcmp(argv[1], "--reject-fixed-lighting") ||
                !strcmp(argv[1], "--reject-fixed-point-lighting")) {
            object[0] = NV_KELVIN_PRIMITIVE;
            gpu->pfifo.method_consumer = nv2a_pb_exec_method;
            pgraph_method(gpu, 0, 0, 0x100);
            if (!strcmp(argv[1], "--reject-lighting-value"))
                pgraph_method(gpu, 0, NV097_SET_LIGHTING_ENABLE, 2);
            pgraph_method(gpu, 0, NV097_SET_LIGHTING_ENABLE, 1);
            assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_C] & NV_PGRAPH_CSV0_C_LIGHTING);
            pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, 0);
            int point = !strcmp(argv[1], "--reject-fixed-point-lighting");
            pgraph_method(gpu, 0, NV097_SET_BEGIN_END, point ? 1 : 5);
            pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, point ? 0 : 2u << 24);
            pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        }
        if (!strcmp(argv[1], "--reject-point-smooth-value")) {
            object[0] = NV_KELVIN_PRIMITIVE;
            gpu->pfifo.method_consumer = nv2a_pb_exec_method;
            pgraph_method(gpu, 0, 0, 0x100);
            pgraph_method(gpu, 0, NV097_SET_POINT_SMOOTH_ENABLE, 2);
        }
        if (!strcmp(argv[1], "--reject-point-params-value")) {
            object[0] = NV_KELVIN_PRIMITIVE;
            gpu->pfifo.method_consumer = nv2a_pb_exec_method;
            pgraph_method(gpu, 0, 0, 0x100);
            pgraph_method(gpu, 0, NV097_SET_POINT_PARAMS, 0);
            pgraph_method(gpu, 0, NV097_SET_POINT_PARAMS_ENABLE, 2);
        }
        if (!strcmp(argv[1], "--reject-occlusion-value")) {
            object[0] = NV_KELVIN_PRIMITIVE;
            gpu->pfifo.method_consumer = nv2a_pb_exec_method;
            pgraph_method(gpu, 0, 0, 0x100);
            pgraph_method(gpu, 0, NV097_SET_OCCLUDE_ZSTENCIL_EN, 4);
        }
        if (!strcmp(argv[1], "--reject-texture-matrix-enable") ||
            !strcmp(argv[1], "--reject-fixed-texgen") ||
            !strcmp(argv[1], "--reject-texgen-value") ||
            !strcmp(argv[1], "--reject-texture-matrix-value") ||
            !strcmp(argv[1], "--reject-foggen-value")) {
            object[0] = NV_KELVIN_PRIMITIVE;
            gpu->pfifo.method_consumer = nv2a_pb_exec_method;
            pgraph_method(gpu, 0, 0, 0x100);
            if(!strcmp(argv[1],"--reject-foggen-value"))
                pgraph_method(gpu,0,NV097_SET_FOG_GEN_MODE,4);
            else if(!strcmp(argv[1],"--reject-texgen-value"))
                pgraph_method(gpu,0,NV097_SET_TEXGEN_S,1);
            else if(!strcmp(argv[1],"--reject-texture-matrix-value"))
                pgraph_method(gpu,0,NV097_SET_TEXTURE_MATRIX_ENABLE,2);
            else {
                pgraph_method(gpu,0,NV097_SET_TRANSFORM_EXECUTION_MODE,0);
                if(!strcmp(argv[1],"--reject-fixed-texgen"))
                    pgraph_method(gpu,0,NV097_SET_TEXGEN_S,NV097_SET_TEXGEN_S_EYE_LINEAR);
                else pgraph_method(gpu,0,NV097_SET_TEXTURE_MATRIX_ENABLE,1);
                pgraph_method(gpu,0,NV097_SET_BEGIN_END,5);
                pgraph_method(gpu,0,NV097_DRAW_ARRAYS,2u<<24);
                pgraph_method(gpu,0,NV097_SET_BEGIN_END,0);
            }
        }
        if (!strcmp(argv[1], "--reject-dither-value") ||
            !strcmp(argv[1], "--reject-dither-reduction")) {
            object[0] = NV_KELVIN_PRIMITIVE;
            gpu->pfifo.method_consumer = nv2a_pb_exec_method;
            pgraph_method(gpu, 0, 0, 0x100);
            if (!strcmp(argv[1], "--reject-dither-value"))
                pgraph_method(gpu, 0, NV097_SET_DITHER_ENABLE, 2);
            g_xbox_mem_offset = (ptrdiff_t)ram - (ptrdiff_t)0x80000000u;
            uint32_t *color_dma = (uint32_t *)(ramin + 0x200);
            color_dma[0] = 2; color_dma[1] = sizeof(ram) - 1; color_dma[2] = 0;
            pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_COLOR, 0x200);
            pgraph_method(gpu, 0, NV097_SET_SURFACE_CLIP_HORIZONTAL, 8u << 16);
            pgraph_method(gpu, 0, NV097_SET_SURFACE_CLIP_VERTICAL, 8u << 16);
            pgraph_method(gpu, 0, NV097_SET_SURFACE_PITCH, 16);
            pgraph_method(gpu, 0, NV097_SET_SURFACE_FORMAT,
                          NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5);
            pgraph_method(gpu, 0, NV097_SET_COLOR_CLEAR_VALUE, 0xC1794F13);
            pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
            assert(lduw_le_p(ram) == 0x7A62);
            pgraph_method(gpu, 0, NV097_SET_DITHER_ENABLE, 1);
            pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
        }
        return 1; /* A rejection case must terminate inside the real helper. */
    }
    assert(nv2a_mmio_read(gpu, 0x3214, 1) & NV_PFIFO_CACHE1_STATUS_LOW_MARK);
    /* NINE selects CR1F, reads it, then writes the unlock value 0x57. */
    crt_write(gpu, 0x1F, 0x57);
    assert(nv2a_mmio_read(gpu, 0x6013D4, 1) == 0x1F);
    assert(nv2a_mmio_read(gpu, 0x6013D5, 1) == 0x57);
    assert(gpu->vga.cr[0x1F] == 0x57);
    crt_write(gpu, 0x52, 4);
    assert(gpu->vga.cr[0x1F] == 0x57 && gpu->vga.cr[0x52] == 4);
    /* Standard VGA write protection must survive the alias integration. */
    crt_write(gpu, 0, 0x33);
    crt_write(gpu, 0x11, 0x80);
    crt_write(gpu, 0, 0x99);
    assert(gpu->vga.cr[0] == 0x33);
    crt_write(gpu, 7, 0xFF);
    assert(gpu->vga.cr[7] == 0x10);
    /* PCI and PBUS share one configuration store. */
    pci_set_long(gpu->parent_obj.config + PCI_COMMAND, 7);
    assert(nv2a_mmio_read(gpu, 0x1804, 4) == 7);
    nv2a_mmio_write(gpu, 0x1804, 5, 4);
    assert(pci_get_long(gpu->parent_obj.config + PCI_COMMAND) == 5);
    /* NINE clears PGRAPH interrupts during startup. A clear write must not
     * manufacture the context-switch interrupt and recursively reenter D3D. */
    nv2a_mmio_write(gpu, 0x400100, 0xFFFFFFFF, 4);
    assert(nv2a_mmio_read(gpu, 0x400100, 4) == 0);
    gpu->pgraph.pending_interrupts = 0x1001;
    nv2a_mmio_write(gpu, 0x400140, 0x1001, 4);
    assert(nv2a_mmio_read(gpu, 0x400140, 4) == 0x1001);
    assert(nv2a_mmio_read(gpu, 0x100, 4) & NV_PMC_INTR_0_PGRAPH);
    nv2a_mmio_write(gpu, 0x400100, 0x1000, 4);
    assert(nv2a_mmio_read(gpu, 0x400100, 4) == 1);
    nv2a_mmio_write(gpu, 0x400100, 1, 4);
    assert(nv2a_mmio_read(gpu, 0x400100, 4) == 0);
    assert(!(nv2a_mmio_read(gpu, 0x100, 4) & NV_PMC_INTR_0_PGRAPH));
    /* A real DMA object and RAMHT object identity, not predecoded methods. */
    uint32_t *dma_object = (uint32_t *)(ramin + 0x100);
    dma_object[0] = 2; dma_object[1] = sizeof(ram) - 1; dma_object[2] = 0;
    ((uint32_t *)ramin)[2] = 1;
    ((uint32_t *)ramin)[3] = 0x80010020; /* handle 1 -> graphics instance 0x200 */
    ((uint32_t *)(ramin + 0x200))[0] = 0x97;
    gpu->pfifo.regs[NV_PFIFO_MODE] = 1;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_PUSH1] = 0x100;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_PUSH0] = 1;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUSH] = 1;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_PULL0] = 1;
    gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_INSTANCE] = 0x10;
    gpu->pgraph.regs[NV_PGRAPH_FIFO] = 1;
    gpu->pfifo.method_consumer = consume;
    uint32_t *commands = (uint32_t *)ram;
    commands[0] = 0x31; /* NINE's physical-zero jump pattern */
    nv2a_mmio_write(gpu, 0x800040, 48, 4);
    assert(nv2a_mmio_read(gpu, 0x800044, 4) == 48 && calls == 0);
    assert(nv2a_mmio_read(gpu, 0x3214, 1) & NV_PFIFO_CACHE1_STATUS_LOW_MARK);
    commands[12] = 0x40000; commands[13] = 1; /* SET_OBJECT */
    commands[14] = 0x80200; commands[15] = 0x1111; commands[16] = 0x2222;
    nv2a_mmio_write(gpu, 0x800040, 64, 4);
    assert(calls == 1 && methods[0] == 0x200 && parameters[0] == 0x1111);
    assert(GET_MASK(gpu->pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE], NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT) == 1);
    nv2a_mmio_write(gpu, 0x800040, 68, 4);
    assert(calls == 2 && methods[1] == 0x204 && parameters[1] == 0x2222);
    commands[17] = 0x40080210; commands[18] = 0x3333; commands[19] = 0x4444;
    nv2a_mmio_write(gpu, 0x800040, 80, 4);
    assert(calls == 4 && methods[2] == 0x210 && methods[3] == 0x210);
    commands[20] = 102; commands[21] = 0;
    commands[25] = 0x40214; commands[26] = 0x5555; commands[27] = 0x20000;
    nv2a_mmio_write(gpu, 0x800040, 88, 4);
    assert(calls == 5 && methods[4] == 0x214 && parameters[4] == 0x5555);
    assert(nv2a_mmio_read(gpu, 0x800044, 4) == 88);
    commands[22] = 0x20000060; /* old jump */
    nv2a_mmio_write(gpu, 0x800040, 96, 4);
    assert(nv2a_mmio_read(gpu, 0x800044, 4) == 96 && calls == 5);
    commands[24] = 0x40218; commands[25] = 0x6666;
    nv2a_mmio_write(gpu, 0x3200, 0, 4); /* disabled pusher retains pending data */
    nv2a_mmio_write(gpu, 0x800040, 104, 4);
    assert(nv2a_mmio_read(gpu, 0x800044, 4) == 96 && calls == 5);
    nv2a_mmio_write(gpu, 0x3200, 1, 4);
    assert(nv2a_mmio_read(gpu, 0x800044, 4) == 104 && calls == 6);
    ((uint32_t *)ramin)[14] = 7;
    ((uint32_t *)ramin)[15] = 0x80000030; /* DMA identity uses ENGINE_SOFTWARE */
    commands[26] = 0x40180; commands[27] = 7;
    nv2a_mmio_write(gpu, 0x800040, 112, 4);
    assert(calls == 6 && gpu->pgraph.dma_notifies == 0x300);
    assert(nv2a_mmio_read(gpu, 0x4001E0, 4) == 0x200);
    assert((nv2a_mmio_read(gpu, 0x40014C, 4) & 0xFF) == 0x97);
    /* A notification consumes its own word, traps actual data and pauses
     * before the following method until ERROR is acknowledged by the ISR. */
    commands[28] = 0x40100; commands[29] = 0x28;
    commands[30] = 0x40200; commands[31] = 0xABCD;
    unsigned irq_levels[2] = {0};
    assert(gpu->parent_obj.irq_level == 0);
    gpu->parent_obj.irq_notify = observe_pci_irq;
    gpu->parent_obj.irq_opaque = irq_levels;
    nv2a_mmio_write(gpu, 0x400140, NV_PGRAPH_INTR_ERROR, 4);
    nv2a_mmio_write(gpu, NV_PMC_INTR_EN_0, 1, 4);
    nv2a_mmio_write(gpu, 0x800040, 128, 4);
    assert(nv2a_mmio_read(gpu, 0x800044, 4) == 120 && calls == 6);
    assert(gpu->pgraph.waiting_for_nop && (gpu->pmc.pending_interrupts & NV_PMC_INTR_0_PGRAPH));
    assert(irq_levels[1] == 1 && irq_levels[0] == 0);
    assert(GET_MASK(gpu->pgraph.regs[NV_PGRAPH_TRAPPED_ADDR], NV_PGRAPH_TRAPPED_ADDR_MTHD) == 0x100);
    assert(gpu->pgraph.regs[NV_PGRAPH_TRAPPED_DATA_LOW] == 0x28);
    assert(gpu->pgraph.regs[NV_PGRAPH_NSOURCE] == NV_PGRAPH_NSOURCE_NOTIFICATION);
    nv2a_mmio_write(gpu, 0x400100, NV_PGRAPH_INTR_CONTEXT_SWITCH, 4);
    assert(gpu->pgraph.waiting_for_nop && nv2a_mmio_read(gpu, 0x800044, 4) == 120);
    assert(irq_levels[1] == 1 && irq_levels[0] == 0);
    gpu->irq_in_service = true;
    nv2a_mmio_write(gpu, 0x400100, NV_PGRAPH_INTR_ERROR, 4);
    assert(!gpu->pgraph.waiting_for_nop && !(gpu->pmc.pending_interrupts & NV_PMC_INTR_0_PGRAPH));
    assert(irq_levels[1] == 1 && irq_levels[0] == 1);
    gpu->parent_obj.irq_notify = NULL;
    gpu->parent_obj.irq_opaque = NULL;
    assert(nv2a_mmio_read(gpu, 0x800044, 4) == 120 && calls == 6);
    gpu->irq_in_service = false;
    nv2a_fifo_run(gpu);
    assert(nv2a_mmio_read(gpu, 0x800044, 4) == 128 && calls == 7 && parameters[6] == 0xABCD);
    /* The DMA descriptor limit includes its last byte. */
    nv2a_mmio_write(gpu, 0x3200, 0, 4);
    commands[1023] = 0;
    nv2a_mmio_write(gpu, 0x800044, 4092, 4);
    nv2a_mmio_write(gpu, 0x800040, 4096, 4);
    nv2a_mmio_write(gpu, 0x3200, 1, 4);
    assert(nv2a_mmio_read(gpu, 0x800044, 4) == 4096 && calls == 7);
    ((uint32_t *)(ramin + 0x240))[0] = 0x39;
    pgraph_method(gpu, 1, 0, 0x240);
    pgraph_method(gpu, 1, 0x180, 0x300);
    assert(gpu->pgraph.memory_to_memory.dma_notifies == 0x300);
    ((uint32_t *)(ramin + 0x260))[0] = 0x62;
    pgraph_method(gpu, 3, 0, 0x260);
    pgraph_method(gpu, 3, 0x184, 0x300);
    pgraph_method(gpu, 3, 0x188, 0x310);
    pgraph_method(gpu, 3, 0x304, 0x00400020);
    assert(gpu->pgraph.context_surfaces_2d.object_instance == 0x260);
    assert(gpu->pgraph.context_surfaces_2d.dma_image_source == 0x300);
    assert(gpu->pgraph.context_surfaces_2d.dma_image_dest == 0x310);
    assert(gpu->pgraph.context_surfaces_2d.source_pitch == 32);
    assert(gpu->pgraph.context_surfaces_2d.dest_pitch == 64);
    ((uint32_t *)(ramin + 0x280))[0] = 0x9F;
    pgraph_method(gpu, 2, 0, 0x280);
    pgraph_method(gpu, 2, 0x19C, 0x260);
    pgraph_method(gpu, 2, 0x2FC, 3);
    assert(gpu->pgraph.image_blit.context_surfaces == 0x260);
    assert(gpu->pgraph.image_blit.operation == 3);
    assert((nv2a_mmio_read(gpu, 0x40014C, 4) & 0xFF) == 0x9F);
    /* Semaphore releases must reach the actual DMA base plus offset. */
    uint32_t *semaphore = (uint32_t *)(ramin + 0x300);
    semaphore[0] = 2 | (0x200u << 20); semaphore[1] = 0x1FF; semaphore[2] = 0;
    memset(ram + 0x200, 0xA5, 0x200);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_SEMAPHORE, 0x300);
    pgraph_method(gpu, 0, NV097_SET_SEMAPHORE_OFFSET, 0x1FC);
    pgraph_method(gpu, 0, NV097_BACK_END_WRITE_SEMAPHORE_RELEASE, 0x12345678);
    assert(ldl_le_p(ram + 0x3FC) == 0x12345678);
    assert(ldl_le_p(ram + 0x3F8) == 0xA5A5A5A5);
    assert(gpu->pgraph.regs[NV_PGRAPH_SEMAPHOREOFFSET] == 0x1FC);
    assert(nv_dma_span(gpu, 0x300, 0x1FF, 1) == ram + 0x3FF);
    /* A colour clear uses a distinct, nonzero object base. Low physical
     * offsets must never be mistaken for addresses in the XBE image. */
    g_xbox_mem_offset = (ptrdiff_t)ram - (ptrdiff_t)0x80000000u;
    gpu->pfifo.method_consumer = nv2a_pb_exec_method;
    for (unsigned unit = 0; unit < 4; ++unit) {
        gpu->pgraph.texture_matrix_enable[unit] = true;
        pgraph_method(gpu, 0, NV097_SET_TEXTURE_MATRIX_ENABLE + unit * 4, 0);
        assert(!gpu->pgraph.texture_matrix_enable[unit]);
    }
    pgraph_method(gpu, 0, NV097_SET_LINE_WIDTH, 8);
    assert(GET_MASK(gpu->pgraph.regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_LINEWIDTH) == 8);
    pgraph_method(gpu, 0, NV097_SET_SWATH_WIDTH, 4);
    assert(GET_MASK(gpu->pgraph.regs[NV_PGRAPH_SETUPRASTER], NV_PGRAPH_SETUPRASTER_SWATHWIDTH) == 4);
    pgraph_method(gpu, 0, NV097_SET_OCCLUDE_ZSTENCIL_EN, 3);
    assert(GET_MASK(gpu->pgraph.regs[NV_PGRAPH_ZCOMPRESSOCCLUDE], NV_PGRAPH_ZCOMPRESSOCCLUDE_OCCLUDE_ENABLE) == 3);
    pgraph_method(gpu, 0, NV097_SET_OCCLUDE_ZSTENCIL_EN, 0);
    uint32_t *colour = (uint32_t *)(ramin + 0x340);
    colour[0] = 2 | (0x400u << 20); colour[1] = 0xFF; colour[2] = 0;
    memset(ram + 0x400, 0xA5, 0x100);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_COLOR, 0x340);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_CLIP_HORIZONTAL, 2u << 16);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_CLIP_VERTICAL, 2u << 16);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_HORIZONTAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_VERTICAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_CLIP_MIN, 0);
    pgraph_method(gpu, 0, NV097_SET_CLIP_MAX, 0x4B7FFFFF);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_PITCH, 8);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_FORMAT, 8);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_COLOR_OFFSET, 0x20);
    pgraph_method(gpu, 0, NV097_SET_COLOR_CLEAR_VALUE, 0xFF123456);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_HORIZONTAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_VERTICAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
    for (unsigned i = 0; i < 4; ++i) assert(ldl_le_p(ram + 0x420 + i * 4) == 0xFF123456);
    assert(ldl_le_p(ram + 0x41C) == 0xA5A5A5A5);
    assert(ldl_le_p(ram + 0x430) == 0xA5A5A5A5);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_HORIZONTAL, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_VERTICAL, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_COLOR_CLEAR_VALUE, 0xFFAABBCC);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_R);
    assert(ldl_le_p(ram + 0x42C) == 0xFFAA3456);
    assert(ldl_le_p(ram + 0x420) == 0xFF123456 && ldl_le_p(ram + 0x428) == 0xFF123456);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_FORMAT, 3);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_PITCH, 4);
    uint16_t *rgb565 = (uint16_t *)(ram + 0x420);
    for (unsigned i = 0; i < 4; ++i) rgb565[i] = 0x1234;
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_R);
    assert(rgb565[3] == 0xAA34 && rgb565[0] == 0x1234 && rgb565[2] == 0x1234);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_FORMAT, 8);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_PITCH, 8);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_HORIZONTAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_VERTICAL, 1u << 16);
    /* Reports use their own object and the original GPU clock. */
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_REPORT, 0x300);
    pgraph_method(gpu, 0, NV097_CLEAR_REPORT_VALUE, 0);
    uint64_t before = ptimer_get_clock(gpu);
    pgraph_method(gpu, 0, NV097_GET_REPORT, 0x01000100);
    uint64_t stamp; memcpy(&stamp, ram + 0x300, 8);
    assert(stamp >= before && stamp <= ptimer_get_clock(gpu));
    assert(ldl_le_p(ram + 0x308) == 0 && ldl_le_p(ram + 0x30C) == 0);
    assert(ldl_le_p(ram + 0x310) == 0xA5A5A5A5);
    /* Draw through distinct vertex A/B and texture A/B descriptors. */
    uint32_t *vertex_b = (uint32_t *)(ramin + 0x360);
    uint32_t *vertex_a = (uint32_t *)(ramin + 0x380);
    uint32_t *texture_b = (uint32_t *)(ramin + 0x3C0);
    vertex_b[0] = 2 | (0x600u << 20); vertex_b[1] = 0xFF; vertex_b[2] = 0;
    vertex_a[0] = 2 | (0x800u << 20); vertex_a[1] = 0xFF; vertex_a[2] = 0;
    texture_b[0] = 2 | (0xA00u << 20); texture_b[1] = 0xFF; texture_b[2] = 0;
    const float positions[6] = {0, 0, 8, 0, 0, 8};
    memcpy(ram + 0x620, positions, sizeof(positions));
    memset(ram + 0x820, 0, sizeof(positions));
    stl_le_p(ram + 0xA40, 0xFF2468AC);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_VERTEX_A, 0x380);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_VERTEX_B, 0x360);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_A, 0x380);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_B, 0x3C0);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_OFFSET, 0x80000020);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 0x822);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 9 * 4, 0x20);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 9 * 4, 0x822);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_OFFSET, 0x40);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_FORMAT, 0x1202);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL1, 4u << 16);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_IMAGE_RECT, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL0, NV097_SET_TEXTURE_CONTROL0_ENABLE);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    for (unsigned i = 0; i < 4; ++i) assert(ldl_le_p(ram + 0x420 + i * 4) == 0xFF2468AC);
    /* Actual NINE ARRAY_ELEMENT32 index72 uses the same mapped vertex bank. */
    vertex_b[1] = 0x2FF;
    vertex_a[1] = 0x2FF;
    memcpy(ram + 0x620 + 72 * 8, positions, sizeof(positions));
    for (unsigned i = 0; i < 4; ++i) stl_le_p(ram + 0x420 + i * 4, 0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_ARRAY_ELEMENT32, 72);
    pgraph_method(gpu, 0, NV097_ARRAY_ELEMENT32, 74);
    pgraph_method(gpu, 0, NV097_ARRAY_ELEMENT32, 73);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    for (unsigned i = 0; i < 4; ++i) assert(ldl_le_p(ram + 0x420 + i * 4) == 0xFF2468AC);
    vertex_b[1] = 0xFF;
    vertex_a[1] = 0xFF;
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL0, 0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    for (unsigned i = 0; i < 4; ++i) assert(ldl_le_p(ram + 0x420 + i * 4) == 0xFFFFFFFF);
    /* Cache break preserves descriptors and exposes updated vertex memory. */
    gpu->pgraph.regs[NV_PGRAPH_CSV0_C] = 0x80000000u | NV_PGRAPH_CSV0_C_LOCALEYE;
    pgraph_method(gpu, 0, NV097_SET_LIGHT_CONTROL, 0x00020001);
    assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_C] ==
           (0x80000000u | NV_PGRAPH_CSV0_C_SEPARATE_SPECULAR | NV_PGRAPH_CSV0_C_ALPHA_FROM_MATERIAL_SPECULAR));
    pgraph_method(gpu, 0, NV097_SET_LIGHT_CONTROL, 0);
    assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_C] == 0x80000000u);
    gpu->pgraph.regs[NV_PGRAPH_CSV0_C] = 0;
    uint32_t raster_before = gpu->pgraph.regs[NV_PGRAPH_SETUPRASTER];
    gpu->pgraph.regs[NV_PGRAPH_SETUPRASTER] = raster_before | (1u << 24);
    gpu->pgraph.regs[NV_PGRAPH_CSV0_C] = NV_PGRAPH_CSV0_C_LIGHT_TWO_SIDE_ENABLE | 0x100;
    pgraph_method(gpu, 0, NV097_SET_LIGHT_TWO_SIDE_ENABLE, 0);
    assert(gpu->pgraph.regs[NV_PGRAPH_SETUPRASTER] == (raster_before & ~(1u << 24)));
    assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_C] == 0x100);
    gpu->pgraph.regs[NV_PGRAPH_CSV0_C] = 0;
    gpu->pgraph.regs[NV_PGRAPH_CSV0_D] = NV_PGRAPH_CSV0_D_POINTPARAMSENABLE | 0x80000000u;
    gpu->pgraph.regs[NV_PGRAPH_CONTROL_3] = NV_PGRAPH_CONTROL_3_POINTPARAMSENABLE | 0x80000000u;
    pgraph_method(gpu, 0, NV097_SET_POINT_PARAMS_ENABLE, 0);
    assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_D] == 0x80000000u);
    assert(gpu->pgraph.regs[NV_PGRAPH_CONTROL_3] == 0x80000000u);
    gpu->pgraph.regs[NV_PGRAPH_CSV0_D] = 0;
    gpu->pgraph.regs[NV_PGRAPH_CONTROL_3] = 0;
    /* Parameters and their enable bits retain original state without
     * changing the triangle pixels checked immediately below. */

    for(unsigned light=0;light<8;light++) {
        for(unsigned part=0;part<29;part++) {
            uint32_t word=0x3F000000u+light*256+part;
            pgraph_method(gpu,0,NV097_SET_LIGHT_AMBIENT_COLOR+light*128+part*4,word);
            if(part<9) assert(gpu->pgraph.ltctxb[light*6+part/3][part%3]==word);
            else if(part==9) assert(gpu->pgraph.ltc1[NV_IGRAPH_XF_LTC1_r0+light][0]==word);
            else if(part<13) assert(memcmp(&gpu->pgraph.light_infinite_half_vector[light][part-10],&word,4)==0);
            else if(part<16) assert(memcmp(&gpu->pgraph.light_infinite_direction[light][part-13],&word,4)==0);
            else if(part<19) assert(gpu->pgraph.ltctxa[light*2][part-16]==word);
            else if(part<23) assert(gpu->pgraph.ltctxa[light*2+1][part-19]==word);
            else if(part<26) assert(memcmp(&gpu->pgraph.light_local_position[light][part-23],&word,4)==0);
            else assert(memcmp(&gpu->pgraph.light_local_attenuation[light][part-26],&word,4)==0);
        }
    }
    for(unsigned k=0;k<3;k++) {
        uint32_t word=0x3F400000+k;
        pgraph_method(gpu,0,NV097_SET_SCENE_AMBIENT_COLOR+k*4,word);assert(gpu->pgraph.ltctxa[NV_IGRAPH_XF_LTCTXA_FR_AMB][k]==word);
        pgraph_method(gpu,0,NV097_SET_MATERIAL_EMISSION+k*4,word);assert(gpu->pgraph.ltctxa[NV_IGRAPH_XF_LTCTXA_CM_COL][k]==word);
    }
    pgraph_method(gpu,0,NV097_SET_MATERIAL_ALPHA,0x3F000000);assert(gpu->pgraph.material_alpha==0.5f);
    uint32_t light_modes_before=gpu->pgraph.regs[NV_PGRAPH_CSV0_D];
    pgraph_method(gpu,0,NV097_SET_LIGHT_ENABLE_MASK,0x9639);
    assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_D]==((light_modes_before&~0xFFFFu)|0x9639));
    pgraph_method(gpu,0,NV097_SET_LIGHT_ENABLE_MASK,light_modes_before&0xFFFFu);
    memset(gpu->pgraph.ltctxa,0,sizeof gpu->pgraph.ltctxa);memset(gpu->pgraph.ltctxb,0,sizeof gpu->pgraph.ltctxb);memset(gpu->pgraph.ltc1,0,sizeof gpu->pgraph.ltc1);
    const uint32_t point_words[8] = {0, 0x3F800000, 0x40000000, 0x40800000,
                                     0x40800000, 0x40800000, 0x3E800000, 0x3F000000};
    for (unsigned i = 0; i < 8; ++i)
        pgraph_method(gpu, 0, NV097_SET_POINT_PARAMS + i * 4, point_words[i]);
    assert(memcmp(gpu->pgraph.point_params, point_words, sizeof(point_words)) == 0);
    assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_D] == 0);
    assert(gpu->pgraph.regs[NV_PGRAPH_CONTROL_3] == 0);
    pgraph_method(gpu, 0, NV097_SET_POINT_PARAMS_ENABLE, 1);
    assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_D] == NV_PGRAPH_CSV0_D_POINTPARAMSENABLE);
    assert(gpu->pgraph.regs[NV_PGRAPH_CONTROL_3] == NV_PGRAPH_CONTROL_3_POINTPARAMSENABLE);
    uint32_t point_raster_before = gpu->pgraph.regs[NV_PGRAPH_SETUPRASTER];
    pgraph_method(gpu, 0, NV097_SET_POINT_SMOOTH_ENABLE, 1);
    assert(gpu->pgraph.regs[NV_PGRAPH_SETUPRASTER] ==
           (point_raster_before | NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE));
    pgraph_method(gpu, 0, NV097_SET_POINT_SMOOTH_ENABLE, 0);
    assert(gpu->pgraph.regs[NV_PGRAPH_SETUPRASTER] ==
           (point_raster_before & ~NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE));
    pgraph_method(gpu, 0, NV097_SET_POINT_SMOOTH_ENABLE, 1);
    pgraph_method(gpu, 0, NV097_SET_POINT_SIZE, 8);
    assert(gpu->pgraph.regs[NV_PGRAPH_POINTSIZE] == 8);
    pgraph_method(gpu, 0, NV097_SET_POINT_SIZE, NV097_SET_POINT_SIZE_V_MAX + 1);
    assert(gpu->pgraph.regs[NV_PGRAPH_POINTSIZE] == 8);
    float shifted_positions[6] = {8, 8, 16, 8, 8, 16};
    memcpy(ram + 0x620, shifted_positions, sizeof(shifted_positions));
    for (unsigned i = 0; i < 4; ++i) stl_le_p(ram + 0x420 + i * 4, 0xFF123456);
    pgraph_method(gpu, 0, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    for (unsigned i = 0; i < 4; ++i) assert(ldl_le_p(ram + 0x420 + i * 4) == 0xFF123456);
    memcpy(ram + 0x620, positions, sizeof(positions));
    pgraph_method(gpu, 0, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    for (unsigned i = 0; i < 4; ++i) assert(ldl_le_p(ram + 0x420 + i * 4) == 0xFFFFFFFF);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL0, NV097_SET_TEXTURE_CONTROL0_ENABLE);
    ram[0xA40] = 2;
    stl_le_p(ram + 0x888, 0xFFABCDEF);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_FORMAT, 0x0B02);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_PALETTE, 0x8C);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    for (unsigned i = 0; i < 4; ++i) assert(ldl_le_p(ram + 0x420 + i * 4) == 0xFFABCDEF);
    /* Rebinding contexts with unchanged offsets must change the data read. */
    uint32_t *new_a = (uint32_t *)(ramin + 0x400);
    uint32_t *new_vb = (uint32_t *)(ramin + 0x420);
    uint32_t *new_tb = (uint32_t *)(ramin + 0x440);
    new_a[0] = 2 | (0xE00u << 20); new_a[1] = 0xFF; new_a[2] = 0;
    new_vb[0] = 2 | (0xC00u << 20); new_vb[1] = 0xFF; new_vb[2] = 0;
    new_tb[0] = 2 | (0xD00u << 20); new_tb[1] = 0xFF; new_tb[2] = 0;
    memcpy(ram + 0xC20, positions, sizeof(positions));
    memset(ram + 0xE20, 0, sizeof(positions));
    ram[0xD40] = 2;
    stl_le_p(ram + 0xE88, 0xFF987654);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_VERTEX_A, 0x400);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_VERTEX_B, 0x420);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_A, 0x400);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_B, 0x440);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    for (unsigned i = 0; i < 4; ++i) assert(ldl_le_p(ram + 0x420 + i * 4) == 0xFF987654);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_IMAGE_RECT, 0);
    stl_le_p(ram + 0xE80, 0xFFFF0000);
    stl_le_p(ram + 0xE84, 0xFF00FF00);
    stl_le_p(ram + 0xE88, 0xFF0000FF);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, 0x80);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, 0x440);
    pgraph_method(gpu, 0, NV097_SET_SHADE_MODE, NV097_SET_SHADE_MODE_V_FLAT);
    pgraph_method(gpu, 0, NV097_SET_PROVOKING_VERTEX, NV097_SET_PROVOKING_VERTEX_FIRST);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFFFF0000);
    pgraph_method(gpu, 0, NV097_SET_PROVOKING_VERTEX, NV097_SET_PROVOKING_VERTEX_LAST);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF0000FF);
    pgraph_method(gpu, 0, NV097_SET_SHADE_MODE, NV097_SET_SHADE_MODE_V_SMOOTH);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFFDF1010);
    pgraph_method(gpu, 0, NV097_SET_EYE_POSITION + 8, 0x40600000);
    assert(nv2a_vsh_constant(NV_IGRAPH_XF_XFCTX_EYEP)[2] == 3.5f);
    pgraph_method(gpu, 0, NV097_SET_FOG_PLANE + 4, 0x3FC00000);
    assert(nv2a_vsh_constant(NV_IGRAPH_XF_XFCTX_FOG)[1] == 1.5f);
    gpu->pgraph.regs[NV_PGRAPH_CSV0_D] |= NV_PGRAPH_CSV0_D_FOGGENMODE;
    pgraph_method(gpu, 0, NV097_SET_FOG_GEN_MODE, NV097_SET_FOG_GEN_MODE_V_SPEC_ALPHA);
    assert(GET_MASK(gpu->pgraph.regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_FOGGENMODE) ==
           NV_PGRAPH_CSV0_D_FOGGENMODE_SPEC_ALPHA);
    pgraph_method(gpu, 0, NV097_SET_TEXGEN_PLANE_S + 64 + 20, 0x40200000);
    assert(nv2a_vsh_constant(NV_IGRAPH_XF_XFCTX_TG0MAT + 8 + 1)[1] == 2.5f);
    pgraph_method(gpu, 0, NV097_SET_MODEL_VIEW_MATRIX, 0xC69F3E14);
    uint32_t matrix_word;
    memcpy(&matrix_word, nv2a_vsh_constant(NV_IGRAPH_XF_XFCTX_MMAT0), 4);
    assert(matrix_word == 0xC69F3E14);
    const uint32_t matrix_methods[] = {
        NV097_SET_PROJECTION_MATRIX + 15 * 4,
        NV097_SET_MODEL_VIEW_MATRIX + 63 * 4,
        NV097_SET_INVERSE_MODEL_VIEW_MATRIX + 63 * 4,
        NV097_SET_COMPOSITE_MATRIX + 15 * 4,
        NV097_SET_TEXTURE_MATRIX + 63 * 4
    };
    const unsigned matrix_rows[] = {7, 35, 39, 3, 95};
    for (unsigned i = 0; i < 5; ++i) {
        uint32_t word = 0x3F800000u + i;
        pgraph_method(gpu, 0, matrix_methods[i], word);
        memcpy(&matrix_word, nv2a_vsh_constant(matrix_rows[i]) + 3, 4);
        assert(matrix_word == word);
    }
    /* Matrix method writes feed the same constant source read by programs. */
    const uint32_t constant_colour[] = {0x3E800000, 0x3F000000, 0x3F400000, 0x3F800000};
    for (unsigned i = 0; i < 4; ++i)
        pgraph_method(gpu, 0, NV097_SET_MODEL_VIEW_MATRIX + i * 4, constant_colour[i]);
    const uint32_t constant_program[4] = {
        0, (1u << 21) | (8u << 13) | 0x1B, 3u << 26,
        (0xFu << 12) | (1u << 11) | (3u << 3) | 1u
    };
    float constant_input[NV2A_VSH_INPUTS][4] = {{0}};
    Nv2aVshOutput constant_output;
    nv2a_vsh_set_instruction(0, constant_program);
    nv2a_vsh_set_start_slot(0);
    assert(nv2a_vsh_run(constant_input, &constant_output));
    assert(constant_output.d0[0] == 0.25f && constant_output.d0[1] == 0.5f &&
           constant_output.d0[2] == 0.75f && constant_output.d0[3] == 1.0f);
    /* The same colour state must also reach actual uploaded vertex programs. */
    const uint32_t *program=s_passthrough_program;
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_PROGRAM_START, 0);
    for (unsigned i = 0; i < 8; ++i)
        pgraph_method(gpu, 0, NV097_SET_TRANSFORM_PROGRAM + i * 4, program[i]);
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, 2);
    uint32_t lighting_before = gpu->pgraph.regs[NV_PGRAPH_CSV0_C];
    const uint32_t material_mask = NV_PGRAPH_CSV0_C_EMISSION |
        NV_PGRAPH_CSV0_C_AMBIENT | NV_PGRAPH_CSV0_C_DIFFUSE | NV_PGRAPH_CSV0_C_SPECULAR;
    const uint32_t material_sources[] = {0, 0xa5, 0x6a, 0xaa};
    for (unsigned choice = 0; choice < 4; ++choice) {
        uint32_t parameter = material_sources[choice];
        uint32_t preserved = lighting_before | NV_PGRAPH_CSV0_C_LIGHTING |
                             NV_PGRAPH_CSV0_C_LOCALEYE | 0x80000000u;
        gpu->pgraph.regs[NV_PGRAPH_CSV0_C] = preserved | material_mask;
        pgraph_method(gpu, 0, NV097_SET_COLOR_MATERIAL, parameter);
        uint32_t expected = preserved & ~material_mask;
        SET_MASK(expected, NV_PGRAPH_CSV0_C_EMISSION, (parameter >> 0) & 3);
        SET_MASK(expected, NV_PGRAPH_CSV0_C_AMBIENT, (parameter >> 2) & 3);
        SET_MASK(expected, NV_PGRAPH_CSV0_C_DIFFUSE, (parameter >> 4) & 3);
        SET_MASK(expected, NV_PGRAPH_CSV0_C_SPECULAR, (parameter >> 6) & 3);
        assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_C] == expected);
    }
    gpu->pgraph.regs[NV_PGRAPH_CSV0_C] = lighting_before;
    pgraph_method(gpu, 0, NV097_SET_COLOR_MATERIAL, 0);
    lighting_before = gpu->pgraph.regs[NV_PGRAPH_CSV0_C];
    pgraph_method(gpu, 0, NV097_SET_LIGHTING_ENABLE, 1);
    assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_C] ==
           (lighting_before | NV_PGRAPH_CSV0_C_LIGHTING));
    pgraph_method(gpu, 0, NV097_SET_SHADE_MODE, NV097_SET_SHADE_MODE_V_FLAT);
    pgraph_method(gpu, 0, NV097_SET_PROVOKING_VERTEX, NV097_SET_PROVOKING_VERTEX_FIRST);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFFFF0000);
    pgraph_method(gpu, 0, NV097_SET_PROVOKING_VERTEX, NV097_SET_PROVOKING_VERTEX_LAST);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF0000FF);
    pgraph_method(gpu, 0, NV097_SET_LIGHTING_ENABLE, 0);
    assert(gpu->pgraph.regs[NV_PGRAPH_CSV0_C] ==
           (lighting_before & ~NV_PGRAPH_CSV0_C_LIGHTING));
    fprintf(stderr, "[LIGHTING] original CSV0_C enable preserves program triangle colours\n");
    /* Repeated indices with enabled context writes must execute each vertex. */
    const uint32_t increment_constant[4] = {
        0, (3u << 21) | (2u << 13) | (3u << 9),
        (3u << 26), (2u << 28) | (8u << 12) | (2u << 3)
    };
    const float zero_constant[4] = {0};
    nv2a_vsh_set_constant(2, zero_constant);
    nv2a_vsh_set_instruction(0, increment_constant);
    nv2a_vsh_set_instruction(1, program);
    nv2a_vsh_set_instruction(2, program + 4);
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 1);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_ARRAY_ELEMENT16, 0);
    pgraph_method(gpu, 0, NV097_ARRAY_ELEMENT32, 0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(nv2a_vsh_constant(2)[0] == 3.0f);
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
    nv2a_vsh_set_instruction(0, program);
    nv2a_vsh_set_instruction(1, program + 4);
    /* NV97 quads use their fourth colour regardless of the selection bit. */
    const float quad[8] = {0, 0, 8, 0, 8, 8, 0, 8};
    memcpy(ram + 0xC20, quad, sizeof(quad));
    stl_le_p(ram + 0xE8C, 0xFFFFFF00);
    pgraph_method(gpu, 0, NV097_SET_PROVOKING_VERTEX, NV097_SET_PROVOKING_VERTEX_FIRST);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 8);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 3u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFFFFFF00 && ldl_le_p(ram + 0x428) == 0xFFFFFF00);
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, 0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 8);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 3u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFFFFFF00 && ldl_le_p(ram + 0x428) == 0xFFFFFF00);
    /* Logical depth survives a compression toggle, and distinct DMA bases
     * with the same zeta offset retain separate depth surfaces. */
    memcpy(ram + 0xC20, positions, sizeof(positions));
    pgraph_method(gpu, 0, NV097_SET_SURFACE_FORMAT, 0x28);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_PITCH, 0x00080008);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_ZETA, 0x400);
    pgraph_method(gpu, 0, NV097_SET_SURFACE_ZETA_OFFSET, 0xA0);
    pgraph_method(gpu, 0, NV097_SET_COMPRESS_ZBUFFER_EN, 1);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_FUNC, 0x201);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_MASK, 1);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_TEST_ENABLE, 1);
    pgraph_method(gpu, 0, NV097_SET_ZSTENCIL_CLEAR_VALUE, 0x01000000);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 1);
    stl_le_p(ram + 0xE80, 0xFFFF0000);
    /* Optional depth/stencil early culling retains the exact per-pixel tests.
     * A passing red triangle and equal-depth green triangle must produce the
     * same occlusion result on both input paths and all four enable states. */
    for (unsigned path = 0; path < 2; ++path) for (unsigned cull = 0; cull < 4; ++cull) {
        pgraph_method(gpu, 0, NV097_SET_OCCLUDE_ZSTENCIL_EN, cull);
        pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, path * 2);
        pgraph_method(gpu, 0, NV097_SET_COMPRESS_ZBUFFER_EN, 1);
        pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 1);
        stl_le_p(ram + 0xE80, 0xFFFF0000);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFFFF0000);
        pgraph_method(gpu, 0, NV097_SET_COMPRESS_ZBUFFER_EN, 0);
        stl_le_p(ram + 0xE80, 0xFF00FF00);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFFFF0000);
    }
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, 0);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_ZETA, 0x420);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 1);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF00FF00);
    pgraph_method(gpu, 0, NV097_SET_CONTEXT_DMA_ZETA, 0x400);
    stl_le_p(ram + 0xE80, 0xFF0000FF);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF00FF00);
    stl_le_p(ram + 0xE80, 0xFF00FF00);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_TEST_ENABLE, 0);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_MASK, 0);
    /* Rectangle depth clear changes only the chosen pixel on the same
     * surface; stencil-only clear leaves that depth intact. */
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_HORIZONTAL, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_VERTICAL, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_ZSTENCIL_CLEAR_VALUE, 0x01000000);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_Z);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_HORIZONTAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_VERTICAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_ZSTENCIL_CLEAR_VALUE, 0xA5);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_STENCIL);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_MASK, 1);
    stl_le_p(ram + 0xE80, 0xFF0000FF);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    pgraph_method(gpu, 0, NV097_SET_COLOR_CLEAR_VALUE, 0xFF00FF00);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_TEST_ENABLE, 1);
    stl_le_p(ram + 0xE80, 0xFFFF0000);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF00FF00 && ldl_le_p(ram + 0x42C) == 0xFFFF0000);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_TEST_ENABLE, 0);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_HORIZONTAL, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_VERTICAL, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_ZSTENCIL_CLEAR_VALUE, 0xB6);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_STENCIL);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_HORIZONTAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_CLEAR_RECT_VERTICAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_COLOR_CLEAR_VALUE, 0xFF000000);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
    pgraph_method(gpu, 0, NV097_SET_STENCIL_FUNC, 0x202);
    pgraph_method(gpu, 0, NV097_SET_STENCIL_FUNC_REF, 6);
    pgraph_method(gpu, 0, NV097_SET_STENCIL_FUNC_MASK, 0xF);
    pgraph_method(gpu, 0, NV097_SET_STENCIL_OP_FAIL, NV097_SET_STENCIL_OP_V_KEEP);
    pgraph_method(gpu, 0, NV097_SET_STENCIL_OP_ZFAIL, NV097_SET_STENCIL_OP_V_KEEP);
    pgraph_method(gpu, 0, NV097_SET_STENCIL_OP_ZPASS, NV097_SET_STENCIL_OP_V_KEEP);
    pgraph_method(gpu, 0, NV097_SET_STENCIL_TEST_ENABLE, 1);
    stl_le_p(ram + 0xE80, 0xFF0000FF);
    for (unsigned path = 0; path < 2; ++path) for (unsigned cull = 0; cull < 4; ++cull) {
        pgraph_method(gpu, 0, NV097_SET_OCCLUDE_ZSTENCIL_EN, cull);
        pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, path * 2);
        pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFF000000 && ldl_le_p(ram + 0x42C) == 0xFF0000FF);
    }
    pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, 0);
    pgraph_method(gpu, 0, NV097_SET_OCCLUDE_ZSTENCIL_EN, 0);
    pgraph_method(gpu, 0, NV097_SET_STENCIL_FUNC, 0x205);
    stl_le_p(ram + 0xE80, 0xFF00FF00);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF00FF00 && ldl_le_p(ram + 0x42C) == 0xFF0000FF);
    pgraph_method(gpu, 0, NV097_SET_STENCIL_TEST_ENABLE, 0);
    pgraph_method(gpu, 0, NV097_SET_COLOR_CLEAR_VALUE, 0xFF000000);
    pgraph_method(gpu, 0, NV097_SET_CULL_FACE_ENABLE, 1);
    pgraph_method(gpu, 0, NV097_SET_CULL_FACE, NV097_SET_CULL_FACE_V_BACK);
    pgraph_method(gpu, 0, NV097_SET_FRONT_FACE, NV097_SET_FRONT_FACE_V_CW);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF00FF00);
    pgraph_method(gpu, 0, NV097_SET_FRONT_FACE, NV097_SET_FRONT_FACE_V_CCW);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF000000);
    pgraph_method(gpu, 0, NV097_SET_CULL_FACE_ENABLE, 0);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_HORIZONTAL, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_VERTICAL, 0x00010001);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF000000 && ldl_le_p(ram + 0x42C) == 0xFF00FF00);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_TYPE, 1);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF00FF00 && ldl_le_p(ram + 0x42C) == 0xFF000000);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_TYPE, 0);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_HORIZONTAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_WINDOW_CLIP_VERTICAL, 1u << 16);
    pgraph_method(gpu, 0, NV097_SET_CLIP_MIN, 0x3F800000);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF000000);
    pgraph_method(gpu, 0, NV097_SET_ZMIN_MAX_CONTROL, 0x10);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF00FF00);
    pgraph_method(gpu, 0, NV097_SET_CLIP_MIN, 0);
    pgraph_method(gpu, 0, NV097_SET_ZMIN_MAX_CONTROL, 0);
    /* Equal Z values and unequal W clipping use the same pixel consumer. */
    const float w_positions[12] = {0,0,0,2, 8,0,0,2, 0,8,0,2};
    memcpy(ram + 0xC20, w_positions, sizeof(w_positions));
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 0x1042);
    pgraph_method(gpu, 0, NV097_SET_CLIP_MAX, 0x3F800000);
    pgraph_method(gpu, 0, NV097_SET_CONTROL0, NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE);
    pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF000000);
    pgraph_method(gpu, 0, NV097_SET_ZMIN_MAX_CONTROL, 0x10);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
    pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
    pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
    assert(ldl_le_p(ram + 0x420) == 0xFF00FF00);
    pgraph_method(gpu, 0, NV097_SET_CONTROL0, 0);
    pgraph_method(gpu, 0, NV097_SET_ZMIN_MAX_CONTROL, 0);
    pgraph_method(gpu, 0, NV097_SET_CLIP_MAX, 0x4B7FFFFF);
    /* Strip winding alternates before culling; colour selection keeps the
     * original first vertex on each triangle, on both input paths. */
    const float strip[8] = {0,-6, 8,-6, 0,2, 8,2};
    memcpy(ram + 0xC20, strip, sizeof(strip));
    stl_le_p(ram + 0xE80, 0xFFFF0000);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 0x822);
    pgraph_method(gpu, 0, NV097_SET_CULL_FACE_ENABLE, 1);
    pgraph_method(gpu, 0, NV097_SET_FRONT_FACE, NV097_SET_FRONT_FACE_V_CW);
    for (unsigned path = 0; path < 2; ++path) {
        pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, path * 2);
        pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 6);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 3u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFFFF0000 && ldl_le_p(ram + 0x42C) == 0xFF00FF00);
    }
    /* Alpha kill uses sampled texture alpha, including alpha=1/255, and
     * must leave depth untouched even when colour writes are disabled. */
    pgraph_method(gpu, 0, NV097_SET_CULL_FACE_ENABLE, 0);
    memcpy(ram + 0xC20, positions, sizeof(positions));
    for (unsigned i = 0; i < 3; ++i) stl_le_p(ram + 0xE80 + i * 4, 0xFFFFFFFF);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_OFFSET, 0x40);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_FORMAT, 0x1202);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_IMAGE_RECT, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL1, 4u << 16);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_TEST_ENABLE, 1);
    pgraph_method(gpu, 0, NV097_SET_DEPTH_MASK, 1);
    pgraph_method(gpu, 0, NV097_SET_COLOR_CLEAR_VALUE, 0xFF000000);
    pgraph_method(gpu, 0, NV097_SET_ZSTENCIL_CLEAR_VALUE, 0x01000000);
    for (unsigned path = 0; path < 2; ++path) {
        pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, path * 2);
        pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL0, NV097_SET_TEXTURE_CONTROL0_ENABLE | 4);
        pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF1);
        stl_le_p(ram + 0xD40, 0x002468AC);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFF000000);
        pgraph_method(gpu, 0, NV097_SET_COLOR_MASK, 0);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        pgraph_method(gpu, 0, NV097_SET_COLOR_MASK, 0x01010101);
        stl_le_p(ram + 0xD40, 0x012468AC);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0x012468AC);
        pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF1);
        pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL0, NV097_SET_TEXTURE_CONTROL0_ENABLE);
        stl_le_p(ram + 0xD40, 0x002468AC);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0x002468AC);
    }
    /* Stage one kills before the final combiner forces output alpha to one.
     * Disabled textures and PASSTHRU perform no alpha-killed texture fetch. */
    pgraph_method(gpu, 0, NV097_SET_DEPTH_TEST_ENABLE, 0);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_OFFSET + 64, 0x44);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_FORMAT + 64, 0x1202);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_IMAGE_RECT + 64, 0x00010001);
    pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL1 + 64, 4u << 16);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_CONTROL, 0);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 9);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x2000);
    for (unsigned path = 0; path < 2; ++path) {
        pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, path * 2);
        pgraph_method(gpu, 0, NV097_SET_SHADER_STAGE_PROGRAM, 1u << 5);
        pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL0 + 64, NV097_SET_TEXTURE_CONTROL0_ENABLE | 4);
        pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
        stl_le_p(ram + 0xD44, 0x00406080);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFF000000);
        stl_le_p(ram + 0xD44, 0x01406080);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFF406080);
        pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL0 + 64, 4);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFFFFFFFF);
        pgraph_method(gpu, 0, NV097_SET_TEXTURE_CONTROL0 + 64, NV097_SET_TEXTURE_CONTROL0_ENABLE | 4);
        pgraph_method(gpu, 0, NV097_SET_SHADER_STAGE_PROGRAM, 4u << 5);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFF000000);
    }
    /* Original fixed fog source selection feeds the existing fog/combiner
     * pixels. Program oFog stays authoritative regardless of foggen. */
    const float specular[4] = {0, 0, 0, 0.25f}, fog_coord = 0.75f;
    for (unsigned i = 0; i < 3; ++i) {
        memcpy(ram + 0xE90 + i * 16, specular, sizeof(specular));
        memcpy(ram + 0xEC0 + i * 4, &fog_coord, sizeof(fog_coord));
    }
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * 4, 0x90);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * 4, 0x1042);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 5 * 4, 0xC0);
    pgraph_method(gpu, 0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 5 * 4, 0x412);
    pgraph_method(gpu, 0, NV097_SET_SHADER_STAGE_PROGRAM, 0);
    pgraph_method(gpu, 0, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x13);
    pgraph_method(gpu, 0, NV097_SET_FOG_MODE, NV097_SET_FOG_MODE_V_LINEAR);
    pgraph_method(gpu, 0, NV097_SET_FOG_PARAMS, 0x3F800000);
    pgraph_method(gpu, 0, NV097_SET_FOG_PARAMS + 4, 0x3F800000);
    pgraph_method(gpu, 0, NV097_SET_FOG_PARAMS + 8, 0);
    for (unsigned path = 0; path < 2; ++path) {
        pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, path * 2);
        for (unsigned source = 0; source < 2; ++source) {
            pgraph_method(gpu, 0, NV097_SET_FOG_GEN_MODE, source ? NV097_SET_FOG_GEN_MODE_V_FOG_X :
                                                                NV097_SET_FOG_GEN_MODE_V_SPEC_ALPHA);
            assert(GET_MASK(gpu->pgraph.regs[NV_PGRAPH_CSV0_D], NV_PGRAPH_CSV0_D_FOGGENMODE) ==
                   (source ? NV_PGRAPH_CSV0_D_FOGGENMODE_FOG_X : NV_PGRAPH_CSV0_D_FOGGENMODE_SPEC_ALPHA));
            pgraph_method(gpu, 0, NV097_SET_FOG_ENABLE, 1);
            pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
            pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
            pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
            assert(ldl_le_p(ram + 0x420) == (path ? 0xFF000000 : (source ? 0xFFBFBFBF : 0xFF404040)));
            pgraph_method(gpu, 0, NV097_SET_FOG_ENABLE, 0);
            pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
            pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
            pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
            assert(ldl_le_p(ram + 0x420) == 0xFFFFFFFF);
        }
    }
    /* The original battle enables dithering. The consumer's byte channels
     * need no further precision reduction on an A8R8G8B8 target. Exercise
     * the real register, clear and both drawing paths without changing other
     * CONTROL_0 fields or silently accepting lower-precision reduction. */
    uint32_t control_before = gpu->pgraph.regs[NV_PGRAPH_CONTROL_0];
    pgraph_method(gpu, 0, NV097_SET_DITHER_ENABLE, 1);
    assert(gpu->pgraph.regs[NV_PGRAPH_CONTROL_0] ==
           (control_before | NV_PGRAPH_CONTROL_0_DITHERENABLE));
    pgraph_method(gpu, 0, NV097_SET_COLOR_CLEAR_VALUE, 0xC1794F13);
    for (unsigned path = 0; path < 2; ++path) {
        pgraph_method(gpu, 0, NV097_SET_TRANSFORM_EXECUTION_MODE, path * 2);
        pgraph_method(gpu, 0, NV097_CLEAR_SURFACE, 0xF0);
        assert(ldl_le_p(ram + 0x420) == 0xC1794F13);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 5);
        pgraph_method(gpu, 0, NV097_DRAW_ARRAYS, 2u << 24);
        pgraph_method(gpu, 0, NV097_SET_BEGIN_END, 0);
        assert(ldl_le_p(ram + 0x420) == 0xFFFFFFFF);
    }
    pgraph_method(gpu, 0, NV097_SET_DITHER_ENABLE, 0);
    assert(gpu->pgraph.regs[NV_PGRAPH_CONTROL_0] ==
           (control_before & ~NV_PGRAPH_CONTROL_0_DITHERENABLE));
    fprintf(stderr, "[DITHER] original bit, RGB8 clear and both pixel paths passed\n");
    check_screen_program_w();
    check_compiled_signed_mmio(gpu);
    return 0;
}
#if defined(_WIN32)
void xbox_FramebufferWindowPresentHost(const void *pixels,unsigned pitch,unsigned width,unsigned height) {
    (void)pixels;(void)pitch;(void)width;(void)height;
}
#endif
