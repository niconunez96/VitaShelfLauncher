#ifndef VP_UI_RENDERER_H
#define VP_UI_RENDERER_H
#include <vita2d.h>
#include "vitashelflauncher.h"
#include "ui_text.h"
#include "ui_presentation.h"

/* Read-only presentation snapshot. All catalog actions remain in main/core. */
typedef struct {
    const VpApp *app;
    VpUiFonts *font;
    const VpPlatformGroup *groups;
    size_t group_count;
    const size_t *visible;
    size_t visible_count;
    vita2d_texture *covers[VP_UI_GRID_PAGE];
    vita2d_texture *background;
    const VpDownloadQueue *downloads;
    size_t download_selection;
    int settings_section, settings_field;
    bool settings_detail, settings_romm;
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
} VpUiFrame;

void vp_ui_prepare(const VpUiFrame *frame);
void vp_ui_render(const VpUiFrame *frame);
void vp_ui_load_theme(const char *ini);
void vp_ui_shutdown(void);
#endif
