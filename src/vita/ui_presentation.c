/* Presentation-only data: never normalize catalog slugs or persisted records. */
#include "ui_presentation.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned int vp_ui_sync_percent(const VpUiSyncStatus *status) {
    size_t processed;
    unsigned int percent;
    if (status->state == VP_SYNC_SUCCEEDED) return 100;
    if (!status->total) return 0;
    processed = status->processed < status->total ? status->processed : status->total;
    percent = (unsigned int)(100.0 * processed / status->total);
    /* Completion includes publication/cache writes, not only the last request. */
    return status->state == VP_SYNC_RUNNING && percent == 100 ? 99 : percent;
}

void vp_ui_sync_text(const VpUiSyncStatus *status, char *text, size_t capacity) {
    const char *title = status->operation == VP_SYNC_CATALOG ? "RomM: catálogo" :
        status->operation == VP_SYNC_METADATA ? "RomM: metadata" :
        status->operation == VP_SYNC_STEAMGRID ? "SteamGridDB: carátulas" : "Sincronización";
    const char *state = status->state == VP_SYNC_SUCCEEDED ? "completado" :
        status->state == VP_SYNC_FAILED ? "fallido" :
        status->stage == VP_SYNC_WAITING ? "en cola" :
        status->stage == VP_SYNC_CHECKING ? "conectando" :
        status->stage == VP_SYNC_SAVING ? "guardando" : "sincronizando";
    if (status->total || status->state == VP_SYNC_SUCCEEDED)
        (void)snprintf(text, capacity, "%s · %s · %u%% (%u/%u)", title, state,
            vp_ui_sync_percent(status), (unsigned)status->processed, (unsigned)status->total);
    else
        (void)snprintf(text, capacity, "%s · %s · %u procesados", title, state,
            (unsigned)status->processed);
}

static int same(const char *a, const char *b) {
    if (a == NULL) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
    return *a == *b;
}

const VpUiPlatform *vp_ui_platform(const char *slug) {
    static const struct { const char *slug; VpUiPlatform style; } styles[] = {
        {"nes", {"platform_nes", 0xec8c82}}, {"famicom", {"platform_famicom", 0xe6aa79}},
        {"snes", {"platform_snes", 0xab9def}}, {"gb", {"platform_gameboy", 0xa8c98d}},
        {"gameboy", {"platform_gameboy", 0xa8c98d}}, {"gbc", {"platform_gameboycolor", 0xa8c98d}},
        {"gba", {"platform_gba", 0xb499ee}}, {"nds", {"platform_nds", 0x96c9e2}},
        {"ds", {"platform_nds", 0x96c9e2}},
        {"dreamcast", {"platform_dreamcast", 0xe6aa79}}, {"dc", {"platform_dreamcast", 0xe6aa79}},
        {"3ds", {"platform_n3ds", 0x96c9e2}}, {"n3ds", {"platform_n3ds", 0x96c9e2}},
        {"nintendo-3ds", {"platform_n3ds", 0x96c9e2}},
        {"gamecube", {"platform_gamecube", 0xb499ee}}, {"ngc", {"platform_gamecube", 0xb499ee}},
        {"nintendo-gamecube", {"platform_gamecube", 0xb499ee}},
        {"ps2", {"platform_ps2", 0x93b6e7}}, {"playstation2", {"platform_ps2", 0x93b6e7}},
        {"playstation-2", {"platform_ps2", 0x93b6e7}},
        {"wii", {"platform_wii", 0x96c9e2}}, {"nintendo-wii", {"platform_wii", 0x96c9e2}},
        {"genesis", {"platform_genesis", 0x849de8}}, {"mega-drive", {"platform_genesis", 0x849de8}},
        {"megadrive", {"platform_genesis", 0x849de8}},
        {"mastersystem", {"platform_mastersystem", 0xe78d95}}, {"sms", {"platform_mastersystem", 0xe78d95}},
        {"gamegear", {"platform_gamegear", 0x8cc7e2}}, {"psx", {"platform_psx", 0x93b6e7}},
        {"playstation", {"platform_psx", 0x93b6e7}}, {"ps1", {"platform_psx", 0x93b6e7}},
        {"psp", {"platform_psp", 0x9eaee8}},
        {"ps vita", {"platform_vita", 0x86daca}}, {"vita", {"platform_vita", 0x86daca}},
        {"psvita", {"platform_vita", 0x86daca}}, {"arcade", {"platform_arcade", 0xe7b883}},
        {"neogeo", {"platform_neogeo", 0xe7b883}}, {"pce", {"platform_pce", 0xe9b499}},
        {"pc-engine", {"platform_pce", 0xe9b499}}, {"turbografx-16", {"platform_pce", 0xe9b499}},
        {"atari2600", {"platform_atari2600", 0xdea571}}, {"wonderswan", {"platform_wonderswan", 0x9dcbd8}},
        {"homebrew", {"platform_homebrew", 0x9cdab3}},
        {VP_FAVORITES_PLATFORM, {"ui_star", 0xffd166}}
    };
    static const VpUiPlatform fallback = {"platform_generic", 0x96d8cb};
    size_t i;
    for (i = 0; i < sizeof(styles) / sizeof(styles[0]); ++i)
        if (same(slug, styles[i].slug)) return &styles[i].style;
    return &fallback;
}

