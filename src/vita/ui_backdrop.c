#include "ui_backdrop.h"
#include "ui_presentation.h"
#ifndef VP_UI_TESTING
#include "vita_thread_priority.h"
#endif
#include <png.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WALL_WIDTH 960
#define WALL_HEIGHT 544
typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool running,pending,ready,busy;
    char request[48],result[48];
    unsigned char *pixels;
} WallpaperWorker;
static WallpaperWorker worker={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
static vita2d_texture *current,*previous;
static char desired[48],loaded[48],failed[48];
static unsigned long long changed_at,fade_at;
static bool started;

static unsigned char *decode(const char *name) {
    char path[160];
    png_image image;
    unsigned char *pixels;
    FILE *file;
    long size;
    (void)snprintf(path,sizeof(path),"%sassets/blueglass/wallpapers/%s.png",
#ifdef VP_UI_TESTING
        "",
#else
        "app0:",
#endif
        name);
    file=fopen(path,"rb");
    if (!file) return NULL;
    if (fseek(file,0,SEEK_END) || (size=ftell(file))<=0 || size>2*1024*1024) { fclose(file); return NULL; }
    fclose(file);
    memset(&image,0,sizeof(image)); image.version=PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image,path)) return NULL;
    if (image.width!=WALL_WIDTH || image.height!=WALL_HEIGHT) { png_image_free(&image); return NULL; }
    image.format=PNG_FORMAT_RGBA;
    pixels=malloc(PNG_IMAGE_SIZE(image));
    if (!pixels || !png_image_finish_read(&image,NULL,pixels,0,NULL)) { free(pixels); pixels=NULL; }
    png_image_free(&image);
    return pixels;
}

static void *run(void *unused) {
    (void)unused;
#ifndef VP_UI_TESTING
    vp_vita_thread_set_priority(VP_VITA_THREAD_PRIORITY_BACKGROUND);
#endif
    for (;;) {
        char name[48];
        unsigned char *pixels;
        pthread_mutex_lock(&worker.mutex);
        while (worker.running && (!worker.pending || worker.ready)) pthread_cond_wait(&worker.condition,&worker.mutex);
        if (!worker.running) { pthread_mutex_unlock(&worker.mutex); break; }
        (void)snprintf(name,sizeof(name),"%s",worker.request); worker.pending=false;
        pthread_mutex_unlock(&worker.mutex);
        pixels=decode(name);
        pthread_mutex_lock(&worker.mutex);
        worker.pixels=pixels; worker.ready=true;
        (void)snprintf(worker.result,sizeof(worker.result),"%s",name);
        pthread_mutex_unlock(&worker.mutex);
    }
    return NULL;
}

bool vp_ui_backdrop_init(void) {
    if (started) return true;
    worker.running=true;
    if (pthread_create(&worker.thread,NULL,run,NULL)) { worker.running=false; return false; }
    started=true;
    return true;
}

static vita2d_texture *upload(unsigned char *pixels) {
    vita2d_texture *texture;
    unsigned int row,stride;
    unsigned char *destination;
    if (!pixels) return NULL;
    texture=vita2d_create_empty_texture(WALL_WIDTH,WALL_HEIGHT);
    if (!texture) return NULL;
    stride=vita2d_texture_get_stride(texture);
    destination=vita2d_texture_get_datap(texture);
    for (row=0;row<WALL_HEIGHT;row++) memcpy(destination+row*stride,pixels+row*WALL_WIDTH*4,WALL_WIDTH*4);
    vita2d_texture_set_filters(texture,SCE_GXM_TEXTURE_FILTER_LINEAR,SCE_GXM_TEXTURE_FILTER_LINEAR);
    return texture;
}

void vp_ui_backdrop_prepare(int wallpaper,unsigned long long now) {
    const char *name=vp_ui_wallpaper(wallpaper)->id;
    if (!started) return;
    if (strcmp(desired,name)) {
        (void)snprintf(desired,sizeof(desired),"%s",name); changed_at=now;
        failed[0]='\0';
    }
    if (previous && now-fade_at>=220000) { vita2d_free_texture(previous); previous=NULL; }
    pthread_mutex_lock(&worker.mutex);
    if (worker.ready) {
        unsigned char *pixels=worker.pixels;
        char result[48];
        (void)snprintf(result,sizeof(result),"%s",worker.result);
        worker.pixels=NULL; worker.ready=false; worker.busy=false;
        pthread_cond_signal(&worker.condition);
        pthread_mutex_unlock(&worker.mutex);
        if (!strcmp(result,desired)) {
            vita2d_texture *texture=upload(pixels);
            if (texture) {
                if (previous) vita2d_free_texture(previous);
                previous=current; current=texture; fade_at=now;
                (void)snprintf(loaded,sizeof(loaded),"%s",result);
            } else {
                (void)snprintf(failed,sizeof(failed),"%s",result);
            }
        }
        free(pixels);
        return;
    }
    if (!worker.busy && (!current || now-changed_at>=120000) && strcmp(desired,loaded) && strcmp(desired,failed)) {
        (void)snprintf(worker.request,sizeof(worker.request),"%s",desired);
        worker.pending=true; worker.busy=true;
        pthread_cond_signal(&worker.condition);
    }
    pthread_mutex_unlock(&worker.mutex);
}

void vp_ui_backdrop_draw(unsigned long long now,int motion_ms,unsigned int opacity,bool animated) {
    VpUiBackdropTransform t=vp_ui_backdrop_transform(now,animated);
    int duration=motion_ms>220?220:motion_ms;
    float progress=duration?(float)(now-fade_at)/(duration*1000.f):1.f;
    if (progress>1) progress=1;
    if (current) {
        if (previous && progress<1) vita2d_draw_texture_tint_scale(previous,t.x,t.y,t.scale,t.scale,0x00ffffffu|(opacity<<24));
        vita2d_draw_texture_tint_scale(current,t.x,t.y,t.scale,t.scale,0x00ffffffu|((unsigned int)(opacity*progress)<<24));
    }
}

void vp_ui_backdrop_shutdown(void) {
    if (started) {
        pthread_mutex_lock(&worker.mutex); worker.running=false;
        pthread_cond_signal(&worker.condition); pthread_mutex_unlock(&worker.mutex);
        pthread_join(worker.thread,NULL); started=false;
    }
    free(worker.pixels); worker.pixels=NULL; worker.ready=worker.pending=worker.busy=false;
    if (current) vita2d_free_texture(current);
    if (previous) vita2d_free_texture(previous);
    current=previous=NULL;
    desired[0]=loaded[0]=failed[0]='\0';
}
