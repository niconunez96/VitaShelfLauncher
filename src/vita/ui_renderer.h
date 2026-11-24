#ifndef VP_UI_RENDERER_H
#define VP_UI_RENDERER_H
#include <vita2d.h>
#include "vitashelflauncher.h"
#include "ui_text.h"
#include "ui_presentation.h"
#include "integrations.h"

/* Read-only presentation snapshot. All catalog actions remain in main/core. */
typedef struct {
    const VpApp *app;
    VpUiFonts *font;
    const VpPlatformGroup *groups;
    size_t group_count;
    const size_t *visible;
    size_t visible_count;
    vita2d_texture *covers[VP_UI_GRID_WINDOW];
    vita2d_texture *background;
    const VpDownloadQueue *downloads;
    size_t download_selection;
    VpLibraryModal library_modal;
    size_t library_modal_row;
    float library_modal_progress;
    int settings_section, settings_field;
    bool settings_detail, settings_integration_open;
    int settings_integration;
    bool steamgrid_configured;
    const char *image_service_host;
    VpConnectionState steamgrid_state, image_service_state;
    VpUiSyncStatus sync;
    VpUiPreferences preferences;
    const char *app_version;
    bool romm_catalog_available;
    char romm_last_sync[32];
    const char *romm_url, *pair_code, *active_theme;
    VpRommConnectionState romm_state;
    bool busy;
    unsigned int notice_id;
    char date[32], time[16];
    int battery_percent;
    bool battery_charging;
    unsigned long long storage_free, storage_total;
    bool touch_scrolling;
    float touch_scroll, list_scroll, grid_scroll;
    int carousel_base;
    vita2d_texture *selected_cover;
} VpUiFrame;

void vp_ui_prepare(const VpUiFrame *frame);
void vp_ui_render(const VpUiFrame *frame);
void vp_ui_load_theme(const char *ini);
int vp_ui_touch_platform(int x, int y);
float vp_ui_scroll_origin(VpLibraryLevel level);
void vp_ui_shutdown(void);
#endif