const char *vp_ui_platform_label(const char *slug) {
    static const struct { const char *asset,*label; } labels[]={
        {"platform_psx","PS1"},{"platform_ps2","PS2"},{"platform_psp","PSP"},{"platform_vita","Vita"},
        {"platform_nes","NES"},{"platform_famicom","FC"},{"platform_snes","SNES"},
        {"platform_gameboy","GB"},{"platform_gameboycolor","GBC"},{"platform_gba","GBA"},
        {"platform_nds","DS"},{"platform_n3ds","3DS"},{"platform_gamecube","GC"},{"platform_wii","Wii"},
        {"platform_dreamcast","DC"},{"platform_genesis","MD"},{"platform_mastersystem","MS"},
        {"platform_gamegear","GG"},{"platform_neogeo","Neo Geo"},{"platform_pce","PCE"},
        {"ui_star","Fav"}
    };
    const char *asset=vp_ui_platform(slug)->asset;
    size_t i;
    for (i=0;i<sizeof(labels)/sizeof(labels[0]);i++) if (!strcmp(asset,labels[i].asset)) return labels[i].label;
    return "ROM";
}

VpUiRect vp_ui_tile(size_t slot) {
    VpUiRect r = {48 + (int)(slot % 5) * 176, 158 + (int)(slot / 5) * 160, 160, 146};
    return r;
}

VpUiRect vp_ui_console_tile(size_t slot) {
    VpUiRect r = {44 + (int)slot * 148, 252, 136, 170};
    return r;
}

void vp_ui_console_motion_update(VpUiConsoleMotion *m, size_t selected, size_t total,
                                 unsigned long long now, int duration_ms) {
    size_t i;
    float progress, target;
    if (!total || total>VP_MAX_LIBRARY_GROUPS || selected>=total) {
        memset(m,0,sizeof(*m));
        return;
    }
    target=(float)vp_ui_console_start(selected,total);
    if (m->count!=total || duration_ms<=0) {
        memset(m,0,sizeof(*m));
        m->count=total; m->selected=selected; m->changed_at=now;
        m->scroll=m->from_scroll=target;
        m->lift[selected]=m->from_lift[selected]=1;
        return;
    }
    progress=vp_ui_ease(now>=m->changed_at?(float)(now-m->changed_at)/(duration_ms*1000.f):0);
    m->scroll=m->from_scroll+((float)vp_ui_console_start(m->selected,total)-m->from_scroll)*progress;
    for (i=0;i<total;i++)
        m->lift[i]=m->from_lift[i]+((i==m->selected?1.f:0.f)-m->from_lift[i])*progress;
    if (m->selected==selected) return;
    /* Wrapping skips the intervening pages instead of sweeping the whole catalog. */
    if ((selected==0 && m->selected==total-1) || (selected==total-1 && m->selected==0))
        m->scroll=target;
    m->from_scroll=m->scroll;
    memcpy(m->from_lift,m->lift,sizeof(m->lift));
    m->selected=selected; m->changed_at=now;
}

