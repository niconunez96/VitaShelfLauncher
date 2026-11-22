/* Presentation-only data: never normalize catalog slugs or persisted records. */
#include "ui_presentation.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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
        {"homebrew", {"platform_homebrew", 0x9cdab3}}
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
        {"platform_gamegear","GG"},{"platform_neogeo","Neo Geo"},{"platform_pce","PCE"}
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

VpUiRect vp_ui_list_row(size_t slot) {
    VpUiRect r = {64, 170 + (int)slot * 54, 284, 48};
    return r;
}

VpUiRect vp_ui_carousel_tile(int offset) {
    VpUiRect r;
    if (!offset) { r = (VpUiRect){370, 150, 220, 246}; return r; }
    r = (VpUiRect){offset < 0 ? 228 + (offset+1)*132 : 604 + (offset-1)*132, 190, 122, 168};
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

const VpUiWallpaper *vp_ui_wallpaper(int index) {
    static const VpUiWallpaper wallpapers[VP_UI_WALLPAPER_COUNT] = {
        {"dark-blue", "Azul oscuro", "wallpaper_dark-blue"},
        {"violet", "Violeta", "wallpaper_violet"},
        {"blue-waves", "Ondas azules", "wallpaper_blue-waves"},
        {"light-blue", "Azul claro", "wallpaper_light-blue"}
    };
    return &wallpapers[index>=0 && index<VP_UI_WALLPAPER_COUNT?index:0];
}

bool vp_ui_preferences_set(VpUiPreferences *preferences, const char *key, const char *value) {
    int i;
    if (!strcmp(key,"ui_wallpaper")) {
        for (i=0;i<VP_UI_WALLPAPER_COUNT;i++) {
            if (!strcmp(value,vp_ui_wallpaper(i)->id)) { preferences->wallpaper=i; return true; }
        }
    } else if (!strcmp(key,"ui_wallpaper_animated") && (!strcmp(value,"0") || !strcmp(value,"1"))) {
        preferences->animated=value[0]=='1';
        return true;
    }
    return false;
}

VpUiRect vp_ui_wallpaper_tile(size_t slot) {
    VpUiRect r={310+(int)(slot%2)*300,104+(int)(slot/2)*174,286,160};
    return r;
}

VpUiRect vp_ui_wallpaper_motion_row(void) {
    VpUiRect r={310,450,586,38};
    return r;
}

int vp_ui_touch_appearance_slot(int x, int y) {
    size_t i;
    for (i=0;i<VP_UI_WALLPAPER_COUNT;i++) if (inside(vp_ui_wallpaper_tile(i),x,y)) return (int)i;
    return inside(vp_ui_wallpaper_motion_row(),x,y)?VP_UI_WALLPAPER_COUNT:-1;
}

VpUiBackdropTransform vp_ui_backdrop_transform(unsigned long long now, bool animated) {
    VpUiBackdropTransform transform={0,0,1};
    float seconds=(float)(now%180000000ULL)/1000000.f;
    if (!animated) return transform;
    transform.scale=1.025f+.005f*sinf(seconds*6.2831853f/90.f);
    transform.x=960.f*(1-transform.scale)/2+4*sinf(seconds*6.2831853f/60.f);
    transform.y=544.f*(1-transform.scale)/2+2*cosf(seconds*6.2831853f/90.f);
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
