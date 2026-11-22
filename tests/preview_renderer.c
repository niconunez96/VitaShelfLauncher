/* Native-sized layout probe using the actual Inter files via Cairo/FreeType.
 * GPU blending, texture caches and device frame timing still need a real Vita. */
#define _POSIX_C_SOURCE 200809L
#include "ui_renderer.h"
#include "ui_backdrop.h"
#include <psp2/io/fcntl.h>
#include <cairo.h>
#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static cairo_t *cr;
static FT_Library freetype;
static unsigned long long clock_us=1000000;
static size_t textures,fonts,drawn;
static bool missing;
static const char *unavailable_asset;
static char last_text[768];
static char frame_text[16384];
static _Alignas(16) unsigned char pool[1024*1024];
static size_t pool_used;
static int clip_x,clip_y,clip_w,clip_h;
static void source(unsigned int c);

void vita2d_set_clip_rectangle(int x,int y,int xx,int yy) {
    clip_x=x; clip_y=y; clip_w=xx-x; clip_h=yy-y;
}
void vita2d_enable_clipping(void) {
    cairo_save(cr); cairo_rectangle(cr,clip_x,clip_y,clip_w,clip_h); cairo_clip(cr);
}
void vita2d_disable_clipping(void) { cairo_restore(cr); }

void *vita2d_pool_memalign(unsigned int size,unsigned int alignment) {
    size_t start=(pool_used+alignment-1)&~((size_t)alignment-1);
    assert(start+size<=sizeof(pool)); pool_used=start+size; return pool+start;
}
void vita2d_draw_array(int mode,const vita2d_color_vertex *v,size_t count) {
    size_t i;
    assert(mode==SCE_GXM_PRIMITIVE_TRIANGLE_FAN && count>=3);
    source(v[0].color);
    cairo_move_to(cr,v[1].x,v[1].y);
    for (i=2;i<count;i++) cairo_line_to(cr,v[i].x,v[i].y);
    cairo_close_path(cr); cairo_fill(cr);
}

