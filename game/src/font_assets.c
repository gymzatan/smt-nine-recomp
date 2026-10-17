#include "font_assets.h"
#include <windows.h>
#include "game_paths.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
typedef struct FontPage {uint8_t *base,*pixels;uint32_t width,height,base_bytes,pixel_bytes;int used;} FontPage;
static FontPage *pages;static unsigned page_count;
static uint8_t *title_base,*title_pixels;
static int title_used;
typedef const void *(*TextureAssetLookup)(const void *,size_t,unsigned,unsigned,unsigned *,unsigned *);
extern void d3d8_SetTextureAssetLookup(TextureAssetLookup lookup);
static const void *lookup(const void *raw,size_t bytes,unsigned width,unsigned height,unsigned *host_width,unsigned *host_height)
{
    if(title_base && width==512 && height==512 && bytes==1048576 &&
       !memcmp(raw,title_base,64) && !memcmp(raw,title_base,bytes)) {
        *host_width=512;*host_height=512;
        if(!title_used){fprintf(stderr,"[TITLE GPU] original background with proportional logo layers\n");title_used=1;}
        return title_pixels;
    }
    if(width!=512 || height!=512 || bytes!=131072)return NULL;
    for(unsigned i=0;i<page_count;i++)if(!memcmp(raw,pages[i].base,64) && !memcmp(raw,pages[i].base,bytes)) {
        *host_width=pages[i].width;*host_height=pages[i].height;
        if(!pages[i].used){fprintf(stderr,"[FONT GPU] page=%u logical=512x512 host=%ux%u\n",i,*host_width,*host_height);pages[i].used=1;}
        return pages[i].pixels;
    }
    return NULL;
}
int nine_title_assets_init(float aspect)
{
    float factor=(4.0f/3.0f)/aspect;
    if(factor==1.0f)return 1;
    char path[MAX_PATH];snprintf(path,sizeof path,"%s/title_layers.bin",SMT9_RUNTIME_DIR);
    FILE *file=fopen(path,"rb");if(!file){fprintf(stderr,"[TITLE] missing title layers: %s\n",path);return 0;}
    char magic[8];uint32_t header[3];
    int ok=fread(magic,8,1,file)==1 && !memcmp(magic,"N9TITLE\0",8) &&
        fread(header,sizeof header,1,file)==1 && header[0]==512 && header[1]==512 && header[2]==1048576;
    if(!ok){fclose(file);return 0;}
    title_base=malloc(1048576);title_pixels=malloc(1048576);uint8_t *logos=malloc(1048576);
    ok=title_base && title_pixels && logos && fread(title_base,1048576,1,file)==1 &&
        fread(title_pixels,1048576,1,file)==1 && fread(logos,1048576,1,file)==1;
    if(ok)ok=fgetc(file)==EOF && !ferror(file);fclose(file);
    if(!ok){free(logos);return 0;}
    /* The full-screen background stretches; logos share the title menu anchor. */
    for(unsigned y=0;y<512;y++)for(unsigned x=0;x<512;x++) {
        float source=((float)x+0.5f-NINE_TITLE_UI_ORIGIN)/factor+NINE_TITLE_UI_ORIGIN-0.5f;
        if(source<0 || source>=511)continue;
        unsigned left=(unsigned)source;float t=source-left;
        const uint8_t *a=logos+((size_t)y*512+left)*4,*b=a+4;
        float aa=a[3]/255.0f,ba=b[3]/255.0f,alpha=aa*(1-t)+ba*t;
        uint8_t *pixel=title_pixels+((size_t)y*512+x)*4;
        for(unsigned channel=0;channel<3;channel++)
            pixel[channel]=(uint8_t)(pixel[channel]*(1-alpha)+a[channel]*aa*(1-t)+b[channel]*ba*t+0.5f);
    }
    free(logos);d3d8_SetTextureAssetLookup(lookup);return 1;
}
int nine_font_assets_init(unsigned scale)
{
    if(scale==1)return 1;
    char path[MAX_PATH];snprintf(path,sizeof path,"%s/fonts/font%u.bin",SMT9_RUNTIME_DIR,scale);
    FILE *file=fopen(path,"rb");if(!file){fprintf(stderr,"[FONT] missing generated font: %s\n",path);return 0;}
    char magic[8];uint32_t parameters[2];int ok=fread(magic,8,1,file)==1 && !memcmp(magic,"N9FONT\0\0",8) && fread(parameters,sizeof parameters,1,file)==1 && parameters[0]==scale && parameters[1]>0 && parameters[1]<=128;
    if(!ok){fclose(file);return 0;}
    page_count=parameters[1];pages=calloc(page_count,sizeof *pages);if(!pages){fclose(file);return 0;}
    for(unsigned i=0;ok && i<page_count;i++) {
        FontPage *p=&pages[i];uint32_t header[4];ok=fread(header,sizeof header,1,file)==1;
        if(!ok)break;
        p->width=header[0];p->height=header[1];p->base_bytes=header[2];p->pixel_bytes=header[3];
        if(p->width!=512*scale || p->height!=512*scale || p->base_bytes!=131072 || p->pixel_bytes!=p->width*p->height*4){ok=0;break;}
        p->base=malloc(p->base_bytes);p->pixels=malloc(p->pixel_bytes);
        ok=p->base && p->pixels && fread(p->base,p->base_bytes,1,file)==1 && fread(p->pixels,p->pixel_bytes,1,file)==1;
    }
    if(ok)ok=fgetc(file)==EOF && !ferror(file);fclose(file);
    unsigned page=0;
    for(unsigned font=0;ok && font<2;font++) {
        snprintf(path,sizeof path,"%s/Media/SysFont/sys_f%u.xpr",SMT9_GAME_DIR,font?18:24);file=fopen(path,"rb");if(!file){ok=0;break;}
        uint32_t header[3];ok=fread(header,sizeof header,1,file)==1 && header[0]==0x30525058 && header[2]>=12 && header[1]>=header[2] && (header[1]-header[2])%131072==0;
        if(ok)ok=fseek(file,(long)header[2],SEEK_SET)==0;
        uint8_t *base=malloc(131072);if(!base)ok=0;
        for(unsigned i=0;ok && i<(header[1]-header[2])/131072;i++,page++)ok=page<page_count && fread(base,131072,1,file)==1 && !memcmp(base,pages[page].base,131072);
        free(base);fclose(file);
    }
    if(!ok || page!=page_count){fprintf(stderr,"[FONT] generated atlases differ from active font resources; run build_native_fonts.py\n");return 0;}
    d3d8_SetTextureAssetLookup(lookup);fprintf(stderr,"[FONT] original maps, native scale=%u, pages=%u\n",scale,page_count);return 1;
}