void vp_ui_console_motion_drag(VpUiConsoleMotion *m, float position, unsigned long long now) {
    if (!m->count) return;
    m->scroll=vp_ui_scroll_clamp(position,m->count,VP_UI_CONSOLE_PAGE);
    m->from_scroll=m->scroll;
    m->changed_at=now;
}

VpUiRect vp_ui_console_motion_tile(const VpUiConsoleMotion *m, size_t index) {
    VpUiRect r=vp_ui_console_tile(0);
    float lift=index<m->count?m->lift[index]:0;
    float scale=1+.10f*lift;
    r.x=(int)lroundf(r.x+((float)index-m->scroll)*148-r.w*(scale-1)/2);
    r.y=(int)lroundf(r.y-8*lift-r.h*(scale-1)/2);
    r.w=(int)lroundf(r.w*scale); r.h=(int)lroundf(r.h*scale);
    return r;
}

bool vp_ui_console_motion_visible(const VpUiConsoleMotion *m, size_t index) {
    return index<m->count && (float)index>=floorf(m->scroll) &&
        (float)index<ceilf(m->scroll)+VP_UI_CONSOLE_PAGE;
}

VpUiRect vp_ui_list_row(size_t slot) {
    VpUiRect r = {64, 170 + (int)slot * 54, 284, 48};
    return r;
}

float vp_ui_scroll_clamp(float position, size_t count, size_t page) {
    float last=count>page?(float)(count-page):0;
    return fmaxf(0,fminf(position,last));
}

size_t vp_ui_list_start(size_t selected, size_t count, size_t start) {
    if (selected<start) start=selected;
    if (selected>=start+VP_UI_LIST_PAGE) start=selected-VP_UI_LIST_PAGE+1;
    return (size_t)vp_ui_scroll_clamp((float)start,count,VP_UI_LIST_PAGE);
}

VpUiRect vp_ui_scrolled_list_row(size_t index, float scroll) {
    VpUiRect r=vp_ui_list_row(0);
    r.y+=(int)lroundf(((float)index-scroll)*54.f);
    return r;
}

size_t vp_ui_grid_rows(size_t count) {
    return (count+VP_UI_GRID_COLUMNS-1)/VP_UI_GRID_COLUMNS;
}

size_t vp_ui_grid_start(size_t selected, size_t count, size_t start) {
    size_t row=selected/VP_UI_GRID_COLUMNS;
    if (row<start) start=row;
    if (row>=start+VP_UI_GRID_ROWS) start=row-VP_UI_GRID_ROWS+1;
    return (size_t)vp_ui_scroll_clamp((float)start,vp_ui_grid_rows(count),VP_UI_GRID_ROWS);
}

VpUiRect vp_ui_scrolled_grid_tile(size_t index, float scroll) {
    VpUiRect r=vp_ui_tile(index%VP_UI_GRID_COLUMNS);
    r.y+=(int)lroundf(((float)(index/VP_UI_GRID_COLUMNS)-scroll)*160.f);
    return r;
}

VpUiRect vp_ui_carousel_tile(int offset) {
    VpUiRect r;
    if (!offset) { r = (VpUiRect){370, 150, 220, 246}; return r; }
    r = (VpUiRect){offset < 0 ? 228 + (offset+1)*132 : 604 + (offset-1)*132, 190, 122, 168};
    return r;
}

void vp_ui_carousel_motion_update(VpUiCarouselMotion *m, size_t selected, size_t total,
                                  unsigned long long now, int duration_ms) {
    float progress, target;
    if (!total || selected>=total) {
        memset(m,0,sizeof(*m));
        return;
    }
    target=(float)selected;
    if (m->count!=total || duration_ms<=0) {
        memset(m,0,sizeof(*m));
        m->count=total; m->selected=selected; m->changed_at=now;
        m->scroll=m->from_scroll=target;
        return;
    }
    if (m->selected==selected) {
        progress=now>=m->changed_at?(float)(now-m->changed_at)/(duration_ms*1000.f):0;
        if (progress>1) progress=1;
        m->scroll=m->from_scroll+(target-m->from_scroll)*vp_ui_ease(progress);
        m->pop=fmaxf(m->from_pop*(1-vp_ui_ease(progress)),sinf(3.14159265f*progress));
        return;
    }
    /* Catalog wraps: express the pose in the destination's virtual space.
       The nearest equivalent is one card away, so it slides instead of
       snapping or sweeping through every game. */
    if ((selected==0 && m->selected==total-1) || (selected==total-1 && m->selected==0))
        m->scroll+=(selected==0?-(float)total:(float)total);
    /* Retargeting keeps the current interpolated pose, so no frame jumps. */
    m->from_scroll=m->scroll;
    m->from_pop=m->pop;
    m->selected=selected; m->changed_at=now;
}

