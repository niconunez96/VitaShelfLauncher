#ifndef VP_UI_PRESENTATION_H
#define VP_UI_PRESENTATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { int x, y, w, h; } VpUiRect;
typedef struct { const char *asset; uint32_t rgb; } VpUiPlatform;
typedef struct { int motion_ms, radius, wallpaper_alpha; } VpUiAppearance;
typedef enum {
    VP_SETTINGS_INTEGRATIONS,
    VP_SETTINGS_DOWNLOADS,
    VP_SETTINGS_ABOUT,
    VP_SETTINGS_COUNT
} VpSettingsSection;
#define VP_UI_GRID_PAGE 10
#define VP_UI_LIST_PAGE 5
#define VP_UI_CONSOLE_PAGE 6
#define VP_UI_CAROUSEL_PAGE 7
const VpUiPlatform *vp_ui_platform(const char *slug);
const char *vp_ui_platform_label(const char *slug);
VpUiRect vp_ui_tile(size_t slot);
VpUiRect vp_ui_console_tile(size_t slot);
VpUiRect vp_ui_list_row(size_t slot);
VpUiRect vp_ui_carousel_tile(int offset);
VpUiRect vp_ui_settings_row(size_t slot);
VpUiRect vp_ui_settings_content_card(void);
int vp_ui_touch_console_slot(int x, int y);
int vp_ui_touch_carousel_offset(int x, int y);
int vp_ui_touch_settings_slot(int x, int y);
size_t vp_ui_console_start(size_t selected, size_t total);
size_t vp_ui_page_size(int view);
int vp_ui_touch_tile_slot(int x, int y);
int vp_ui_touch_list_slot(int x, int y);
int vp_ui_touch_dock_screen(int x, int y);
bool vp_ui_touch_view_toggle(int x, int y);
bool vp_ui_touch_back(int x, int y);
float vp_ui_ease(float progress);
void vp_ui_appearance_load(VpUiAppearance *appearance, const char *ini);
#endif
