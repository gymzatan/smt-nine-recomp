
static void verify_luma16_upload(float expected) {
    if(!hw_enabled())return;
    HwTexture *entry=s_hw_textures;while(entry && entry->identity.offset!=s_gpu.texs[0].offset)entry=entry->next;assert(entry);
    float actual=0;
    if(entry->native){float pixels[4];assert(nvk_image_read(entry->native,pixels,8));actual=pixels[0];}
    else {
        D3D8Texture *texture=(D3D8Texture *)entry->texture;D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(texture->d3d11_texture,&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        ID3D11Texture2D *staging=NULL;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(),&desc,NULL,&staging)));ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource *)staging,(ID3D11Resource *)texture->d3d11_texture);D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped)));actual=*(float *)mapped.pData;ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource *)staging,0);ID3D11Texture2D_Release(staging);
    }
    fprintf(stderr,"[LUMA16 UPLOAD] actual=%.9g expected=%.9g raw=%u\n",actual,expected,*(uint16_t *)entry->raw);assert(actual==expected);
}
static void verify_luma16_texture(void) {
    seed();method(NV097_SET_BEGIN_END,0);method(NV097_SET_DEPTH_TEST_ENABLE,0);method(NV097_SET_TRANSFORM_EXECUTION_MODE,0);method(NV097_SET_CONTEXT_DMA_A,0x100);
    method(NV097_SET_TEXTURE_OFFSET,0x80000);method(NV097_SET_TEXTURE_FORMAT,(2u<<4)|(D3DFMT_LIN_L16<<8)|1);method(NV097_SET_TEXTURE_IMAGE_RECT,(2u<<16)|2);method(NV097_SET_TEXTURE_CONTROL1,8u<<16);method(NV097_SET_TEXTURE_CONTROL0,NV_PGRAPH_TEXCTL0_0_ENABLE);
    method(NV097_SET_TEXTURE_ADDRESS,3u|(3u<<8));method(NV097_SET_TEXTURE_FILTER,0x01010000);method(NV097_SET_SHADER_STAGE_PROGRAM,1);
    method(NV097_SET_COMBINER_COLOR_ICW,(8u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(24u<<24)|0x200000);
    const uint16_t raw[]={0,0x1234,0x8001,65535,32767,32768};
    for(unsigned i=0;i<sizeof raw/sizeof raw[0];i++) {
        uint16_t *data=(uint16_t *)(xbox_GetMemoryOffset()+0x80080000u);data[0]=data[1]=data[4]=data[5]=raw[i];
        float value[4],expected=raw[i]/65535.0f;uint32_t packed;
        assert(sample_tex_value(&s_gpu.texs[0],0,0,0,0,0,&packed,value));
        assert(value[0]==expected && value[1]==expected && value[2]==expected && value[3]==1);
        shadow_coordinates_for_test(0,0,1);uint32_t pixel=shadow_draw_for_test();verify_luma16_upload(expected);float rgba[4]={expected,expected,expected,1};
        float *colors=(float *)(xbox_GetMemoryOffset()+0x8006e000u);for(unsigned v=0;v<3;v++)memcpy(colors+v*4,rgba,sizeof rgba);
        method(NV097_SET_VERTEX_DATA_ARRAY_OFFSET+12,0x6e000);method(NV097_SET_VERTEX_DATA_ARRAY_FORMAT+12,0x1042);
        method(NV097_SET_COMBINER_COLOR_ICW,(4u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(20u<<24)|0x200000);
        uint32_t control=shadow_draw_for_test();if(!hw_enabled())assert(pixel==pack_color(rgba));fprintf(stderr,"[LUMA16 CONTROL] raw=%u pixel=%08x control=%08x software=%08x\n",raw[i],pixel,control,pack_color(rgba));if(hw_enabled())assert(pixel==control);
        method(NV097_SET_COMBINER_COLOR_ICW,(8u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(24u<<24)|0x200000);
    }
    /* Filtering uses original float taps; two neighboring encoded values remain distinct. */
    uint16_t *data=(uint16_t *)(xbox_GetMemoryOffset()+0x80080000u);data[0]=32767;data[1]=32768;data[4]=32768;data[5]=32767;
    method(NV097_SET_TEXTURE_FILTER,0x02020000);shadow_coordinates_for_test(0,0,1);float *coordinates=(float *)(xbox_GetMemoryOffset()+0x80050000u);for(unsigned vertex=0;vertex<3;vertex++)coordinates[vertex*4]=coordinates[vertex*4+1]=1;uint32_t filtered_pixel=shadow_draw_for_test();
    float *colors=(float *)(xbox_GetMemoryOffset()+0x8006e000u);for(unsigned v=0;v<3;v++){colors[v*4]=colors[v*4+1]=colors[v*4+2]=.5f;colors[v*4+3]=1;}
    method(NV097_SET_COMBINER_COLOR_ICW,(4u<<24)|0x200000);method(NV097_SET_COMBINER_ALPHA_ICW,(20u<<24)|0x200000);uint32_t filtered_control=shadow_draw_for_test();if(hw_enabled())assert(filtered_pixel==filtered_control);else assert(filtered_pixel==0xff808080);
    float filtered[4];rc_texel(&s_gpu.texs[0],0,1,1,0,0,filtered);assert(fabsf(filtered[0]-.5f)<0.0000001f && filtered[1]==filtered[0] && filtered[2]==filtered[0] && filtered[3]==1);
    fprintf(stderr,"[LUMA16] legal pitchedY16 uses real16bitRAM and original float sampler/cache, exact channels/alpha, adjacent values and filter passed\n");
}

