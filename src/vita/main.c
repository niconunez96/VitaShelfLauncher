#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/devctl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <vita2d.h>

#include <curl/curl.h>

#include <pthread.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "vita_backend.h"
#include "vitashelflauncher.h"
#include "ui_audio.h"
#include "ui_presentation.h"
#include "ui_renderer.h"
#include "ui_backdrop.h"
#include "catalog_cache.h"
#include "library_preferences.h"
#include "vita_thread_priority.h"
#include "local_library.h"
#include "integrations.h"

/* vitasdk 2026.08 omits these from SceAppMgrSystemEventType. The ABI values are
 * stable and are declared in newer vita-headers releases. */
#ifndef SCE_APPMGR_SYSTEMEVENT_ON_DEACTIVATE
#define SCE_APPMGR_SYSTEMEVENT_ON_DEACTIVATE ((int)0x10000002)
#endif
#ifndef SCE_APPMGR_SYSTEMEVENT_REQUEST_QUIT
#define SCE_APPMGR_SYSTEMEVENT_REQUEST_QUIT ((int)0x20000001)
#endif

#define RGBA(r, g, b, a) ((unsigned int)((r) | ((g) << 8) | ((b) << 16) | ((a) << 24)))
#define VP_CONFIG_PATH "ux0:data/vitashelflauncher/config.ini"
#define VP_CATALOG_CACHE_PATH "ux0:data/vitashelflauncher/catalog.cache"
#define VP_CATALOG_CACHE_PARTIAL "ux0:data/vitashelflauncher/catalog.cache.part"
#define VP_CATALOG_CACHE_VERSION 3U
#define VP_LIBRARY_PREFERENCES_PATH "ux0:data/vitashelflauncher/library.ini"
#define VP_LIBRARY_PREFERENCES_PARTIAL "ux0:data/vitashelflauncher/library.ini.part"
#define VP_LIBRARY_PREFERENCES_LINE 1024
#define VP_ARTWORK_CACHE "ux0:data/vitashelflauncher/cache/covers"
#define VP_DOWNLOAD_ROOT "ux0:data/vitashelflauncher/downloads"
#define VP_PSP_ISO_ROOT "ux0:pspemu/ISO"
#define VP_DSVITA_ROOT "ux0:data/dsvita"
#define VP_TOUCH_WIDTH 1920
#define VP_TOUCH_HEIGHT 1088
#define VP_TOUCH_TAP_SLOP 32
#define VP_TOUCH_SWIPE_SLOP 32
static VpRommConfig romm_config;
static VpHomebrewConfig homebrew_config;
static VpLaunchConfig launch_config;
static VpApp app;
static char active_theme[VP_TEXT] = "default";
static char romm_pair_code[VP_TEXT];
static VpRommConnection romm_connection;
static bool romm_check_pending;
static int settings_section;
static VpUiPreferences ui_preferences=VP_UI_PREFERENCES_DEFAULT;
static int settings_field;
static size_t download_selection;
static bool settings_detail_active;
static bool settings_integration_active;
static int settings_integration;
static char steamgrid_api_key[VP_TEXT];
static VpConnectionState steamgrid_state, image_service_state;
static unsigned long integration_revision;
static bool local_scan_pending=true;
static char catalog_server[VP_URL];
static char catalog_synced_at[32];
static bool settings_ime_active;
static SceWChar16 settings_ime_buffer[VP_URL];
static unsigned long long installation_armed_at;
typedef enum { VP_TOUCH_AXIS_NONE, VP_TOUCH_AXIS_HORIZONTAL, VP_TOUCH_AXIS_VERTICAL } VpTouchAxis;
typedef struct {
    bool active, dragging, settling, moved, cancelled;
    int start_x, start_y, last_x, last_y;
    VpTouchAxis axis;
    VpLibraryLevel level;
    VpView view;
    size_t count, selected;
    char platform[VP_TEXT];
    float origin, position, from_position, target;
    unsigned long long released_at;
} VpTouchGesture;
static VpTouchGesture touch_gesture;
static size_t list_start;
static size_t list_selected=VP_MAX_ITEMS;
static size_t grid_start;
static size_t grid_selected=VP_MAX_ITEMS;
static VpLibraryModal library_modal;
static size_t library_modal_row;
static unsigned long long library_modal_opened_at;
static float library_modal_progress(void);

static int library_modal_rows(void) {
    if (library_modal == VP_LIBRARY_MODAL_NONE) return 0;
    if (library_modal == VP_LIBRARY_MODAL_ACTIONS) {
        return app.selected_item < app.catalog.count &&
               vp_platform_is_arcade(app.catalog.items[app.selected_item].platform)
                   ? VP_LIBRARY_ACTIONS_ARCADE_ROWS : VP_LIBRARY_ACTIONS_ROWS;
    }
    if (library_modal == VP_LIBRARY_MODAL_CORES) return (int)vp_arcade_core_count();
    return VP_LIBRARY_FILTERS_ROWS;
}

typedef enum {
    VP_JOB_NONE,
    VP_JOB_SYNC_PROVIDERS,
    VP_JOB_PAIR_AND_SYNC,
    VP_JOB_VALIDATE_ROMM,
    VP_JOB_DOWNLOAD_ROM,
    VP_JOB_INSTALL_HOMEBREW,
    VP_JOB_SYNC_COVERS,
    VP_JOB_SCAN_LOCAL,
    VP_JOB_CHECK_STEAMGRID,
    VP_JOB_CHECK_IMAGES,
    VP_JOB_SYNC_STEAMGRID
} VpJobKind;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    bool active;
    bool completed;
    bool succeeded;
    VpJobKind kind;
    VpRommConfig romm;
    VpHomebrewConfig homebrew;
    VpItem item;
    VpCatalog catalog;
    char destination[VP_PATH];
    char partial[VP_PATH + 8];
    char resolved_target[VP_PATH];
    char pair_code[VP_TEXT];
    char error[VP_TEXT];
    unsigned long long downloaded;
    unsigned long long total;
    size_t covers_completed;
    size_t covers_total;
    bool cancel_requested;
    bool pause_requested;
    unsigned long romm_revision;
    VpRommConnectionState romm_state;
    bool token_exchanged;
    char steamgrid_key[VP_TEXT];
    unsigned long integration_revision;
    VpConnectionState connection_state;
    size_t omitted;
    size_t covers_missing;
    VpUiSyncStatus sync;
} VpBackgroundJob;

/* The core fetch buffer and backend diagnostics are deliberately single-owner. The background
 * worker keeps network work off the UI thread; cover PNG decoding has its own worker. */
static VpBackgroundJob background_job = {
    .mutex = PTHREAD_MUTEX_INITIALIZER
};
/* UI-owned intent: the worker receives an immutable copy when its slot is free. */
static struct {
    bool active;
    unsigned long revision;
    char code[VP_TEXT];
} romm_pairing;
/* UI-owned intent for explicit sync/check requests that arrive while the singleton worker
 * is busy. Dispatched before automatic scans so user input is never silently dropped. */
static struct {
    bool active;
    VpJobKind kind;
} pending_user_job;
static struct {
    VpUiSyncStatus status;
    unsigned long romm_revision, integration_revision;
} sync_results[VP_INTEGRATION_COUNT];

static const char *background_job_label(VpJobKind kind) {
    switch (kind) {
        case VP_JOB_SCAN_LOCAL: return "búsqueda de juegos instalados";
        case VP_JOB_VALIDATE_ROMM: return "comprobación de RomM";
        case VP_JOB_SYNC_PROVIDERS: return "sincronización del catálogo";
        case VP_JOB_PAIR_AND_SYNC: return "canje del código de RomM";
        case VP_JOB_SYNC_COVERS: return "sincronización de metadata";
        case VP_JOB_SYNC_STEAMGRID: return "sincronización de SteamGridDB";
        case VP_JOB_CHECK_STEAMGRID: return "comprobación de SteamGridDB";
        case VP_JOB_CHECK_IMAGES: return "comprobación de Image Service";
        case VP_JOB_DOWNLOAD_ROM: return "descarga de un juego";
        case VP_JOB_INSTALL_HOMEBREW: return "instalación de homebrew";
        default: return "finalización de la tarea anterior";
    }
}

static const char *background_job_active_label(void) {
    const char *label;
    pthread_mutex_lock(&background_job.mutex);
    label = background_job_label(background_job.active ? background_job.kind : VP_JOB_NONE);
    pthread_mutex_unlock(&background_job.mutex);
    return label;
}

/* One visible title per service, reused by toasts and queued-job notices. */
static const char *sync_job_title(VpJobKind kind) {
    switch (kind) {
        case VP_JOB_SYNC_PROVIDERS: return "RomM: catálogo";
        case VP_JOB_VALIDATE_ROMM: return "RomM: conexión";
        case VP_JOB_SYNC_COVERS: return "RomM: metadata";
        case VP_JOB_CHECK_STEAMGRID: return "SteamGridDB: conexión";
        case VP_JOB_SYNC_STEAMGRID: return "SteamGridDB: carátulas";
        case VP_JOB_CHECK_IMAGES: return "Image Service: conexión";
        default: return background_job_label(kind);
    }
}

static void sync_progress(size_t processed, size_t total, void *context) {
    VpBackgroundJob *job = context;
    pthread_mutex_lock(&job->mutex);
    job->sync.stage = VP_SYNC_PROCESSING;
    job->sync.processed = processed;
    job->sync.total = total;
    pthread_mutex_unlock(&job->mutex);
}

static void sync_item_finished(VpBackgroundJob *job, bool ready, bool missing, bool failed) {
    pthread_mutex_lock(&job->mutex);
    job->sync.stage = VP_SYNC_PROCESSING;
    job->sync.processed++;
    job->sync.ready += ready;
    job->sync.missing += missing;
    job->sync.failed += failed;
    pthread_mutex_unlock(&job->mutex);
}

/* Called on the UI thread after joining. Retain results independently of the next job. */
static void sync_result_publish(VpBackgroundJob *job) {
    int integration = job->sync.operation == VP_SYNC_STEAMGRID ? VP_INTEGRATION_STEAMGRID : VP_INTEGRATION_ROMM;
    VpUiSyncStatus status = job->sync;
    if (status.state == VP_SYNC_IDLE) return;
    if ((integration == VP_INTEGRATION_ROMM && job->romm_revision != romm_connection.revision) ||
        (status.operation != VP_SYNC_CATALOG && job->integration_revision != integration_revision)) return;
    status.state = job->succeeded ? VP_SYNC_SUCCEEDED : VP_SYNC_FAILED;
    if (job->succeeded && !status.total) status.total = status.processed;
    if (status.operation == VP_SYNC_CATALOG)
        (void)snprintf(status.detail, sizeof(status.detail), "RomM: catálogo — %s", job->succeeded ?
            "completado." : "fallido. Revisa RomM y la red.");
    else if (status.operation == VP_SYNC_STEAMGRID)
        (void)snprintf(status.detail, sizeof(status.detail),
            "SteamGridDB: carátulas — %s · %u listas · %u sin coincidencia · %u fallidas%s",
            job->succeeded ? "completado" : "fallido", (unsigned)status.ready,
            (unsigned)status.missing, (unsigned)status.failed,
            job->succeeded ? "" : ". Revisa la conexión y Image Service.");
    else
        (void)snprintf(status.detail, sizeof(status.detail), "RomM: metadata — %s · %u/%u carátulas%s",
            job->succeeded ? "completado" : "fallido", (unsigned)status.ready,
            (unsigned)status.total, job->succeeded ? "" : ". Revisa la conexión.");
    if (!job->succeeded && status.operation == VP_SYNC_STEAMGRID) {
        const char *reason = job->connection_state == VP_CONNECTION_INVALID_KEY ?
            "API key inválida o sin permisos; revisa la clave." :
            job->connection_state == VP_CONNECTION_RATE_LIMITED ?
            "Límite de solicitudes alcanzado; reintenta más tarde." :
            job->connection_state == VP_CONNECTION_UNREACHABLE ?
            "No se pudo conectar; revisa la red y reintenta." : NULL;
        if (reason) (void)snprintf(status.detail, sizeof(status.detail),
            "SteamGridDB: carátulas — fallido. %s", reason);
    }
    if (!job->succeeded && status.operation == VP_SYNC_CATALOG &&
        (job->romm_state == VP_ROMM_INVALID_TOKEN || job->romm_state == VP_ROMM_FORBIDDEN))
        (void)snprintf(status.detail, sizeof(status.detail), "RomM: catálogo — fallido. %s",
            job->romm_state == VP_ROMM_INVALID_TOKEN ?
                "El token ya no funciona; ingresa un nuevo pair code en RomM." :
                "El token no tiene permisos de lectura; revisa RomM.");
    sync_results[integration].status = status;
    sync_results[integration].romm_revision = job->romm_revision;
    sync_results[integration].integration_revision = job->integration_revision;
}

/* The caller holds the job mutex; only copy status for the current configuration. */
static VpUiSyncStatus sync_status_snapshot(int integration) {
    VpUiSyncStatus status = {0};
    bool romm = integration == VP_INTEGRATION_ROMM;
    int active_integration = background_job.sync.operation == VP_SYNC_STEAMGRID ?
        VP_INTEGRATION_STEAMGRID : VP_INTEGRATION_ROMM;
    if (integration != VP_INTEGRATION_ROMM && integration != VP_INTEGRATION_STEAMGRID) return status;
    if (romm && romm_pairing.active && romm_pairing.revision == romm_connection.revision) {
        status.state = VP_SYNC_RUNNING;
        status.operation = VP_SYNC_CATALOG;
        status.stage = VP_SYNC_WAITING;
        (void)snprintf(status.detail, sizeof(status.detail), "En espera de: %s. El código se canjeará automáticamente.",
            background_job_label(background_job.kind));
        return status;
    }
    if ((!romm || sync_results[integration].romm_revision == romm_connection.revision) &&
        (sync_results[integration].status.operation == VP_SYNC_CATALOG ||
         sync_results[integration].integration_revision == integration_revision))
        status = sync_results[integration].status;
    if (background_job.active && background_job.sync.state == VP_SYNC_RUNNING &&
        active_integration == integration &&
        (!romm || background_job.romm_revision == romm_connection.revision) &&
        (background_job.sync.operation == VP_SYNC_CATALOG ||
         background_job.integration_revision == integration_revision))
        status = background_job.sync;
    return status;
}
static VpDownloadQueue download_queue;
/* Three grid rows can be visible during a drag; retain the selected cover too. */
#define VP_COVER_SLOTS (VP_UI_GRID_WINDOW + 1)
static VpPlatformGroup platform_groups[VP_MAX_LIBRARY_GROUPS];
static size_t visible_indexes[VP_MAX_ITEMS];
static size_t platform_group_count;
static size_t visible_count_cache;
static VpLibraryLevel cached_library_level;
static char cached_platform[VP_TEXT];
static bool catalog_view_cache_dirty = true;
static bool cover_cache_dirty;
static size_t catalog_omitted;

static bool download_progress(unsigned long long downloaded, unsigned long long total, void *context);
static void cover_path_for(const char *id, char *path, size_t capacity);
static void artwork_path_for(const char *id, char *path, size_t capacity);
static bool sync_cover_metadata(VpApp *app);
static bool request_user_job(VpApp *app, VpJobKind kind);
static bool start_pending_user_job(void);

static void show_notification(VpApp *app, const char *format, ...) {
    va_list arguments;
    if (app == NULL) return;
    va_start(arguments, format);
    (void)vsnprintf(app->notice, sizeof(app->notice), format, arguments);
    va_end(arguments);
    app->notice_id++;
}

static void request_romm_check(void) {
    if (romm_pairing.active) return;
    if (romm_config.base_url[0] == '\0' || romm_config.token[0] == '\0') {
        romm_connection.state = VP_ROMM_UNCONFIGURED;
        romm_check_pending = false;
    } else if (romm_connection.state != VP_ROMM_CHECKING) {
        romm_check_pending = true;
    }
}