static const char *local(const char *path) { return !strncmp(path,"app0:",5)?path+5:path; }
static void source(unsigned int c) {
    cairo_set_source_rgba(cr,(c&255)/255.,((c>>8)&255)/255.,((c>>16)&255)/255.,(c>>24)/255.);
}
static void clear_canvas(void) {
    cairo_save(cr);
    cairo_set_operator(cr,CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}
void vita2d_draw_rectangle(float x,float y,float w,float h,unsigned int c) {
    assert(isfinite(x)&&isfinite(y)&&w>=0&&h>=0);
    source(c); cairo_rectangle(cr,x,y,w,h); cairo_fill(cr);
}
void vita2d_draw_fill_circle(float x,float y,float r,unsigned int c) {
    assert(r>=0); source(c); cairo_arc(cr,x,y,r,0,6.283185307); cairo_fill(cr);
}
void vita2d_draw_line(float x,float y,float xx,float yy,unsigned int c) {
    source(c); cairo_set_line_width(cr,1); cairo_move_to(cr,x,y); cairo_line_to(cr,xx,yy); cairo_stroke(cr);
}
static void sync_texture(const vita2d_texture *t) {
    if (t->raw==1) {
        /* CPU upload is RGBA on Vita; Cairo's image surface is native ARGB. */
        unsigned char *bytes=cairo_image_surface_get_data(t->surface);
        int stride=cairo_image_surface_get_stride(t->surface),x,y;
        for (y=0;y<t->h;y++) for (x=0;x<t->w;x++) {
            unsigned char *p=bytes+y*stride+x*4,red=p[0]; p[0]=p[2]; p[2]=red;
        }
        ((vita2d_texture *)t)->raw=2;
        cairo_surface_mark_dirty(t->surface);
    }
}
void vita2d_draw_texture_tint_part_scale(const vita2d_texture *t,float x,float y,float tx,float ty,float w,float h,float sx,float sy,unsigned int tint) {
    assert(t&&t->surface&&sx>0&&sy>0);
    sync_texture(t);
    cairo_save(cr); cairo_rectangle(cr,x,y,w*sx,h*sy); cairo_clip(cr);
    cairo_translate(cr,x,y); cairo_scale(cr,sx,sy);
    cairo_set_source_surface(cr,t->surface,-tx,-ty);
    cairo_paint_with_alpha(cr,(tint>>24)/255.); cairo_restore(cr);
}
void vita2d_draw_texture_tint_scale(const vita2d_texture *t,float x,float y,float sx,float sy,unsigned int tint) {
    vita2d_draw_texture_tint_part_scale(t,x,y,0,0,(float)t->w,(float)t->h,sx,sy,tint);
}
unsigned int vita2d_texture_get_width(const vita2d_texture *t) { return (unsigned)t->w; }
unsigned int vita2d_texture_get_height(const vita2d_texture *t) { return (unsigned)t->h; }
vita2d_texture *vita2d_load_PNG_file(const char *path) {
    vita2d_texture *t;
    if (missing) return NULL;
    t=calloc(1,sizeof(*t)); assert(t);
    t->surface=cairo_image_surface_create_from_png(local(path));
    if (cairo_surface_status(t->surface)!=CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(t->surface); free(t); return NULL; }
    t->w=cairo_image_surface_get_width(t->surface); t->h=cairo_image_surface_get_height(t->surface);
    textures++; return t;
}
vita2d_texture *vita2d_create_empty_texture(unsigned int w,unsigned int h) {
    vita2d_texture *t=calloc(1,sizeof(*t)); assert(t);
    t->surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,(int)w,(int)h);
    t->w=(int)w; t->h=(int)h; t->raw=1; textures++; return t;
}
unsigned int vita2d_texture_get_stride(const vita2d_texture *t) { return (unsigned)cairo_image_surface_get_stride(t->surface); }
void *vita2d_texture_get_datap(const vita2d_texture *t) { return cairo_image_surface_get_data(t->surface); }
void vita2d_texture_set_filters(vita2d_texture *t,int a,int b) { (void)t;(void)a;(void)b; }
void vita2d_free_texture(vita2d_texture *t) { cairo_surface_destroy(t->surface); free(t); textures--; }
void vita2d_wait_rendering_done(void) {}
SceUID sceIoOpen(const char *path,int flags,int mode) {
    (void)mode;
    return missing || (unavailable_asset && strstr(path,unavailable_asset))?-1:open(local(path),flags);
}
unsigned long long sceKernelGetProcessTimeWide(void) { clock_us+=16667; return clock_us; }
vita2d_font *vita2d_load_font_file(const char *path) {
    FT_Face face;
    vita2d_font *font=calloc(1,sizeof(*font)); assert(font);
    if (FT_New_Face(freetype,local(path),0,&face)) { free(font); return NULL; }
    font->face=face; font->cairo_face=cairo_ft_font_face_create_for_ft_face(face,0);
    fonts++; return font;
}
void vita2d_free_font(vita2d_font *font) {
    cairo_font_face_destroy(font->cairo_face); FT_Done_Face(font->face); free(font); fonts--;
}
int vita2d_font_text_width(vita2d_font *font,unsigned int size,const char *s) {
    cairo_text_extents_t extents;
    cairo_set_font_face(cr,font->cairo_face); cairo_set_font_size(cr,size);
    cairo_text_extents(cr,s,&extents); assert(cairo_status(cr)==CAIRO_STATUS_SUCCESS);
    return (int)ceil(extents.x_advance);
}
int vita2d_font_draw_text(vita2d_font *font,int x,int y,unsigned int color,unsigned int size,const char *s) {
    int width=vita2d_font_text_width(font,size,s);
    assert(size>=16 && size<=30);
    if (!(x>=0 && y>=0 && x+width<=960 && y<=544)) {
        fprintf(stderr,"Text overflow x=%d y=%d width=%d: %s\n",x,y,width,s); abort();
    }
    source(color); cairo_move_to(cr,x,y); cairo_show_text(cr,s);
    assert(cairo_status(cr)==CAIRO_STATUS_SUCCESS);
    (void)snprintf(last_text,sizeof(last_text),"%s",s);
    assert(strlen(frame_text)+strlen(s)+2<sizeof(frame_text));
    strcat(frame_text,s); strcat(frame_text,"\n");
    drawn++; return width;
}

