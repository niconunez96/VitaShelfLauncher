#include "ui_renderer.h"
#include "ui_backdrop.h"
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C(r,g,b,a) ((unsigned int)(r)|((unsigned int)(g)<<8)|((unsigned int)(b)<<16)|((unsigned int)(a)<<24))
typedef struct { const char *name; vita2d_texture *texture; bool attempted; } Asset;
#define A(name) {name,NULL,false}
static Asset assets[]={
    A("focus"),A("console_surface"),A("console_focus"),A("platform_nes"),A("platform_famicom"),A("platform_snes"),
    A("platform_gameboy"),A("platform_gameboycolor"),A("platform_gba"),A("platform_nds"),
    A("platform_genesis"),A("platform_mastersystem"),A("platform_gamegear"),A("platform_psx"),
    A("platform_psp"),A("platform_vita"),A("platform_arcade"),A("platform_neogeo"),A("platform_pce"),
    A("platform_atari2600"),A("platform_wonderswan"),A("platform_homebrew"),A("platform_generic"),
    A("platform_dreamcast"),A("platform_n3ds"),A("platform_gamecube"),A("platform_ps2"),A("platform_wii"),
    A("header_nes"),A("header_famicom"),A("header_snes"),A("header_gameboy"),A("header_gameboycolor"),
    A("header_gba"),A("header_nds"),A("header_genesis"),A("header_mastersystem"),A("header_gamegear"),
    A("header_psx"),A("header_psp"),A("header_vita"),A("header_arcade"),A("header_neogeo"),A("header_pce"),
    A("header_atari2600"),A("header_wonderswan"),A("header_homebrew"),A("header_generic"),
    A("header_dreamcast"),A("header_n3ds"),A("header_gamecube"),A("header_ps2"),A("header_wii"),
    A("ui_library"),A("ui_store"),A("ui_settings"),A("ui_download"),A("ui_check"),A("ui_star"),
    A("ui_home"),A("ui_key"),A("ui_sync"),A("ui_folder"),A("ui_general"),A("ui_interface"),
    A("ui_storage"),A("ui_info"),A("ui_server"),A("ui_psx"),A("ui_romm"),A("ui_buttons"),
    A("wallpaper_dark-blue"),A("wallpaper_violet"),A("wallpaper_blue-waves"),A("wallpaper_light-blue")
};
#undef A
static unsigned int bg,surface,ink,muted,accent;
static unsigned int wall_primary,wall_muted,panel_primary,panel_muted;
static int asset_budget;
static unsigned long long notice_at;
static char last_notice[VP_TEXT];
static unsigned int last_notice_id;
static VpUiAppearance appearance={180,14,255};
static VpUiConsoleMotion console_motion;
static VpUiCarouselMotion carousel_motion;
static bool carousel_touch_scrolling;

int vp_ui_touch_platform(int x,int y) { return vp_ui_console_motion_touch(&console_motion,x,y); }
float vp_ui_scroll_origin(VpLibraryLevel level) {
    return level==VP_LEVEL_PLATFORMS?console_motion.scroll:carousel_motion.scroll;
}

void vp_ui_load_theme(const char *ini) { vp_ui_appearance_load(&appearance,ini); }
static unsigned int alpha(unsigned int c,unsigned int a) { return (c&0xffffffu)|(a<<24); }
static unsigned int rgb(uint32_t c) { return C((c>>16)&255,(c>>8)&255,c&255,255); }
static unsigned int mix(unsigned int a,unsigned int b,float t) {
    unsigned int result=0;
    int channel;
    for (channel=0;channel<3;channel++) {
        float from=(float)((a>>(channel*8))&255),to=(float)((b>>(channel*8))&255);
        result|=(unsigned int)(from+(to-from)*t)<<(channel*8);
    }
    return result|0xff000000u;
}
static unsigned int color(const char *s,unsigned int fallback) {
    unsigned int r,g,b;
    return sscanf(s,"#%02x%02x%02x",&r,&g,&b)==3?C(r,g,b,255):fallback;
}

static void resolve_text_colors(const VpUiFrame *f) {
    switch (f->preferences.text_color) {
        case VP_UI_TEXT_COLOR_LIGHT:
            wall_primary=C(243,247,255,255); wall_muted=C(224,234,250,235);
            panel_primary=C(21,36,70,255); panel_muted=C(44,63,102,255);
            break;
        case VP_UI_TEXT_COLOR_DARK:
            wall_primary=C(28,42,76,255); wall_muted=C(46,64,104,235);
            panel_primary=C(19,33,64,255); panel_muted=C(42,60,96,255);
            break;
        case VP_UI_TEXT_COLOR_BLACK:
            wall_primary=C(10,16,30,255); wall_muted=C(24,32,52,235);
            panel_primary=C(8,14,28,255); panel_muted=C(28,38,58,255);
            break;
        default:
            wall_primary=ink; wall_muted=C(237,244,255,240);
            panel_primary=ink; panel_muted=C(237,244,255,240);
            break;
    }
}

static void round_rect(float x,float y,float w,float h,float radius,unsigned int c) {
    static const float cs[]={1.f,.9659258f,.8660254f,.7071068f,.5f,.258819f,0.f};
    float r=fminf(radius,fminf(w,h)/2);
    vita2d_color_vertex *vertices;
    int corner,step,index=1;
    if (w<=0 || h<=0) return;
    /* One non-overlapping fan: translucent rectangles/circles would blend over
     * each other and leave dark discs at every corner. Pool lifetime spans GPU use. */
    vertices=vita2d_pool_memalign(30*sizeof(*vertices),sizeof(float));
    if (!vertices) return;
    vertices[0]=(vita2d_color_vertex){x+w/2,y+h/2,.5f,c};
    for (corner=0;corner<4;corner++) for (step=0;step<=6;step++) {
        float a=cs[step],b=cs[6-step],px,py;
        if (corner==0) { px=x+w-r+r*b; py=y+r-r*a; }
        else if (corner==1) { px=x+w-r+r*a; py=y+h-r+r*b; }
        else if (corner==2) { px=x+r-r*b; py=y+h-r+r*a; }
        else { px=x+r-r*a; py=y+r-r*b; }
        vertices[index++]=(vita2d_color_vertex){px,py,.5f,c};
    }
    vertices[index++]=vertices[1];
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN,vertices,(size_t)index);
}

static vita2d_texture *asset(const char *name) {
    size_t i;
    for (i=0;i<sizeof(assets)/sizeof(assets[0]);i++) {
        Asset *a=&assets[i];
        if (strcmp(name,a->name)) continue;
        if (!a->attempted && asset_budget>0) {
            char path[160]; unsigned char header[29]={0}; SceUID fd;
            bool heading=!strncmp(name,"header_",7),wallpaper=!strncmp(name,"wallpaper_",10);
            unsigned int w=wallpaper?240:(!strcmp(name,"console_surface")?136:
                (!strncmp(name,"platform_",9) || !strcmp(name,"ui_buttons")?128:
                (!strcmp(name,"focus") || !strcmp(name,"console_focus")?96:64)));
            unsigned int h=wallpaper?136:(!strcmp(name,"console_surface")?170:w);
            a->attempted=true; --asset_budget;
            (void)snprintf(path,sizeof(path),"app0:assets/blueglass/%s/%s.png",wallpaper?"wallpapers":(heading?"headers":(!strncmp(name,"platform_",9)?"logos":"ui")),name);
            fd=sceIoOpen(path,SCE_O_RDONLY,0);
            if (fd>=0) {
                int count=sceIoRead(fd,header,sizeof(header));
                SceOff length=sceIoLseek(fd,0,SCE_SEEK_END); sceIoClose(fd);
                unsigned int actual_w=(unsigned int)header[18]*256+header[19];
                unsigned int actual_h=(unsigned int)header[22]*256+header[23];
                if (count==(int)sizeof(header) && length>0 && length<=512*1024 && vp_safe_cover_png(header,sizeof(header)) &&
                    (heading?(actual_w>0 && actual_w<=128 && actual_h>0 && actual_h<=64):(actual_w==w && actual_h==h))) {
                    a->texture=vita2d_load_PNG_file(path);
                    if (a->texture) vita2d_texture_set_filters(a->texture,SCE_GXM_TEXTURE_FILTER_LINEAR,SCE_GXM_TEXTURE_FILTER_LINEAR);
                }
            }
        }
        return a->texture;
    }
    return NULL;
}