void vp_ui_carousel_motion_drag(VpUiCarouselMotion *m, float position, unsigned long long now) {
    if (!m->count) return;
    /* Keep a signed virtual position so crossing either seam preserves motion. */
    m->scroll=m->count>1?position:0;
    m->from_scroll=m->scroll;
    m->pop=0;
    m->from_pop=0;
    m->changed_at=now;
}

void vp_ui_carousel_motion_rebase(VpUiCarouselMotion *m) {
    float shift;
    if (!m->count) return;
    shift=roundf(((float)m->selected-m->scroll)/(float)m->count)*(float)m->count;
    m->scroll+=shift;
    m->from_scroll+=shift;
}

size_t vp_ui_wrap_index(int position, size_t count) {
    int index;
    if (!count) return 0;
    index=position%(int)count;
    return (size_t)(index<0?index+(int)count:index);
}

float vp_ui_carousel_motion_focus(const VpUiCarouselMotion *m, int position) {
    float distance=fabsf((float)position-m->scroll);
    return distance>=1?0:1-distance;
}

VpUiRect vp_ui_carousel_motion_tile(const VpUiCarouselMotion *m, int position) {
    float offset=(float)position-m->scroll;
    int low=(int)floorf(offset);
    float blend=offset-(float)low;
    VpUiRect from=vp_ui_carousel_tile(low),to=vp_ui_carousel_tile(low+1),r;
    float pop=m->pop*fmaxf(0,1-fabsf(offset)/2);
    float scale=1+.08f*pop;
    r.x=(int)lroundf(from.x+(to.x-from.x)*blend);
    r.y=(int)lroundf(from.y+(to.y-from.y)*blend);
    r.w=(int)lroundf(from.w+(to.w-from.w)*blend);
    r.h=(int)lroundf(from.h+(to.h-from.h)*blend);
    r.x=(int)lroundf(r.x-r.w*(scale-1)/2);
    r.y=(int)lroundf(r.y-6*pop-r.h*(scale-1)/2);
    r.w=(int)lroundf(r.w*scale); r.h=(int)lroundf(r.h*scale);
    return r;
}

VpUiRect vp_ui_settings_row(size_t slot) {
    VpUiRect r = {310, 108 + (int)slot * 60, 418, 50};
    return r;
}

VpUiRect vp_ui_settings_content_card(void) {
    VpUiRect r = {310, 148, 600, 270};
    return r;
}

static bool inside(VpUiRect r, int x, int y) {
    return x >= r.x && x < r.x+r.w && y >= r.y && y < r.y+r.h;
}

VpUiRect vp_ui_library_panel(float progress) {
    VpUiRect r = {960, 20, 440, 464};
    if (progress < 0.f) progress = 0.f;
    if (progress > 1.f) progress = 1.f;
    r.x = (int)lroundf(960.f - 464.f * progress);
    return r;
}

VpUiRect vp_ui_library_panel_row(size_t slot, float progress) {
    VpUiRect panel = vp_ui_library_panel(progress);
    VpUiRect r = {panel.x + 24, panel.y + 100 + (int)slot * 64, panel.w - 48, 52};
    return r;
}

VpUiRect vp_ui_library_panel_core_row(size_t slot, float progress) {
    VpUiRect panel = vp_ui_library_panel(progress);
    VpUiRect r = {panel.x + 18, panel.y + 70 + (int)slot * 34, panel.w - 36, 30};
    return r;
}

int vp_ui_touch_library_panel_row(float progress, int x, int y,
                                  VpLibraryModal modal, int rows) {
    int slot;
    if (rows < 0) rows = 0;
    for (slot = 0; slot < rows; ++slot) {
        VpUiRect r = modal == VP_LIBRARY_MODAL_CORES
                         ? vp_ui_library_panel_core_row((size_t)slot, progress)
                         : vp_ui_library_panel_row((size_t)slot, progress);
        if (inside(r, x, y)) return slot;
    }
    return -1;
}