static void settle(VpUiFrame *frame) {
    size_t i;
    for (i=0;i<45;i++) {
        struct timespec pause={0,2000000};
        pool_used=0; frame_text[0]='\0'; vp_ui_prepare(frame); clear_canvas(); vp_ui_render(frame); nanosleep(&pause,NULL);
    }
}

static unsigned int image_pixel(const unsigned char *bytes,int stride,int x,int y) {
    unsigned int value;
    memcpy(&value,bytes+y*stride+x*4,sizeof(value));
    return value;
}

static int brightness(unsigned int pixel) {
    return (int)(pixel&255)+(int)((pixel>>8)&255)+(int)((pixel>>16)&255);
}

static void check_console_material(VpUiFrame *frame,VpApp *app,cairo_surface_t *canvas,const char *directory) {
    int stride=cairo_image_surface_get_stride(canvas),x,y;
    size_t i,bytes=(size_t)stride*544;
    unsigned char *snapshot=malloc(bytes),*current;
    VpUiRect first=vp_ui_console_tile(0);
    VpUiConsoleMotion motion={0};
    VpUiRect focused;
    assert(snapshot);
    app->active_screen=VP_SCREEN_LIBRARY;
    app->library_level=VP_LEVEL_PLATFORMS;
    frame->group_count=VP_UI_CONSOLE_PAGE;
    app->selected_platform=0;
    vp_ui_console_motion_update(&motion,0,VP_UI_CONSOLE_PAGE,0,0);
    focused=vp_ui_console_motion_tile(&motion,0);
    vp_ui_load_theme("ui_motion_ms=0\nui_wallpaper_alpha=0\n");
    strcpy(app->theme.background,"#000000");
    settle(frame); cairo_surface_flush(canvas);
    memcpy(snapshot,cairo_image_surface_get_data(canvas),bytes);
    strcpy(app->theme.background,"#ffffff");
    settle(frame); cairo_surface_flush(canvas);
    current=cairo_image_surface_get_data(canvas);
    /* Opposite wallpapers must not change any card body pixel, including the lifted card.
     * The ten-pixel inset excludes only the intentionally illuminated perimeter. */
    for (i=0;i<VP_UI_CONSOLE_PAGE;i++) {
        VpUiRect r=vp_ui_console_motion_tile(&motion,i);
        for (y=10;y<r.h-10;y++) for (x=10;x<r.w-10;x++)
            assert(image_pixel(snapshot,stride,r.x+x,r.y+y)==image_pixel(current,stride,r.x+x,r.y+y));
    }
    /* Compare the exterior outline on the same backdrop, including asset fallbacks. */
    app->selected_platform=1;
    strcpy(app->theme.background,"#000000");
    settle(frame); cairo_surface_flush(canvas);
    {
        int contrast=0;
        for (y=-2;y<=1;y++) {
            int lit=brightness(image_pixel(snapshot,stride,focused.x+focused.w/2,focused.y+y));
            int unlit=brightness(image_pixel(current,stride,focused.x+focused.w/2,focused.y+y));
            if (lit-unlit>contrast) contrast=lit-unlit;
        }
        assert(contrast>100);
    }
    if (!missing && (!unavailable_asset || strcmp(unavailable_asset,"console_surface.png"))) {
        int lit=brightness(image_pixel(current,stride,first.x+20,first.y+16));
        int dark=brightness(image_pixel(current,stride,first.x+116,first.y+146));
        assert(lit>dark+30 && dark<200);
    }
    if (directory) {
        char path[1024];
        (void)snprintf(path,sizeof(path),"%s/console-%s.png",directory,missing?"fallback":"material");
        assert(!cairo_surface_write_to_png(canvas,path));
    }
    free(snapshot);
}