static void fit(vita2d_texture *texture,float x,float y,float w,float h,unsigned int tint) {
    float tw,th,s;
    if (!texture) return;
    tw=(float)vita2d_texture_get_width(texture); th=(float)vita2d_texture_get_height(texture);
    if (tw<=0 || th<=0) return;
    s=fminf(w/tw,h/th);
    vita2d_draw_texture_tint_scale(texture,x+(w-tw*s)/2,y+(h-th*s)/2,s,s,tint);
}

static void icon(const char *name,float x,float y,float size) {
    vita2d_texture *texture=asset(name);
    if (texture) fit(texture,x,y,size,size,0xffffffffu);
}

static void text(const VpUiFrame *f,VpTypeRole role,int x,int y,int width,unsigned int c,const char *value) {
    vp_ui_text(f->font,role,false,x,y,width,c,value);
}

/* Nine slices preserve the corner radius and glow at every card size. */
static void focus(VpUiRect r) {
    vita2d_texture *texture=asset("focus");
    int ix,iy;
    float xs[]={r.x-18,r.x+14,r.x+r.w-14,r.x+r.w+18};
    float ys[]={r.y-18,r.y+14,r.y+r.h-14,r.y+r.h+18};
    if (!texture) {
        round_rect(r.x-3,r.y-3,r.w+6,r.h+6,16,C(130,220,255,255));
        round_rect(r.x,r.y,r.w,r.h,14,C(40,100,220,255));
        return;
    }
    for (iy=0;iy<3;iy++) for (ix=0;ix<3;ix++) {
        float w=xs[ix+1]-xs[ix],h=ys[iy+1]-ys[iy];
        if (w>0 && h>0)
            vita2d_draw_texture_tint_part_scale(texture,xs[ix],ys[iy],ix*32,iy*32,32,32,w/32,h/32,0xffffffffu);
    }
}

static void glass(VpUiRect r,bool selected,bool dark) {
    unsigned int fill=dark?alpha(mix(bg,C(15,24,50,255),.45f),180):alpha(mix(surface,ink,.55f),70);
    round_rect(r.x,r.y+5,r.w,r.h,14,C(14,33,77,30));
    round_rect(r.x-1,r.y-1,r.w+2,r.h+2,15,C(224,239,255,90));
    round_rect(r.x,r.y,r.w,r.h,14,selected?alpha(accent,140):fill);
    if (selected) focus(r);
}

static void toggle(VpUiRect r,bool on) {
    float radius=r.h/2;
    round_rect(r.x,r.y,r.w,r.h,radius,on?alpha(accent,235):C(24,42,78,255));
    round_rect(on?r.x+r.w-r.h+2:r.x+2,r.y+2,r.h-4,r.h-4,radius-2,C(240,247,255,255));
}

static void console_card(VpUiRect r,bool selected,float lift) {
    vita2d_texture *material=asset("console_surface"),*ring=selected?asset("console_focus"):NULL;
    int ix,iy;
    float xs[]={r.x-18,r.x+14,r.x+r.w-14,r.x+r.w+18};
    float ys[]={r.y-18,r.y+14,r.y+r.h-14,r.y+r.h+18};
    if (lift>0) round_rect(r.x-4*lift,r.y+6,r.w+8*lift,r.h+4*lift,18,C(8,15,35,(unsigned int)(40*lift)));
    round_rect(r.x,r.y+4+5*lift,r.w,r.h,14,C(10,18,40,(unsigned int)(45+35*lift)));
    /* Draw the fallback outline underneath the opaque surface, never over its body. */
    if (selected && !ring) round_rect(r.x-2,r.y-2,r.w+4,r.h+4,16,C(174,210,255,255));
    if (material) fit(material,r.x,r.y,r.w,r.h,0xffffffffu);
    else {
        round_rect(r.x,r.y,r.w,r.h,14,C(88,101,133,255));
        round_rect(r.x+1,r.y+1,r.w-2,r.h-2,13,C(32,40,62,255));
    }
    if (!ring) return;
    for (iy=0;iy<3;iy++) for (ix=0;ix<3;ix++) {
        float w=xs[ix+1]-xs[ix],h=ys[iy+1]-ys[iy];
        if ((ix!=1 || iy!=1) && w>0 && h>0)
            vita2d_draw_texture_tint_part_scale(ring,xs[ix],ys[iy],ix*32,iy*32,32,32,w/32,h/32,0xffffffffu);
    }
}

static size_t position(const VpUiFrame *f) {
    size_t i;
    for (i=0;i<f->visible_count;i++) if (f->visible[i]==f->app->selected_item) return i;
    return 0;
}

static const char *platform_asset(const VpUiFrame *f) {
    if (f->app->active_screen!=VP_SCREEN_LIBRARY) return NULL;
    if (f->app->library_level==VP_LEVEL_PLATFORMS)
        return f->group_count?vp_ui_platform(f->groups[f->app->selected_platform].platform)->asset:NULL;
    return vp_ui_platform(f->app->active_platform)->asset;
}

static const char *heading_asset(const VpUiFrame *f) {
    static char name[64];
    const char *logo=platform_asset(f);
    if (!logo) return NULL;
    if (!strcmp(logo,"platform_psx")) return "ui_psx";
    if (!strcmp(logo,"ui_star")) return "ui_star";
    (void)snprintf(name,sizeof(name),"header_%s",logo+9);
    return name;
}