static bool save_catalog_cache(const VpCatalog *catalog, const VpRommConfig *config) {
    static const char magic[] = "VPCAT01";
    FILE *file;
    unsigned int version = VP_CATALOG_CACHE_VERSION;
    bool written;
    if (catalog == NULL || config == NULL || catalog->count > VP_MAX_ITEMS) return false;
    (void)sceIoMkdir("ux0:data", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher", 0777);
    file = fopen(VP_CATALOG_CACHE_PARTIAL, "wb");
    if (file == NULL) return false;
    written = fwrite(magic, 1, sizeof(magic), file) == sizeof(magic) &&
              fwrite(&version, sizeof(version), 1, file) == 1 &&
              fwrite(config->base_url, sizeof(config->base_url), 1, file) == 1 &&
              fwrite(catalog, sizeof(*catalog), 1, file) == 1;
    if (fclose(file) != 0 || !written) {
        (void)sceIoRemove(VP_CATALOG_CACHE_PARTIAL);
        return false;
    }
    (void)sceIoRemove(VP_CATALOG_CACHE_PATH);
    if (sceIoRename(VP_CATALOG_CACHE_PARTIAL, VP_CATALOG_CACHE_PATH) < 0) {
        (void)sceIoRemove(VP_CATALOG_CACHE_PARTIAL);
        return false;
    }
    return true;
}

static bool load_catalog_cache(VpApp *app, const VpRommConfig *config) {
    static const char magic[] = "VPCAT01";
    static VpCatalog cached_catalog;
    FILE *file;
    unsigned int version;
    char cached_url[VP_URL];
    bool valid;
    if (app == NULL || config == NULL || config->base_url[0] == '\0') return false;
    file = fopen(VP_CATALOG_CACHE_PATH, "rb");
    if (file == NULL) return false;
    valid = fread(cached_url, 1, sizeof(magic), file) == sizeof(magic) &&
            memcmp(cached_url, magic, sizeof(magic)) == 0 &&
            fread(&version, sizeof(version), 1, file) == 1 &&
            (version == VP_CATALOG_CACHE_VERSION || version == 1U || version == 2U) &&
            fread(cached_url, sizeof(cached_url), 1, file) == 1 &&
            strcmp(cached_url, config->base_url) == 0;
    if (valid) valid=vp_catalog_cache_read(file,version,&cached_catalog);
    if (fclose(file) != 0 || !valid || cached_catalog.count > VP_MAX_ITEMS) return false;
    app->catalog = cached_catalog;
    (void)snprintf(catalog_server,sizeof(catalog_server),"%s",config->base_url);
    catalog_synced_at[0]='\0';
    {
        SceIoStat info;
        if (sceIoGetstat(VP_CATALOG_CACHE_PATH,&info)>=0 && info.st_mtime.year) {
            (void)snprintf(catalog_synced_at,sizeof(catalog_synced_at),"%02u/%02u/%04u %02u:%02u",
                info.st_mtime.day,info.st_mtime.month,info.st_mtime.year,info.st_mtime.hour,info.st_mtime.minute);
        }
    }
    catalog_view_cache_dirty = true;
    return true;
}

static void trim(char *value) {
    size_t length;
    char *start = value;
    while (*start == ' ' || *start == '\t') start++;
    if (start != value) memmove(value, start, strlen(start) + 1);
    length = strlen(value);
    while (length > 0 && (value[length - 1] == '\r' || value[length - 1] == '\n' ||
                          value[length - 1] == ' ' || value[length - 1] == '\t')) value[--length] = '\0';
}

static void load_theme_file(VpApp *app, const char *path) {
    FILE *file = fopen(path, "r");
    char text[2048];
    size_t read_count;
    if (file == NULL) return;
    read_count = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[read_count] = '\0';
    (void)vp_theme_load(&app->theme, text);
    vp_ui_load_theme(text);
    app->active_view = app->theme.view;
}

static void load_providers(VpApp *app) {
    FILE *file;
    char line[512];
    VpView configured_view = VP_VIEW_GRID;
    bool active_view_configured = false;
    file = fopen(VP_CONFIG_PATH, "r");
    if (file == NULL) {
        FILE *backup = fopen(VP_CONFIG_PATH ".bak", "r");
        if (backup != NULL) {
            (void)fclose(backup);
            (void)sceIoRename(VP_CONFIG_PATH ".bak", VP_CONFIG_PATH);
            file = fopen(VP_CONFIG_PATH, "r");
        }
    }
    load_theme_file(app, "app0:assets/default/theme.ini");
    if (file == NULL) return;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *equals = strchr(line, '=');
        if (equals == NULL || line[0] == '#') continue;
        *equals++ = '\0';
        trim(line);
        trim(equals);
        if (strcmp(line, "romm_url") == 0) (void)snprintf(romm_config.base_url, sizeof(romm_config.base_url), "%s", equals);
        else if (strcmp(line, "romm_token") == 0) (void)snprintf(romm_config.token, sizeof(romm_config.token), "%s", equals);
        else if (strcmp(line, "image_proxy_url") == 0)
            (void)snprintf(romm_config.image_proxy_url, sizeof(romm_config.image_proxy_url), "%s", equals);
        else if (strcmp(line, "image_service_host") == 0)
            (void)vp_image_service_endpoint(equals, "image", romm_config.image_proxy_url, sizeof(romm_config.image_proxy_url));
        else if (strcmp(line, "steamgriddb_api_key") == 0)
            (void)snprintf(steamgrid_api_key, sizeof(steamgrid_api_key), "%s", equals);
        else if (strcmp(line, "homebrew_catalog") == 0)
            (void)snprintf(homebrew_config.catalog_url, sizeof(homebrew_config.catalog_url), "%s", equals);
        else if (strcmp(line, "active_theme") == 0)
            (void)snprintf(active_theme, sizeof(active_theme), "%s", equals);
        else if (strcmp(line, "active_view") == 0 && vp_view_from_string(equals, &configured_view))
            active_view_configured = true;
        else if (strncmp(line, "ui_wallpaper", 12) == 0 || strcmp(line, "ui_text_color") == 0)
            (void)vp_ui_preferences_set(&ui_preferences, line, equals);
        else if (strncmp(line, "launch_", 7) == 0)
            (void)vp_launch_config_set(&launch_config, line + 7, equals);
    }
    fclose(file);
    if (strcmp(active_theme, "default") != 0) {
        char path[VP_PATH];
        (void)snprintf(path, sizeof(path), "ux0:data/vitashelflauncher/themes/%s/theme.ini", active_theme);
        load_theme_file(app, path);
    }
    if (active_view_configured) app->active_view = configured_view;
}

