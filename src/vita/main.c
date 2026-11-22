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
#include "vita_thread_priority.h"

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
#define VP_CATALOG_CACHE_VERSION 2U
#define VP_TOUCH_WIDTH 1920
#define VP_TOUCH_HEIGHT 1088
#define VP_TOUCH_TAP_SLOP 32
static VpRommConfig romm_config;
static VpHomebrewConfig homebrew_config;
static VpLaunchConfig launch_config;
static VpApp app;
static char active_theme[VP_TEXT] = "default";
static char romm_pair_code[VP_TEXT];
static VpRommConnection romm_connection;
static bool romm_check_pending;
static int settings_section;
static int settings_field;
static size_t download_selection;
static bool settings_detail_active;
static bool settings_romm_active;
static char catalog_server[VP_URL];
static char catalog_synced_at[32];
static bool settings_ime_active;
static SceWChar16 settings_ime_buffer[VP_URL];
static unsigned long long installation_armed_at;

typedef enum {
    VP_JOB_NONE,
    VP_JOB_SYNC_PROVIDERS,
    VP_JOB_PAIR_AND_SYNC,
    VP_JOB_VALIDATE_ROMM,
    VP_JOB_DOWNLOAD_ROM,
    VP_JOB_INSTALL_HOMEBREW,
    VP_JOB_SYNC_COVERS
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
} VpBackgroundJob;

/* The core fetch buffer and backend diagnostics are deliberately single-owner. The background
 * worker keeps network work off the UI thread; cover PNG decoding has its own worker. */
static VpBackgroundJob background_job = {
    .mutex = PTHREAD_MUTEX_INITIALIZER
};
static VpDownloadQueue download_queue;
#define VP_COVER_SLOTS 12
static VpPlatformGroup platform_groups[VP_MAX_PLATFORMS];
static size_t visible_indexes[VP_MAX_ITEMS];
static size_t platform_group_count;
static size_t visible_count_cache;
static VpLibraryLevel cached_library_level;
static char cached_platform[VP_TEXT];
static bool catalog_view_cache_dirty = true;
static bool cover_cache_dirty;

static bool download_progress(unsigned long long downloaded, unsigned long long total, void *context);
static void cover_path_for(const char *id, char *path, size_t capacity);
static void artwork_path_for(const char *id, char *path, size_t capacity);

static void show_notification(VpApp *app, const char *format, ...) {
    va_list arguments;
    if (app == NULL) return;
    va_start(arguments, format);
    (void)vsnprintf(app->notice, sizeof(app->notice), format, arguments);
    va_end(arguments);
    app->notice_id++;
}

static void request_romm_check(void) {
    if (romm_config.base_url[0] == '\0' || romm_config.token[0] == '\0') {
        romm_connection.state = VP_ROMM_UNCONFIGURED;
        romm_check_pending = false;
    } else if (romm_connection.state != VP_ROMM_CHECKING) {
        romm_connection.state = VP_ROMM_CHECKING;
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
            (version == VP_CATALOG_CACHE_VERSION || version == 1U) &&
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
    FILE *file = fopen(VP_CONFIG_PATH, "r");
    char line[512];
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
        else if (strcmp(line, "homebrew_catalog") == 0)
            (void)snprintf(homebrew_config.catalog_url, sizeof(homebrew_config.catalog_url), "%s", equals);
        else if (strcmp(line, "active_theme") == 0)
            (void)snprintf(active_theme, sizeof(active_theme), "%s", equals);
        else if (strncmp(line, "launch_", 7) == 0)
            (void)vp_launch_config_set(&launch_config, line + 7, equals);
    }
    fclose(file);
    if (strcmp(active_theme, "default") != 0) {
        char path[VP_PATH];
        (void)snprintf(path, sizeof(path), "ux0:data/vitashelflauncher/themes/%s/theme.ini", active_theme);
        load_theme_file(app, path);
    }
}