void vp_ui_prepare(const VpUiFrame *f) {
    static const char *common[]={"focus","ui_buttons","ui_library","ui_store","ui_settings","ui_download","ui_check","ui_star","ui_psx"};
    size_t i;
    bool platforms=f->app->active_screen==VP_SCREEN_LIBRARY && f->app->library_level==VP_LEVEL_PLATFORMS;
    bool carousel=f->app->active_screen==VP_SCREEN_LIBRARY && f->app->library_level==VP_LEVEL_ROMS &&
        f->app->active_view==VP_VIEW_CAROUSEL && f->visible_count>0;
    asset_budget=3;
    for (i=0;i<sizeof(common)/sizeof(common[0]);i++) (void)asset(common[i]);
    unsigned long long now=sceKernelGetProcessTimeWide();
    if (platforms) {
        vp_ui_console_motion_update(&console_motion,f->app->selected_platform,f->group_count,
            now,appearance.motion_ms);
        if (f->touch_scrolling)
            vp_ui_console_motion_drag(&console_motion,f->touch_scroll,now);
        (void)asset("console_surface"); (void)asset("console_focus");
        for (i=0;i<f->group_count;i++) if (vp_ui_console_motion_visible(&console_motion,i))
            (void)asset(vp_ui_platform(f->groups[i].platform)->asset);
    } else memset(&console_motion,0,sizeof(console_motion));
    if (f->app->active_screen==VP_SCREEN_SETTINGS) {
        const char *icons[]={"ui_server","ui_download","ui_romm","ui_info","ui_home","ui_key","ui_sync","ui_folder","ui_interface"};
        for (i=0;i<sizeof(icons)/sizeof(icons[0]);i++) (void)asset(icons[i]);
        if (f->settings_detail && f->settings_section==VP_SETTINGS_APPEARANCE)
            for (i=0;i<VP_UI_WALLPAPER_COUNT;i++) (void)asset(vp_ui_wallpaper((int)i)->thumbnail);
    }
    if (carousel) {
        if (carousel_touch_scrolling && !f->touch_scrolling)
            vp_ui_carousel_motion_rebase(&carousel_motion);
        vp_ui_carousel_motion_update(&carousel_motion,position(f),f->visible_count,
            now,appearance.motion_ms);
        if (f->touch_scrolling)
            vp_ui_carousel_motion_drag(&carousel_motion,f->touch_scroll,now);
    } else memset(&carousel_motion,0,sizeof(carousel_motion));
    carousel_touch_scrolling=carousel && f->touch_scrolling;
    if (heading_asset(f)) (void)asset(heading_asset(f));
    asset_budget=0;
    vp_ui_backdrop_prepare(f->preferences.wallpaper,sceKernelGetProcessTimeWide());
}

static void backdrop(const VpUiFrame *f,unsigned long long now) {
    int y;
    bool settings=f->app->active_screen==VP_SCREEN_SETTINGS;
    vp_ui_backdrop_draw(now,appearance.motion_ms,(unsigned int)appearance.wallpaper_alpha,
        f->preferences.animated);
    if (!settings && f->app->library_level==VP_LEVEL_ROMS && f->background && f->app->active_view!=VP_VIEW_GRID)
        fit(f->background,370,58,590,380,C(255,255,255,125));
    for (y=0;y<72;y+=8) vita2d_draw_rectangle(0,y,960,8,C(21,35,73,(unsigned int)(80-y)));
}

static void header(const VpUiFrame *f) {
    static const char *names[]={"Biblioteca","Tienda","Ajustes"};
    static const char *icons[]={"ui_library","ui_store","ui_settings"};
    int i;
    static const int tab_x[]={68,218,330},tab_w[]={146,108,120};
    round_rect(64,13,390,44,22,C(100,126,181,65));
    for (i=0;i<3;i++) {
        bool selected=(int)f->app->active_screen==i;
        VpUiRect r={tab_x[i],17,tab_w[i],36};
        if (selected) { round_rect(r.x,r.y,r.w,r.h,18,C(16,82,215,210)); focus(r); }
        icon(icons[i],r.x+12,r.y+7,23);
        vp_ui_text(f->font,VP_TYPE_NAV,selected,r.x+42,r.y+24,r.w-46,selected?ink:C(231,237,255,210),names[i]);
    }
    text(f,VP_TYPE_STATUS,664,39,100,wall_primary,f->date);
    vita2d_draw_line(772,23,772,43,alpha(wall_primary,140));
    text(f,VP_TYPE_STATUS,784,39,60,wall_primary,f->time);
    vita2d_draw_line(852,23,852,43,alpha(wall_primary,140));
    if (f->battery_percent>=0) {
        char percentage[16]; int percent=f->battery_percent>100?100:f->battery_percent;
        (void)snprintf(percentage,sizeof(percentage),"%d%%",percent);
        text(f,VP_TYPE_STATUS,864,39,44,wall_primary,percentage);
        round_rect(912,25,30,15,3,C(237,244,255,245));
        vita2d_draw_rectangle(942,29,3,7,ink);
        round_rect(914,27,26,11,2,C(38,61,108,255));
        if (percent) vita2d_draw_rectangle(915,28,24*percent/100.f,9,f->battery_charging?C(98,218,249,255):C(107,235,126,255));
    }
}

static void heading(const VpUiFrame *f) {
    char subtitle[96]; const char *title="Biblioteca",*logo=heading_asset(f);
    if (f->app->active_screen!=VP_SCREEN_LIBRARY) return;
    if (f->app->library_level==VP_LEVEL_ROMS) {
        if (!strcmp(f->app->active_platform,VP_FAVORITES_PLATFORM)) title=VP_FAVORITES_DISPLAY;
        else title=f->visible_count?f->app->catalog.items[f->visible[position(f)]].platform_display:f->app->active_platform;
        (void)snprintf(subtitle,sizeof(subtitle),"%u juegos",(unsigned)f->visible_count);
    } else (void)snprintf(subtitle,sizeof(subtitle),"%u consolas   •   %u juegos",(unsigned)f->group_count,(unsigned)f->app->catalog.count);
    if (logo && !strcmp(logo,"ui_star"))
        fit(asset(logo),74,94,28,28,C(185,204,234,210));
    else if (logo && !strcmp(logo,"ui_psx")) icon(logo,61,81,54);
    else if (logo) fit(asset(logo),24,76,128,64,0xffffffffu);
    text(f,VP_TYPE_PAGE_TITLE,172,115,620,wall_primary,title);
    text(f,VP_TYPE_PAGE_SUBTITLE,173,140,610,wall_muted,subtitle);
    if (f->app->library_level==VP_LEVEL_ROMS && f->visible_count) {
        char count[32]; (void)snprintf(count,sizeof(count),"%u / %u",(unsigned)position(f)+1,(unsigned)f->visible_count);
        text(f,VP_TYPE_STATUS,870,141,70,wall_primary,count);
    }
}

static void platform_card(const VpUiFrame *f,size_t i) {
    const VpUiPlatform *platform=vp_ui_platform(f->groups[i].platform);
    VpUiRect r=vp_ui_console_motion_tile(&console_motion,i);
    float scale=r.w/136.f;
    console_card(r,i==f->app->selected_platform,console_motion.lift[i]);
    if (!strcmp(platform->asset,"ui_star")) {
        float size=40*scale;
        fit(asset(platform->asset),r.x+(r.w-size)/2,r.y+(r.h-size)/2-6*scale,
            size,size,C(185,204,234,210));
    } else fit(asset(platform->asset),r.x+10*scale,r.y+22*scale,r.w-20*scale,116*scale,0xffffffffu);
    round_rect(r.x+(r.w-42*scale)/2,r.y+r.h-16*scale,42*scale,4*scale,2*scale,rgb(platform->rgb));
}

static void platforms(const VpUiFrame *f) {
    size_t i;
    vita2d_set_clip_rectangle(24,210,944,444);
    vita2d_enable_clipping();
    for (i=0;i<f->group_count;i++) {
        if (i!=f->app->selected_platform && vp_ui_console_motion_visible(&console_motion,i)) platform_card(f,i);
    }
    if (vp_ui_console_motion_visible(&console_motion,f->app->selected_platform))
        platform_card(f,f->app->selected_platform);
    vita2d_disable_clipping();
    if (f->group_count) {
        const VpPlatformGroup *group=&f->groups[f->app->selected_platform]; char detail[VP_TEXT+40];
        (void)snprintf(detail,sizeof(detail),"%s  ·  %u juegos",group->display,(unsigned)group->count);
        text(f,VP_TYPE_CONSOLE_TITLE,48,462,850,wall_primary,detail);
    }
}

