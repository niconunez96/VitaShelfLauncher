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
typedef enum {
    VP_LIBRARY_MODAL_NONE,
    VP_LIBRARY_MODAL_ACTIONS,
    VP_LIBRARY_MODAL_FILTERS,
    VP_LIBRARY_MODAL_CORES
} VpLibraryModal;
#define VP_LIBRARY_ACTIONS_ROWS 3
#define VP_LIBRARY_ACTIONS_ARCADE_ROWS 4
#define VP_LIBRARY_FILTERS_ROWS 3
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
typedef enum {
    VP_INTEGRATION_ROMM,
    VP_INTEGRATION_STEAMGRID,
    VP_INTEGRATION_IMAGES,
    VP_INTEGRATION_COUNT
} VpIntegration;

typedef enum { VP_SYNC_IDLE, VP_SYNC_RUNNING, VP_SYNC_SUCCEEDED, VP_SYNC_FAILED } VpSyncState;
typedef enum { VP_SYNC_NONE, VP_SYNC_CATALOG, VP_SYNC_METADATA, VP_SYNC_STEAMGRID } VpSyncOperation;
typedef enum { VP_SYNC_CHECKING, VP_SYNC_PROCESSING, VP_SYNC_SAVING, VP_SYNC_WAITING } VpSyncStage;
typedef struct {
    VpSyncState state;
    VpSyncOperation operation;
    VpSyncStage stage;
    size_t processed, total, ready, missing, failed;
    char detail[VP_TEXT];
} VpUiSyncStatus;

unsigned int vp_ui_sync_percent(const VpUiSyncStatus *status);
void vp_ui_sync_text(const VpUiSyncStatus *status, char *text, size_t capacity);
#define VP_UI_GRID_PAGE 10
#define VP_UI_GRID_COLUMNS 5
#define VP_UI_GRID_ROWS 2
#define VP_UI_GRID_WINDOW (VP_UI_GRID_PAGE + VP_UI_GRID_COLUMNS)
#define VP_UI_LIST_PAGE 5
#define VP_UI_CONSOLE_PAGE 6
#define VP_UI_CAROUSEL_PAGE 7
#define VP_UI_CAROUSEL_REACH 4
typedef struct {
    size_t selected, count;
    unsigned long long changed_at;
    float scroll, from_scroll;
    float lift[VP_MAX_LIBRARY_GROUPS], from_lift[VP_MAX_LIBRARY_GROUPS];
} VpUiConsoleMotion;
typedef struct {
    size_t selected, count;
    unsigned long long changed_at;
    float scroll, from_scroll, pop, from_pop;
} VpUiCarouselMotion;
void vp_ui_console_motion_update(VpUiConsoleMotion *motion, size_t selected, size_t total,
                                  unsigned long long now, int duration_ms);
void vp_ui_console_motion_drag(VpUiConsoleMotion *motion, float position, unsigned long long now);
VpUiRect vp_ui_console_motion_tile(const VpUiConsoleMotion *motion, size_t index);
bool vp_ui_console_motion_visible(const VpUiConsoleMotion *motion, size_t index);
int vp_ui_console_motion_touch(const VpUiConsoleMotion *motion, int x, int y);
void vp_ui_carousel_motion_update(VpUiCarouselMotion *motion, size_t selected, size_t total,
                                   unsigned long long now, int duration_ms);
void vp_ui_carousel_motion_drag(VpUiCarouselMotion *motion, float position, unsigned long long now);
void vp_ui_carousel_motion_rebase(VpUiCarouselMotion *motion);
size_t vp_ui_wrap_index(int position, size_t count);
float vp_ui_scroll_clamp(float position, size_t count, size_t page);
size_t vp_ui_list_start(size_t selected, size_t count, size_t start);
size_t vp_ui_grid_rows(size_t count);
size_t vp_ui_grid_start(size_t selected, size_t count, size_t start);
VpUiRect vp_ui_scrolled_grid_tile(size_t index, float scroll);
VpUiRect vp_ui_scrolled_list_row(size_t index, float scroll);
VpUiRect vp_ui_carousel_motion_tile(const VpUiCarouselMotion *motion, int position);
float vp_ui_carousel_motion_focus(const VpUiCarouselMotion *motion, int position);
const VpUiPlatform *vp_ui_platform(const char *slug);
const char *vp_ui_platform_label(const char *slug);
VpUiRect vp_ui_tile(size_t slot);
VpUiRect vp_ui_console_tile(size_t slot);
VpUiRect vp_ui_list_row(size_t slot);
VpUiRect vp_ui_carousel_tile(int offset);
VpUiRect vp_ui_settings_row(size_t slot);
VpUiRect vp_ui_settings_content_card(void);
VpUiRect vp_ui_library_panel(float progress);
VpUiRect vp_ui_library_panel_row(size_t slot, float progress);
VpUiRect vp_ui_library_panel_core_row(size_t slot, float progress);
int vp_ui_touch_library_panel_row(float progress, int x, int y,
                                  VpLibraryModal modal, int rows);
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