static bool save_romm_config(void) {
    FILE *file;
    FILE *existing;
    size_t index;
    bool failed;
    bool had_config;
    (void)sceIoMkdir("ux0:data", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher", 0777);
    existing = fopen(VP_CONFIG_PATH, "r");
    had_config = existing != NULL;
    if (existing != NULL) (void)fclose(existing);
    file = fopen(VP_CONFIG_PATH ".tmp", "w");
    if (file == NULL) return false;
    (void)fprintf(file, "romm_url=%s\nromm_token=%s\nimage_proxy_url=%s\nhomebrew_catalog=%s\nactive_theme=%s\n",
                   romm_config.base_url, romm_config.token, romm_config.image_proxy_url,
                     homebrew_config.catalog_url, active_theme);
    (void)fprintf(file, "steamgriddb_api_key=%s\n", steamgrid_api_key);
    (void)fprintf(file, "active_view=%s\n", vp_view_name(app.active_view));
    (void)fprintf(file, "ui_wallpaper=%s\nui_text_color=%s\nui_wallpaper_animated=%d\n",
                  vp_ui_wallpaper(ui_preferences.wallpaper)->id,
                  vp_ui_text_color(ui_preferences.text_color)->id, ui_preferences.animated?1:0);
    for (index = 0; index < launch_config.count; index++) {
        const VpLaunchProfile *profile = &launch_config.profiles[index];
        (void)fprintf(file, "launch_%s=%s:%s\n", profile->platform, profile->titleid,
                      profile->core);
    }
    failed = ferror(file) != 0;
    if (fclose(file) != 0) failed = true;
    if (failed) {
        (void)sceIoRemove(VP_CONFIG_PATH ".tmp");
        return false;
    }
    if (had_config) {
        (void)sceIoRemove(VP_CONFIG_PATH ".bak");
        if (sceIoRename(VP_CONFIG_PATH, VP_CONFIG_PATH ".bak") < 0) {
            (void)sceIoRemove(VP_CONFIG_PATH ".tmp");
            return false;
        }
    }
    if (sceIoRename(VP_CONFIG_PATH ".tmp", VP_CONFIG_PATH) < 0) {
        if (had_config) (void)sceIoRename(VP_CONFIG_PATH ".bak", VP_CONFIG_PATH);
        (void)sceIoRemove(VP_CONFIG_PATH ".tmp");
        return false;
    }
    if (had_config) (void)sceIoRemove(VP_CONFIG_PATH ".bak");
    return true;
}

static bool write_library_preference_line(const char *line, void *context) {
    FILE *file = context;
    return file != NULL && line != NULL &&
           fputs(line, file) >= 0 && fputc('\n', file) != EOF;
}

static bool save_library_preferences(const VpApp *value) {
    FILE *file;
    FILE *existing;
    char line[VP_LIBRARY_PREFERENCES_LINE];
    bool written;
    bool had_preferences;
    if (value == NULL) return false;
    (void)sceIoMkdir("ux0:data", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher", 0777);
    file = fopen(VP_LIBRARY_PREFERENCES_PARTIAL, "wb");
    if (file == NULL) return false;
    existing = fopen(VP_LIBRARY_PREFERENCES_PATH, "rb");
    had_preferences = existing != NULL;
    written = vp_library_preferences_serialize(&value->catalog, &value->library_filters,
                                               romm_config.base_url,
                                               write_library_preference_line, file);
    /* A provider refresh or asynchronous local scan may leave some saved games
     * temporarily absent. Retain their scoped records instead of forgetting them. */
    if (existing != NULL) {
        while (written && fgets(line, sizeof(line), existing) != NULL) {
            const char *equals = strchr(line, '=');
            bool present = false;
            if (strncmp(line, "item.", 5) != 0 || equals == NULL) continue;
            for (size_t index = 0; index < value->catalog.count; index++) {
                char key[VP_TEXT * 2 + 48];
                vp_library_item_key(&value->catalog.items[index], romm_config.base_url,
                                     key, sizeof(key));
                if ((size_t)(equals - (line + 5)) == strlen(key) &&
                    strncmp(line + 5, key, strlen(key)) == 0) {
                    present = true;
                    break;
                }
            }
            if (!present && fputs(line, file) < 0) written = false;
        }
        if (ferror(existing)) written = false;
        if (fclose(existing) != 0) written = false;
    }
    if (fclose(file) != 0 || !written) {
        (void)sceIoRemove(VP_LIBRARY_PREFERENCES_PARTIAL);
        return false;
    }
    if (had_preferences) {
        (void)sceIoRemove(VP_LIBRARY_PREFERENCES_PATH ".bak");
        if (sceIoRename(VP_LIBRARY_PREFERENCES_PATH, VP_LIBRARY_PREFERENCES_PATH ".bak") < 0) {
            (void)sceIoRemove(VP_LIBRARY_PREFERENCES_PARTIAL);
            return false;
        }
    }
    if (sceIoRename(VP_LIBRARY_PREFERENCES_PARTIAL, VP_LIBRARY_PREFERENCES_PATH) < 0) {
        if (had_preferences)
            (void)sceIoRename(VP_LIBRARY_PREFERENCES_PATH ".bak", VP_LIBRARY_PREFERENCES_PATH);
        (void)sceIoRemove(VP_LIBRARY_PREFERENCES_PARTIAL);
        return false;
    }
    if (had_preferences) (void)sceIoRemove(VP_LIBRARY_PREFERENCES_PATH ".bak");
    return true;
}

static bool load_library_preferences(VpApp *value) {
    FILE *file;
    char line[VP_LIBRARY_PREFERENCES_LINE];
    bool failed;
    if (value == NULL) return false;
    value->library_filters.show_hidden = false;
    value->library_filters.downloaded_only = false;
    file = fopen(VP_LIBRARY_PREFERENCES_PATH, "rb");
    if (file == NULL) {
        FILE *backup = fopen(VP_LIBRARY_PREFERENCES_PATH ".bak", "rb");
        if (backup != NULL) {
            (void)fclose(backup);
            if (sceIoRename(VP_LIBRARY_PREFERENCES_PATH ".bak", VP_LIBRARY_PREFERENCES_PATH) < 0)
                return false;
            file = fopen(VP_LIBRARY_PREFERENCES_PATH, "rb");
            if (file == NULL) return false;
        }
    }
    if (file == NULL) return save_library_preferences(value);
    vp_library_preferences_reset(&value->catalog);
    while (fgets(line, sizeof(line), file) != NULL)
        (void)vp_library_preferences_parse_line(line, &value->catalog,
                                                &value->library_filters,
                                                romm_config.base_url);
    failed = ferror(file) != 0;
    if (fclose(file) != 0 || failed) return false;
    return true;
}

static bool file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static bool cached_cover_valid(const char *path) {
    unsigned char header[29];
    FILE *file=fopen(path,"rb");
    size_t size;
    if (!file) return false;
    size=fread(header,1,sizeof(header),file);
    fclose(file);
    return vp_safe_cover_png(header,size);
}

static bool installed_item_path(const VpItem *item, char *path, size_t capacity) {
    if (vp_platform_is_psp(item->platform)) {
        if (vp_item_psp_image_path(item, VP_PSP_ISO_ROOT, "iso", path, capacity) && file_exists(path))
            return true;
        return vp_item_psp_image_path(item, VP_PSP_ISO_ROOT, "cso", path, capacity) &&
               file_exists(path);
    }
    if (vp_platform_is_nds(item->platform)) {
        if (vp_item_nds_rom_path(item, VP_DSVITA_ROOT, "nds", path, capacity) && file_exists(path))
            return true;
        return vp_item_nds_rom_path(item, VP_DSVITA_ROOT, NULL, path, capacity) &&
               file_exists(path);
    }
    return vp_item_download_path(item, VP_DOWNLOAD_ROOT, path, capacity) && file_exists(path);
}

static void refresh_downloaded_items(VpApp *app) {
    size_t index;
    if (app == NULL) return;
    for (index = 0; index < app->catalog.count; index++) {
        VpItem *item = &app->catalog.items[index];
        char path[VP_PATH];
        if (item->source != VP_SOURCE_ROMM) continue;
        if (item->multi_file || !installed_item_path(item, path, sizeof(path))) {
            item->installed = false;
            item->launch_target[0] = '\0';
            continue;
        }
        item->installed = true;
        (void)snprintf(item->launch_target, sizeof(item->launch_target), "%s", path);
    }
}

static void apply_provider_catalog(VpApp *app, const VpCatalog *catalog) {
    if (app == NULL || catalog == NULL) return;
    catalog_omitted=vp_catalog_replace_providers(&app->catalog,catalog);
    (void)load_library_preferences(app);
    app->library_level = VP_LEVEL_PLATFORMS;
    app->active_platform[0] = '\0';
    app->selected_platform = 0;
    app->selected_item = 0;
    refresh_downloaded_items(app);
    catalog_view_cache_dirty = true;
}

static bool fetch_romm_catalog(const char *url, const char *token, char *response,
                               size_t capacity, void *context) {
    VpRommConnectionState *state = context;
    long status = vp_vita_probe_romm(url, token, response, capacity, NULL);
    /* The most recent authenticated request wins, even if the initial probe worked.
     * Homebrew uses the ordinary fetch backend and cannot overwrite this state. */
    if (status == 401) *state = VP_ROMM_INVALID_TOKEN;
    else if (status == 403) *state = VP_ROMM_FORBIDDEN;
    else if (status < 200 || status >= 300) *state = VP_ROMM_UNREACHABLE;
    return status >= 200 && status < 300 && response[0] != '\0';
}

static void *background_job_run(void *context) {
    VpBackgroundJob *job = context;
    vp_vita_thread_set_priority(VP_VITA_THREAD_PRIORITY_BACKGROUND);
    VpJobKind kind;
    VpRommConfig config;
    VpHomebrewConfig homebrew;
    const VpCatalog *catalog;
    VpItem item;
    char pair_code[VP_TEXT];
    char destination[VP_PATH];
    char partial[VP_PATH + 8];
    char resolved[VP_PATH];
    bool succeeded = false;
    bool romm_requested = false;
    bool homebrew_requested = false;
    char error[VP_TEXT];

    pthread_mutex_lock(&job->mutex);
    kind = job->kind;
    config = job->romm;
    homebrew = job->homebrew;
    /* The job owns this snapshot until the worker is joined. Keep the full
     * catalog off the worker stack, including during cover synchronization. */
    catalog = &job->catalog;
    item = job->item;
    (void)snprintf(destination, sizeof(destination), "%s", job->destination);
    (void)snprintf(partial, sizeof(partial), "%s", job->partial);
    (void)snprintf(resolved, sizeof(resolved), "%s", job->resolved_target[0] != '\0'
                                                        ? job->resolved_target : job->destination);
    (void)snprintf(pair_code, sizeof(pair_code), "%s", job->pair_code);
    pthread_mutex_unlock(&job->mutex);

    if (kind == VP_JOB_VALIDATE_ROMM) {
        VpRommProbeBackend probe_backend = {vp_vita_probe_romm, NULL};
        job->romm_state = vp_romm_check_connection(&config, probe_backend);
        succeeded = job->romm_state == VP_ROMM_CONNECTED;
    } else if (kind == VP_JOB_SYNC_PROVIDERS || kind == VP_JOB_PAIR_AND_SYNC) {
        VpFetchBackend fetch_backend = {vp_vita_fetch, NULL};
        VpFetchBackend romm_backend = {fetch_romm_catalog, &job->romm_state};
        VpRommProbeBackend probe_backend = {vp_vita_probe_romm, NULL};
        VpSyncProgressBackend progress_backend = {sync_progress, job};
        vp_catalog_init(&job->catalog);
        if (kind == VP_JOB_PAIR_AND_SYNC) {
            VpRommPairBackend pair_backend = {vp_vita_exchange_pair_code, NULL};
            succeeded = vp_romm_exchange_pair_code(&config, pair_code, pair_backend);
            if (!succeeded) goto finish;
            job->token_exchanged = true;
        }
        job->romm_state = vp_romm_check_connection(&config, probe_backend);
        if (config.base_url[0] != '\0' && config.token[0] != '\0') {
            romm_requested = true;
            succeeded = job->romm_state == VP_ROMM_CONNECTED &&
                        vp_romm_sync_with_progress(&job->catalog, &config, romm_backend, progress_backend);
        }
        if (homebrew.catalog_url[0] != '\0') {
            homebrew_requested = true;
            succeeded = vp_homebrew_sync(&job->catalog, &homebrew, fetch_backend) &&
                        (!romm_requested || succeeded);
        }
        if (!romm_requested && !homebrew_requested) succeeded = false;
        pthread_mutex_lock(&job->mutex);
        job->sync.stage = VP_SYNC_SAVING;
        pthread_mutex_unlock(&job->mutex);
        if (succeeded && !save_catalog_cache(&job->catalog, &config)) {
            vp_vita_set_error("No se pudo guardar el catálogo");
            succeeded = false;
        }
    } else if (kind == VP_JOB_SYNC_COVERS) {
        VpCoverTransformBackend cover_backend = {vp_vita_transform_cover, NULL};
        VpCoverTransformBackend artwork_backend = {vp_vita_transform_background, NULL};
        size_t index;
        succeeded = true;
        for (index = 0; index < catalog->count; index++) {
            const VpItem *cover = &catalog->items[index];
            char destination[VP_PATH];
            char partial[VP_PATH + 8];
            bool transformed;
            bool item_failed;
            if (cover->source != VP_SOURCE_ROMM || cover->cover_url[0] == '\0') continue;
            cover_path_for(cover->id, destination, sizeof(destination));
            if (snprintf(partial, sizeof(partial), "%s.tmp", destination) >= (int)sizeof(partial)) {
                sync_item_finished(job, false, false, true);
                succeeded = false;
                break;
            }
            transformed = vp_cache_cover(cover, &config, partial, cover_backend);
            if (transformed) {
                (void)sceIoRemove(destination);
                transformed = sceIoRename(partial, destination) >= 0;
                if (!transformed) (void)sceIoRemove(partial);
            }
            if (!transformed) succeeded = false;
            item_failed = !transformed;
            if (transformed) {
                pthread_mutex_lock(&job->mutex);
                job->covers_completed++;
                pthread_mutex_unlock(&job->mutex);
            }
            if (cover->background_url[0]!='\0') {
                VpItem artwork=*cover;
                (void)snprintf(artwork.cover_url,sizeof(artwork.cover_url),"%s",cover->background_url);
                artwork_path_for(cover->id,destination,sizeof(destination));
                (void)snprintf(partial,sizeof(partial),"%s.tmp",destination);
                if (vp_cache_cover(&artwork,&config,partial,artwork_backend)) {
                    (void)sceIoRemove(destination);
                    if (sceIoRename(partial,destination)<0) {
                        (void)sceIoRemove(partial);
                        item_failed = true;
                    }
                } else item_failed = true;
            }
            if (item_failed) succeeded = false;
            sync_item_finished(job, !item_failed, false, item_failed);
        }
    } else if (kind == VP_JOB_SCAN_LOCAL) {
        succeeded=vp_local_scan(&job->catalog,"ux0:app","ur0:appmeta",&job->omitted);
        if (succeeded) for (size_t i=0;i<job->catalog.count;i++)
            (void)vp_local_artwork_load(&job->catalog.items[i],VP_ARTWORK_CACHE);
    } else if (kind == VP_JOB_CHECK_STEAMGRID || kind == VP_JOB_CHECK_IMAGES) {
        VpRommProbeBackend backend={vp_vita_probe_service,NULL};
        job->connection_state=kind==VP_JOB_CHECK_STEAMGRID?
            vp_steamgrid_check(job->steamgrid_key,backend):vp_image_service_check(
                config.image_proxy_url[0]?config.image_proxy_url:config.base_url,backend);
        succeeded=job->connection_state==VP_CONNECTION_CONNECTED;
    } else if (kind == VP_JOB_SYNC_STEAMGRID) {
        VpRommProbeBackend backend={vp_vita_probe_service,NULL};
        VpCoverTransformBackend transform={vp_vita_transform_cover,NULL};
        succeeded=true;
        job->connection_state=vp_steamgrid_check(job->steamgrid_key,backend);
        if (job->connection_state!=VP_CONNECTION_CONNECTED) { succeeded=false; goto finish; }
        for (size_t i=0;i<job->catalog.count;i++) {
            VpItem *cover=&job->catalog.items[i];
            char path[VP_PATH], temp[VP_PATH+8];
            VpArtworkResult result;
            if (cover->source!=VP_SOURCE_LOCAL || strcmp(cover->platform,"psvita")) continue;
            cover_path_for(cover->id,path,sizeof(path));
            if (cover->cover_url[0] && cached_cover_valid(path)) {
                pthread_mutex_lock(&job->mutex);
                job->covers_completed++;
                pthread_mutex_unlock(&job->mutex);
                sync_item_finished(job, true, false, false);
                continue;
            }
            result=vp_steamgrid_cover(cover,job->steamgrid_key,backend,&job->connection_state);
            if (result==VP_ARTWORK_FAILED) {
                sync_item_finished(job, false, false, true);
                succeeded=false;
                break;
            }
            if (result==VP_ARTWORK_NOT_FOUND) {
                job->covers_missing++;
                sync_item_finished(job, false, true, false);
                continue;
            }
            bool ready = false;
            (void)snprintf(temp,sizeof(temp),"%s.tmp",path);
            if (vp_cache_cover(cover,&config,temp,transform)) {
                (void)sceIoRemove(path);
                if (sceIoRename(temp,path)>=0) {
                    ready = vp_local_artwork_save(cover,VP_ARTWORK_CACHE);
                    if (!ready) succeeded=false;
                    else {
                        pthread_mutex_lock(&job->mutex);
                        job->covers_completed++;
                        pthread_mutex_unlock(&job->mutex);
                    }
                } else { (void)sceIoRemove(temp); succeeded=false; }
            } else succeeded=false;
            sync_item_finished(job, ready, false, !ready);
            if (!ready) break;
        }
    } else {
        VpTransferBackend transfer_backend = {vp_vita_transfer, NULL};
        const char *target = kind == VP_JOB_DOWNLOAD_ROM ? partial : destination;
        if (kind == VP_JOB_INSTALL_HOMEBREW)
            succeeded = vp_install_homebrew(&item, target, transfer_backend);
        else
            succeeded = vp_vita_transfer_with_progress(item.download_url, config.token, target,
                                                       VP_TRANSFER_DOWNLOAD, download_progress,
                                                       job, NULL);
        if (succeeded && kind == VP_JOB_DOWNLOAD_ROM) {
            succeeded = sceIoRename(partial, destination) >= 0;
            if (!succeeded) (void)sceIoRemove(partial);
        }
        if (succeeded && kind == VP_JOB_DOWNLOAD_ROM && vp_platform_is_psp(item.platform)) {
            char extension[VP_DOWNLOAD_EXTENSION];
            if (vp_item_extension(&item, extension, sizeof(extension)) &&
                strcmp(extension, "zip") == 0) {
                succeeded = vp_vita_extract_psp_archive(&item, destination, VP_PSP_ISO_ROOT,
                                                         resolved, sizeof(resolved));
                if (!succeeded) (void)sceIoRemove(destination);
            }
        }
        if (succeeded && kind == VP_JOB_DOWNLOAD_ROM && vp_platform_is_nds(item.platform)) {
            char extension[VP_DOWNLOAD_EXTENSION];
            if (vp_item_extension(&item, extension, sizeof(extension)) &&
                strcmp(extension, "zip") == 0) {
                succeeded = vp_vita_extract_nds_archive(&item, destination, VP_DSVITA_ROOT,
                                                         resolved, sizeof(resolved));
                if (!succeeded) (void)sceIoRemove(destination);
            }
        }
    }
finish:
    (void)snprintf(error, sizeof(error), "%s", succeeded ? "" : vp_vita_last_error());

    pthread_mutex_lock(&job->mutex);
    if (job->token_exchanged ||
        ((kind == VP_JOB_SYNC_PROVIDERS || kind == VP_JOB_PAIR_AND_SYNC) && succeeded)) {
        job->romm = config;
    }
    job->succeeded = succeeded;
    (void)snprintf(job->resolved_target, sizeof(job->resolved_target), "%s", resolved);
    (void)snprintf(job->error, sizeof(job->error), "%s", error);
    job->completed = true;
    pthread_mutex_unlock(&job->mutex);
    return NULL;
}

static bool background_job_start(VpJobKind kind, const VpItem *item, const char *destination,
                                 const char *partial) {
    int result;
    pthread_mutex_lock(&background_job.mutex);
    if (background_job.active) {
        pthread_mutex_unlock(&background_job.mutex);
        return false;
    }
    background_job.kind = kind;
    background_job.completed = false;
    background_job.succeeded = false;
    background_job.error[0] = '\0';
    background_job.downloaded = 0;
    background_job.total = 0;
    background_job.covers_completed = 0;
    background_job.covers_total = 0;
    background_job.cancel_requested = false;
    background_job.pause_requested = false;
    background_job.romm = romm_config;
    background_job.romm_revision = romm_connection.revision;
    background_job.romm_state = VP_ROMM_UNREACHABLE;
    background_job.token_exchanged = false;
    background_job.connection_state=VP_CONNECTION_UNCONFIGURED;
    background_job.integration_revision=integration_revision;
    background_job.omitted=0;
    background_job.covers_missing=0;
    memset(&background_job.sync, 0, sizeof(background_job.sync));
    if (kind == VP_JOB_SYNC_PROVIDERS || kind == VP_JOB_PAIR_AND_SYNC || kind == VP_JOB_SYNC_STEAMGRID) {
        background_job.sync.state = VP_SYNC_RUNNING;
        background_job.sync.operation = kind == VP_JOB_SYNC_STEAMGRID ? VP_SYNC_STEAMGRID : VP_SYNC_CATALOG;
    }
    (void)snprintf(background_job.steamgrid_key,sizeof(background_job.steamgrid_key),"%s",steamgrid_api_key);
    if (kind==VP_JOB_SYNC_STEAMGRID) background_job.catalog=app.catalog;
    if (kind==VP_JOB_SYNC_STEAMGRID) for (size_t i=0;i<background_job.catalog.count;i++)
        if (background_job.catalog.items[i].source==VP_SOURCE_LOCAL && !strcmp(background_job.catalog.items[i].platform,"psvita"))
            background_job.covers_total++;
    background_job.sync.total = background_job.covers_total;
    background_job.homebrew = homebrew_config;
    (void)snprintf(background_job.pair_code, sizeof(background_job.pair_code), "%s",
        kind == VP_JOB_PAIR_AND_SYNC && romm_pairing.active ? romm_pairing.code : romm_pair_code);
    memset(&background_job.item, 0, sizeof(background_job.item));
    if (item != NULL) background_job.item = *item;
    (void)snprintf(background_job.destination, sizeof(background_job.destination), "%s",
                   destination != NULL ? destination : "");
    (void)snprintf(background_job.partial, sizeof(background_job.partial), "%s",
                   partial != NULL ? partial : "");
    background_job.resolved_target[0] = '\0';
    background_job.active = true;
    result = pthread_create(&background_job.thread, NULL, background_job_run, &background_job);
    if (result != 0) background_job.active = false;
    pthread_mutex_unlock(&background_job.mutex);
    if (result != 0) sync_result_publish(&background_job);
    if (result == 0 && (kind == VP_JOB_VALIDATE_ROMM || kind == VP_JOB_SYNC_PROVIDERS ||
                         kind == VP_JOB_PAIR_AND_SYNC)) {
        romm_connection.state = kind == VP_JOB_PAIR_AND_SYNC ||
                                (romm_config.base_url[0] && romm_config.token[0])
                                    ? VP_ROMM_CHECKING : VP_ROMM_UNCONFIGURED;
        romm_check_pending = false;
    }
    if (result==0) {
        if (kind==VP_JOB_SCAN_LOCAL) local_scan_pending=false;
        if (kind==VP_JOB_CHECK_STEAMGRID || kind==VP_JOB_SYNC_STEAMGRID) steamgrid_state=VP_CONNECTION_CHECKING;
        if (kind==VP_JOB_CHECK_IMAGES) image_service_state=VP_CONNECTION_CHECKING;
    }
    return result == 0;
}

static bool start_pending_romm_pairing(void) {
    bool busy;
    if (!romm_pairing.active) return false;
    if (romm_pairing.revision != romm_connection.revision) {
        memset(&romm_pairing, 0, sizeof(romm_pairing));
        return false;
    }
    if (background_job_start(VP_JOB_PAIR_AND_SYNC, NULL, NULL, NULL)) {
        memset(&romm_pairing, 0, sizeof(romm_pairing));
        show_notification(&app, "Canjeando código y sincronizando catálogo");
        return true;
    }
    pthread_mutex_lock(&background_job.mutex);
    busy = background_job.active;
    pthread_mutex_unlock(&background_job.mutex);
    if (!busy) {
        memset(&romm_pairing, 0, sizeof(romm_pairing));
        show_notification(&app, "No se pudo iniciar el canje de RomM; reintenta con Triángulo");
    }
    return false;
}

static bool request_user_job(VpApp *app, VpJobKind kind) {
    bool busy;
    if (background_job_start(kind, NULL, NULL, NULL)) {
        show_notification(app, "%s — iniciado", sync_job_title(kind));
        return true;
    }
    pthread_mutex_lock(&background_job.mutex);
    busy = background_job.active;
    pthread_mutex_unlock(&background_job.mutex);
    if (!busy) {
        show_notification(app, "%s — fallido: no se pudo iniciar", sync_job_title(kind));
        return false;
    }
    if (pending_user_job.active) {
        if (pending_user_job.kind == kind) return true;
        show_notification(app, "%s — fallido: ya hay una tarea pendiente; espera a que termine",
            sync_job_title(kind));
        return false;
    }
    pending_user_job.active = true;
    pending_user_job.kind = kind;
    show_notification(app, "%s — en cola (%s)", sync_job_title(kind), background_job_active_label());
    return true;
}

static bool start_pending_user_job(void) {
    VpJobKind kind;
    bool busy;
    if (!pending_user_job.active) return false;
    kind = pending_user_job.kind;
    if (kind == VP_JOB_SYNC_COVERS) {
        if (!sync_cover_metadata(&app)) return false;
    } else if (!background_job_start(kind, NULL, NULL, NULL)) {
        pthread_mutex_lock(&background_job.mutex);
        busy = background_job.active;
        pthread_mutex_unlock(&background_job.mutex);
        if (!busy) {
            memset(&pending_user_job, 0, sizeof(pending_user_job));
            show_notification(&app, "%s — fallido: no se pudo iniciar", sync_job_title(kind));
        }
        return false;
    }
    memset(&pending_user_job, 0, sizeof(pending_user_job));
    if (kind != VP_JOB_SYNC_COVERS)
        show_notification(&app, "%s — iniciado", sync_job_title(kind));
    return true;
}

static void request_romm_pairing(VpApp *app) {
    VpJobKind active_kind;
    pthread_mutex_lock(&background_job.mutex);
    active_kind = background_job.active ? background_job.kind : VP_JOB_NONE;
    pthread_mutex_unlock(&background_job.mutex);
    if (active_kind == VP_JOB_PAIR_AND_SYNC) {
        show_notification(app, "El canje de RomM ya está en curso; espera a que termine");
        return;
    }
    romm_pairing.active = true;
    romm_pairing.revision = romm_connection.revision;
    (void)snprintf(romm_pairing.code, sizeof(romm_pairing.code), "%s", romm_pair_code);
    romm_check_pending = false;
    if (!start_pending_romm_pairing() && romm_pairing.active)
        show_notification(app, "Canje de RomM en cola: %s", background_job_label(active_kind));
}

static bool download_progress(unsigned long long downloaded, unsigned long long total, void *context) {
    VpBackgroundJob *job = context;
    bool canceled;
    pthread_mutex_lock(&job->mutex);
    job->downloaded = downloaded;
    job->total = total;
    canceled = job->cancel_requested || job->pause_requested;
    pthread_mutex_unlock(&job->mutex);
    return canceled;
}

static bool start_next_download(void) {
    VpDownloadEntry *entry;
    if (romm_pairing.active) return start_pending_romm_pairing();
    if (pending_user_job.active) return false;
    if (romm_check_pending && background_job_start(VP_JOB_VALIDATE_ROMM, NULL, NULL, NULL))
        return true;
    pthread_mutex_lock(&background_job.mutex);
    if (background_job.active) {
        pthread_mutex_unlock(&background_job.mutex);
        return false;
    }
    pthread_mutex_unlock(&background_job.mutex);
    entry = vp_download_queue_start_next(&download_queue);
    if (entry == NULL) return false;
    if (background_job_start(VP_JOB_DOWNLOAD_ROM, &entry->item, entry->destination, entry->partial))
        return true;
    vp_download_queue_finish(entry, false, "No se pudo iniciar la descarga");
    return false;
}

static bool pause_active_download(VpApp *app, bool exiting) {
    char item_id[VP_TEXT];
    bool active;
    size_t index;

    pthread_mutex_lock(&background_job.mutex);
    active = background_job.active && background_job.kind == VP_JOB_DOWNLOAD_ROM;
    if (active) {
        (void)snprintf(item_id, sizeof(item_id), "%s", background_job.item.id);
        background_job.pause_requested = true;
        background_job.cancel_requested = false;
    }
    pthread_mutex_unlock(&background_job.mutex);
    if (active) {
        VpDownloadEntry *entry = vp_download_queue_find(&download_queue, item_id);
        if (entry != NULL && entry->state == VP_DOWNLOAD_DOWNLOADING)
            (void)vp_download_queue_pause(&download_queue, item_id);
    } else {
        for (index = 0; index < download_queue.count; index++) {
            if (download_queue.entries[index].state != VP_DOWNLOAD_DOWNLOADING) continue;
            (void)vp_download_queue_pause(&download_queue, download_queue.entries[index].item.id);
            active = true;
            break;
        }
    }
    if (!active) return false;
    if (!exiting) show_notification(app, "Descarga pausada; X para reanudar");
    return true;
}

static void background_job_poll(VpApp *app) {
    VpJobKind kind;
    VpItem item;
    VpRommConfig config;
    char destination[VP_PATH];
    char resolved[VP_PATH];
    char error[VP_TEXT];
    bool succeeded;
    bool canceled;
    bool paused;
    size_t covers_completed;
    size_t covers_total;
    unsigned long romm_revision;
    VpRommConnectionState romm_state;
    bool token_exchanged;

    pthread_mutex_lock(&background_job.mutex);
    if (!background_job.active || !background_job.completed) {
        pthread_mutex_unlock(&background_job.mutex);
        return;
    }
    kind = background_job.kind;
    succeeded = background_job.succeeded;
    item = background_job.item;
    config = background_job.romm;
    (void)snprintf(destination, sizeof(destination), "%s", background_job.destination);
    (void)snprintf(resolved, sizeof(resolved), "%s", background_job.resolved_target[0] != '\0'
                                                         ? background_job.resolved_target
                                                         : background_job.destination);
    (void)snprintf(error, sizeof(error), "%s", background_job.error);
    canceled = background_job.cancel_requested;
    paused = background_job.pause_requested;
    covers_completed = background_job.covers_completed;
    covers_total = background_job.covers_total;
    romm_revision = background_job.romm_revision;
    romm_state = background_job.romm_state;
    token_exchanged = background_job.token_exchanged;
    pthread_mutex_unlock(&background_job.mutex);

    (void)pthread_join(background_job.thread, NULL);
    pthread_mutex_lock(&background_job.mutex);
    background_job.active = false;
    background_job.kind = VP_JOB_NONE;
    pthread_mutex_unlock(&background_job.mutex);

    if (background_job.sync.operation != VP_SYNC_CATALOG) sync_result_publish(&background_job);
    if (kind==VP_JOB_SCAN_LOCAL) {
        if (succeeded) {
            for (size_t i=0;i<background_job.catalog.count;i++) {
                VpItem *fresh=&background_job.catalog.items[i];
                VpItem *previous=vp_catalog_find(&app->catalog,fresh->id);
                if (previous) {
                    fresh->favorite=previous->favorite;
                    fresh->hidden=previous->hidden;
                    fresh->last_played=previous->last_played;
                }
            }
            if (vp_catalog_source_matches(&app->catalog,&background_job.catalog,VP_SOURCE_LOCAL)) {
                (void)start_next_download();
                return;
            }
            size_t omitted=background_job.omitted+vp_catalog_replace_source(&app->catalog,&background_job.catalog,VP_SOURCE_LOCAL);
            (void)load_library_preferences(app);
            app->library_level=VP_LEVEL_PLATFORMS;
            app->selected_platform=0; app->selected_item=0; app->active_platform[0]=0;
            catalog_view_cache_dirty=true; cover_cache_dirty=true;
            if (omitted) show_notification(app,"Límite de biblioteca: %u juegos omitidos",(unsigned)omitted);
        }
        (void)start_next_download();
        return;
    }
    if (kind==VP_JOB_CHECK_STEAMGRID || kind==VP_JOB_CHECK_IMAGES || kind==VP_JOB_SYNC_STEAMGRID) {
        memset(background_job.steamgrid_key,0,sizeof(background_job.steamgrid_key));
        if (background_job.integration_revision==integration_revision) {
            if (kind==VP_JOB_CHECK_IMAGES) image_service_state=background_job.connection_state;
            else steamgrid_state=background_job.connection_state;
            if (kind!=VP_JOB_SYNC_STEAMGRID)
                show_notification(app, "%s — %s", sync_job_title(kind),
                    succeeded ? "completado" : "fallido");
            if (kind==VP_JOB_SYNC_STEAMGRID) {
                for (size_t i=0;i<background_job.catalog.count;i++) {
                    const VpItem *updated=&background_job.catalog.items[i];
                    VpItem *current=vp_catalog_find(&app->catalog,updated->id);
                    if (updated->source!=VP_SOURCE_LOCAL || !current || strcmp(updated->title,current->title)) continue;
                    (void)snprintf(current->cover_url,sizeof(current->cover_url),"%s",updated->cover_url);
                    (void)snprintf(current->remote_id,sizeof(current->remote_id),"%s",updated->remote_id);
                }
                cover_cache_dirty=true;
                show_notification(app,succeeded?"SteamGridDB: carátulas — completado · %u/%u listas (%u sin coincidencia)":"SteamGridDB: carátulas — fallido · %u/%u listas; revisa Integraciones",
                    (unsigned)covers_completed,(unsigned)background_job.covers_total,(unsigned)background_job.covers_missing);
            }
        }
        (void)start_next_download();
        return;
    }
    if (kind==VP_JOB_SYNC_COVERS) cover_cache_dirty=true;
    if (kind == VP_JOB_VALIDATE_ROMM || kind == VP_JOB_SYNC_PROVIDERS || kind == VP_JOB_PAIR_AND_SYNC) {
        /* URL edits invalidate in-flight authentication, pairing and catalog results. */
        if (!vp_romm_connection_apply(&romm_connection, romm_revision, romm_state)) {
            (void)start_next_download();
            return;
        }
        if (romm_state == VP_ROMM_CONNECTED) memset(romm_pair_code, 0, sizeof(romm_pair_code));
        if (token_exchanged) {
            romm_config = config;
            memset(romm_pair_code, 0, sizeof(romm_pair_code));
            if (!save_romm_config()) {
                background_job.succeeded = false;
                sync_result_publish(&background_job);
                show_notification(app, "RomM: conexión — fallido: no se pudo guardar la configuración");
                (void)start_next_download();
                return;
            }
        }
        if (kind == VP_JOB_VALIDATE_ROMM) {
            (void)start_next_download();
            return;
        }
    }

    if (background_job.sync.operation == VP_SYNC_CATALOG) sync_result_publish(&background_job);
    if (!succeeded && kind != VP_JOB_DOWNLOAD_ROM) {
        show_notification(app, "%s — fallido: %.140s", sync_job_title(kind), error);
        (void)start_next_download();
        return;
    }
    if (kind == VP_JOB_SYNC_PROVIDERS || kind == VP_JOB_PAIR_AND_SYNC) {
        apply_provider_catalog(app, &background_job.catalog);
        (void)snprintf(catalog_server,sizeof(catalog_server),"%s",config.base_url);
        catalog_synced_at[0]='\0';
        {
            SceDateTime now;
            if (sceRtcGetCurrentClockLocalTime(&now)>=0)
                (void)snprintf(catalog_synced_at,sizeof(catalog_synced_at),"%02u/%02u/%04u %02u:%02u",
                    now.day,now.month,now.year,now.hour,now.minute);
        }
        show_notification(app, "RomM: catálogo — completado · %u juegos", (unsigned)app->catalog.count);
        if (catalog_omitted) show_notification(app,"RomM: catálogo — %u juegos remotos omitidos",(unsigned)catalog_omitted);
    } else if (kind == VP_JOB_SYNC_COVERS) {
        show_notification(app, "RomM: metadata — completado · %u/%u carátulas",
                          (unsigned)covers_completed, (unsigned)covers_total);
    } else if (kind == VP_JOB_DOWNLOAD_ROM) {
        VpDownloadEntry *entry = vp_download_queue_find(&download_queue, item.id);
        if (entry != NULL) {
            if (paused && !succeeded) {
                (void)vp_download_queue_pause(&download_queue, item.id);
            } else if (canceled) {
                entry->state = VP_DOWNLOAD_CANCELED;
                entry->percent = 0;
                entry->error[0] = '\0';
            } else {
                vp_download_queue_finish(entry, succeeded, error);
            }
        }
        if (paused && !succeeded) {
            show_notification(app, "Descarga pausada; X para reanudar");
            return;
        }
        if (!succeeded && canceled) {
            if (entry != NULL)
                show_notification(app, "Descarga cancelada");
            (void)start_next_download();
            return;
        }
        if (!succeeded) {
            show_notification(app, "Fallo la descarga: %.120s", item.title);
            (void)start_next_download();
            return;
        }
        VpItem *completed = vp_catalog_find(&app->catalog, item.id);
        if (completed != NULL) {
            completed->installed = true;
            catalog_view_cache_dirty = true;
            (void)snprintf(completed->launch_target, sizeof(completed->launch_target), "%s",
                           resolved);
        }
        show_notification(app, "Descarga finalizada: %.120s", item.title);
        vp_ui_audio_play(VP_UI_SOUND_DOWNLOAD_COMPLETE);
        (void)start_next_download();
    } else if (kind == VP_JOB_INSTALL_HOMEBREW) {
        VpItem *completed = vp_catalog_find(&app->catalog, item.id);
        if (completed != NULL) {
            completed->installed = true;
            catalog_view_cache_dirty = true;
        }
        show_notification(app, "Instalacion finalizada; revisa LiveArea");
        local_scan_pending=true;
    }
    if (kind != VP_JOB_DOWNLOAD_ROM) (void)start_next_download();
}

static bool sync_romm_catalog(VpApp *app) {
    if ((romm_config.base_url[0] == '\0' || romm_config.token[0] == '\0') &&
        homebrew_config.catalog_url[0] == '\0') return false;
    return request_user_job(app, VP_JOB_SYNC_PROVIDERS);
}

static bool sync_cover_metadata(VpApp *app) {
    size_t index;
    int result;
    if (app == NULL || romm_config.base_url[0] == '\0' || romm_config.token[0] == '\0') return false;
    pthread_mutex_lock(&background_job.mutex);
    if (background_job.active) {
        pthread_mutex_unlock(&background_job.mutex);
        if (pending_user_job.active && pending_user_job.kind != VP_JOB_SYNC_COVERS) {
            show_notification(app, "%s — fallido: ya hay una tarea pendiente; espera a que termine",
                sync_job_title(VP_JOB_SYNC_COVERS));
            return false;
        }
        if (!pending_user_job.active) {
            pending_user_job.active = true;
            pending_user_job.kind = VP_JOB_SYNC_COVERS;
        }
        show_notification(app, "%s — en cola (%s)", sync_job_title(VP_JOB_SYNC_COVERS),
            background_job_active_label());
        return true;
    }
    background_job.kind = VP_JOB_SYNC_COVERS;
    background_job.completed = false;
    background_job.succeeded = false;
    background_job.error[0] = '\0';
    background_job.downloaded = 0;
    background_job.total = 0;
    background_job.covers_completed = 0;
    background_job.covers_total = 0;
    memset(&background_job.sync, 0, sizeof(background_job.sync));
    background_job.sync.state = VP_SYNC_RUNNING;
    background_job.sync.operation = VP_SYNC_METADATA;
    background_job.sync.stage = VP_SYNC_PROCESSING;
    background_job.romm_revision = romm_connection.revision;
    background_job.integration_revision = integration_revision;
    background_job.cancel_requested = false;
    background_job.pause_requested = false;
    background_job.romm = romm_config;
    background_job.homebrew = homebrew_config;
    background_job.catalog = app->catalog;
    for (index = 0; index < background_job.catalog.count; index++) {
        const VpItem *item = &background_job.catalog.items[index];
        if (item->source == VP_SOURCE_ROMM && item->cover_url[0] != '\0')
            background_job.covers_total++;
    }
    background_job.sync.total = background_job.covers_total;
    background_job.active = true;
    result = pthread_create(&background_job.thread, NULL, background_job_run, &background_job);
    if (result != 0) background_job.active = false;
    pthread_mutex_unlock(&background_job.mutex);
    if (result != 0) sync_result_publish(&background_job);
    else show_notification(app, "%s — iniciado", sync_job_title(VP_JOB_SYNC_COVERS));
    return result == 0;
}

static bool sync_steamgrid_artwork(VpApp *app) {
    char endpoint[VP_URL];
    if (!app || !steamgrid_api_key[0] ||
        !vp_image_service_endpoint(romm_config.image_proxy_url[0]?romm_config.image_proxy_url:romm_config.base_url,
                                   "image",endpoint,sizeof(endpoint))) return false;
    return request_user_job(app, VP_JOB_SYNC_STEAMGRID);
}

static void settings_text_to_wide(const char *source, SceWChar16 *destination, size_t capacity) {
    size_t index;
    for (index = 0; index + 1 < capacity && source[index] != '\0'; index++)
        destination[index] = (unsigned char)source[index];
    destination[index] = 0;
}

static void settings_wide_to_text(const SceWChar16 *source, char *destination, size_t capacity) {
    size_t index;
    for (index = 0; index + 1 < capacity && source[index] != 0; index++)
        destination[index] = source[index] <= 0x7f ? (char)source[index] : '?';
    destination[index] = '\0';
}

static bool settings_start_ime(void) {
    SceImeDialogParam param;
    char host[VP_URL];
    const char *value = settings_field == 0 ? romm_config.base_url : romm_pair_code;
    size_t capacity = settings_field == 0 ? sizeof(romm_config.base_url) : sizeof(romm_pair_code);
    bool external=settings_integration!=VP_INTEGRATION_ROMM;
    if (external) {
        if (settings_field!=0) return false;
        value=settings_integration==VP_INTEGRATION_STEAMGRID?steamgrid_api_key:romm_config.image_proxy_url;
        capacity=settings_integration==VP_INTEGRATION_STEAMGRID?sizeof(steamgrid_api_key):sizeof(romm_config.image_proxy_url);
        if (settings_integration==VP_INTEGRATION_IMAGES) {
            (void)snprintf(host,sizeof(host),"%s",value);
            if (strlen(host)>=6 && !strcmp(host+strlen(host)-6,"/image")) host[strlen(host)-6]=0;
            value=host;
        }
    } else {
        if (settings_field > 1) return false;
        if (settings_field == 1 && !vp_romm_pair_code_enabled(romm_connection.state)) return false;
    }
    settings_text_to_wide(value, settings_ime_buffer, sizeof(settings_ime_buffer) / sizeof(settings_ime_buffer[0]));
    sceImeDialogParamInit(&param);
    param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_SPANISH;
    param.type = settings_field == 0 ? SCE_IME_TYPE_URL : SCE_IME_TYPE_BASIC_LATIN;
    param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param.title = settings_field == 0 ? (const SceWChar16 *)u"URL de RomM" : (const SceWChar16 *)u"Pair code de RomM";
    if (external) {
        param.type=settings_integration==VP_INTEGRATION_STEAMGRID?SCE_IME_TYPE_BASIC_LATIN:SCE_IME_TYPE_URL;
        param.title=settings_integration==VP_INTEGRATION_STEAMGRID?
            (const SceWChar16 *)u"SteamGridDB API key":(const SceWChar16 *)u"Image Service host";
    }
    param.maxTextLength = (SceUInt32)(capacity - 1);
    param.initialText = settings_ime_buffer;
    param.inputTextBuffer = settings_ime_buffer;
    param.enterLabel = SCE_IME_ENTER_LABEL_GO;
    settings_ime_active = sceImeDialogInit(&param) >= 0;
    return settings_ime_active;
}

static void settings_finish_ime(VpApp *app) {
    SceImeDialogResult result;
    if (!settings_ime_active || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;
    memset(&result, 0, sizeof(result));
    (void)sceImeDialogGetResult(&result);
    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        if (settings_integration!=VP_INTEGRATION_ROMM) {
            char candidate[VP_URL], previous[VP_URL], endpoint[VP_URL];
            char *value=settings_integration==VP_INTEGRATION_STEAMGRID?steamgrid_api_key:romm_config.image_proxy_url;
            size_t capacity=settings_integration==VP_INTEGRATION_STEAMGRID?sizeof(steamgrid_api_key):sizeof(romm_config.image_proxy_url);
            settings_wide_to_text(settings_ime_buffer,candidate,sizeof(candidate));
            trim(candidate);
            (void)snprintf(previous,sizeof(previous),"%s",value);
            if (strpbrk(candidate,"\r\n") || strlen(candidate)>=capacity ||
                (settings_integration==VP_INTEGRATION_IMAGES && candidate[0] &&
                 !vp_image_service_endpoint(candidate,"image",endpoint,sizeof(endpoint)))) {
                show_notification(app,"Valor inválido; usa un host HTTP(S) para Image Service");
            } else {
                (void)snprintf(value,capacity,"%s",settings_integration==VP_INTEGRATION_IMAGES && candidate[0]?endpoint:candidate);
                if (!save_romm_config()) {
                    (void)snprintf(value,capacity,"%s",previous);
                    show_notification(app,"No se pudo guardar la configuración");
                } else {
                    integration_revision++;
                    steamgrid_state=steamgrid_api_key[0]?VP_CONNECTION_NOT_CHECKED:VP_CONNECTION_UNCONFIGURED;
                    image_service_state=romm_config.image_proxy_url[0]?VP_CONNECTION_NOT_CHECKED:VP_CONNECTION_UNCONFIGURED;
                    show_notification(app,"Configuración guardada; selecciona Probar conexión");
                }
            }
            memset(candidate,0,sizeof(candidate)); memset(previous,0,sizeof(previous));
            goto ime_finished;
        }
        char *value = settings_field == 0 ? romm_config.base_url : romm_pair_code;
        size_t capacity = settings_field == 0 ? sizeof(romm_config.base_url) : sizeof(romm_pair_code);
        char previous[VP_URL];
        (void)snprintf(previous, sizeof(previous), "%s", value);
        settings_wide_to_text(settings_ime_buffer, value, capacity);
        if (settings_field == 0 && strcmp(previous, value) != 0) {
            vp_romm_connection_reset(&romm_connection, &romm_config);
            romm_check_pending = romm_connection.state == VP_ROMM_CHECKING;
            memset(romm_pair_code, 0, sizeof(romm_pair_code));
            if (!romm_config.image_proxy_url[0]) {
                integration_revision++;
                image_service_state=romm_config.base_url[0]?VP_CONNECTION_NOT_CHECKED:VP_CONNECTION_UNCONFIGURED;
                steamgrid_state=steamgrid_api_key[0]?VP_CONNECTION_NOT_CHECKED:VP_CONNECTION_UNCONFIGURED;
            }
        }
        (void)snprintf(app->notice, sizeof(app->notice), "%s actualizado; Triangulo guarda y sincroniza",
                       settings_field == 0 ? "URL" : "Pair code");
    }
ime_finished:
    (void)sceImeDialogTerm();
    memset(settings_ime_buffer,0,sizeof(settings_ime_buffer));
    settings_ime_active = false;
}

static void handle_settings_input(VpApp *app, unsigned int pressed) {
    if (!settings_detail_active) {
        if (pressed & SCE_CTRL_UP && settings_section > 0) {
            settings_section--;
            vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
        }
        if (pressed & SCE_CTRL_DOWN && settings_section < VP_SETTINGS_COUNT-1) {
            settings_section++;
            vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
        }
        if (pressed & SCE_CTRL_CROSS) {
            settings_detail_active = true;
            settings_field = settings_section==VP_SETTINGS_APPEARANCE?ui_preferences.wallpaper:0;
            vp_ui_audio_play(VP_UI_SOUND_ENTER);
        }
        return;
    }
    if (pressed & SCE_CTRL_CIRCLE) {
        if (settings_integration_active) settings_integration_active = false;
        else settings_detail_active = false;
        vp_ui_audio_play(VP_UI_SOUND_BACK);
        return;
    }
    if (settings_section == VP_SETTINGS_APPEARANCE) {
        if (pressed & SCE_CTRL_UP) {
            if (settings_field==VP_UI_APPEARANCE_TEXT_COLOR_SLOT) settings_field=2;
            else if (settings_field==VP_UI_APPEARANCE_MOTION_SLOT) settings_field=VP_UI_APPEARANCE_TEXT_COLOR_SLOT;
            else settings_field=settings_field>=2?settings_field-2:VP_UI_APPEARANCE_MOTION_SLOT;
            vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
        }
        if (pressed & SCE_CTRL_DOWN) {
            if (settings_field<2) settings_field+=2;
            else if (settings_field<VP_UI_WALLPAPER_COUNT) settings_field=VP_UI_APPEARANCE_TEXT_COLOR_SLOT;
            else if (settings_field==VP_UI_APPEARANCE_TEXT_COLOR_SLOT) settings_field=VP_UI_APPEARANCE_MOTION_SLOT;
            else settings_field=0;
            vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
        }
        if (pressed & (SCE_CTRL_LEFT|SCE_CTRL_RIGHT)) {
            if (settings_field<VP_UI_WALLPAPER_COUNT) {
                settings_field^=1;
                vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
            } else if (settings_field==VP_UI_APPEARANCE_TEXT_COLOR_SLOT) {
                VpUiPreferences previous=ui_preferences;
                int delta=(pressed & SCE_CTRL_RIGHT)?1:-1;
                ui_preferences.text_color=(ui_preferences.text_color+delta+VP_UI_TEXT_COLOR_COUNT)%VP_UI_TEXT_COLOR_COUNT;
                if (!save_romm_config()) {
                    ui_preferences=previous;
                    show_notification(app,"No se pudo guardar la apariencia");
                } else vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
            }
        }
        if (pressed & SCE_CTRL_CROSS) {
            VpUiPreferences previous=ui_preferences;
            if (settings_field==VP_UI_APPEARANCE_MOTION_SLOT) ui_preferences.animated=!ui_preferences.animated;
            else if (settings_field==VP_UI_APPEARANCE_TEXT_COLOR_SLOT)
                ui_preferences.text_color=(ui_preferences.text_color+1)%VP_UI_TEXT_COLOR_COUNT;
            else ui_preferences.wallpaper=settings_field;
            if (!save_romm_config()) {
                ui_preferences=previous;
                show_notification(app,"No se pudo guardar la apariencia");
            } else vp_ui_audio_play(VP_UI_SOUND_ENTER);
        }
        return;
    }
    if (settings_section == VP_SETTINGS_DOWNLOADS) {
        VpDownloadEntry *entry;
        if (download_queue.count == 0) return;
        if (download_selection >= download_queue.count) download_selection = download_queue.count - 1;
        if (pressed & SCE_CTRL_UP) {
            download_selection = (download_selection + download_queue.count - 1) % download_queue.count;
            vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
        }
        if (pressed & SCE_CTRL_DOWN) {
            download_selection = (download_selection + 1) % download_queue.count;
            vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
        }
        entry = &download_queue.entries[download_selection];
        if (pressed & SCE_CTRL_CROSS) {
            if (entry->state == VP_DOWNLOAD_FAILED || entry->state == VP_DOWNLOAD_CANCELED ||
                entry->state == VP_DOWNLOAD_PAUSED) {
                bool resumed = entry->state == VP_DOWNLOAD_PAUSED;
                (void)vp_download_queue_retry(&download_queue, entry->item.id);
                (void)start_next_download();
                show_notification(app, "%s descarga: %.110s",
                                  resumed ? "Reanudando" : "Reintentando", entry->item.title);
            } else if (entry->state == VP_DOWNLOAD_PENDING || entry->state == VP_DOWNLOAD_DOWNLOADING) {
                bool active = false;
                (void)vp_download_queue_cancel(&download_queue, entry->item.id);
                pthread_mutex_lock(&background_job.mutex);
                active = background_job.active && background_job.kind == VP_JOB_DOWNLOAD_ROM &&
                         strcmp(background_job.item.id, entry->item.id) == 0;
                if (active) {
                    background_job.cancel_requested = true;
                    background_job.pause_requested = false;
                }
                pthread_mutex_unlock(&background_job.mutex);
                show_notification(app, active ? "Cancelando descarga" : "Descarga cancelada");
                if (!active) (void)start_next_download();
            }
        }
        if (pressed & SCE_CTRL_SQUARE) {
            char item_id[VP_TEXT];
            bool active;
            (void)snprintf(item_id, sizeof(item_id), "%s", entry->item.id);
            pthread_mutex_lock(&background_job.mutex);
            active = background_job.active && background_job.kind == VP_JOB_DOWNLOAD_ROM &&
                     strcmp(background_job.item.id, item_id) == 0;
            if (active) {
                background_job.cancel_requested = true;
                background_job.pause_requested = false;
            }
            pthread_mutex_unlock(&background_job.mutex);
            (void)vp_download_queue_remove(&download_queue, item_id);
            if (download_selection >= download_queue.count && download_queue.count > 0)
                download_selection = download_queue.count - 1;
            show_notification(app, "Descarga eliminada de la cola");
            vp_ui_audio_play(VP_UI_SOUND_BACK);
            if (!active) (void)start_next_download();
        }
        return;
    }
    if (settings_section != VP_SETTINGS_INTEGRATIONS) return;
    if (!settings_integration_active) {
        if (pressed & SCE_CTRL_UP) settings_integration=(settings_integration+VP_INTEGRATION_COUNT-1)%VP_INTEGRATION_COUNT;
        if (pressed & SCE_CTRL_DOWN) settings_integration=(settings_integration+1)%VP_INTEGRATION_COUNT;
        if (pressed & SCE_CTRL_CROSS) {
            settings_integration_active = true;
            settings_field=0;
            if (settings_integration==VP_INTEGRATION_ROMM) request_romm_check();
            else (void)request_user_job(app, settings_integration==VP_INTEGRATION_STEAMGRID?
                VP_JOB_CHECK_STEAMGRID:VP_JOB_CHECK_IMAGES);
            vp_ui_audio_play(VP_UI_SOUND_ENTER);
        }
        return;
    }
    if (settings_integration!=VP_INTEGRATION_ROMM) {
        int count=settings_integration==VP_INTEGRATION_STEAMGRID?3:2;
        if (pressed & SCE_CTRL_UP) { settings_field=(settings_field+count-1)%count; vp_ui_audio_play(VP_UI_SOUND_NAVIGATION); }
        if (pressed & SCE_CTRL_DOWN) { settings_field=(settings_field+1)%count; vp_ui_audio_play(VP_UI_SOUND_NAVIGATION); }
        if (pressed & SCE_CTRL_CROSS) {
            if (settings_field==0) {
                if (!settings_start_ime()) show_notification(app,"No se pudo abrir el teclado de PS Vita");
            } else if (settings_field==1) {
                (void)request_user_job(app, settings_integration==VP_INTEGRATION_STEAMGRID?
                    VP_JOB_CHECK_STEAMGRID:VP_JOB_CHECK_IMAGES);
            } else if (!sync_steamgrid_artwork(app)) {
                show_notification(app,"Configura la API key de SteamGridDB y el host de Image Service");
            }
        }
        return;
    }
    if (pressed & SCE_CTRL_UP) {
        settings_field = (settings_field + 3) % 4;
        vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
    }
    if (pressed & SCE_CTRL_DOWN) {
        settings_field = (settings_field + 1) % 4;
        vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
    }
    if (pressed & SCE_CTRL_CROSS) {
        if (settings_field == 2) {
            if (!sync_romm_catalog(app))
                show_notification(app, "%s — fallido: revisa la configuración",
                    sync_job_title(VP_JOB_SYNC_PROVIDERS));
        } else if (settings_field == 3) {
            if (!sync_cover_metadata(app))
                show_notification(app, "%s — fallido: revisa la configuración",
                    sync_job_title(VP_JOB_SYNC_COVERS));
        } else if (settings_field == 1 && !vp_romm_pair_code_enabled(romm_connection.state)) {

        } else if (!settings_start_ime()) {
            show_notification(app, "No se pudo abrir el teclado de PS Vita");
        }
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
        if (romm_pair_code[0] != '\0') {
            request_romm_pairing(app);
        } else if (!save_romm_config()) {
            show_notification(app, "No se pudo guardar la configuracion");
        } else if (!sync_romm_catalog(app)) {
            show_notification(app, "%s — fallido: revisa la configuración",
                sync_job_title(VP_JOB_SYNC_PROVIDERS));
        }
    }
}

typedef struct {
    char id[VP_TEXT];
    vita2d_texture *texture;
    bool attempted;
} VpCoverSlot;

static VpCoverSlot cover_slots[VP_COVER_SLOTS];
static size_t cover_indexes[VP_COVER_SLOTS];

static void refresh_catalog_view_cache(VpApp *app) {
    if (app == NULL) return;
    if (app->library_level == VP_LEVEL_PLATFORMS) {
        if (catalog_view_cache_dirty || cached_library_level != VP_LEVEL_PLATFORMS) {
            platform_group_count = vp_catalog_library_platforms(&app->catalog,
                                                                 &app->library_filters,
                                                                 platform_groups,
                                                                 VP_MAX_LIBRARY_GROUPS);
            if (app->selected_platform >= platform_group_count)
                app->selected_platform = platform_group_count ? platform_group_count - 1 : 0;
            cached_library_level = VP_LEVEL_PLATFORMS;
            cached_platform[0] = '\0';
            catalog_view_cache_dirty = false;
        }
        return;
    }
    if (catalog_view_cache_dirty || cached_library_level != VP_LEVEL_ROMS ||
        strcmp(cached_platform, app->active_platform) != 0) {
        visible_count_cache = vp_app_visible(app, visible_indexes, VP_MAX_ITEMS);
        cached_library_level = VP_LEVEL_ROMS;
        (void)snprintf(cached_platform, sizeof(cached_platform), "%s", app->active_platform);
        catalog_view_cache_dirty = false;
    }
}

static void safe_id(const char *id, char *destination, size_t capacity) {
    size_t index;
    if (capacity == 0) return;
    for (index = 0; id[index] != '\0' && index + 1 < capacity; index++) {
        char value = id[index];
        destination[index] = ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                              (value >= '0' && value <= '9') || value == '-' || value == '_') ? value : '_';
    }
    destination[index] = '\0';
}

static void cover_path_for(const char *id, char *path, size_t capacity) {
    char safe[VP_TEXT];
    safe_id(id, safe, sizeof(safe));
    (void)snprintf(path, capacity, "ux0:data/vitashelflauncher/cache/covers/fit-v2-%s.png", safe);
}

static void artwork_path_for(const char *id,char *path,size_t capacity) {
    char safe[VP_TEXT];
    safe_id(id,safe,sizeof(safe));
    (void)snprintf(path,capacity,"ux0:data/vitashelflauncher/cache/covers/art-v1-%s.png",safe);
}

static void record_cover_event(const char *event, const char *id, const char *path) {
    const char *log_path = "ux0:data/vitashelflauncher/covers.log";
    SceIoStat info;
    FILE *log;
    /* Bound diagnostics and close each event so it survives a later decoder/GPU fault.
     * Log cache IDs/paths only, never source URLs or bearer tokens. */
    bool rotate = sceIoGetstat(log_path, &info) >= 0 && info.st_size >= 64 * 1024;
    log = fopen(log_path, rotate ? "w" : "a");
    if (log == NULL) return;
    (void)fprintf(log, "time_us=%llu event=%s id=%s path=%s\n",
                  (unsigned long long)sceKernelGetProcessTimeWide(), event, id,
                  path != NULL ? path : "-");
    fclose(log);
}

static VpCoverSlot *find_cover_slot(const char *id) {
    size_t index;
    if (id == NULL || id[0] == '\0') return NULL;
    for (index = 0; index < VP_COVER_SLOTS; index++) {
        if (cover_slots[index].id[0] != '\0' && strcmp(cover_slots[index].id, id) == 0)
            return &cover_slots[index];
    }
    return NULL;
}

static vita2d_texture *cover_texture_for(const char *id) {
    VpCoverSlot *slot = find_cover_slot(id);
    return slot != NULL ? slot->texture : NULL;
}

static VpCoverSlot *acquire_slot(const char *id, const size_t *indexes, size_t count) {
    size_t index;
    size_t other;
    VpCoverSlot *slot = NULL;
    for (index = 0; index < VP_COVER_SLOTS; index++) {
        if (cover_slots[index].id[0] == '\0') {
            slot = &cover_slots[index];
            break;
        }
    }
    if (slot == NULL) {
        for (index = 0; index < VP_COVER_SLOTS; index++) {
            bool visible = false;
            for (other = 0; other < count; other++) {
                if (strcmp(cover_slots[index].id, app.catalog.items[indexes[other]].id) == 0) {
                    visible = true;
                    break;
                }
            }
            if (!visible) {
                slot = &cover_slots[index];
                break;
            }
        }
    }
    if (slot == NULL) slot = &cover_slots[0];
    if (slot->texture != NULL) {
        record_cover_event("evict", slot->id, NULL);
        vita2d_free_texture(slot->texture);
    }
    slot->texture = NULL;
    slot->attempted = false;
    (void)snprintf(slot->id, sizeof(slot->id), "%s", id);
    return slot;
}

static vita2d_texture *load_texture_file(const char *id, const char *path) {
    FILE *file;
    unsigned char header[29];
    size_t read_count;
    long file_size;
    vita2d_texture *texture;
    record_cover_event("load-start", id, path);
    file = fopen(path, "rb");
    if (file == NULL) {
        record_cover_event("load-open-failed", id, path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 || file_size > 512 * 1024 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        record_cover_event("load-size-rejected", id, path);
        return NULL;
    }
    read_count = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (!vp_safe_cover_png(header, read_count) &&
        !((!strncmp(path,"ux0:app/",8) || !strncmp(path,"ur0:appmeta/",12)) &&
          vp_safe_vita_icon_png(header,read_count))) {
        record_cover_event("load-header-rejected", id, path);
        return NULL;
    }
    texture = vita2d_load_PNG_file(path);
    if (texture != NULL)
        vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    record_cover_event(texture != NULL ? "load-ok" : "load-failed", id, path);
    return texture;
}

/* PNG decoding is CPU work on Vita's single core. A dedicated decoder keeps it
 * off the render/preparation thread; the one-slot handoff is owned by this
 * thread, so only the main thread ever creates, frees or installs textures. */
typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool running;
    bool request_pending;
    bool result_ready;
    char request_id[VP_TEXT];
    char request_path[VP_PATH];
    char result_id[VP_TEXT];
    char result_path[VP_PATH];
    vita2d_texture *result_texture;
} VpCoverDecoder;

static VpCoverDecoder cover_decoder = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER
};