static void hidden_overlay(VpUiRect r) {
    float size=fminf(36.f,fminf(r.w,r.h)*.55f);
    float cx=r.x+r.w/2.f,cy=r.y+r.h/2.f;
    const unsigned int ink=C(211,222,239,255),disc=C(18,27,46,255);
    round_rect(r.x,r.y,r.w,r.h,10,C(8,14,28,170));
    vita2d_draw_fill_circle(cx,cy,size*.68f,disc);
    /* Outline eye and diagonal slash, scaled down for list thumbnails. */
    for (int side=-1;side<=1;side+=2) {
        for (int step=0;step<12;step++) {
            float t=step/12.f,next=(step+1)/12.f;
            float x=cx+(t-.5f)*size,xx=cx+(next-.5f)*size;
            float y=cy+side*sinf(t*3.14159265f)*size*.27f;
            float yy=cy+side*sinf(next*3.14159265f)*size*.27f;
            vita2d_draw_line(x,y-.5f,xx,yy-.5f,ink);
            vita2d_draw_line(x,y+.5f,xx,yy+.5f,ink);
        }
    }
    vita2d_draw_fill_circle(cx,cy,size*.13f,ink);
    vita2d_draw_fill_circle(cx,cy,size*.075f,disc);
    for (int offset=-2;offset<=2;offset++)
        vita2d_draw_line(cx-size*.43f,cy-size*.43f+offset,
                         cx+size*.43f,cy+size*.43f+offset,disc);
    vita2d_draw_line(cx-size*.43f,cy-size*.43f-.5f,cx+size*.43f,cy+size*.43f-.5f,ink);
    vita2d_draw_line(cx-size*.43f,cy-size*.43f+.5f,cx+size*.43f,cy+size*.43f+.5f,ink);
}

static void cover(vita2d_texture *texture,const VpItem *item,VpUiRect r,unsigned int tint) {
    round_rect(r.x,r.y,r.w,r.h,10,C(17,31,65,225));
    if (texture) fit(texture,r.x+3,r.y+3,r.w-6,r.h-6,tint);
    else {
        fit(asset(vp_ui_platform(item->platform)->asset),r.x+12,r.y+r.h*.2f,r.w-24,r.h*.4f,0xffffffffu);
        /* Missing artwork remains identifiable and actionable offline. */
    }
    if (item->hidden) hidden_overlay(r);
}

static void carousel_card(const VpUiFrame *f,int position,int base) {
    size_t index=vp_ui_wrap_index(base+position,f->visible_count);
    VpUiRect r;
    float weight;
    unsigned int tint;
    r=vp_ui_carousel_motion_tile(&carousel_motion,base+position);
    weight=vp_ui_carousel_motion_focus(&carousel_motion,base+position);
    tint=alpha(mix(C(245,248,255,255),0xffffffffu,weight),(unsigned int)(210+45.f*weight));
    cover(f->covers[position+VP_UI_CAROUSEL_REACH],&f->app->catalog.items[f->visible[index]],r,tint);
    if (position) vita2d_draw_rectangle(r.x,r.y+r.h+4,r.w,18,C(148,175,233,35));
    else focus(r);
}

static void carousel_cards(const VpUiFrame *f,int base) {
    int position,count=(int)f->visible_count;
    int first=-count/2;
    if (!(count%2) && carousel_motion.scroll>=(float)base) first++;
    for (position=-VP_UI_CAROUSEL_REACH;position<=VP_UI_CAROUSEL_REACH;position++) {
        float offset=(float)(base+position)-carousel_motion.scroll;
        if (fabsf(offset)>VP_UI_CAROUSEL_PAGE/2.f+.5f) continue;
        if (count<VP_UI_CAROUSEL_PAGE && (position<first || position>=first+count)) continue;
        if (!position) continue;
        carousel_card(f,position,base);
    }
    carousel_card(f,0,base);
}

static void tag(const VpUiFrame *f,int x,int y,int width,const char *value) {
    round_rect(x,y,width,22,7,C(85,111,173,100));
    vita2d_draw_line(x+6,y,x+width-6,y,C(255,255,255,210));
    text(f,VP_TYPE_GRID_TITLE,x+7,y+16,width-14,C(250,252,255,255),value);
}

static void roms(const VpUiFrame *f) {
    size_t pos=position(f),i,start;
    const VpItem *selected=&f->app->catalog.items[f->visible[pos]];
    if (f->app->active_view==VP_VIEW_GRID) {
        float scroll=f->grid_scroll;
        size_t window=scroll>floorf(scroll)?VP_UI_GRID_WINDOW:VP_UI_GRID_PAGE;
        start=(size_t)floorf(scroll)*VP_UI_GRID_COLUMNS;
        vita2d_set_clip_rectangle(24,148,944,480);
        vita2d_enable_clipping();
        for (i=start;i<f->visible_count && i<start+window;i++) {
            const VpItem *item=&f->app->catalog.items[f->visible[i]];
            bool active=i==pos; VpUiRect r=vp_ui_scrolled_grid_tile(i,scroll);
            cover(f->covers[i-start],item,r,0xffffffffu);
            if (active) {
                const char *platform=vp_ui_platform_label(item->platform);
                int tag_width=vp_ui_text_width(f->font,VP_TYPE_GRID_TITLE,false,platform)+14;
                if (tag_width>68) tag_width=68;
                round_rect(r.x,r.y+r.h-29,r.w,29,8,C(13,32,79,225));
                if (r.y+r.h-10>=148 && r.y+r.h-10<=480) {
                    text(f,VP_TYPE_GRID_TITLE,r.x+9,r.y+r.h-10,r.w-tag_width-19,C(250,252,255,255),item->title);
                    tag(f,r.x+r.w-tag_width-4,r.y+r.h-25,tag_width,platform);
                }
                focus(r);
            }
            if (item->favorite) icon("ui_star",r.x+r.w-25,r.y+5,20);
            icon(item->installed?"ui_check":"ui_download",r.x+5,r.y+5,19);
        }
        vita2d_disable_clipping();
    } else if (f->app->active_view==VP_VIEW_LIST) {
        VpUiRect hero={382,160,212,262};
        float scroll=f->list_scroll;
        start=(size_t)floorf(scroll);
        round_rect(37,174,3,258,1,C(235,244,255,140));
        vita2d_set_clip_rectangle(48,166,356,440);
        vita2d_enable_clipping();
        for (i=start;i<f->visible_count && i<start+VP_UI_LIST_PAGE+1;i++) {
            const VpItem *item=&f->app->catalog.items[f->visible[i]];
            VpUiRect r=vp_ui_scrolled_list_row(i,scroll); bool active=i==pos;
            if (active) glass(r,true,false);
            fit(f->covers[i-start],r.x+10,r.y+4,38,40,0xffffffffu);
            vp_ui_text(f->font,VP_TYPE_LIST_TITLE,active,r.x+62,r.y+29,r.w-74,
                       item->hidden?alpha(panel_primary,140):panel_primary,item->title);
            if (item->hidden) hidden_overlay((VpUiRect){r.x+10,r.y+4,38,40});
        }
        vita2d_disable_clipping();
        if ((float)pos>=scroll && (float)pos<scroll+VP_UI_LIST_PAGE)
            round_rect(36,174+(int)lroundf(((float)pos-scroll)*54.f),5,44,2,C(28,94,203,210));
        cover(f->selected_cover,selected,hero,0xffffffffu); focus(hero);
        text(f,VP_TYPE_HERO_TITLE,620,203,300,wall_primary,selected->title);
        tag(f,620,218,86,vp_ui_platform_label(selected->platform));
        vp_ui_paragraph(f->font,VP_TYPE_DESCRIPTION,620,269,292,8,wall_muted,
            selected->description[0]?selected->description:"Sin descripción disponible.");
    } else {
        carousel_cards(f,f->touch_scrolling?f->carousel_base:(int)pos);
        {
            int title_width=vp_ui_text_width(f->font,VP_TYPE_HERO_TITLE,false,selected->title);
            const char *platform=vp_ui_platform_label(selected->platform);
            int tag_width=vp_ui_text_width(f->font,VP_TYPE_GRID_TITLE,false,platform)+14,x;
            if (title_width>560) title_width=560;
            x=(960-title_width-tag_width-12)/2;
            text(f,VP_TYPE_HERO_TITLE,x,438,title_width,wall_primary,selected->title);
            tag(f,x+title_width+12,419,tag_width,platform);
            for (i=0;i<f->visible_count && i<7;i++)
                vita2d_draw_fill_circle(480+((int)i-(int)(f->visible_count<7?f->visible_count:7)/2)*15,462,3.5f,
                    i==pos%7?wall_primary:alpha(wall_muted,230));
        }
    }
}