static bool save_romm_config(void) {
    FILE *file;
    size_t index;
    (void)sceIoMkdir("ux0:data", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher", 0777);
    file = fopen(VP_CONFIG_PATH, "w");
    if (file == NULL) return false;
    (void)fprintf(file, "romm_url=%s\nromm_token=%s\nimage_proxy_url=%s\nhomebrew_catalog=%s\nactive_theme=%s\n",
                   romm_config.base_url, romm_config.token, romm_config.image_proxy_url,
                   homebrew_config.catalog_url, active_theme);
    for (index = 0; index < launch_config.count; index++) {
        const VpLaunchProfile *profile = &launch_config.profiles[index];
        (void)fprintf(file, "launch_%s=%s:%s\n", profile->platform, profile->titleid,
                      profile->core);
    }
    return fclose(file) == 0;
}

static bool file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static void refresh_downloaded_items(VpApp *app) {
    size_t index;
    if (app == NULL) return;
    for (index = 0; index < app->catalog.count; index++) {
        VpItem *item = &app->catalog.items[index];
        char path[VP_PATH];
        if (item->source != VP_SOURCE_ROMM) continue;
        if (item->multi_file ||
            !vp_item_download_path(item, "ux0:data/vitashelflauncher/downloads", path, sizeof(path)) ||
            !file_exists(path)) {
            item->installed = false;
            item->launch_target[0] = '\0';
            continue;
        }
        item->installed = true;
        (void)snprintf(item->launch_target, sizeof(item->launch_target), "%s", path);
    }
}

static void apply_provider_catalog(VpApp *app, const VpCatalog *catalog) {
    size_t index = 0;
    if (app == NULL || catalog == NULL) return;
    while (index < app->catalog.count) {
        if (app->catalog.items[index].source == VP_SOURCE_ROMM ||
            app->catalog.items[index].source == VP_SOURCE_HOMEBREW) {
            app->catalog.items[index] = app->catalog.items[app->catalog.count - 1];
            app->catalog.count--;
        } else {
            index++;
        }
    }
    for (index = 0; index < catalog->count; index++)
        (void)vp_catalog_add(&app->catalog, &catalog->items[index]);
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
                        vp_romm_sync(&job->catalog, &config, romm_backend);
        }
        if (homebrew.catalog_url[0] != '\0') {
            homebrew_requested = true;
            succeeded = vp_homebrew_sync(&job->catalog, &homebrew, fetch_backend) &&
                        (!romm_requested || succeeded);
        }
        if (!romm_requested && !homebrew_requested) succeeded = false;
        if (succeeded) (void)save_catalog_cache(&job->catalog, &config);
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
            if (cover->source != VP_SOURCE_ROMM || cover->cover_url[0] == '\0') continue;
            cover_path_for(cover->id, destination, sizeof(destination));
            if (snprintf(partial, sizeof(partial), "%s.tmp", destination) >= (int)sizeof(partial)) {
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
            if (cover->background_url[0]!='\0') {
                VpItem artwork=*cover;
                (void)snprintf(artwork.cover_url,sizeof(artwork.cover_url),"%s",cover->background_url);
                artwork_path_for(cover->id,destination,sizeof(destination));
                (void)snprintf(partial,sizeof(partial),"%s.tmp",destination);
                if (vp_cache_cover(&artwork,&config,partial,artwork_backend)) {
                    (void)sceIoRemove(destination);
                    if (sceIoRename(partial,destination)<0) (void)sceIoRemove(partial);
                }
            }
            pthread_mutex_lock(&job->mutex);
            job->covers_completed++;
            pthread_mutex_unlock(&job->mutex);
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
    }
finish:
    (void)snprintf(error, sizeof(error), "%s", succeeded ? "" : vp_vita_last_error());

    pthread_mutex_lock(&job->mutex);
    if (job->token_exchanged ||
        ((kind == VP_JOB_SYNC_PROVIDERS || kind == VP_JOB_PAIR_AND_SYNC) && succeeded)) {
        job->romm = config;
    }
    job->succeeded = succeeded;
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
    background_job.homebrew = homebrew_config;
    (void)snprintf(background_job.pair_code, sizeof(background_job.pair_code), "%s", romm_pair_code);
    memset(&background_job.item, 0, sizeof(background_job.item));
    if (item != NULL) background_job.item = *item;
    (void)snprintf(background_job.destination, sizeof(background_job.destination), "%s",
                   destination != NULL ? destination : "");
    (void)snprintf(background_job.partial, sizeof(background_job.partial), "%s",
                   partial != NULL ? partial : "");
    background_job.active = true;
    result = pthread_create(&background_job.thread, NULL, background_job_run, &background_job);
    if (result != 0) background_job.active = false;
    pthread_mutex_unlock(&background_job.mutex);
    if (result == 0 && (kind == VP_JOB_VALIDATE_ROMM || kind == VP_JOB_SYNC_PROVIDERS ||
                         kind == VP_JOB_PAIR_AND_SYNC)) {
        romm_connection.state = kind == VP_JOB_PAIR_AND_SYNC ||
                                (romm_config.base_url[0] && romm_config.token[0])
                                    ? VP_ROMM_CHECKING : VP_ROMM_UNCONFIGURED;
        romm_check_pending = false;
    }
    return result == 0;
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
                show_notification(app, "Token recibido, pero no se pudo guardar la configuracion");
                (void)start_next_download();
                return;
            }
        }
        if (kind == VP_JOB_VALIDATE_ROMM) {
            (void)start_next_download();
            return;
        }
    }

    if (!succeeded && kind != VP_JOB_DOWNLOAD_ROM) {
        show_notification(app, "Operacion fallida: %.140s", error);
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
        show_notification(app, "Biblioteca sincronizada: %u juegos", (unsigned)app->catalog.count);
    } else if (kind == VP_JOB_SYNC_COVERS) {
        show_notification(app, "Metadata sincronizada: %u/%u caratulas",
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
            (void)snprintf(completed->launch_target, sizeof(completed->launch_target), "%s",
                           destination);
        }
        show_notification(app, "Descarga finalizada: %.120s", item.title);
        vp_ui_audio_play(VP_UI_SOUND_DOWNLOAD_COMPLETE);
        (void)start_next_download();
    } else if (kind == VP_JOB_INSTALL_HOMEBREW) {
        VpItem *completed = vp_catalog_find(&app->catalog, item.id);
        if (completed != NULL) completed->installed = true;
        show_notification(app, "Instalacion finalizada; revisa LiveArea");
    }
    if (kind != VP_JOB_DOWNLOAD_ROM) (void)start_next_download();
}