static void *cover_decode_thread(void *context) {
    VpCoverDecoder *decoder = context;
    vp_vita_thread_set_priority(VP_VITA_THREAD_PRIORITY_BACKGROUND);
    char id[VP_TEXT];
    char path[VP_PATH];
    for (;;) {
        vita2d_texture *texture;
        pthread_mutex_lock(&decoder->mutex);
        while (decoder->running && !decoder->request_pending)
            (void)pthread_cond_wait(&decoder->condition, &decoder->mutex);
        if (!decoder->running) {
            pthread_mutex_unlock(&decoder->mutex);
            break;
        }
        (void)snprintf(id, sizeof(id), "%s", decoder->request_id);
        (void)snprintf(path, sizeof(path), "%s", decoder->request_path);
        decoder->request_pending = false;
        pthread_mutex_unlock(&decoder->mutex);

        texture = load_texture_file(id, path);

        pthread_mutex_lock(&decoder->mutex);
        if (decoder->result_ready && decoder->result_texture != NULL)
            vita2d_free_texture(decoder->result_texture);
        (void)snprintf(decoder->result_id, sizeof(decoder->result_id), "%s", id);
        (void)snprintf(decoder->result_path, sizeof(decoder->result_path), "%s", path);
        decoder->result_texture = texture;
        decoder->result_ready = true;
        pthread_mutex_unlock(&decoder->mutex);
    }
    return NULL;
}