static const char *download_label(VpDownloadState state) {
    switch (state) {
        case VP_DOWNLOAD_PENDING:return "En cola"; case VP_DOWNLOAD_DOWNLOADING:return "Descargando";
        case VP_DOWNLOAD_COMPLETED:return "Completada"; case VP_DOWNLOAD_FAILED:return "Fallida";
        case VP_DOWNLOAD_CANCELED:return "Cancelada"; case VP_DOWNLOAD_PAUSED:return "Pausada";
    }
    return "Desconocido";
}

static void empty(const VpUiFrame *f,const char *symbol,const char *title,const char *message) {
    VpUiRect r={180,190,600,230}; glass(r,false,false); icon(symbol,448,211,64);
    text(f,VP_TYPE_HERO_TITLE,210,321,540,panel_primary,title);
    vp_ui_paragraph(f->font,VP_TYPE_PAGE_SUBTITLE,210,358,540,2,panel_muted,message);
}

static void settings_empty(const VpUiFrame *f,const char *symbol,const char *title,const char *message) {
    VpUiRect r=vp_ui_settings_content_card();
    glass(r,false,false); icon(symbol,r.x+(r.w-64)/2,r.y+21,64);
    text(f,VP_TYPE_HERO_TITLE,r.x+30,r.y+131,r.w-60,panel_primary,title);
    vp_ui_paragraph(f->font,VP_TYPE_PAGE_SUBTITLE,r.x+30,r.y+168,r.w-60,2,panel_muted,message);
}

static const char *connection_label(VpConnectionState state, bool images) {
    switch (state) {
        case VP_CONNECTION_UNCONFIGURED:return images?"Configura el host de tu servicio de imágenes.":"Ingresa tu API key de SteamGridDB.";
        case VP_CONNECTION_NOT_CHECKED:return "Selecciona Probar conexión para verificar la configuración.";
        case VP_CONNECTION_CHECKING:return "Comprobando la conexión...";
        case VP_CONNECTION_CONNECTED:return "Conectado correctamente.";
        case VP_CONNECTION_INVALID_KEY:return images?"El servicio rechazó la conexión. Revisa el host.":"API key inválida o sin permisos. Revisa la clave.";
        case VP_CONNECTION_RATE_LIMITED:return "Límite de solicitudes alcanzado. Reintenta más tarde.";
        case VP_CONNECTION_INVALID_RESPONSE:return "Respuesta no válida. Revisa el host y el servicio.";
        case VP_CONNECTION_UNREACHABLE:return "No se pudo conectar. Revisa la red y reintenta.";
    }
    return "Sin datos de conexión.";
}

static void sync_status(const VpUiFrame *f, int y) {
    char label[128], detail[VP_TEXT];
    const VpUiSyncStatus *status = &f->sync;
    unsigned int percent = vp_ui_sync_percent(status);
    unsigned int tint = status->state == VP_SYNC_FAILED ? C(245,165,151,255) : panel_primary;
    vp_ui_sync_text(status, label, sizeof(label));
    text(f,VP_TYPE_DESCRIPTION,316,y,584,tint,label);
    round_rect(316,y+9,584,4,2,C(238,245,255,60));
    if (percent) round_rect(316,y+9,584*percent/100.f,4,2,tint);
    if (status->state == VP_SYNC_RUNNING) {
        (void)snprintf(detail,sizeof(detail),"%u listas · %u sin coincidencia · %u fallidas",
            (unsigned)status->ready,(unsigned)status->missing,(unsigned)status->failed);
        if (status->operation == VP_SYNC_CATALOG)
            (void)snprintf(detail,sizeof(detail),"Actualizando el catálogo en segundo plano.");
        if (status->detail[0]) (void)snprintf(detail,sizeof(detail),"%s",status->detail);
    } else (void)snprintf(detail,sizeof(detail),"%s",status->detail);
    vp_ui_paragraph(f->font,VP_TYPE_DESCRIPTION,316,y+35,584,2,panel_muted,detail);
}

