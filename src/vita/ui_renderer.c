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

static void console_card(VpUiRect r,bool selected) {
    vita2d_texture *material=asset("console_surface"),*ring=selected?asset("console_focus"):NULL;
    int ix,iy;
    float xs[]={r.x-18,r.x+14,r.x+r.w-14,r.x+r.w+18};
    float ys[]={r.y-18,r.y+14,r.y+r.h-14,r.y+r.h+18};
    round_rect(r.x,r.y+4,r.w,r.h,14,C(10,18,40,45));
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
    (void)snprintf(name,sizeof(name),"header_%s",logo+9);
    return name;
}

void vp_ui_prepare(const VpUiFrame *f) {
    static const char *common[]={"focus","ui_buttons","ui_library","ui_store","ui_settings","ui_download","ui_check","ui_star","ui_psx"};
    size_t i,start;
    asset_budget=3;
    for (i=0;i<sizeof(common)/sizeof(common[0]);i++) (void)asset(common[i]);
    if (f->app->active_screen==VP_SCREEN_LIBRARY && f->app->library_level==VP_LEVEL_PLATFORMS) {
        (void)asset("console_surface"); (void)asset("console_focus");
        start=vp_ui_console_start(f->app->selected_platform,f->group_count);
        for (i=start;i<f->group_count && i<start+VP_UI_CONSOLE_PAGE;i++) (void)asset(vp_ui_platform(f->groups[i].platform)->asset);
    } else if (f->app->active_screen==VP_SCREEN_SETTINGS) {
        const char *icons[]={"ui_server","ui_download","ui_romm","ui_info","ui_home","ui_key","ui_sync","ui_folder","ui_interface"};
        for (i=0;i<sizeof(icons)/sizeof(icons[0]);i++) (void)asset(icons[i]);
        if (f->settings_detail && f->settings_section==VP_SETTINGS_APPEARANCE)
            for (i=0;i<VP_UI_WALLPAPER_COUNT;i++) (void)asset(vp_ui_wallpaper((int)i)->thumbnail);
    }
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
        title=f->visible_count?f->app->catalog.items[f->visible[position(f)]].platform_display:f->app->active_platform;
        (void)snprintf(subtitle,sizeof(subtitle),"%u juegos",(unsigned)f->visible_count);
    } else (void)snprintf(subtitle,sizeof(subtitle),"%u consolas   •   %u juegos",(unsigned)f->group_count,(unsigned)f->app->catalog.count);
    if (logo && !strcmp(logo,"ui_psx")) icon(logo,61,81,54);
    else if (logo) fit(asset(logo),24,76,128,64,0xffffffffu);
    text(f,VP_TYPE_PAGE_TITLE,172,115,620,wall_primary,title);
    text(f,VP_TYPE_PAGE_SUBTITLE,173,140,610,wall_muted,subtitle);
    if (f->app->library_level==VP_LEVEL_ROMS && f->visible_count) {
        char count[32]; (void)snprintf(count,sizeof(count),"%u / %u",(unsigned)position(f)+1,(unsigned)f->visible_count);
        text(f,VP_TYPE_STATUS,870,141,70,wall_primary,count);
    }
}

static void platforms(const VpUiFrame *f) {
    size_t start=vp_ui_console_start(f->app->selected_platform,f->group_count),i;
    for (i=start;i<f->group_count && i<start+VP_UI_CONSOLE_PAGE;i++) {
        const VpUiPlatform *platform=vp_ui_platform(f->groups[i].platform);
        VpUiRect r=vp_ui_console_tile(i-start);
        bool selected=i==f->app->selected_platform;
        console_card(r,selected);
        fit(asset(platform->asset),r.x+10,r.y+22,r.w-20,116,0xffffffffu);
        round_rect(r.x+(r.w-42)/2,r.y+r.h-16,42,4,2,rgb(platform->rgb));
    }
    if (f->group_count) {
        const VpPlatformGroup *group=&f->groups[f->app->selected_platform]; char detail[VP_TEXT+40];
        (void)snprintf(detail,sizeof(detail),"%s  ·  %u juegos",group->display,(unsigned)group->count);
        text(f,VP_TYPE_CONSOLE_TITLE,48,462,850,wall_primary,detail);
    }
}