static bool cover_decoder_init(void) {
    int result;
    pthread_mutex_lock(&cover_decoder.mutex);
    if (cover_decoder.running) {
        pthread_mutex_unlock(&cover_decoder.mutex);
        return true;
    }
    cover_decoder.request_pending = false;
    cover_decoder.result_ready = false;
    cover_decoder.result_texture = NULL;
    cover_decoder.running = true;
    result = pthread_create(&cover_decoder.thread, NULL, cover_decode_thread, &cover_decoder);
    if (result != 0) cover_decoder.running = false;
    pthread_mutex_unlock(&cover_decoder.mutex);
    return result == 0;
}

static void cover_decoder_shutdown(void) {
    bool join = false;
    pthread_mutex_lock(&cover_decoder.mutex);
    if (cover_decoder.running) {
        cover_decoder.running = false;
        (void)pthread_cond_signal(&cover_decoder.condition);
        join = true;
    }
    pthread_mutex_unlock(&cover_decoder.mutex);
    if (join) (void)pthread_join(cover_decoder.thread, NULL);
    pthread_mutex_lock(&cover_decoder.mutex);
    if (cover_decoder.result_texture != NULL)
        vita2d_free_texture(cover_decoder.result_texture);
    cover_decoder.result_texture = NULL;
    cover_decoder.result_ready = false;
    cover_decoder.request_pending = false;
    pthread_mutex_unlock(&cover_decoder.mutex);
}