static bool sync_romm_catalog(VpApp *app) {
    (void)app;
    if ((romm_config.base_url[0] == '\0' || romm_config.token[0] == '\0') &&
        homebrew_config.catalog_url[0] == '\0') return false;
    return background_job_start(VP_JOB_SYNC_PROVIDERS, NULL, NULL, NULL);
}

static bool sync_cover_metadata(VpApp *app) {
    size_t index;
    int result;
    if (app == NULL || romm_config.base_url[0] == '\0' || romm_config.token[0] == '\0') return false;
    pthread_mutex_lock(&background_job.mutex);
    if (background_job.active) {
        pthread_mutex_unlock(&background_job.mutex);
        return false;
    }
    background_job.kind = VP_JOB_SYNC_COVERS;
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
    background_job.homebrew = homebrew_config;
    background_job.catalog = app->catalog;
    for (index = 0; index < background_job.catalog.count; index++) {
        const VpItem *item = &background_job.catalog.items[index];
        if (item->source == VP_SOURCE_ROMM && item->cover_url[0] != '\0')
            background_job.covers_total++;
    }
    if (background_job.covers_total == 0) {
        pthread_mutex_unlock(&background_job.mutex);
        return false;
    }
    background_job.active = true;
    result = pthread_create(&background_job.thread, NULL, background_job_run, &background_job);
    if (result != 0) background_job.active = false;
    pthread_mutex_unlock(&background_job.mutex);
    return result == 0;
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
    const char *value = settings_field == 0 ? romm_config.base_url : romm_pair_code;
    size_t capacity = settings_field == 0 ? sizeof(romm_config.base_url) : sizeof(romm_pair_code);
    if (settings_field > 1) return false;
    if (settings_field == 1 && !vp_romm_pair_code_enabled(romm_connection.state)) return false;
    settings_text_to_wide(value, settings_ime_buffer, sizeof(settings_ime_buffer) / sizeof(settings_ime_buffer[0]));
    sceImeDialogParamInit(&param);
    param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_SPANISH;
    param.type = settings_field == 0 ? SCE_IME_TYPE_URL : SCE_IME_TYPE_BASIC_LATIN;
    param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param.title = settings_field == 0 ? (const SceWChar16 *)u"URL de RomM" : (const SceWChar16 *)u"Pair code de RomM";
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
        char *value = settings_field == 0 ? romm_config.base_url : romm_pair_code;
        size_t capacity = settings_field == 0 ? sizeof(romm_config.base_url) : sizeof(romm_pair_code);
        char previous[VP_URL];
        (void)snprintf(previous, sizeof(previous), "%s", value);
        settings_wide_to_text(settings_ime_buffer, value, capacity);
        if (settings_field == 0 && strcmp(previous, value) != 0) {
            vp_romm_connection_reset(&romm_connection, &romm_config);
            romm_check_pending = romm_connection.state == VP_ROMM_CHECKING;
            memset(romm_pair_code, 0, sizeof(romm_pair_code));
        }
        (void)snprintf(app->notice, sizeof(app->notice), "%s actualizado; Triangulo guarda y sincroniza",
                       settings_field == 0 ? "URL" : "Pair code");
    }
    (void)sceImeDialogTerm();
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
            vp_ui_audio_play(VP_UI_SOUND_ENTER);
        }
        return;
    }
    if (pressed & SCE_CTRL_CIRCLE) {
        if (settings_romm_active) settings_romm_active = false;
        else settings_detail_active = false;
        vp_ui_audio_play(VP_UI_SOUND_BACK);
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
    if (!settings_romm_active) {
        if (pressed & SCE_CTRL_CROSS) {
            settings_romm_active = true;
            request_romm_check();
            vp_ui_audio_play(VP_UI_SOUND_ENTER);
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
            if (sync_romm_catalog(app))
                show_notification(app, "Sincronizacion de catalogo iniciada");
            else
                show_notification(app, "No se pudo iniciar la sincronizacion");
        } else if (settings_field == 3) {
            if (sync_cover_metadata(app))
                show_notification(app, "Sincronizacion de metadata iniciada");
            else
                show_notification(app, "No se pudo iniciar metadata; revisa RomM");
        } else if (settings_field == 1 && !vp_romm_pair_code_enabled(romm_connection.state)) {

        } else if (!settings_start_ime()) {
            show_notification(app, "No se pudo abrir el teclado de PS Vita");
        }
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
        if (romm_pair_code[0] != '\0' && vp_romm_pair_code_enabled(romm_connection.state)) {
            if (background_job_start(VP_JOB_PAIR_AND_SYNC, NULL, NULL, NULL))
                show_notification(app, "Canjeando codigo y sincronizando catalogo");
            else
                show_notification(app, "Hay una operacion en curso; espera a que termine");
        } else if (!save_romm_config()) {
            show_notification(app, "No se pudo guardar la configuracion");
        } else if (sync_romm_catalog(app)) {
            show_notification(app, "Sincronizacion de catalogo iniciada");
        } else {
            show_notification(app, "No se pudo iniciar la sincronizacion");
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
            platform_group_count = vp_catalog_platforms(&app->catalog, platform_groups,
                                                        VP_MAX_PLATFORMS);
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
    if (!vp_safe_cover_png(header, read_count)) {
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
    else if (path[0] != '\0') (void)sceIoRemove(path);
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
        if (item->cover_url[0] == '\0') continue;
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
            show_notification(app, "Hay una operacion en curso; espera a que termine");
        app->installation_armed = false;
        installation_armed_at = 0;
        return;
    }
    if (item->installed && item->launch_target[0] != '\0') {
        const VpLaunchProfile *profile = vp_launch_config_find(&launch_config, item->platform);
        char uri[VP_LAUNCH_URI];
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
        if (!vp_item_download_path(item, "ux0:data/vitashelflauncher/downloads", path, sizeof(path))) {
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

    if (app->active_screen == VP_SCREEN_LIBRARY) {
        if (app->library_level == VP_LEVEL_ROMS) {
            size_t page_size = vp_ui_page_size(app->active_view);
            size_t position;
            size_t start;
            position = visible_position(visible_indexes, visible_count, app->selected_item);
            start = (position / page_size) * page_size;
            if (visible_count && app->active_view == VP_VIEW_CAROUSEL) {
                cover_indexes[cover_count++] = visible_indexes[position];
                for (int distance=1;distance<=3;distance++) {
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
        if (app->active_view == VP_VIEW_CAROUSEL) {
            for (int offset=-3;offset<=3;offset++) {
                size_t slot=(position+visible_count+(size_t)((offset+(int)visible_count)%(int)visible_count))%visible_count;
                frame->covers[offset+3] = cover_texture_for(app->catalog.items[visible_indexes[slot]].id);
            }
        } else {
            for (index = start; index < visible_count && index < start + page; index++)
                frame->covers[index - start] = cover_texture_for(app->catalog.items[visible_indexes[index]].id);
        }
    }
    frame->downloads = &download_queue; frame->download_selection = download_selection;
    frame->settings_section = settings_section; frame->settings_field = settings_field;
    frame->settings_detail = settings_detail_active;
    frame->settings_romm = settings_romm_active;
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
    pthread_mutex_unlock(&background_job.mutex);
    vp_ui_prepare(frame);
}

static unsigned int touch_navigation_pressed(int x, int y) {
    int slot;
    int screen = vp_ui_touch_dock_screen(x, y);
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
        if (y>=492 && y<534 && x>=350 && x<595) return SCE_CTRL_CROSS;
        if (app.library_level == VP_LEVEL_PLATFORMS) {
            size_t target;
            slot = vp_ui_touch_console_slot(x, y);
            if (slot < 0) return 0;
            target = vp_ui_console_start(app.selected_platform,platform_group_count)+(size_t)slot;
            if (target >= platform_group_count) return 0;
            if (target != app.selected_platform) {
                app.selected_platform = target;
                vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);
                return 0;
            }
            return SCE_CTRL_CROSS;
        }
        if (vp_ui_touch_back(x, y)) return SCE_CTRL_CIRCLE;
        if (vp_ui_touch_view_toggle(x, y)) return SCE_CTRL_TRIANGLE;
        if (y>=492 && y<534 && x>=700 && x<804) return SCE_CTRL_SQUARE;
        if (app.active_view == VP_VIEW_CAROUSEL) {
            int offset=vp_ui_touch_carousel_offset(x,y);
            if (offset<0) return SCE_CTRL_LEFT;
            if (offset>0 && offset<99) return SCE_CTRL_RIGHT;
            if (!offset) return SCE_CTRL_CROSS;
            return 0;
        }
        slot = app.active_view == VP_VIEW_GRID ? vp_ui_touch_tile_slot(x, y) : vp_ui_touch_list_slot(x, y);
        if (slot >= 0) {
            size_t page = vp_ui_page_size(app.active_view);
            size_t position = visible_position(visible_indexes, visible_count_cache, app.selected_item);
            size_t target = (position / page) * page + (size_t)slot;
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
    if (vp_ui_touch_view_toggle(x,y) && settings_detail_active && settings_romm_active && settings_section==VP_SETTINGS_INTEGRATIONS) return SCE_CTRL_TRIANGLE;
    if (!settings_detail_active) {
        slot=vp_ui_touch_settings_slot(x,y);
        if (slot>=0) { settings_section=slot; return SCE_CTRL_CROSS; }
        return 0;
    }
    if (settings_section == VP_SETTINGS_INTEGRATIONS && !settings_romm_active) {
        if (vp_ui_touch_settings_slot(x,y)==0) return SCE_CTRL_CROSS;
    } else if (settings_section == VP_SETTINGS_INTEGRATIONS) {
        for (slot = 0; slot < 4; slot++) {
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

static unsigned int touch_tap_pressed(SceTouchData *touch, bool *active,
                                      int *start_x, int *start_y, int *last_x, int *last_y) {
    if (touch->reportNum > 0) {
        int x = (int)touch->report[0].x;
        int y = (int)touch->report[0].y;
        if (!*active) {
            *start_x = x;
            *start_y = y;
            *active = true;
        }
        *last_x = x;
        *last_y = y;
        return 0;
    }
    if (!*active) return 0;
    *active = false;
    if (abs(*last_x - *start_x) > VP_TOUCH_TAP_SLOP || abs(*last_y - *start_y) > VP_TOUCH_TAP_SLOP)
        return 0;
    return touch_navigation_pressed(*last_x * 960 / VP_TOUCH_WIDTH,
                                    *last_y * 544 / VP_TOUCH_HEIGHT);
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
    bool touch_active = false;
    int touch_start_x = 0, touch_start_y = 0, touch_last_x = 0, touch_last_y = 0;
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
    load_providers(&app);
    request_romm_check();
    (void)load_catalog_cache(&app, &romm_config);
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
            if (settings_ime_active) {
                vita2d_swap_buffers();
                continue;
            }
        }
        handle_app_events(&app, &request_quit);
        if (request_quit) break;
        background_job_poll(&app);
        if (romm_check_pending)
            (void)background_job_start(VP_JOB_VALIDATE_ROMM, NULL, NULL, NULL);
        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~previous;
        previous = pad.buttons;
        memset(&touch, 0, sizeof(touch));
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0)
            pressed |= touch_tap_pressed(&touch, &touch_active, &touch_start_x, &touch_start_y,
                                         &touch_last_x, &touch_last_y);
        if (pressed & SCE_CTRL_START) {
            (void)pause_active_download(&app, true);
            break;
        }
        if (app.installation_armed &&
            sceKernelGetProcessTimeWide() - installation_armed_at >= 4000000ULL) {
            app.installation_armed = false;
            installation_armed_at = 0;
        }
        if (app.active_screen == VP_SCREEN_SETTINGS) {
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
                if (pressed & SCE_CTRL_TRIANGLE) vp_app_cycle_view(&app);
                if ((pressed & SCE_CTRL_SQUARE) && app.library_level == VP_LEVEL_ROMS &&
                    app.selected_item < app.catalog.count)
                    vp_item_toggle_favorite(&app.catalog.items[app.selected_item]);
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