int vp_ui_console_motion_touch(const VpUiConsoleMotion *m, int x, int y) {
    size_t i;
    if (!m->count || x<24 || x>=944) return -1;
    if (vp_ui_console_motion_visible(m,m->selected) &&
        inside(vp_ui_console_motion_tile(m,m->selected),x,y)) return (int)m->selected;
    /* Reverse paint order: the selected card is above all other cards. */
    for (i=m->count;i>0;i--)
        if (i-1!=m->selected && vp_ui_console_motion_visible(m,i-1) &&
            inside(vp_ui_console_motion_tile(m,i-1),x,y)) return (int)i-1;
    return -1;
}

const VpUiWallpaper *vp_ui_wallpaper(int index) {
    static const VpUiWallpaper wallpapers[VP_UI_WALLPAPER_COUNT] = {
        {"dark-blue", "Azul oscuro", "wallpaper_dark-blue"},
        {"violet", "Violeta", "wallpaper_violet"},
        {"blue-waves", "Ondas azules", "wallpaper_blue-waves"},
        {"light-blue", "Azul claro", "wallpaper_light-blue"}
    };
    return &wallpapers[index>=0 && index<VP_UI_WALLPAPER_COUNT?index:0];
}

const VpUiTextColor *vp_ui_text_color(int index) {
    static const VpUiTextColor colors[VP_UI_TEXT_COLOR_COUNT] = {
        {"white", "Blanco"},
        {"light", "Claro"},
        {"dark", "Oscuro"},
        {"black", "Negro"}
    };
    return &colors[index>=0 && index<VP_UI_TEXT_COLOR_COUNT?index:0];
}

bool vp_ui_preferences_set(VpUiPreferences *preferences, const char *key, const char *value) {
    int i;
    if (!strcmp(key,"ui_wallpaper")) {
        for (i=0;i<VP_UI_WALLPAPER_COUNT;i++) {
            if (!strcmp(value,vp_ui_wallpaper(i)->id)) { preferences->wallpaper=i; return true; }
        }
    } else if (!strcmp(key,"ui_text_color")) {
        for (i=0;i<VP_UI_TEXT_COLOR_COUNT;i++) {
            if (!strcmp(value,vp_ui_text_color(i)->id)) { preferences->text_color=i; return true; }
        }
    } else if (!strcmp(key,"ui_wallpaper_animated") && (!strcmp(value,"0") || !strcmp(value,"1"))) {
        preferences->animated=value[0]=='1';
        return true;
    }
    return false;
}

VpUiRect vp_ui_wallpaper_tile(size_t slot) {
    VpUiRect r={310+(int)(slot%2)*300,102+(int)(slot/2)*146,286,134};
    return r;
}

VpUiRect vp_ui_text_color_row(void) {
    VpUiRect r={310,396,586,38};
    return r;
}

VpUiRect vp_ui_wallpaper_motion_row(void) {
    VpUiRect r={310,440,586,38};
    return r;
}

int vp_ui_touch_appearance_slot(int x, int y) {
    size_t i;
    for (i=0;i<VP_UI_WALLPAPER_COUNT;i++) if (inside(vp_ui_wallpaper_tile(i),x,y)) return (int)i;
    if (inside(vp_ui_text_color_row(),x,y)) return VP_UI_APPEARANCE_TEXT_COLOR_SLOT;
    return inside(vp_ui_wallpaper_motion_row(),x,y)?VP_UI_APPEARANCE_MOTION_SLOT:-1;
}

#define VP_UI_BACKDROP_LOOP_US 16000000ULL
#define VP_UI_BACKDROP_SCALE_BASE 1.05f
#define VP_UI_BACKDROP_SCALE_SWING .02f
#define VP_UI_BACKDROP_X_SWING 11.f
#define VP_UI_BACKDROP_Y_SWING 7.f