static bool cover_decode_request(const char *id, const char *path) {
    bool queued = false;
    pthread_mutex_lock(&cover_decoder.mutex);
    if (cover_decoder.running && !cover_decoder.request_pending && !cover_decoder.result_ready) {
        (void)snprintf(cover_decoder.request_id, sizeof(cover_decoder.request_id), "%s", id);
        (void)snprintf(cover_decoder.request_path, sizeof(cover_decoder.request_path), "%s", path);
        cover_decoder.request_pending = true;
        queued = true;
        (void)pthread_cond_signal(&cover_decoder.condition);
    }
    pthread_mutex_unlock(&cover_decoder.mutex);
    return queued;
}

static void cover_decode_collect(void) {
    char id[VP_TEXT];
    char path[VP_PATH];
    vita2d_texture *texture;
    VpCoverSlot *slot;
    pthread_mutex_lock(&cover_decoder.mutex);
    if (!cover_decoder.result_ready) {
        pthread_mutex_unlock(&cover_decoder.mutex);
        return;
    }
    (void)snprintf(id, sizeof(id), "%s", cover_decoder.result_id);
    (void)snprintf(path, sizeof(path), "%s", cover_decoder.result_path);
    texture = cover_decoder.result_texture;
    cover_decoder.result_texture = NULL;
    cover_decoder.result_ready = false;
    pthread_mutex_unlock(&cover_decoder.mutex);

    slot = find_cover_slot(id);
    if (slot != NULL && slot->texture == NULL) {
        slot->texture = texture;
        slot->attempted = true;
        return;
    }
    if (texture != NULL) vita2d_free_texture(texture);
    else if (!strncmp(path,VP_ARTWORK_CACHE "/",strlen(VP_ARTWORK_CACHE "/"))) (void)sceIoRemove(path);
}

static void covers_step(VpApp *app, const size_t *indexes, size_t count) {
    size_t index;
    cover_decode_collect();
    if (count > VP_COVER_SLOTS) count = VP_COVER_SLOTS;
    for (index = 0; index < count; index++) {
        const VpItem *item = &app->catalog.items[indexes[index]];
        VpCoverSlot *slot;
        char path[VP_PATH];
        SceUID cover_fd;
        if (item->cover_url[0] == '\0' && item->source!=VP_SOURCE_LOCAL) continue;
        if (find_cover_slot(item->id) != NULL) continue;
        cover_path_for(item->id, path, sizeof(path));
        /* Probe through Vita I/O without opening a stdio stream just to check
         * for existence. Cached PNGs are decoded by the cover worker. */
        cover_fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (cover_fd<0) {
            char safe[VP_TEXT];
            safe_id(item->id,safe,sizeof(safe));
            (void)snprintf(path,sizeof(path),"ux0:data/vitashelflauncher/cache/covers/processed-%s.png",safe);
            cover_fd=sceIoOpen(path,SCE_O_RDONLY,0);
        }
        if (cover_fd<0 && item->source==VP_SOURCE_LOCAL && item->fs_name[0]) {
            (void)snprintf(path,sizeof(path),"%s",item->fs_name);
            cover_fd=sceIoOpen(path,SCE_O_RDONLY,0);
        }
        if (cover_fd < 0) continue;
        sceIoClose(cover_fd);
        if (!cover_decode_request(item->id, path)) continue;
        slot = acquire_slot(item->id, indexes, count);
        if (slot == NULL) return;
        slot->attempted = true;
        return;
    }
    /* At most one artwork decode after visible covers. A separate cache key
     * prevents it from aliasing the selected game's cover texture. */
    if (app->library_level==VP_LEVEL_ROMS && app->active_view!=VP_VIEW_GRID && app->selected_item<app->catalog.count) {
        const VpItem *item=&app->catalog.items[app->selected_item];
        char id[VP_TEXT],path[VP_PATH];
        (void)snprintf(id,sizeof(id),"art:%.180s",item->id);
        if (item->background_url[0] && !find_cover_slot(id)) {
            SceUID fd;
            artwork_path_for(item->id,path,sizeof(path));
            fd=sceIoOpen(path,SCE_O_RDONLY,0);
            if (fd>=0) {
                sceIoClose(fd);
                if (cover_decode_request(id,path)) {
                    VpCoverSlot *slot=acquire_slot(id,indexes,count);
                    if (slot) slot->attempted=true;
                }
            }
        }
    }
}

static size_t visible_position(const size_t *visible, size_t count, size_t item_index) {
    size_t index;
    for (index = 0; index < count; index++) {
        if (visible[index] == item_index) return index;
    }
    return 0;
}

static void activate_selected(VpApp *app) {
    VpItem *item;
    if (app->catalog.count == 0 || app->selected_item >= app->catalog.count) return;
    item = &app->catalog.items[app->selected_item];
    if (item->source == VP_SOURCE_HOMEBREW) {
        char destination[VP_TEXT];
        char path[VP_PATH];
        safe_id(item->id, destination, sizeof(destination));
        if (!app->installation_armed) {
            app->installation_armed = true;
            installation_armed_at = sceKernelGetProcessTimeWide();
            show_notification(app, "Presiona X otra vez para instalar %.150s", item->title);
            return;
        }
        (void)snprintf(path, sizeof(path), "ux0:data/vitashelflauncher/downloads/%s.vpk", destination);
        if (background_job_start(VP_JOB_INSTALL_HOMEBREW, item, path, NULL))
            show_notification(app, "Instalacion iniciada: %.110s", item->title);
        else
            show_notification(app, "%s — en curso (%s); reintenta cuando termine",
                sync_job_title(VP_JOB_INSTALL_HOMEBREW), background_job_active_label());
        app->installation_armed = false;
        installation_armed_at = 0;
        return;
    }
    if (vp_platform_is_psp(item->platform) && item->installed &&
        item->launch_target[0] != '\0') {
        bool adrenaline_installed = false;
        bool plugin_installed = false;
        bool plugin_enabled = false;
        (void)vp_vita_psp_runtime_status(&adrenaline_installed, &plugin_installed,
                                         &plugin_enabled);
        if (!adrenaline_installed) {
            show_notification(app, "Instala Adrenaline (PSPEMUCFW) para abrir juegos PSP");
            return;
        }
        if (!plugin_installed || !plugin_enabled) {
            if (!app->installation_armed) {
                app->installation_armed = true;
                installation_armed_at = sceKernelGetProcessTimeWide();
                show_notification(app, "Presiona X otra vez para instalar el complemento PSP");
                return;
            }
            const char *error = "No se pudo instalar el complemento PSP";
            if (vp_vita_psp_plugin_install()) {
                (void)vp_vita_psp_runtime_status(NULL, &plugin_installed, &plugin_enabled);
                if (plugin_installed && plugin_enabled) error = NULL;
                else error = vp_vita_last_error();
            } else {
                error = vp_vita_last_error();
            }
            app->installation_armed = false;
            installation_armed_at = 0;
            if (error != NULL) {
                show_notification(app, "%.150s", error);
                return;
            }
            show_notification(app, "Complemento PSP instalado; pulsa X para jugar");
            return;
        }
        if (!vp_vita_write_psp_boot_request(item->launch_target)) {
            show_notification(app, "%.150s", vp_vita_last_error());
            return;
        }
        if (!vp_vita_launch_uri("psgm:play?titleid=PSPEMUCFW")) {
            show_notification(app, "No se pudo abrir Adrenaline; verifica que este instalado");
            return;
        }
        sceKernelExitProcess(0);
        return;
    }
    if (item->installed && item->launch_target[0] != '\0') {
        if (item->source==VP_SOURCE_LOCAL && !strcmp(item->platform,"psvita")) {
            char uri[VP_LAUNCH_URI];
            if (!vp_vita_title_launch_uri(item,uri,sizeof(uri)) || !vp_vita_launch_uri(uri))
                show_notification(app,"No se pudo abrir %.100s; verifica que sigue instalado",item->title);
            else sceKernelExitProcess(0);
            return;
        }
        char rom_key[VP_TEXT];
        const VpLaunchProfile *profile = NULL;
        char uri[VP_LAUNCH_URI];
        if (vp_rom_launch_key(item, rom_key, sizeof(rom_key)))
            profile = vp_launch_config_find(&launch_config, rom_key);
        if (profile == NULL) profile = vp_launch_config_find(&launch_config, item->platform);
        if (profile == NULL && vp_platform_is_arcade(item->platform))
            profile = vp_launch_config_find(&launch_config, "arcade");
        if (profile == NULL) {
            (void)snprintf(app->notice, sizeof(app->notice),
                           "Sin emulador para %.40s; agrega launch_%.40s=<titleid>[:<core>] en config.ini",
                           item->platform, item->platform);
            return;
        }
        if (!vp_build_launch_uri(profile, item->launch_target, uri, sizeof(uri))) {
            (void)snprintf(app->notice, sizeof(app->notice), "No se pudo preparar el lanzamiento");
            return;
        }
        if (!vp_vita_launch_uri(uri)) {
            (void)snprintf(app->notice, sizeof(app->notice),
                           "No se pudo abrir %.32s; verifica que el emulador este instalado",
                           profile->titleid);
            return;
        }
        sceKernelExitProcess(0);
        return;
    }
    if (item->source == VP_SOURCE_ROMM) {
        char path[VP_PATH];
        char partial[VP_PATH + 8];
        if (item->multi_file) {
            (void)snprintf(app->notice, sizeof(app->notice),
                           "Esta ROM tiene varios archivos; descargala desde RomM y extraela en el emulador");
            return;
        }
        if (item->download_url[0] == '\0') {
            (void)snprintf(app->notice, sizeof(app->notice),
                           "RomM no expuso un archivo descargable para esta ROM");
            return;
        }
        if (vp_platform_is_nds(item->platform) && !vp_vita_path_exists(VP_DSVITA_ROOT)) {
            show_notification(app, "Se necesita la carpeta ux0:data/dsvita para descargar juegos NDS");
            return;
        }
        if (!vp_item_install_path(item, VP_DOWNLOAD_ROOT, VP_PSP_ISO_ROOT, VP_DSVITA_ROOT,
                                  path, sizeof(path))) {
            (void)snprintf(app->notice, sizeof(app->notice), "No se pudo preparar el nombre de descarga");
            return;
        }
        (void)snprintf(partial, sizeof(partial), "%s.part", path);
        if (!vp_download_queue_enqueue(&download_queue, item, path, partial)) {
            (void)snprintf(app->notice, sizeof(app->notice), "No se pudo encolar: ya existe o la cola esta llena");
            return;
        }
        (void)start_next_download();
        show_notification(app, "Descarga iniciada: %.110s", item->title);
        vp_ui_audio_play(VP_UI_SOUND_DOWNLOAD_START);
        return;
    }
    (void)snprintf(app->notice, sizeof(app->notice), "Este elemento no tiene un archivo ejecutable");
}

static void refresh_download_queue_progress(void) {
    VpItem item;
    unsigned long long downloaded;
    unsigned long long total;
    bool active;
    pthread_mutex_lock(&background_job.mutex);
    active = background_job.active && background_job.kind == VP_JOB_DOWNLOAD_ROM;
    item = background_job.item;
    downloaded = background_job.downloaded;
    total = background_job.total;
    pthread_mutex_unlock(&background_job.mutex);
    if (active) {
        VpDownloadEntry *entry = vp_download_queue_find(&download_queue, item.id);
        if (entry != NULL && entry->state == VP_DOWNLOAD_DOWNLOADING)
            vp_download_queue_progress(entry, downloaded, total);
    }
}

/* Called only after the previous GPU frame completes, and before starting a scene.
 * Cache preparation may retire textures that the previous frame referenced. */