static void settings(const VpUiFrame *f) {
    static const char *names[]={"Integraciones","Descargas","Apariencia","Acerca de"};
    static const char *icons[]={"ui_server","ui_download","ui_interface","ui_info"};
    int i,section=f->settings_section;
    icon("ui_settings",44,199,54);
    text(f,VP_TYPE_PAGE_TITLE,104,239,200,wall_primary,"Ajustes");
    vp_ui_paragraph(f->font,VP_TYPE_PAGE_SUBTITLE,48,280,222,3,wall_muted,"Personaliza tu experiencia en VitaShelf Launcher.");
    if (!f->settings_detail) {
        for (i=0;i<VP_SETTINGS_COUNT;i++) {
            VpUiRect r=vp_ui_settings_row((size_t)i); glass(r,section==i,false);
            icon(icons[i],r.x+22,r.y+9,32);
            text(f,VP_TYPE_SETTINGS_ROW,r.x+80,r.y+32,r.w-116,panel_primary,names[i]);
            vita2d_draw_line(r.x+r.w-28,r.y+18,r.x+r.w-21,r.y+25,panel_primary);
            vita2d_draw_line(r.x+r.w-21,r.y+25,r.x+r.w-28,r.y+32,panel_primary);
        }
        return;
    }
    if (section==VP_SETTINGS_APPEARANCE) {
        char color_value[64];
        for (i=0;i<VP_UI_WALLPAPER_COUNT;i++) {
            VpUiRect r=vp_ui_wallpaper_tile((size_t)i);
            const VpUiWallpaper *wallpaper=vp_ui_wallpaper(i);
            glass(r,f->settings_field==i,false);
            fit(asset(wallpaper->thumbnail),r.x+23,r.y+6,240,110,C(255,255,255,255));
            round_rect(r.x+8,r.y+102,r.w-16,26,8,C(15,30,61,230));
            text(f,VP_TYPE_DESCRIPTION,r.x+18,r.y+121,r.w-66,C(250,252,255,255),wallpaper->label);
            if (f->preferences.wallpaper==i) icon("ui_check",r.x+r.w-40,r.y+104,22);
        }
        {
            VpUiRect r=vp_ui_text_color_row();
            glass(r,f->settings_field==VP_UI_APPEARANCE_TEXT_COLOR_SLOT,false);
            (void)snprintf(color_value,sizeof(color_value),"%s",vp_ui_text_color(f->preferences.text_color)->label);
            text(f,VP_TYPE_DESCRIPTION,r.x+16,r.y+25,400,panel_primary,"Color de texto");
            text(f,VP_TYPE_DESCRIPTION,r.x+440,r.y+25,130,panel_primary,color_value);
        }
        {
            VpUiRect r=vp_ui_wallpaper_motion_row();
            VpUiRect sw={r.x+r.w-66,r.y+7,44,24};
            const char *value=f->preferences.animated?"Activado":"Desactivado";
            glass(r,f->settings_field==VP_UI_APPEARANCE_MOTION_SLOT,false);
            text(f,VP_TYPE_DESCRIPTION,r.x+16,r.y+25,400,panel_primary,"Fondo animado");
            text(f,VP_TYPE_DESCRIPTION,sw.x-12-vp_ui_text_width(f->font,VP_TYPE_DESCRIPTION,false,value),
                 r.y+25,110,panel_primary,value);
            toggle(sw,f->preferences.animated);
        }
        return;
    }
    if (section==VP_SETTINGS_DOWNLOADS) {
        size_t start=(f->download_selection/4)*4,j;
        if (!f->downloads->count) { settings_empty(f,"ui_download","Todo al día","Agrega un juego desde la biblioteca para descargarlo."); return; }
        for (j=start;j<f->downloads->count && j<start+4;j++) {
            const VpDownloadEntry *e=&f->downloads->entries[j]; VpUiRect r={310,120+(int)(j-start)*76,600,68};
            char status[64]; glass(r,j==f->download_selection,false);
            icon(e->state==VP_DOWNLOAD_COMPLETED?"ui_check":"ui_download",r.x+10,r.y+11,28);
            text(f,VP_TYPE_LIST_TITLE,r.x+48,r.y+24,365,panel_primary,e->item.title);
            (void)snprintf(status,sizeof(status),"%s %u%%",download_label(e->state),e->percent);
            text(f,VP_TYPE_STATUS,r.x+414,r.y+24,174,panel_primary,status);
            if (e->state==VP_DOWNLOAD_FAILED) text(f,VP_TYPE_DESCRIPTION,r.x+48,r.y+49,536,panel_muted,e->error);
            else {
                round_rect(r.x+48,r.y+47,536,4,2,C(238,245,255,60));
                if (e->percent) round_rect(r.x+48,r.y+47,536*fminf(e->percent,100)/100.f,4,2,accent);
            }
        }
        return;
    }
    if (section==VP_SETTINGS_INTEGRATIONS && !f->settings_integration_open) {
        static const char *integrations[]={"RomM","SteamGridDB","Image Service"};
        static const char *symbols[]={"ui_romm","ui_library","ui_server"};
        for (i=0;i<VP_INTEGRATION_COUNT;i++) {
            VpUiRect r=vp_ui_settings_row((size_t)i);
            glass(r,f->settings_integration==i,false); icon(symbols[i],r.x+22,r.y+9,32);
            text(f,VP_TYPE_SETTINGS_ROW,r.x+80,r.y+32,r.w-116,panel_primary,integrations[i]);
            vita2d_draw_line(r.x+r.w-28,r.y+18,r.x+r.w-21,r.y+25,panel_primary);
            vita2d_draw_line(r.x+r.w-21,r.y+25,r.x+r.w-28,r.y+32,panel_primary);
        }
        return;
    }
    if (section==VP_SETTINGS_INTEGRATIONS && f->settings_integration!=VP_INTEGRATION_ROMM) {
        bool images=f->settings_integration==VP_INTEGRATION_IMAGES;
        int count=images?2:3;
        char host[VP_URL];
        const char *configured=f->image_service_host?f->image_service_host:"";
        const char *titles[]={images?"Host":"API key","Probar conexión","Sincronizar carátulas"};
        const char *symbols[]={images?"ui_server":"ui_key","ui_sync","ui_folder"};
        const char *values[3];
        (void)snprintf(host,sizeof(host),"%s",configured);
        if (strlen(host)>=6 && !strcmp(host+strlen(host)-6,"/image")) host[strlen(host)-6]=0;
        values[0]=images?(host[0]?host:"X para configurar el host"):
            (f->steamgrid_configured?"••••••••  ·  X para cambiar":"X para ingresar tu API key");
        values[1]=images?"Verifica /health en tu servicio":"Verifica la API key y la conexión";
        values[2]="Prepara imágenes para los juegos de Vita instalados";
        text(f,VP_TYPE_LIST_TITLE,316,99,584,panel_primary,images?"Image Service":"SteamGridDB");
        for (i=0;i<count;i++) {
            VpUiRect r={310,112+i*72,600,62};
            glass(r,f->settings_field==i,false); icon(symbols[i],r.x+12,r.y+13,30);
            text(f,VP_TYPE_LIST_TITLE,r.x+58,r.y+24,528,panel_primary,titles[i]);
            text(f,VP_TYPE_DESCRIPTION,r.x+58,r.y+48,528,panel_primary,values[i]);
        }
        if (!images && f->sync.state != VP_SYNC_IDLE) sync_status(f,350);
        else vp_ui_paragraph(f->font,VP_TYPE_DESCRIPTION,316,358,584,2,panel_muted,
            connection_label(images?f->image_service_state:f->steamgrid_state,images));
        vp_ui_paragraph(f->font,VP_TYPE_DESCRIPTION,316,435,584,2,panel_muted,images?
            "Servicio compartido por RomM y SteamGridDB. Las imágenes se guardan en la Vita.":
            "Obtén tu API key en steamgriddb.com/profile/preferences/api. Configura también Image Service.");
        return;
    }
    if (section==VP_SETTINGS_INTEGRATIONS) {
        const char *titles[]={"URL del servidor","Pair code","Sincronizar ahora","Sincronizar metadata"};
        const char *symbols[]={"ui_home","ui_key","ui_sync","ui_folder"};
        bool pair_enabled=vp_romm_pair_code_enabled(f->romm_state);
        const char *values[]={f->romm_url[0]?f->romm_url:"X para configurar",
            pair_enabled?(f->pair_code[0]?"Código ingresado; Triángulo para canjear":"X para ingresar el código de 8 caracteres"):
            (f->romm_state==VP_ROMM_CONNECTED?"Vinculado; no necesitas otro código":"Espera a que termine la comprobación"),
            "Actualiza el catálogo en segundo plano","Descarga y prepara las imágenes"};
        const char *status="Configura la URL y genera un pair code en RomM.";
        text(f,VP_TYPE_LIST_TITLE,316,99,584,panel_primary,"RomM");
        for (i=0;i<4;i++) {
            VpUiRect r={310,112+i*72,600,62}; bool disabled=i==1 && !pair_enabled;
            glass(r,!disabled && f->settings_field==i,false);
            icon(i==1 && f->romm_state==VP_ROMM_CONNECTED?"ui_check":symbols[i],r.x+12,r.y+13,30);
            text(f,VP_TYPE_LIST_TITLE,r.x+58,r.y+24,528,panel_primary,titles[i]);
            text(f,VP_TYPE_DESCRIPTION,r.x+58,r.y+48,528,disabled?panel_muted:panel_primary,values[i]);
        }
        switch (f->romm_state) {
            case VP_ROMM_CONNECTED:status="RomM conectado correctamente.";break;
            case VP_ROMM_CHECKING:status="Comprobando la conexión con RomM...";break;
            case VP_ROMM_INVALID_TOKEN:status="El token ya no funciona. Ingresa un nuevo pair code.";break;
            case VP_ROMM_FORBIDDEN:status="El token no tiene permisos de lectura. Revisa RomM.";break;
            case VP_ROMM_UNREACHABLE:status="No se pudo verificar RomM. Revisa la red y reintenta.";break;
            default:break;
        }
        if (f->sync.state != VP_SYNC_IDLE) sync_status(f,414);
        else vp_ui_paragraph(f->font,VP_TYPE_DESCRIPTION,316,428,584,2,panel_muted,status);
        return;
    }
    {
        VpUiRect r={310,108,600,360}; char detail[VP_URL+32]; glass(r,false,false);
        icon("ui_info",334,126,36);
        text(f,VP_TYPE_HERO_TITLE,384,155,502,panel_primary,"Acerca de");
        (void)snprintf(detail,sizeof(detail),"VitaShelf Launcher · %s",f->app_version?f->app_version:"—");
        text(f,VP_TYPE_LIST_TITLE,334,191,552,panel_primary,detail);
        (void)snprintf(detail,sizeof(detail),"Libre: %.1f GiB / %.1f GiB",f->storage_free/1073741824.,f->storage_total/1073741824.);
        text(f,VP_TYPE_DESCRIPTION,334,224,552,panel_muted,detail);
        text(f,VP_TYPE_DESCRIPTION,334,250,552,panel_muted,"Descargas: ux0:data/vitashelflauncher/downloads/");
        text(f,VP_TYPE_DESCRIPTION,334,276,552,panel_muted,"Imágenes: ux0:data/vitashelflauncher/cache/covers/");
        text(f,VP_TYPE_DESCRIPTION,334,302,552,panel_muted,"PSP ISO: ux0:pspemu/ISO/");
        if (f->romm_state==VP_ROMM_CONNECTED) {
            const char *server=f->romm_url?f->romm_url:"",*scheme=strstr(server,"://"),*end,*at;
            size_t count=0,j;
            if (scheme) server=scheme+3;
            end=server+strcspn(server,"/?#");
            for (at=server;at<end;at++) if (*at=='@') server=at+1;
            icon("ui_romm",334,328,24);
            text(f,VP_TYPE_LIST_TITLE,370,348,516,panel_primary,"RomM");
            (void)snprintf(detail,sizeof(detail),"Servidor: %.*s",(int)(end-server),server);
            text(f,VP_TYPE_DESCRIPTION,334,378,552,panel_muted,detail);
            for (j=0;j<f->app->catalog.count;j++) if (f->app->catalog.items[j].source==VP_SOURCE_ROMM) count++;
            if (f->romm_catalog_available) (void)snprintf(detail,sizeof(detail),"Catálogo: %u juegos",(unsigned)count);
            else (void)snprintf(detail,sizeof(detail),"Catálogo: Sin sincronizar");
            text(f,VP_TYPE_DESCRIPTION,334,410,552,panel_muted,detail);
            (void)snprintf(detail,sizeof(detail),"Última sincronización: %s",
                f->romm_catalog_available && f->romm_last_sync[0]?f->romm_last_sync:"Sin datos");
            text(f,VP_TYPE_DESCRIPTION,334,442,552,panel_muted,detail);
        }
    }
}