VpUiBackdropTransform vp_ui_backdrop_transform(unsigned long long now, bool animated) {
    VpUiBackdropTransform transform={0,0,1};
    float phase;
    if (!animated) return transform;
    phase=6.2831853f*(float)(now%VP_UI_BACKDROP_LOOP_US)/(float)VP_UI_BACKDROP_LOOP_US;
    transform.scale=VP_UI_BACKDROP_SCALE_BASE+VP_UI_BACKDROP_SCALE_SWING*sinf(phase);
    transform.x=960.f*(1-transform.scale)/2+VP_UI_BACKDROP_X_SWING*sinf(phase+1.0471976f);
    transform.y=544.f*(1-transform.scale)/2+VP_UI_BACKDROP_Y_SWING*cosf(phase+2.0943951f);
    return transform;
}

size_t vp_ui_console_start(size_t selected, size_t total) {
    size_t center = VP_UI_CONSOLE_PAGE/2;
    size_t start = selected > center ? selected-center : 0;
    size_t last = total > VP_UI_CONSOLE_PAGE ? total-VP_UI_CONSOLE_PAGE : 0;
    return start > last ? last : start;
}

size_t vp_ui_page_size(int view) {
    return view == 0 ? VP_UI_GRID_PAGE : (view == 1 ? VP_UI_LIST_PAGE : VP_UI_CAROUSEL_PAGE);
}

int vp_ui_touch_console_slot(int x, int y) {
    size_t slot;
    for (slot=0; slot<VP_UI_CONSOLE_PAGE; slot++)
        if (inside(vp_ui_console_tile(slot),x,y)) return (int)slot;
    return -1;
}

int vp_ui_touch_carousel_offset(int x, int y) {
    int offset;
    if (inside(vp_ui_carousel_tile(0),x,y)) return 0;
    for (offset=-3; offset<=3; offset++)
        if (offset && inside(vp_ui_carousel_tile(offset),x,y)) return offset;
    return 99;
}

int vp_ui_touch_settings_slot(int x, int y) {
    size_t slot;
    for (slot=0; slot<VP_SETTINGS_COUNT; slot++) if (inside(vp_ui_settings_row(slot),x,y)) return (int)slot;
    return -1;
}

int vp_ui_touch_tile_slot(int x, int y) {
    size_t slot;
    for (slot = 0; slot < VP_UI_GRID_PAGE; slot++) {
        VpUiRect r = vp_ui_tile(slot);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return (int)slot;
    }
    return -1;
}

int vp_ui_touch_list_slot(int x, int y) {
    int slot;
    for (slot = 0; slot < VP_UI_LIST_PAGE; slot++)
        if (inside(vp_ui_list_row((size_t)slot),x,y)) return slot;
    return -1;
}

int vp_ui_touch_dock_screen(int x, int y) {
    if (y < 14 || y >= 56 || x < 64 || x >= 454) return -1;
    return x<216?0:(x<328?1:2);
}

bool vp_ui_touch_view_toggle(int x, int y) {
    return x >= 804 && x < 936 && y >= 492 && y < 534;
}

bool vp_ui_touch_back(int x, int y) {
    return x >= 600 && x < 696 && y >= 492 && y < 534;
}

float vp_ui_ease(float p) {
    if (p < 0) return 0;
    if (p > 1) return 1;
    p = 1 - p;
    return 1 - p * p * p;
}

void vp_ui_appearance_load(VpUiAppearance *appearance, const char *ini) {
    const char *line=ini;
    while (*line) {
        const char *end=strchr(line,'\n');
        const char *equals;
        char *tail;
        long value;
        int *target=NULL, min=0, max=0;
        size_t length=end?(size_t)(end-line):strlen(line);
        equals=memchr(line,'=',length);
        if (equals) {
            size_t key=(size_t)(equals-line);
            if (key==12 && strncmp(line,"ui_motion_ms",key)==0) { target=&appearance->motion_ms; max=600; }
            else if (key==9 && strncmp(line,"ui_radius",key)==0) { target=&appearance->radius; min=6; max=20; }
            else if (key==18 && strncmp(line,"ui_wallpaper_alpha",key)==0) { target=&appearance->wallpaper_alpha; max=255; }
            errno=0; value=strtol(equals+1,&tail,10);
            if (target && errno==0 && tail!=equals+1 && (*tail=='\0' || *tail=='\r' || *tail=='\n') && value>=min && value<=max)
                *target=(int)value;
        }
        if (!end) break;
        line=end+1;
    }
}