static void prepare_shell(VpUiFrame *frame, VpUiFonts *font, VpApp *app) {
    size_t group_count = 0;
    size_t visible_count = 0;
    size_t cover_count = 0;
    const VpItem *selected = NULL;
    vita2d_texture *background_texture = NULL;
    size_t index;

    memset(frame, 0, sizeof(*frame));
    refresh_download_queue_progress();
    if (cover_cache_dirty) {
        for (index=0;index<VP_COVER_SLOTS;index++) {
            if (cover_slots[index].texture) vita2d_free_texture(cover_slots[index].texture);
            memset(&cover_slots[index],0,sizeof(cover_slots[index]));
        }
        cover_cache_dirty=false;
    }

    if (app->active_screen == VP_SCREEN_LIBRARY) {
        refresh_catalog_view_cache(app);
        if (app->library_level == VP_LEVEL_PLATFORMS) {
            group_count = platform_group_count;
            if (group_count > 0 && app->selected_platform >= group_count)
                app->selected_platform = group_count - 1;
        } else {
            visible_count = visible_count_cache;
            if (visible_count > 0) selected = &app->catalog.items[app->selected_item];
        }
    }

    frame->touch_scrolling=touch_gesture.dragging || touch_gesture.settling;
    frame->touch_scroll=touch_gesture.position;
    if (app->active_screen==VP_SCREEN_LIBRARY && app->library_level==VP_LEVEL_ROMS &&
        app->active_view==VP_VIEW_LIST) {
        if (!frame->touch_scrolling) {
            if (list_selected!=app->selected_item)
                list_start=vp_ui_list_start(visible_position(visible_indexes,visible_count,app->selected_item),visible_count,list_start);
            else list_start=(size_t)vp_ui_scroll_clamp((float)list_start,visible_count,VP_UI_LIST_PAGE);
        }
        list_selected=app->selected_item;
        frame->list_scroll=frame->touch_scrolling?touch_gesture.position:(float)list_start;
    } else { list_start=0; list_selected=VP_MAX_ITEMS; }
    if (app->active_screen==VP_SCREEN_LIBRARY && app->library_level==VP_LEVEL_ROMS &&
        app->active_view==VP_VIEW_GRID) {
        if (!frame->touch_scrolling) {
            if (grid_selected!=app->selected_item)
                grid_start=vp_ui_grid_start(visible_position(visible_indexes,visible_count,app->selected_item),visible_count,grid_start);
            else grid_start=(size_t)vp_ui_scroll_clamp((float)grid_start,vp_ui_grid_rows(visible_count),VP_UI_GRID_ROWS);
        }
        grid_selected=app->selected_item;
        frame->grid_scroll=frame->touch_scrolling?touch_gesture.position:(float)grid_start;
    } else { grid_start=0; grid_selected=VP_MAX_ITEMS; }
    if (visible_count && app->active_view==VP_VIEW_CAROUSEL)
        frame->carousel_base=frame->touch_scrolling?(int)lroundf(touch_gesture.position):
            (int)visible_position(visible_indexes,visible_count,app->selected_item);

    if (app->active_screen == VP_SCREEN_LIBRARY) {
        if (app->library_level == VP_LEVEL_ROMS) {
            size_t page_size = vp_ui_page_size(app->active_view);
            size_t position;
            size_t start;
            position = visible_position(visible_indexes, visible_count, app->selected_item);
            start = (position / page_size) * page_size;
            if (app->active_view==VP_VIEW_LIST) {
                start=(size_t)floorf(frame->list_scroll);
                page_size=VP_UI_LIST_PAGE+1;
            } else if (app->active_view==VP_VIEW_GRID) {
                start=(size_t)floorf(frame->grid_scroll)*VP_UI_GRID_COLUMNS;
                page_size=VP_UI_GRID_WINDOW;
            }
            if (visible_count && app->active_view == VP_VIEW_CAROUSEL) {
                position=vp_ui_wrap_index(frame->carousel_base,visible_count);
                cover_indexes[cover_count++] = visible_indexes[position];
                for (int distance=1;distance<=VP_UI_CAROUSEL_REACH;distance++) {
                    cover_indexes[cover_count++] = visible_indexes[(position+visible_count-(size_t)distance%visible_count)%visible_count];
                    cover_indexes[cover_count++] = visible_indexes[(position+(size_t)distance)%visible_count];
                }
            } else {
                if (visible_count) cover_indexes[cover_count++] = visible_indexes[position];
                for (index = start; index < visible_count && index < start + page_size; index++)
                    if (index != position)
                        cover_indexes[cover_count++] = visible_indexes[index];
            }
        }
        covers_step(app, cover_indexes, cover_count);
        if (selected != NULL) {
            char artwork_id[VP_TEXT];
            (void)snprintf(artwork_id,sizeof(artwork_id),"art:%.180s",selected->id);
            background_texture = cover_texture_for(artwork_id);
        }
    }

    if (download_queue.count && download_selection >= download_queue.count)
        download_selection = download_queue.count - 1;
    frame->app = app; frame->font = font;
    frame->groups = platform_groups; frame->group_count = group_count;
    frame->visible = visible_indexes; frame->visible_count = visible_count;
    frame->background = background_texture;
    if (visible_count) {
        size_t position = visible_position(visible_indexes, visible_count, app->selected_item);
        size_t page = vp_ui_page_size(app->active_view);
        size_t start = (position / page) * page;
        frame->selected_cover=cover_texture_for(app->catalog.items[app->selected_item].id);
        if (app->active_view == VP_VIEW_CAROUSEL) {
            for (int offset=-VP_UI_CAROUSEL_REACH;offset<=VP_UI_CAROUSEL_REACH;offset++) {
                size_t slot=vp_ui_wrap_index(frame->carousel_base+offset,visible_count);
                frame->covers[offset+VP_UI_CAROUSEL_REACH] = cover_texture_for(app->catalog.items[visible_indexes[slot]].id);
            }
        } else {
            if (app->active_view==VP_VIEW_LIST) {
                start=(size_t)floorf(frame->list_scroll);
                page=VP_UI_LIST_PAGE+1;
            } else if (app->active_view==VP_VIEW_GRID) {
                start=(size_t)floorf(frame->grid_scroll)*VP_UI_GRID_COLUMNS;
                page=VP_UI_GRID_WINDOW;
            }
            for (index = start; index < visible_count && index < start + page; index++)
                frame->covers[index - start] = cover_texture_for(app->catalog.items[visible_indexes[index]].id);
        }
    }
    frame->downloads = &download_queue; frame->download_selection = download_selection;
    frame->library_modal = library_modal; frame->library_modal_row = library_modal_row;
    frame->library_modal_rows = library_modal_rows();
    frame->library_modal_progress = library_modal_progress();
    frame->settings_section = settings_section; frame->settings_field = settings_field;
    frame->preferences = ui_preferences;
    frame->settings_detail = settings_detail_active;
    frame->settings_integration_open = settings_integration_active;
    frame->settings_integration=settings_integration;
    frame->steamgrid_configured=steamgrid_api_key[0]!=0;
    frame->image_service_host=romm_config.image_proxy_url[0]?romm_config.image_proxy_url:romm_config.base_url;
    frame->steamgrid_state=steamgrid_state;
    frame->image_service_state=image_service_state;
    frame->app_version = VP_APP_VERSION;
    frame->romm_catalog_available = catalog_server[0] && !strcmp(catalog_server,romm_config.base_url);
    (void)snprintf(frame->romm_last_sync,sizeof(frame->romm_last_sync),"%s",
                   frame->romm_catalog_available?catalog_synced_at:"");
    frame->romm_url = romm_config.base_url; frame->pair_code = romm_pair_code;
    frame->romm_state = romm_connection.state;
    frame->active_theme = active_theme;
    frame->notice_id = app->notice_id;
    {
        static unsigned long long status_at;
        static char date[32],clock[16];
        static int battery=-1;
        static bool charging;
        static unsigned long long free_space,total_space;
        unsigned long long now=sceKernelGetProcessTimeWide();
        if (!status_at || now-status_at>=1000000) {
            SceDateTime time;
            if (sceRtcGetCurrentClockLocalTime(&time)>=0) {
                static const char *months[]={"Ene","Feb","Mar","Abr","May","Jun","Jul","Ago","Sep","Oct","Nov","Dic"};
                static const char *weekdays[]={"Dom","Lun","Mar","Mié","Jue","Vie","Sáb"};
                int day=sceRtcGetDayOfWeek(time.year,time.month,time.day);
                (void)snprintf(date,sizeof(date),"%s %u %s",weekdays[day>=0 && day<7?day:0],time.day,months[time.month>=1 && time.month<=12?time.month-1:0]);
                (void)snprintf(clock,sizeof(clock),"%02u:%02u",time.hour,time.minute);
            }
            battery=scePowerGetBatteryLifePercent(); charging=scePowerIsBatteryCharging()>0;
            if (app->active_screen==VP_SCREEN_SETTINGS && settings_section==VP_SETTINGS_ABOUT) {
                SceIoDevInfo info={0};
                if (sceIoDevctl("ux0:",0x3001,NULL,0,&info,sizeof(info))>=0) {
                    free_space=info.free_size; total_space=info.max_size;
                }
            }
            status_at=now;
        }
        (void)snprintf(frame->date,sizeof(frame->date),"%s",date);
        (void)snprintf(frame->time,sizeof(frame->time),"%s",clock);
        frame->battery_percent=battery; frame->battery_charging=charging;
        frame->storage_free=free_space; frame->storage_total=total_space;
    }
    pthread_mutex_lock(&background_job.mutex);
    frame->busy = background_job.active;
    frame->sync = sync_status_snapshot(settings_integration);
    pthread_mutex_unlock(&background_job.mutex);
    vp_ui_prepare(frame);
}

static void close_library_modal(void) {
    library_modal = VP_LIBRARY_MODAL_NONE;
    library_modal_row = 0;
    library_modal_opened_at = 0;
}

static size_t arcade_core_index(const VpItem *item) {
    char key[VP_TEXT];
    const VpLaunchProfile *profile;
    const char *path;
    size_t index;
    if (item == NULL || !vp_rom_launch_key(item, key, sizeof(key))) return 0;
    profile = vp_launch_config_find(&launch_config, key);
    path = profile != NULL ? profile->core : vp_platform_default_core(item->platform);
    if (path == NULL || path[0] == '\0') return 0;
    for (index = 0; index < vp_arcade_core_count(); index++) {
        const VpArcadeCore *core = vp_arcade_core(index);
        if (core != NULL && strcmp(core->core, path) == 0) return index;
    }
    return 0;
}

static void open_library_modal(VpLibraryModal modal) {
    library_modal = modal;
    library_modal_row = modal == VP_LIBRARY_MODAL_CORES && app.selected_item < app.catalog.count
                            ? arcade_core_index(&app.catalog.items[app.selected_item]) : 0;
    library_modal_opened_at = sceKernelGetProcessTimeWide();
}

static float library_modal_progress(void) {
    unsigned long long elapsed;
    if (library_modal == VP_LIBRARY_MODAL_NONE || library_modal_opened_at == 0) return 1.f;
    elapsed = sceKernelGetProcessTimeWide() - library_modal_opened_at;
    return vp_ui_ease(fminf(1.f, (float)elapsed / 220000.f));
}

static void persist_library_preferences(void) {
    if (!save_library_preferences(&app))
        show_notification(&app, "No se pudo guardar la biblioteca");
}

static void handle_library_modal_input(unsigned int pressed) {
    int rows = library_modal_rows();
    if (rows <= 0) return;
    if (pressed & SCE_CTRL_UP) {
        library_modal_row = (library_modal_row + (size_t)rows - 1) % (size_t)rows;
        vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
    }
    if (pressed & SCE_CTRL_DOWN) {
        library_modal_row = (library_modal_row + 1) % (size_t)rows;
        vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
    }
    if (pressed & SCE_CTRL_CIRCLE) {
        if (library_modal == VP_LIBRARY_MODAL_CORES) {
            open_library_modal(VP_LIBRARY_MODAL_ACTIONS);
            library_modal_row = 2;
            vp_ui_audio_play(VP_UI_SOUND_BACK);
            return;
        }
        close_library_modal();
        vp_ui_audio_play(VP_UI_SOUND_BACK);
        return;
    }
    if ((library_modal == VP_LIBRARY_MODAL_ACTIONS && (pressed & SCE_CTRL_SQUARE)) ||
        (library_modal == VP_LIBRARY_MODAL_FILTERS && (pressed & SCE_CTRL_SELECT))) {
        close_library_modal();
        vp_ui_audio_play(VP_UI_SOUND_BACK);
        return;
    }
    if (!(pressed & SCE_CTRL_CROSS)) return;
    if (library_modal == VP_LIBRARY_MODAL_ACTIONS) {
        refresh_catalog_view_cache(&app);
        VpItem *item = app.selected_item < app.catalog.count ?
                        &app.catalog.items[app.selected_item] : NULL;
        if (visible_count_cache == 0 || item == NULL) {
            close_library_modal();
            return;
        }
        if (library_modal_row == 0) vp_item_toggle_favorite(item);
        else if (library_modal_row == 1) vp_item_toggle_hidden(item);
        else if (library_modal_row == 2 && vp_platform_is_arcade(item->platform)) {
            open_library_modal(VP_LIBRARY_MODAL_CORES);
            vp_ui_audio_play(VP_UI_SOUND_ENTER);
            return;
        }
        persist_library_preferences();
        catalog_view_cache_dirty = true;
        refresh_catalog_view_cache(&app);
    } else if (library_modal == VP_LIBRARY_MODAL_CORES) {
        VpItem *item = app.selected_item < app.catalog.count ?
                        &app.catalog.items[app.selected_item] : NULL;
        const VpArcadeCore *core = vp_arcade_core(library_modal_row);
        char key[VP_TEXT];
        if (visible_count_cache == 0 || item == NULL || core == NULL ||
            !vp_rom_launch_key(item, key, sizeof(key))) {
            close_library_modal();
            return;
        }
        if (core->core[0] == '\0') {
            (void)vp_launch_config_remove(&launch_config, key);
            if (!save_romm_config())
                show_notification(&app, "No se pudo guardar el núcleo arcade");
            else
                show_notification(&app, "Núcleo arcade: automático");
        } else {
            char value[VP_TEXT];
            (void)snprintf(value, sizeof(value), "RETROVITA:%s", core->core);
            if (vp_launch_config_set(&launch_config, key, value) && save_romm_config())
                show_notification(&app, "Núcleo arcade: %s", core->label);
            else
                show_notification(&app, "No se pudo guardar el núcleo arcade");
        }
        close_library_modal();
        vp_ui_audio_play(VP_UI_SOUND_ENTER);
        return;
    } else {
        if (library_modal_row == 0) app.library_filters.show_hidden = !app.library_filters.show_hidden;
        else if (library_modal_row == 1) app.library_filters.downloaded_only = !app.library_filters.downloaded_only;
        persist_library_preferences();
        catalog_view_cache_dirty = true;
        refresh_catalog_view_cache(&app);
    }
    close_library_modal();
    vp_ui_audio_play(VP_UI_SOUND_ENTER);
}

static unsigned int touch_navigation_pressed(int x, int y) {
    int slot;
    int screen;
    if (library_modal != VP_LIBRARY_MODAL_NONE) {
        float progress = library_modal_progress();
        VpUiRect panel = vp_ui_library_panel(progress);
        int hint_y = panel.y + panel.h - 38;
        slot = vp_ui_touch_library_panel_row(progress, x, y, library_modal,
                                             library_modal_rows());
        if (slot >= 0) {
            library_modal_row = (size_t)slot;
            return SCE_CTRL_CROSS;
        }
        if (y>=hint_y-18 && y<hint_y+18) {
            if (x>=panel.x+146 && x<panel.x+274) return SCE_CTRL_CROSS;
            if (x>=panel.x+286 && x<panel.x+416) return SCE_CTRL_CIRCLE;
        }
        if (vp_ui_touch_back(x, y)) return SCE_CTRL_CIRCLE;
        return 0;
    }
    screen = vp_ui_touch_dock_screen(x, y);
    if (screen >= 0) {
        if (settings_ime_active) return 0;
        if (app.active_screen != (VpScreen)screen) {
            app.active_screen = (VpScreen)screen;
            vp_ui_audio_play(VP_UI_SOUND_TAB);
        }
        return 0;
    }
    if (app.active_screen == VP_SCREEN_LIBRARY) {
        refresh_catalog_view_cache(&app);
        if (app.library_level == VP_LEVEL_PLATFORMS) {
            size_t target;
            if (y>=492 && y<534 && x>=350 && x<595) return SCE_CTRL_CROSS;
            if (y>=492 && y<534 && x>=745 && x<880) return SCE_CTRL_SELECT;
            slot = vp_ui_touch_platform(x, y);
            if (slot < 0) return 0;
            target = (size_t)slot;
            if (target >= platform_group_count) return 0;
            if (target != app.selected_platform) {
                app.selected_platform = target;
                vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
                return 0;
            }
            return SCE_CTRL_CROSS;
        }
        if (y>=492 && y<534 && x>=345 && x<475) return SCE_CTRL_CROSS;
        if (y>=492 && y<534 && x>=475 && x<557) return SCE_CTRL_TRIANGLE;
        if (y>=492 && y<534 && x>=557 && x<681) return SCE_CTRL_SQUARE;
        if (y>=492 && y<534 && x>=681 && x<810) return SCE_CTRL_SELECT;
        if (y>=492 && y<534 && x>=810 && x<905) return SCE_CTRL_CIRCLE;
        if (app.active_view == VP_VIEW_CAROUSEL) {
            int offset=vp_ui_touch_carousel_offset(x,y);
            if (offset<0) return SCE_CTRL_LEFT;
            if (offset>0 && offset<99) return SCE_CTRL_RIGHT;
            if (!offset) return SCE_CTRL_CROSS;
            return 0;
        }
        slot = app.active_view == VP_VIEW_GRID ? vp_ui_touch_tile_slot(x, y) : vp_ui_touch_list_slot(x, y);
        if (slot >= 0) {
            size_t start=app.active_view==VP_VIEW_LIST?list_start:grid_start*VP_UI_GRID_COLUMNS;
            size_t target=start+(size_t)slot;
            if (target >= visible_count_cache) return 0;
            if (visible_indexes[target] != app.selected_item) {
                app.selected_item = visible_indexes[target];
                vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
                return 0;
            }
            return SCE_CTRL_CROSS;
        }
        return 0;
    }
    if (app.active_screen != VP_SCREEN_SETTINGS) return 0;
    if (y>=492 && y<534 && x>=350 && x<595) return SCE_CTRL_CROSS;
    if (settings_detail_active && settings_section==VP_SETTINGS_DOWNLOADS && y>=492 && y<534 && x>=700 && x<804) return SCE_CTRL_SQUARE;
    if (settings_detail_active && vp_ui_touch_back(x, y)) return SCE_CTRL_CIRCLE;
    if (vp_ui_touch_view_toggle(x,y) && settings_detail_active && settings_integration_active && settings_integration==VP_INTEGRATION_ROMM && settings_section==VP_SETTINGS_INTEGRATIONS) return SCE_CTRL_TRIANGLE;
    if (!settings_detail_active) {
        slot=vp_ui_touch_settings_slot(x,y);
        if (slot>=0) { settings_section=slot; return SCE_CTRL_CROSS; }
        return 0;
    }
    if (settings_section == VP_SETTINGS_APPEARANCE) {
        slot=vp_ui_touch_appearance_slot(x,y);
        if (slot>=0) { settings_field=slot; return SCE_CTRL_CROSS; }
    } else if (settings_section == VP_SETTINGS_INTEGRATIONS && !settings_integration_active) {
        slot=vp_ui_touch_settings_slot(x,y);
        if (slot>=0 && slot<VP_INTEGRATION_COUNT) { settings_integration=slot; return SCE_CTRL_CROSS; }
    } else if (settings_section == VP_SETTINGS_INTEGRATIONS) {
        int count=settings_integration==VP_INTEGRATION_ROMM?4:(settings_integration==VP_INTEGRATION_STEAMGRID?3:2);
        for (slot = 0; slot < count; slot++) {
            int row_y = 112 + slot * 72;
            if (x >= 310 && x < 910 && y >= row_y && y < row_y + 62) {
                settings_field = slot;
                return SCE_CTRL_CROSS;
            }
        }
    } else if (settings_section == VP_SETTINGS_DOWNLOADS) {
        for (slot = 0; slot < 4 && (size_t)slot < download_queue.count; slot++) {
            int row_y = 120 + slot * 76;
            if (x >= 310 && x < 910 && y >= row_y && y < row_y + 68) {
                download_selection = (download_selection / 4) * 4 + (size_t)slot;
                return SCE_CTRL_CROSS;
            }
        }
    }
    return 0;
}

static int touch_coordinate_x(int raw) { return raw * 960 / VP_TOUCH_WIDTH; }
static int touch_coordinate_y(int raw) { return raw * 544 / VP_TOUCH_HEIGHT; }