/* Reuse the footer's face-button atlas in the library panel. */
static void button(int kind,int x,int y);

static void dpad(int x,int y) {
    const unsigned int edge=C(88,102,126,255),body=C(27,35,49,255),arrow=C(192,204,224,255);
    round_rect(x-13,y-6,26,12,3,edge);
    round_rect(x-6,y-13,12,26,3,edge);
    round_rect(x-12,y-5,24,10,2,body);
    round_rect(x-5,y-12,10,24,2,body);
    vita2d_draw_line(x-2,y-8,x,y-10,arrow); vita2d_draw_line(x,y-10,x+2,y-8,arrow);
    vita2d_draw_line(x-2,y+8,x,y+10,arrow); vita2d_draw_line(x,y+10,x+2,y+8,arrow);
    vita2d_draw_line(x-8,y-2,x-10,y,arrow); vita2d_draw_line(x-10,y,x-8,y+2,arrow);
    vita2d_draw_line(x+8,y-2,x+10,y,arrow); vita2d_draw_line(x+10,y,x+8,y+2,arrow);
}

static void select_button(const VpUiFrame *f,int x,int y) {
    vita2d_font *font=f->font->weights[1];
    int width=vita2d_font_text_width(font,10,"SELECT");
    round_rect(x-27,y-11,54,22,10,C(88,102,126,255));
    round_rect(x-26,y-10,52,20,9,C(27,35,49,255));
    vita2d_font_draw_text(font,x-width/2,y+4,C(210,221,240,255),10,"SELECT");
}

static void render_library_modal(const VpUiFrame *f) {
    static const char *actions[]={"Quitar de Favoritos","Añadir a Favoritos","Ocultar juego",
                                  "Mostrar juego","Cerrar"};
    static const char *filters[]={"Ocultar juegos ocultos","Mostrar juegos ocultos",
                                  "Mostrar todos los juegos","Solo juegos descargados","Cerrar"};
    const VpUiRect card=vp_ui_library_panel(f->library_modal_progress);
    const VpItem *item=f->visible_count && f->app->selected_item<f->app->catalog.count?
        &f->app->catalog.items[f->app->selected_item]:NULL;
    bool arcade=item!=NULL && vp_platform_is_arcade(item->platform);
    const char *title=f->library_modal==VP_LIBRARY_MODAL_ACTIONS?"Acciones del juego":
        f->library_modal==VP_LIBRARY_MODAL_CORES?"Núcleo arcade":"Filtros de biblioteca";
    const char *close_label=f->library_modal==VP_LIBRARY_MODAL_CORES?"Volver":"Cerrar";
    const unsigned int primary=C(243,247,255,255),secondary=C(185,200,226,255);
    const int hint_y=card.y+card.h-38;
    int rows;
    int index;
    vita2d_draw_rectangle(0,0,960,544,C(5,11,28,190));
    round_rect(card.x,card.y,card.w,card.h,20,C(12,19,40,255));
    /* Text coordinates are baselines: leave room above the 30px title glyphs. */
    text(f,VP_TYPE_PAGE_TITLE,card.x+24,card.y+54,card.w-48,primary,title);
    rows=f->library_modal_rows>0?f->library_modal_rows:
        f->library_modal==VP_LIBRARY_MODAL_ACTIONS?VP_LIBRARY_ACTIONS_ROWS:VP_LIBRARY_FILTERS_ROWS;
    for (index=0;index<rows;index++) {
        VpUiRect r=f->library_modal==VP_LIBRARY_MODAL_CORES
            ? vp_ui_library_panel_core_row((size_t)index,f->library_modal_progress)
            : vp_ui_library_panel_row((size_t)index,f->library_modal_progress);
        bool selected=(size_t)index==f->library_modal_row;
        const char *label;
        if (f->library_modal==VP_LIBRARY_MODAL_ACTIONS) {
            if (index==0) label=item && item->favorite?actions[0]:actions[1];
            else if (index==1) label=item && item->hidden?actions[3]:actions[2];
            else if (index==2 && arcade) label="Núcleo arcade…";
            else label=actions[4];
        } else if (f->library_modal==VP_LIBRARY_MODAL_CORES) {
            const VpArcadeCore *core=vp_arcade_core((size_t)index);
            label=core!=NULL?core->label:"Automático";
        } else {
            if (index==0) label=f->app->library_filters.show_hidden?filters[0]:filters[1];
            else if (index==1) label=f->app->library_filters.downloaded_only?filters[2]:filters[3];
            else label=filters[4];
        }
        if (selected) {
            round_rect(r.x,r.y,r.w,r.h,12,C(40,56,86,255));
            round_rect(r.x+2,r.y+14,3,r.h-28,1,C(113,208,255,255));
        }
        vp_ui_text(f->font,VP_TYPE_SETTINGS_ROW,selected,r.x+18,r.y+33,r.w-36,primary,label);
    }
    dpad(card.x+36,hint_y);
    text(f,VP_TYPE_FOOTER,card.x+55,hint_y+5,76,secondary,"Elegir");
    button(0,card.x+168,hint_y);
    text(f,VP_TYPE_FOOTER,card.x+186,hint_y+5,88,secondary,"Activar");
    button(1,card.x+306,hint_y);
    text(f,VP_TYPE_FOOTER,card.x+324,hint_y+5,92,secondary,close_label);
}

