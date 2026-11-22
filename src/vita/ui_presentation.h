#ifndef VP_UI_PRESENTATION_H
#define VP_UI_PRESENTATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vitashelflauncher.h"

typedef struct { int x, y, w, h; } VpUiRect;
typedef struct { const char *asset; uint32_t rgb; } VpUiPlatform;
typedef struct { int motion_ms, radius, wallpaper_alpha; } VpUiAppearance;
typedef struct { int wallpaper; int text_color; bool animated; } VpUiPreferences;
typedef struct { const char *id, *label, *thumbnail; } VpUiWallpaper;
typedef struct { const char *id, *label; } VpUiTextColor;
typedef struct { float x, y, scale; } VpUiBackdropTransform;
#define VP_UI_WALLPAPER_COUNT 4
#define VP_UI_TEXT_COLOR_COUNT 4
#define VP_UI_APPEARANCE_TEXT_COLOR_SLOT VP_UI_WALLPAPER_COUNT
#define VP_UI_APPEARANCE_MOTION_SLOT (VP_UI_WALLPAPER_COUNT + 1)
#define VP_UI_APPEARANCE_SLOT_COUNT (VP_UI_WALLPAPER_COUNT + 2)
#define VP_UI_TEXT_COLOR_WHITE 0
#define VP_UI_TEXT_COLOR_LIGHT 1
#define VP_UI_TEXT_COLOR_DARK 2
#define VP_UI_TEXT_COLOR_BLACK 3
#define VP_UI_PREFERENCES_DEFAULT {0, VP_UI_TEXT_COLOR_WHITE, true}
typedef enum {
    VP_SETTINGS_INTEGRATIONS,
    VP_SETTINGS_DOWNLOADS,
    VP_SETTINGS_APPEARANCE,
    VP_SETTINGS_ABOUT,
    VP_SETTINGS_COUNT
} VpSettingsSection;
#define VP_UI_GRID_PAGE 10
#define VP_UI_LIST_PAGE 5
#define VP_UI_CONSOLE_PAGE 6
#define VP_UI_CAROUSEL_PAGE 7
typedef struct {
    size_t selected, count;
    unsigned long long changed_at;
    float scroll, from_scroll;
    float lift[VP_MAX_PLATFORMS], from_lift[VP_MAX_PLATFORMS];
} VpUiConsoleMotion;
void vp_ui_console_motion_update(VpUiConsoleMotion *motion, size_t selected, size_t total,
                                 unsigned long long now, int duration_ms);
VpUiRect vp_ui_console_motion_tile(const VpUiConsoleMotion *motion, size_t index);
bool vp_ui_console_motion_visible(const VpUiConsoleMotion *motion, size_t index);
int vp_ui_console_motion_touch(const VpUiConsoleMotion *motion, int x, int y);
const VpUiPlatform *vp_ui_platform(const char *slug);
const char *vp_ui_platform_label(const char *slug);
VpUiRect vp_ui_tile(size_t slot);
VpUiRect vp_ui_console_tile(size_t slot);
VpUiRect vp_ui_list_row(size_t slot);
VpUiRect vp_ui_carousel_tile(int offset);
VpUiRect vp_ui_settings_row(size_t slot);
VpUiRect vp_ui_settings_content_card(void);
VpUiRect vp_ui_wallpaper_tile(size_t slot);
VpUiRect vp_ui_wallpaper_motion_row(void);
VpUiRect vp_ui_text_color_row(void);
int vp_ui_touch_appearance_slot(int x, int y);
const VpUiWallpaper *vp_ui_wallpaper(int index);
const VpUiTextColor *vp_ui_text_color(int index);
bool vp_ui_preferences_set(VpUiPreferences *preferences, const char *key, const char *value);
VpUiBackdropTransform vp_ui_backdrop_transform(unsigned long long now, bool animated);
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