static bool touch_drag_allowed(int raw_x, int raw_y, VpTouchAxis axis) {
    int x=touch_coordinate_x(raw_x),y=touch_coordinate_y(raw_y);
    if (app.active_screen!=VP_SCREEN_LIBRARY) return false;
    if (app.library_level==VP_LEVEL_PLATFORMS)
        return axis==VP_TOUCH_AXIS_HORIZONTAL && x>=24 && x<944 && y>=210 && y<444;
    if (app.active_view==VP_VIEW_LIST)
        return axis==VP_TOUCH_AXIS_VERTICAL && x>=48 && x<356 && y>=166 && y<440;
    if (app.active_view==VP_VIEW_GRID)
        return axis==VP_TOUCH_AXIS_VERTICAL && x>=24 && x<944 && y>=148 && y<480;
    return app.active_view==VP_VIEW_CAROUSEL && axis==VP_TOUCH_AXIS_HORIZONTAL &&
        x>=24 && x<936 && y>=150 && y<410;
}

static bool touch_context_matches(const VpTouchGesture *gesture) {
    size_t count=app.library_level==VP_LEVEL_PLATFORMS?platform_group_count:visible_count_cache;
    size_t selected=app.library_level==VP_LEVEL_PLATFORMS?app.selected_platform:app.selected_item;
    return app.active_screen==VP_SCREEN_LIBRARY && app.library_level==gesture->level &&
        app.active_view==gesture->view && !strcmp(app.active_platform,gesture->platform) &&
        count==gesture->count && selected==gesture->selected;
}

static void touch_scroll_tick(VpTouchGesture *gesture, unsigned long long now) {
    if (!gesture->dragging && !gesture->settling) return;
    if (!touch_context_matches(gesture)) {
        gesture->dragging=gesture->settling=false;
        gesture->cancelled=true;
        return;
    }
    if (gesture->settling) {
        float progress=now>=gesture->released_at?(float)(now-gesture->released_at)/180000.f:0;
        gesture->position=gesture->from_position+(gesture->target-gesture->from_position)*vp_ui_ease(progress);
        if (progress>=1) {
            gesture->position=gesture->target;
            gesture->settling=false;
        }
    }
}

static void touch_scroll_release(VpTouchGesture *gesture) {
    size_t before=app.library_level==VP_LEVEL_PLATFORMS?app.selected_platform:
        visible_position(visible_indexes,visible_count_cache,app.selected_item);
    size_t target=before;
    float delta=gesture->position-gesture->origin;
    gesture->target=roundf(gesture->position);
    if (app.library_level==VP_LEVEL_PLATFORMS) {
        target=(size_t)vp_ui_scroll_clamp((float)before+roundf(delta),gesture->count,1);
        gesture->target=(float)vp_ui_console_start(target,gesture->count);
    } else if (app.active_view==VP_VIEW_LIST) {
        list_start=(size_t)gesture->target;
    } else if (app.active_view==VP_VIEW_GRID) {
        grid_start=(size_t)gesture->target;
    } else target=vp_ui_wrap_index((int)gesture->target,gesture->count);
    if (target!=before) {
        int columns=app.library_level==VP_LEVEL_PLATFORMS?(int)gesture->count:1;
        vp_app_move(&app,(int)target-(int)before,0,columns);
        vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
    }
    gesture->selected=app.library_level==VP_LEVEL_PLATFORMS?app.selected_platform:app.selected_item;
    gesture->from_position=gesture->position;
    gesture->released_at=sceKernelGetProcessTimeWide();
    gesture->dragging=false;
    gesture->settling=true;
}

static unsigned int touch_gesture_pressed(SceTouchData *touch, VpTouchGesture *gesture) {
    if (touch->reportNum > 0) {
        int x = (int)touch->report[0].x;
        int y = (int)touch->report[0].y;
        if (!gesture->active) {
            bool interrupted=gesture->settling;
            float current=gesture->position;
            memset(gesture,0,sizeof(*gesture));
            gesture->start_x = x;
            gesture->start_y = y;
            gesture->active = true;
            gesture->moved=interrupted;
            gesture->level=app.library_level;
            gesture->view=app.active_view;
            gesture->count=app.library_level==VP_LEVEL_PLATFORMS?platform_group_count:visible_count_cache;
            gesture->selected=app.library_level==VP_LEVEL_PLATFORMS?app.selected_platform:app.selected_item;
            (void)snprintf(gesture->platform,sizeof(gesture->platform),"%s",app.active_platform);
            gesture->origin=interrupted?current:
                (app.library_level==VP_LEVEL_ROMS && app.active_view==VP_VIEW_LIST?
                 (float)list_start:(app.library_level==VP_LEVEL_ROMS && app.active_view==VP_VIEW_GRID?
                 (float)grid_start:vp_ui_scroll_origin(app.library_level)));
        }
        gesture->last_x = x;
        gesture->last_y = y;
        if (touch->reportNum!=1) gesture->cancelled=true;
        if (gesture->cancelled) gesture->dragging=false;
        if (!gesture->dragging && !gesture->cancelled &&
            (abs(x-gesture->start_x)>VP_TOUCH_SWIPE_SLOP || abs(y-gesture->start_y)>VP_TOUCH_SWIPE_SLOP)) {
            VpTouchAxis axis=abs(x-gesture->start_x)>=abs(y-gesture->start_y)?
                VP_TOUCH_AXIS_HORIZONTAL:VP_TOUCH_AXIS_VERTICAL;
            gesture->moved=true;
            if (gesture->count && touch_drag_allowed(gesture->start_x,gesture->start_y,axis) && touch_context_matches(gesture)) {
                gesture->dragging=true;
                gesture->axis=axis;
            } else gesture->cancelled=true;
        }
        if (gesture->dragging && !gesture->cancelled) {
            int delta=gesture->axis==VP_TOUCH_AXIS_HORIZONTAL?
                touch_coordinate_x(x-gesture->start_x):touch_coordinate_y(y-gesture->start_y);
            float stride=app.library_level==VP_LEVEL_PLATFORMS?148.f:
                (app.active_view==VP_VIEW_LIST?54.f:(app.active_view==VP_VIEW_GRID?160.f:132.f));
            size_t page=app.library_level==VP_LEVEL_PLATFORMS?VP_UI_CONSOLE_PAGE:
                (app.active_view==VP_VIEW_LIST?VP_UI_LIST_PAGE:(app.active_view==VP_VIEW_GRID?VP_UI_GRID_ROWS:1));
            size_t count=app.library_level==VP_LEVEL_ROMS && app.active_view==VP_VIEW_GRID?
                vp_ui_grid_rows(gesture->count):gesture->count;
            float position=gesture->origin-(float)delta/stride;
            if (app.library_level==VP_LEVEL_ROMS && app.active_view==VP_VIEW_CAROUSEL)
                gesture->position=count>1?position:0;
            else gesture->position=vp_ui_scroll_clamp(position,count,page);
        }
        return 0;
    }
    if (!gesture->active) return 0;
    gesture->active = false;
    if (gesture->cancelled) {
        gesture->dragging=false;
        return 0;
    }
    if (gesture->dragging) {
        if (touch_context_matches(gesture)) touch_scroll_release(gesture);
        else gesture->dragging=false;
        return 0;
    }
    if (gesture->moved || abs(gesture->last_x-gesture->start_x)>VP_TOUCH_TAP_SLOP ||
        abs(gesture->last_y-gesture->start_y)>VP_TOUCH_TAP_SLOP)
        return 0;
    return touch_navigation_pressed(touch_coordinate_x(gesture->last_x),
                                     touch_coordinate_y(gesture->last_y));
}

static void handle_app_events(VpApp *app, bool *quit) {
    /* Never let a non-draining event source starve input and rendering.
     * Vita3K's unimplemented receiver can return success without an event. */
    for (unsigned int polled = 0; polled < 16; polled++) {
        SceAppMgrSystemEvent event = {0};
        if (sceAppMgrReceiveSystemEvent(&event) != 0) return;
        switch (event.systemEvent) {
        case SCE_APPMGR_SYSTEMEVENT_ON_DEACTIVATE:
            (void)pause_active_download(app, false);
            break;
        case SCE_APPMGR_SYSTEMEVENT_ON_RESUME: {
            bool scan_active;
            pthread_mutex_lock(&background_job.mutex);
            scan_active = background_job.active && background_job.kind == VP_JOB_SCAN_LOCAL;
            pthread_mutex_unlock(&background_job.mutex);
            if (!scan_active && !pending_user_job.active) local_scan_pending=true;
            size_t index;
            bool paused = false;
            for (index = 0; index < download_queue.count; index++) {
                if (download_queue.entries[index].state == VP_DOWNLOAD_PAUSED) {
                    paused = true;
                    break;
                }
            }
            if (paused)
                (void)snprintf(app->notice, sizeof(app->notice), "Descarga en pausa; X para reanudar");
            break;
        }
        case SCE_APPMGR_SYSTEMEVENT_REQUEST_QUIT:
            (void)pause_active_download(app, true);
            *quit = true;
            return;
        default:
            return;
        }
    }
}

int main(void) {
    SceCtrlData pad;
    SceTouchData touch;
    unsigned int previous = 0;
    VpUiFonts fonts;
    VpUiFonts *font=&fonts;
    bool curl_cleanup_allowed;
    bool request_quit = false;
    bool rendered_once = false;
    vp_app_init(&app);
    vp_download_queue_init(&download_queue);
    vp_launch_config_init(&launch_config);
    if (setenv("OPENSSL_CONF", "app0:openssl.cnf", 1) != 0) return 1;
    /* Initialize libcurl before creating any background worker. Letting the first
     * network job initialize it lazily is unsafe on Vita's libcurl build. */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    (void)sceIoMkdir("ux0:data", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher/cache", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher/cache/covers", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher/downloads", 0777);
    (void)sceIoMkdir("ux0:pspemu", 0777);
    (void)sceIoMkdir(VP_PSP_ISO_ROOT, 0777);
    load_providers(&app);
    steamgrid_state=steamgrid_api_key[0]?VP_CONNECTION_NOT_CHECKED:VP_CONNECTION_UNCONFIGURED;
    image_service_state=romm_config.image_proxy_url[0] || romm_config.base_url[0]?
        VP_CONNECTION_NOT_CHECKED:VP_CONNECTION_UNCONFIGURED;
    request_romm_check();
    (void)load_catalog_cache(&app, &romm_config);
    (void)load_library_preferences(&app);
    refresh_downloaded_items(&app);
    vita2d_init();
    if (!vp_ui_fonts_load(font)) { vita2d_fini(); return 1; }
    (void)vp_ui_backdrop_init();
    (void)cover_decoder_init();
    (void)vp_ui_audio_init();
    (void)sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    (void)sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    for (;;) {
        unsigned int pressed;
        VpUiFrame frame;
        if (settings_ime_active) {
            (void)vita2d_common_dialog_update();
            settings_finish_ime(&app);
            background_job_poll(&app);
            if (!start_pending_user_job() && romm_pairing.active) (void)start_pending_romm_pairing();
            if (settings_ime_active) {
                vita2d_swap_buffers();
                continue;
            }
        }
        handle_app_events(&app, &request_quit);
        if (request_quit) break;
        background_job_poll(&app);
        {
            bool user_job_started = start_pending_user_job();
            if (user_job_started) {
                /* the explicit request owns the slot just released by the poll */
            } else if (romm_pairing.active) {
                (void)start_pending_romm_pairing();
            } else if (!pending_user_job.active) {
                if (local_scan_pending)
                    (void)background_job_start(VP_JOB_SCAN_LOCAL,NULL,NULL,NULL);
                if (romm_check_pending)
                    (void)background_job_start(VP_JOB_VALIDATE_ROMM, NULL, NULL, NULL);
            }
        }
        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~previous;
        previous = pad.buttons;
        if (pressed || catalog_view_cache_dirty) {
            touch_gesture.dragging=touch_gesture.settling=false;
            if (touch_gesture.active) touch_gesture.cancelled=true;
        }
        refresh_catalog_view_cache(&app);
        touch_scroll_tick(&touch_gesture,sceKernelGetProcessTimeWide());
        memset(&touch, 0, sizeof(touch));
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0)
            pressed |= touch_gesture_pressed(&touch, &touch_gesture);
        else {
            touch_gesture.dragging=touch_gesture.settling=false;
            touch_gesture.cancelled=true;
        }
        if (pressed & SCE_CTRL_START) {
            (void)pause_active_download(&app, true);
            break;
        }
        if (app.installation_armed &&
            sceKernelGetProcessTimeWide() - installation_armed_at >= 4000000ULL) {
            app.installation_armed = false;
            installation_armed_at = 0;
        }
        if (library_modal != VP_LIBRARY_MODAL_NONE) {
            handle_library_modal_input(pressed);
        } else if (app.active_screen == VP_SCREEN_SETTINGS) {
            handle_settings_input(&app, pressed);
            if (!settings_ime_active && !settings_detail_active && (pressed & SCE_CTRL_LTRIGGER)) {
                vp_app_cycle_screen(&app, -1);
                vp_ui_audio_play(VP_UI_SOUND_TAB);
            }
            if (!settings_ime_active && !settings_detail_active && (pressed & SCE_CTRL_RTRIGGER)) {
                vp_app_cycle_screen(&app, 1);
                vp_ui_audio_play(VP_UI_SOUND_TAB);
            }
        } else {
            if (pressed & SCE_CTRL_LTRIGGER) {
                vp_app_cycle_screen(&app, -1);
                vp_ui_audio_play(VP_UI_SOUND_TAB);
            }
            if (pressed & SCE_CTRL_RTRIGGER) {
                vp_app_cycle_screen(&app, 1);
                vp_ui_audio_play(VP_UI_SOUND_TAB);
            }
            if (app.active_screen == VP_SCREEN_LIBRARY) {
                int columns = app.library_level == VP_LEVEL_PLATFORMS
                                  ? (int)(platform_group_count?platform_group_count:1)
                                  : (app.active_view == VP_VIEW_GRID ? 5 : 1);
                if (pressed & SCE_CTRL_UP) {
                    size_t selected_platform = app.selected_platform;
                    size_t selected_item = app.selected_item;
                    vp_app_move(&app, 0, -1, columns);
                    if (selected_platform != app.selected_platform || selected_item != app.selected_item)
                        vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
                }
                if (pressed & SCE_CTRL_DOWN) {
                    size_t selected_platform = app.selected_platform;
                    size_t selected_item = app.selected_item;
                    vp_app_move(&app, 0, 1, columns);
                    if (selected_platform != app.selected_platform || selected_item != app.selected_item)
                        vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
                }
                if (pressed & SCE_CTRL_LEFT) {
                    size_t selected_platform = app.selected_platform;
                    size_t selected_item = app.selected_item;
                    vp_app_move(&app, -1, 0, columns);
                    if (selected_platform != app.selected_platform || selected_item != app.selected_item)
                        vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
                }
                if (pressed & SCE_CTRL_RIGHT) {
                    size_t selected_platform = app.selected_platform;
                    size_t selected_item = app.selected_item;
                    vp_app_move(&app, 1, 0, columns);
                    if (selected_platform != app.selected_platform || selected_item != app.selected_item)
                        vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
                }
                if (app.library_level != VP_LEVEL_ROMS)
                    pressed &= ~SCE_CTRL_TRIANGLE;
                if (pressed & SCE_CTRL_TRIANGLE) {
                    VpView previous = app.active_view;
                    vp_app_cycle_view(&app);
                    if (!save_romm_config()) {
                        app.active_view = previous;
                        show_notification(&app, "No se pudo guardar la vista");
                    }
                }
                if (pressed & SCE_CTRL_SELECT) {
                    open_library_modal(VP_LIBRARY_MODAL_FILTERS);
                    pressed = 0;
                    vp_ui_audio_play(VP_UI_SOUND_ENTER);
                }
                if ((pressed & SCE_CTRL_SQUARE) && app.library_level == VP_LEVEL_ROMS &&
                    visible_count_cache > 0 && app.selected_item < app.catalog.count) {
                    open_library_modal(VP_LIBRARY_MODAL_ACTIONS);
                    pressed = 0;
                    vp_ui_audio_play(VP_UI_SOUND_ENTER);
                }
                if (pressed & SCE_CTRL_CIRCLE) {
                    vp_app_back(&app);
                    vp_ui_audio_play(VP_UI_SOUND_BACK);
                }
                if (pressed & SCE_CTRL_CROSS) {
                    if (app.library_level == VP_LEVEL_PLATFORMS) {
                        vp_app_open(&app);
                        vp_ui_audio_play(VP_UI_SOUND_ENTER);
                    }
                    else activate_selected(&app);
                }
            }
        }
        /* swap_buffers queues presentation; it is not a fence for texture retirement
         * or the temporary vertex pool that start_drawing resets. Skip the fence on
         * the first frame: no rendering is in flight yet, and calling it there
         * crashes Vita3K before the first draw. */
        if (rendered_once) vita2d_wait_rendering_done();
        prepare_shell(&frame, font, &app);
        vita2d_start_drawing();
        vita2d_clear_screen();
        vp_ui_render(&frame);
        vita2d_end_drawing();
        vita2d_swap_buffers();
        rendered_once = true;
    }
    vp_ui_shutdown();
    vp_ui_audio_shutdown();
    cover_decoder_shutdown();
    {
        size_t index;
        for (index = 0; index < VP_COVER_SLOTS; index++)
            if (cover_slots[index].texture) vita2d_free_texture(cover_slots[index].texture);
    }
    vp_ui_fonts_free(font);
    vita2d_fini();
    pthread_mutex_lock(&background_job.mutex);
    curl_cleanup_allowed = !background_job.active;
    pthread_mutex_unlock(&background_job.mutex);
    if (curl_cleanup_allowed) curl_global_cleanup();
    sceKernelExitProcess(0);
    return 0;
}