/* One small atlas preserves smooth colored outlines at the native footer size. */
static void button(int kind,int x,int y) {
    vita2d_texture *texture=asset("ui_buttons");
    if (texture && kind>=0 && kind<4)
        vita2d_draw_texture_tint_part_scale(texture,x-12,y-12,(kind%2)*64,(kind/2)*64,64,64,.375f,.375f,0xffffffffu);
}

static void footer(const VpUiFrame *f) {
    bool roms=f->app->active_screen==VP_SCREEN_LIBRARY && f->app->library_level==VP_LEVEL_ROMS && f->visible_count;
    bool downloads=f->app->active_screen==VP_SCREEN_SETTINGS && f->settings_detail && f->settings_section==VP_SETTINGS_DOWNLOADS;
    bool connection=f->app->active_screen==VP_SCREEN_SETTINGS && f->settings_detail && f->settings_integration_open && f->settings_integration==VP_INTEGRATION_ROMM && f->settings_section==VP_SETTINGS_INTEGRATIONS;
    const char *action=roms?"Descargar":"Abrir";
    VpUiRect r={24,492,912,42};
    round_rect(r.x-1,r.y-1,r.w+2,r.h+2,22,C(233,244,255,150));
    round_rect(r.x,r.y,r.w,r.h,21,C(194,215,255,120));
    round_rect(43,504,28,19,5,C(225,235,255,210));
    round_rect(80,504,28,19,5,C(225,235,255,210));
    text(f,VP_TYPE_FOOTER,50,519,18,C(26,47,91,255),"L");
    text(f,VP_TYPE_FOOTER,87,519,18,C(26,47,91,255),"R");
    text(f,VP_TYPE_FOOTER,121,518,210,C(20,40,83,255),"Cambiar sección");
    vita2d_draw_line(341,502,341,524,C(116,146,206,120));
    if (roms) {
        const VpItem *item=&f->app->catalog.items[f->visible[position(f)]];
        if (item->installed) action="Jugar";
        else if (item->source==VP_SOURCE_HOMEBREW) action="Instalar";
    }
    if (downloads) action="Cancelar / Reintentar";
    if (f->app->active_screen==VP_SCREEN_SETTINGS && f->settings_detail && f->settings_section==VP_SETTINGS_APPEARANCE)
        action=f->settings_field==VP_UI_APPEARANCE_MOTION_SLOT?"Activar / Desactivar":
               f->settings_field==VP_UI_APPEARANCE_TEXT_COLOR_SLOT?"Cambiar color":"Aplicar fondo";
    if (f->app->active_screen==VP_SCREEN_SETTINGS && !f->settings_detail) action="Seleccionar";
    if (roms) {
        button(3,366,513); text(f,VP_TYPE_FOOTER,383,518,62,C(12,32,76,255),"Vista");
        button(2,510,513); text(f,VP_TYPE_FOOTER,527,518,104,C(12,32,76,255),"Opciones");
        select_button(f,675,513);
        text(f,VP_TYPE_FOOTER,710,518,62,C(12,32,76,255),"Filtros");
        button(1,790,513); text(f,VP_TYPE_FOOTER,807,518,64,C(12,32,76,255),"Volver");
    } else {
        button(0,366,513); text(f,VP_TYPE_FOOTER,383,518,210,C(12,32,76,255),action);
        button(1,613,513); text(f,VP_TYPE_FOOTER,630,518,64,C(12,32,76,255),"Volver");
        if (f->app->active_screen==VP_SCREEN_LIBRARY) {
            select_button(f,775,513);
            text(f,VP_TYPE_FOOTER,810,518,62,C(12,32,76,255),"Filtros");
        }
        if (downloads) {
            button(2,713,513); text(f,VP_TYPE_FOOTER,730,518,56,C(12,32,76,255),"Quitar");
        }
        if (connection) {
            button(3,815,513); text(f,VP_TYPE_FOOTER,832,518,102,C(12,32,76,255),"Guardar");
        }
    }
}

void vp_ui_render(const VpUiFrame *f) {
    unsigned long long now=sceKernelGetProcessTimeWide();
    bg=color(f->app->theme.background,C(53,75,124,255));
    surface=color(f->app->theme.surface,C(141,169,230,255));
    ink=color(f->app->theme.text,C(250,252,255,255));
    muted=color(f->app->theme.muted,C(204,220,249,255));
    accent=color(f->app->theme.accent,C(79,159,255,255));
    resolve_text_colors(f);
    backdrop(f,now); header(f); heading(f);
    if (f->app->active_screen==VP_SCREEN_LIBRARY) {
        if (f->app->library_level==VP_LEVEL_PLATFORMS) {
            if (f->group_count) platforms(f);
            else empty(f,"ui_library","Tu biblioteca te espera","Abre Ajustes / Integraciones / RomM para conectar tu servidor.");
        } else if (f->visible_count) roms(f);
        else empty(f,"ui_folder","No hay juegos en esta consola","Círculo vuelve a la biblioteca.");
    } else if (f->app->active_screen==VP_SCREEN_SETTINGS) settings(f);
    else empty(f,"ui_store","Próximamente","Tu biblioteca y RomM siguen disponibles con L / R.");
    footer(f);
    if (f->library_modal!=VP_LIBRARY_MODAL_NONE) render_library_modal(f);
    if (f->notice_id != last_notice_id) {
        last_notice_id=f->notice_id;
        (void)snprintf(last_notice,sizeof(last_notice),"%s",f->app->notice); notice_at=now;
    }
    if (last_notice[0]) {
        float age=(float)(now-notice_at)/1000000;
        if (age < 4) {
            unsigned int a=(unsigned int)(235*fmaxf(0,fminf(1,(4-age)*2)));
            round_rect(636,64,296,54,14,C(29,52,104,a));
            vp_ui_paragraph(f->font,VP_TYPE_DESCRIPTION,648,85,272,2,alpha(C(250,252,255,255),a),last_notice);
        }
    }
}

void vp_ui_shutdown(void) {
    size_t i;
    vita2d_wait_rendering_done(); vp_ui_backdrop_shutdown();
    for (i=0;i<sizeof(assets)/sizeof(assets[0]);i++) {
        if (assets[i].texture) vita2d_free_texture(assets[i].texture);
        assets[i].texture=NULL; assets[i].attempted=false;
    }
    last_notice[0]='\0'; last_notice_id=0;
    memset(&console_motion,0,sizeof(console_motion));
    memset(&carousel_motion,0,sizeof(carousel_motion));
    carousel_touch_scrolling=false;
}