static void cover(vita2d_texture *texture,const VpItem *item,VpUiRect r,unsigned int tint) {
    round_rect(r.x,r.y,r.w,r.h,10,C(17,31,65,225));
    if (texture) fit(texture,r.x+3,r.y+3,r.w-6,r.h-6,tint);
    else {
        fit(asset(vp_ui_platform(item->platform)->asset),r.x+12,r.y+r.h*.2f,r.w-24,r.h*.4f,0xffffffffu);
        /* Missing artwork remains identifiable and actionable offline. */
    }
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
        start=(pos/VP_UI_GRID_PAGE)*VP_UI_GRID_PAGE;
        for (i=start;i<f->visible_count && i<start+VP_UI_GRID_PAGE;i++) {
            const VpItem *item=&f->app->catalog.items[f->visible[i]];
            bool active=i==pos; VpUiRect r=vp_ui_tile(i-start);
            cover(f->covers[i-start],item,r,0xffffffffu);
            if (active) {
                const char *platform=vp_ui_platform_label(item->platform);
                int tag_width=vp_ui_text_width(f->font,VP_TYPE_GRID_TITLE,false,platform)+14;
                if (tag_width>68) tag_width=68;
                round_rect(r.x,r.y+r.h-29,r.w,29,8,C(13,32,79,225));
                text(f,VP_TYPE_GRID_TITLE,r.x+9,r.y+r.h-10,r.w-tag_width-19,C(250,252,255,255),item->title);
                tag(f,r.x+r.w-tag_width-4,r.y+r.h-25,tag_width,platform);
                focus(r);
            }
            if (item->favorite) icon("ui_star",r.x+r.w-25,r.y+5,20);
            icon(item->installed?"ui_check":"ui_download",r.x+5,r.y+5,19);
        }
    } else if (f->app->active_view==VP_VIEW_LIST) {
        VpUiRect hero={382,160,212,262};
        start=(pos/VP_UI_LIST_PAGE)*VP_UI_LIST_PAGE;
        round_rect(37,174,3,258,1,C(235,244,255,140));
        round_rect(36,174+(int)(pos-start)*52,5,44,2,C(28,94,203,210));
        for (i=start;i<f->visible_count && i<start+VP_UI_LIST_PAGE;i++) {
            const VpItem *item=&f->app->catalog.items[f->visible[i]];
            VpUiRect r=vp_ui_list_row(i-start); bool active=i==pos;
            if (active) glass(r,true,false);
            fit(f->covers[i-start],r.x+10,r.y+4,38,40,0xffffffffu);
            vp_ui_text(f->font,VP_TYPE_LIST_TITLE,active,r.x+62,r.y+29,r.w-74,panel_primary,item->title);
        }
        cover(f->covers[pos-start],selected,hero,0xffffffffu); focus(hero);
        text(f,VP_TYPE_HERO_TITLE,620,203,300,wall_primary,selected->title);
        tag(f,620,218,86,vp_ui_platform_label(selected->platform));
        vp_ui_paragraph(f->font,VP_TYPE_DESCRIPTION,620,269,292,8,wall_muted,
            selected->description[0]?selected->description:"Sin descripción disponible.");
    } else {
        int offset;
        for (offset=-3;offset<=3;offset++) {
            size_t index=(pos+f->visible_count+(size_t)((offset+(int)f->visible_count)%(int)f->visible_count))%f->visible_count;
            const VpItem *item=&f->app->catalog.items[f->visible[index]];
            VpUiRect r=vp_ui_carousel_tile(offset);
            if ((f->visible_count<7 && abs(offset)>(int)f->visible_count/2) ||
                (f->visible_count%2==0 && offset<0 && abs(offset)==(int)f->visible_count/2)) continue;
            cover(f->covers[offset+3],item,r,offset?C(245,248,255,210):0xffffffffu);
            if (offset) {
                vita2d_draw_rectangle(r.x,r.y+r.h+4,r.w,18,C(148,175,233,35));
            } else focus(r);
        }
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
            glass(r,f->settings_field==VP_UI_APPEARANCE_MOTION_SLOT,false);
            text(f,VP_TYPE_DESCRIPTION,r.x+16,r.y+25,400,panel_primary,"Fondo animado");
            text(f,VP_TYPE_DESCRIPTION,r.x+440,r.y+25,130,panel_primary,f->preferences.animated?"Activado":"Desactivado");
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
    if (section==VP_SETTINGS_INTEGRATIONS && !f->settings_romm) {
        VpUiRect r=vp_ui_settings_row(0);
        glass(r,true,false); icon("ui_romm",r.x+22,r.y+9,32);
        text(f,VP_TYPE_SETTINGS_ROW,r.x+80,r.y+32,r.w-116,panel_primary,"RomM");
        vita2d_draw_line(r.x+r.w-28,r.y+18,r.x+r.w-21,r.y+25,panel_primary);
        vita2d_draw_line(r.x+r.w-21,r.y+25,r.x+r.w-28,r.y+32,panel_primary);
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
        vp_ui_paragraph(f->font,VP_TYPE_DESCRIPTION,316,428,584,2,panel_muted,status);
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
        if (f->romm_state==VP_ROMM_CONNECTED) {
            const char *server=f->romm_url?f->romm_url:"",*scheme=strstr(server,"://"),*end,*at;
            size_t count=0,j;
            if (scheme) server=scheme+3;
            end=server+strcspn(server,"/?#");
            for (at=server;at<end;at++) if (*at=='@') server=at+1;
            icon("ui_romm",334,300,24);
            text(f,VP_TYPE_LIST_TITLE,370,320,516,panel_primary,"RomM");
            (void)snprintf(detail,sizeof(detail),"Servidor: %.*s",(int)(end-server),server);
            text(f,VP_TYPE_DESCRIPTION,334,350,552,panel_muted,detail);
            for (j=0;j<f->app->catalog.count;j++) if (f->app->catalog.items[j].source==VP_SOURCE_ROMM) count++;
            if (f->romm_catalog_available) (void)snprintf(detail,sizeof(detail),"Catálogo: %u juegos",(unsigned)count);
            else (void)snprintf(detail,sizeof(detail),"Catálogo: Sin sincronizar");
            text(f,VP_TYPE_DESCRIPTION,334,382,552,panel_muted,detail);
            (void)snprintf(detail,sizeof(detail),"Última sincronización: %s",
                f->romm_catalog_available && f->romm_last_sync[0]?f->romm_last_sync:"Sin datos");
            text(f,VP_TYPE_DESCRIPTION,334,414,552,panel_muted,detail);
        }
    }
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
    bool connection=f->app->active_screen==VP_SCREEN_SETTINGS && f->settings_detail && f->settings_romm && f->settings_section==VP_SETTINGS_INTEGRATIONS;
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
        action=f->settings_field==VP_UI_WALLPAPER_COUNT?"Activar / Desactivar":"Aplicar fondo";
    if (f->app->active_screen==VP_SCREEN_SETTINGS && !f->settings_detail) action="Seleccionar";
    button(0,366,513); text(f,VP_TYPE_FOOTER,383,518,210,C(12,32,76,255),action);
    button(1,613,513); text(f,VP_TYPE_FOOTER,630,518,64,C(12,32,76,255),"Volver");
    if (roms || downloads) {
        button(2,713,513); text(f,VP_TYPE_FOOTER,730,518,76,C(12,32,76,255),downloads?"Quitar":"Favorito");
    }
    if (roms || connection) {
        button(3,815,513); text(f,VP_TYPE_FOOTER,832,518,102,C(12,32,76,255),connection?"Guardar":"Cambiar vista");
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
}