int main(int argc,char **argv) {
    static VpApp app,before;
    static VpDownloadQueue queue;
    static VpPlatformGroup groups[64];
    static size_t visible[12];
    VpUiFonts typefaces;
    VpUiFrame frame={0};
    vita2d_texture *covers[10]={0};
    cairo_surface_t *canvas=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,960,544);
    const char *slugs[]={"arcade","dreamcast","gba","nds","gamecube","psx","psp","3ds","ps2","snes","genesis","wii"};
    const char *titles[]={"Bloody Roar II","Castlevania: Symphony of the Night","Crash Bandicoot","Gran Turismo","Metal Gear Solid",
        "Resident Evil","Silent Hill","Dino Crisis","Tomb Raider","Ridge Racer","A game with a very long title and accents: aventura fantástica","Another game"};
    const char *cases[]={"platforms","carousel","list","grid","settings","integrations","downloads","downloads-empty","romm","about","about-disconnected","about-never-synced","store","empty","last-page","platforms-arcade","platforms-gba","platforms-last","appearance-dark","appearance-violet","appearance-waves","appearance-light-blue","appearance-text-dark"};
    size_t c,i;
    cr=cairo_create(canvas); assert(!FT_Init_FreeType(&freetype)); assert(vp_ui_fonts_load(&typefaces));
    assert(vp_ui_backdrop_init());
    vp_app_init(&app);
    assert(vp_theme_load(&app.theme,"name=VitaShelf Launcher Blueglass\nbackground=#354b7c\nsurface=#8da9e6\naccent=#4f9fff\ntext=#fafcff\nmuted=#ccdff9\n"));
    for (i=0;i<12;i++) {
        VpItem item={0};
        (void)snprintf(item.id,sizeof(item.id),"romm:%u",(unsigned)i);
        (void)snprintf(item.title,sizeof(item.title),"%s",titles[i]);
        (void)snprintf(item.platform,sizeof(item.platform),"psx");
        (void)snprintf(item.platform_display,sizeof(item.platform_display),"PlayStation");
        (void)snprintf(item.description,sizeof(item.description),"Bloody Roar is back with this sequel. Among the new and improved features are several new characters, new stages and enhanced 3D graphics.");
        item.source=i<6?VP_SOURCE_ROMM:VP_SOURCE_LOCAL;
        item.installed=i%2==0; item.favorite=i==0; vp_catalog_add(&app.catalog,&item); visible[i]=i;
    }
    for (i=0;i<12;i++) {
        (void)snprintf(groups[i].platform,sizeof(groups[i].platform),"%s",slugs[i]);
        (void)snprintf(groups[i].display,sizeof(groups[i].display),"%s",slugs[i]); groups[i].count=8;
    }
    queue.count=6;
    for (i=0;i<queue.count;i++) { queue.entries[i].item=app.catalog.items[i]; queue.entries[i].state=(VpDownloadState)(i%6); queue.entries[i].percent=(unsigned)i*20; }
    frame.app=&app; frame.font=&typefaces; frame.groups=groups; frame.group_count=12;
    frame.visible=visible; frame.visible_count=12; frame.downloads=&queue;
    frame.active_theme="default"; frame.romm_url="https://romm.example.test"; frame.pair_code="";
    frame.preferences=(VpUiPreferences)VP_UI_PREFERENCES_DEFAULT;
    frame.app_version="01.00"; frame.romm_catalog_available=true;
    (void)snprintf(frame.romm_last_sync,sizeof(frame.romm_last_sync),"21/11/2026 18:24");
    frame.storage_free=12ULL*1073741824; frame.storage_total=32ULL*1073741824;
    (void)snprintf(frame.date,sizeof(frame.date),"19 Nov"); (void)snprintf(frame.time,sizeof(frame.time),"18:24");
    frame.battery_percent=92;
    if (argc>2) for (i=0;i<10;i++) {
        char path[1024]; (void)snprintf(path,sizeof(path),"%s/%u.png",argv[2],(unsigned)i);
        covers[i]=vita2d_load_PNG_file(path);
    }
    {
        char long_text[1600]; memset(long_text,'W',sizeof(long_text)-1); long_text[sizeof(long_text)-1]='\0';
        vp_ui_text(&typefaces,VP_TYPE_HERO_TITLE,false,10,40,108,0xffffffffu,long_text);
        assert(vita2d_font_text_width(typefaces.weights[3],26,last_text)<=108 && strstr(last_text,"..."));
        vp_ui_text(&typefaces,VP_TYPE_GRID_TITLE,false,0,40,1,0xffffffffu,"é日本語"); assert(!last_text[0]);
        vp_ui_text(&typefaces,VP_TYPE_GRID_TITLE,false,0,40,90,0xffffffffu,"Pokémon — edición fantástica");
        assert(vp_ui_text_width(&typefaces,VP_TYPE_GRID_TITLE,false,last_text)<=90);
        vp_ui_text(&typefaces,VP_TYPE_GRID_TITLE,false,0,40,400,0xffffffffu,"A truncated UTF-8 title \xc3");
        assert(strstr(last_text,"...")!=NULL);
        vp_ui_text(&typefaces,VP_TYPE_GRID_TITLE,false,0,40,400,0xffffffffu,"A title\nwith an unexpected line break");
        assert(strchr(last_text,'\n')==NULL);
    }
    for (c=0;c<sizeof(cases)/sizeof(cases[0]);c++) {
        app.active_screen=VP_SCREEN_LIBRARY; app.library_level=VP_LEVEL_PLATFORMS; app.selected_platform=5; app.selected_item=0;
        frame.group_count=12; frame.visible_count=12; frame.settings_detail=false; frame.settings_romm=false; queue.count=6;
        frame.romm_catalog_available=true;
        if (c>=1 && c<=3) { app.library_level=VP_LEVEL_ROMS; app.active_view=c==1?VP_VIEW_CAROUSEL:(c==2?VP_VIEW_LIST:VP_VIEW_GRID); }
        if (c>=4 && c<=11) {
            app.active_screen=VP_SCREEN_SETTINGS; frame.settings_detail=c!=4;
            frame.settings_section=c>=9?VP_SETTINGS_ABOUT:(c==6 || c==7?VP_SETTINGS_DOWNLOADS:VP_SETTINGS_INTEGRATIONS);
            frame.settings_romm=c==8;
        }
        if (c==7) queue.count=0;
        if (c==11) frame.romm_catalog_available=false;
        if (c==12) app.active_screen=VP_SCREEN_STORES;
        if (c==13) frame.group_count=0;
        if (c==14) { app.library_level=VP_LEVEL_ROMS; app.active_view=VP_VIEW_GRID; app.selected_item=11; }
        if (c>=15) app.selected_platform=c==15?0:(c==16?2:11);
        if (c>=18 && c<=21) {
            app.active_screen=VP_SCREEN_SETTINGS; frame.settings_detail=true;
            frame.settings_section=VP_SETTINGS_APPEARANCE; frame.settings_field=(int)c-18;
            frame.preferences.wallpaper=(int)c-18; frame.preferences.animated=c!=21;
        }
        if (c==22) {
            app.active_screen=VP_SCREEN_SETTINGS; frame.settings_detail=true;
            frame.settings_section=VP_SETTINGS_APPEARANCE; frame.settings_field=VP_UI_APPEARANCE_TEXT_COLOR_SLOT;
            frame.preferences.wallpaper=3; frame.preferences.text_color=VP_UI_TEXT_COLOR_DARK; frame.preferences.animated=true;
        }
        (void)snprintf(app.active_platform,sizeof(app.active_platform),"psx");
        frame.romm_state=c==10?VP_ROMM_UNCONFIGURED:VP_ROMM_CONNECTED;
        for (i=0;i<10;i++) frame.covers[i]=covers[i];
        if (app.active_view==VP_VIEW_CAROUSEL) for (i=0;i<7;i++) frame.covers[i]=covers[(i+7)%10];
        before=app; settle(&frame); assert(!memcmp(&app,&before,sizeof(app)));
        if (c==4) {
            const char *integrations=strstr(frame_text,"Integraciones\n"),*downloads=strstr(frame_text,"Descargas\n"),*about=strstr(frame_text,"Acerca de\n");
            assert(integrations && downloads && about && integrations<downloads && downloads<about);
            assert(!strstr(frame_text,"General\n") && !strstr(frame_text,"Interfaz\n") && !strstr(frame_text,"Almacenamiento\n"));
        }
        if (c==5) assert(strstr(frame_text,"RomM\n") && !strstr(frame_text,"Pair code\n"));
        if (c==8) assert(strstr(frame_text,"URL del servidor\n") && strstr(frame_text,"Pair code\n") && strstr(frame_text,"Guardar\n"));
        if (c==7) assert(strstr(frame_text,"Todo al día\n"));
        if (c>=18) {
            for (i=0;i<VP_UI_WALLPAPER_COUNT;i++) assert(strstr(frame_text,vp_ui_wallpaper((int)i)->label));
            assert(strstr(frame_text,"Color de texto\n"));
            assert(strstr(frame_text,"Fondo animado\n"));
            assert(strstr(frame_text,c==21?"Desactivado\n":"Activado\n"));
            assert(strstr(frame_text,c==22?"Oscuro\n":"Blanco\n"));
        }
        if (c>=9 && c<=11) {
            assert(strstr(frame_text,"VitaShelf Launcher · 01.00\n"));
            assert(strstr(frame_text,"Libre: 12.0 GiB / 32.0 GiB\n"));
            assert(strstr(frame_text,"Descargas: ux0:data/vitashelflauncher/downloads/\n"));
            assert(strstr(frame_text,"Imágenes: ux0:data/vitashelflauncher/cache/covers/\n"));
            if (c==10) assert(!strstr(frame_text,"Servidor:") && !strstr(frame_text,"Catálogo:") && !strstr(frame_text,"Última sincronización:"));
            else {
                assert(strstr(frame_text,"Servidor: romm.example.test\n"));
                if (c==11) {
                    assert(strstr(frame_text,"Catálogo: Sin sincronizar\n"));
                    assert(strstr(frame_text,"Última sincronización: Sin datos\n"));
                } else {
                    assert(strstr(frame_text,"Catálogo: 6 juegos\n"));
                    assert(strstr(frame_text,"Última sincronización: 21/11/2026 18:24\n"));
                }
            }
        }
        if (argc>1) { char path[1024]; (void)snprintf(path,sizeof(path),"%s/%s.png",argv[1],cases[c]); assert(!cairo_surface_write_to_png(canvas,path)); }
    }
    check_console_material(&frame,&app,canvas,argc>1?argv[1]:NULL);
    vp_ui_shutdown();
    unavailable_asset="console_focus.png";
    check_console_material(&frame,&app,canvas,NULL); vp_ui_shutdown();
    unavailable_asset="console_surface.png";
    check_console_material(&frame,&app,canvas,NULL); vp_ui_shutdown();
    unavailable_asset=NULL; missing=true;
    check_console_material(&frame,&app,canvas,argc>1?argv[1]:NULL); vp_ui_shutdown();
    for (i=0;i<10;i++) if (covers[i]) vita2d_free_texture(covers[i]);
    vp_ui_fonts_free(&typefaces);
    assert(!textures && !fonts && drawn>0);
    cairo_destroy(cr); cairo_surface_destroy(canvas); FT_Done_FreeType(freetype);
    puts("Blueglass renderer: 23 states, four wallpapers, text color presets, opaque console bodies, border-only focus, partial/missing assets, Inter metrics and cleanup passed");
    return 0;
}
