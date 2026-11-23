#ifndef VITASHELF_LAUNCHER_H
#define VITASHELF_LAUNCHER_H

#include <stdbool.h>
#include <stddef.h>

#define VP_MAX_ITEMS 256
#define VP_MAX_PLATFORMS 64
#define VP_MAX_LIBRARY_GROUPS (VP_MAX_PLATFORMS + 1)
#define VP_FAVORITES_PLATFORM "__favorites__"
#define VP_FAVORITES_DISPLAY "Favoritos"
#define VP_TEXT 192
#define VP_URL 384
#define VP_PATH 256
#define VP_MAX_QUERY 80
#define VP_MAX_LAUNCH_PROFILES 32
#define VP_LAUNCH_URI 768
#define VP_DOWNLOAD_EXTENSION 16
#define VP_MAX_DOWNLOAD_QUEUE 16
/* A single RomM ROM object already weighs 17-30 KiB with metadata; leave headroom so a
 * metadata-heavy entry does not overflow the fixed core response buffer. */
#define VP_FETCH_RESPONSE_SIZE (256 * 1024)

typedef enum {
    VP_SOURCE_LOCAL,
    VP_SOURCE_ROMM,
    VP_SOURCE_HOMEBREW,
    VP_SOURCE_OFFICIAL_SHORTCUT
} VpSource;

typedef enum {
    VP_KIND_GAME,
    VP_KIND_ROM,
    VP_KIND_APP,
    VP_KIND_STORE
} VpKind;

typedef enum {
    VP_VIEW_GRID,
    VP_VIEW_LIST,
    VP_VIEW_CAROUSEL
} VpView;

typedef enum {
    VP_SCREEN_LIBRARY,
    VP_SCREEN_STORES,
    VP_SCREEN_SETTINGS
} VpScreen;

typedef enum {
    VP_LEVEL_PLATFORMS,
    VP_LEVEL_ROMS
} VpLibraryLevel;

typedef struct {
    char id[VP_TEXT];
    char title[VP_TEXT];
    char platform[VP_TEXT];
    char platform_display[VP_TEXT];
    char description[VP_TEXT];
    char launch_target[VP_PATH];
    char remote_id[32];
    char fs_name[VP_TEXT];
    char download_url[VP_URL];
    char cover_url[VP_URL];
    VpSource source;
    VpKind kind;
    bool installed;
    bool favorite;
    bool multi_file;
    unsigned long last_played;
    char background_url[VP_URL];
    bool hidden;
} VpItem;

typedef struct {
    bool show_hidden;
    bool downloaded_only;
} VpLibraryFilters;

typedef enum {
    VP_DOWNLOAD_PENDING,
    VP_DOWNLOAD_DOWNLOADING,
    VP_DOWNLOAD_COMPLETED,
    VP_DOWNLOAD_FAILED,
    VP_DOWNLOAD_CANCELED,
    VP_DOWNLOAD_PAUSED
} VpDownloadState;

typedef struct {
    VpItem item;
    char destination[VP_PATH];
    char partial[VP_PATH + 8];
    char error[VP_TEXT];
    VpDownloadState state;
    unsigned int percent;
} VpDownloadEntry;

typedef struct {
    VpDownloadEntry entries[VP_MAX_DOWNLOAD_QUEUE];
    size_t count;
} VpDownloadQueue;

typedef struct {
    VpItem items[VP_MAX_ITEMS];
    size_t count;
} VpCatalog;

typedef struct {
    char platform[VP_TEXT];
    char display[VP_TEXT];
    size_t count;
} VpPlatformGroup;

typedef struct {
    char id[VP_TEXT];
    char name[VP_TEXT];
    char background[16];
    char surface[16];
    char accent[16];
    char text[16];
    char muted[16];
    VpView view;
    int card_width;
    int card_height;
} VpTheme;

typedef struct {
    VpCatalog catalog;
    VpTheme theme;
    char query[VP_MAX_QUERY];
    VpLibraryFilters library_filters;
    VpView active_view;
    VpScreen active_screen;
    VpLibraryLevel library_level;
    char active_platform[VP_TEXT];
    size_t selected_platform;
    size_t selected_item;
    bool installation_armed;
    char notice[VP_TEXT];
    unsigned int notice_id;
} VpApp;

typedef bool (*VpFetchFn)(const char *url, const char *bearer_token,
                          char *response, size_t response_capacity, void *context);

typedef struct {
    VpFetchFn execute;
    void *context;
} VpFetchBackend;

typedef bool (*VpRommPairFn)(const char *url, const char *code,
                             char *response, size_t response_capacity, void *context);

typedef struct {
    VpRommPairFn execute;
    void *context;
} VpRommPairBackend;

typedef struct {
    char base_url[VP_URL];
    char token[VP_TEXT];
    char image_proxy_url[VP_URL];
} VpRommConfig;

typedef enum {
    VP_ROMM_UNCONFIGURED,
    VP_ROMM_CHECKING,
    VP_ROMM_CONNECTED,
    VP_ROMM_INVALID_TOKEN,
    VP_ROMM_FORBIDDEN,
    VP_ROMM_UNREACHABLE
} VpRommConnectionState;

/* HTTP status of a complete response, or zero for transport/truncation failures. */
typedef long (*VpRommProbeFn)(const char *url, const char *token, char *response,
                              size_t capacity, void *context);
typedef struct {
    VpRommProbeFn execute;
    void *context;
} VpRommProbeBackend;

/* Owned by the UI thread. Workers return a result tagged with its revision. */
typedef struct {
    unsigned long revision;
    VpRommConnectionState state;
} VpRommConnection;

typedef struct {
    char catalog_url[VP_URL];
} VpHomebrewConfig;

typedef struct {
    char platform[VP_TEXT];
    char titleid[32];
    char core[VP_PATH];
} VpLaunchProfile;

typedef struct {
    VpLaunchProfile profiles[VP_MAX_LAUNCH_PROFILES];
    size_t count;
} VpLaunchConfig;

typedef enum {
    VP_TRANSFER_DOWNLOAD,
    VP_TRANSFER_INSTALL_VPK
} VpTransferAction;

typedef bool (*VpTransferFn)(const char *url, const char *bearer_token, const char *destination,
                             VpTransferAction action, void *context);


typedef struct {
    VpTransferFn execute;
    void *context;
} VpTransferBackend;

typedef bool (*VpCoverTransformFn)(const char *source_url, const char *bearer_token,
                                   const char *service_url, const char *destination, void *context);

typedef struct {
    VpCoverTransformFn execute;
    void *context;
} VpCoverTransformBackend;

void vp_app_init(VpApp *app);
void vp_app_cycle_screen(VpApp *app, int direction);
void vp_app_cycle_view(VpApp *app);
bool vp_view_from_string(const char *value, VpView *view);
const char *vp_view_name(VpView view);
void vp_app_select(VpApp *app, int direction);
void vp_app_move(VpApp *app, int dx, int dy, int columns);
void vp_app_open(VpApp *app);
void vp_app_back(VpApp *app);
size_t vp_app_visible(VpApp *app, size_t *indexes, size_t capacity);
const char *vp_screen_name(VpScreen screen);
void vp_catalog_init(VpCatalog *catalog);
VpItem *vp_catalog_add(VpCatalog *catalog, const VpItem *item);
VpItem *vp_catalog_find(VpCatalog *catalog, const char *id);
size_t vp_catalog_search(const VpCatalog *catalog, const char *query,
                         size_t *indexes, size_t capacity);
size_t vp_catalog_recent(const VpCatalog *catalog, size_t *indexes, size_t capacity);
size_t vp_catalog_platforms(const VpCatalog *catalog, VpPlatformGroup *groups, size_t capacity);
size_t vp_catalog_library_platforms(const VpCatalog *catalog, const VpLibraryFilters *filters,
                                    VpPlatformGroup *groups, size_t capacity);
size_t vp_catalog_platform_items(const VpCatalog *catalog, const char *platform,
                                 size_t *indexes, size_t capacity);
size_t vp_catalog_library_items(const VpCatalog *catalog, const char *platform,
                                const VpLibraryFilters *filters, size_t *indexes, size_t capacity);
bool vp_library_item_visible(const VpItem *item, const VpLibraryFilters *filters);
void vp_item_toggle_favorite(VpItem *item);
void vp_item_toggle_hidden(VpItem *item);
void vp_item_mark_played(VpItem *item, unsigned long timestamp);

void vp_download_queue_init(VpDownloadQueue *queue);
VpDownloadEntry *vp_download_queue_find(VpDownloadQueue *queue, const char *item_id);
bool vp_download_queue_enqueue(VpDownloadQueue *queue, const VpItem *item,
                               const char *destination, const char *partial);
VpDownloadEntry *vp_download_queue_start_next(VpDownloadQueue *queue);
void vp_download_queue_progress(VpDownloadEntry *entry,
                                unsigned long long downloaded, unsigned long long total);
void vp_download_queue_finish(VpDownloadEntry *entry, bool succeeded, const char *error);
bool vp_download_queue_cancel(VpDownloadQueue *queue, const char *item_id);
bool vp_download_queue_pause(VpDownloadQueue *queue, const char *item_id);
bool vp_download_queue_retry(VpDownloadQueue *queue, const char *item_id);
bool vp_download_queue_remove(VpDownloadQueue *queue, const char *item_id);

bool vp_theme_load(VpTheme *theme, const char *ini_text);
const char *vp_source_name(VpSource source);

void vp_launch_config_init(VpLaunchConfig *config);
bool vp_launch_config_set(VpLaunchConfig *config, const char *platform, const char *value);
const VpLaunchProfile *vp_launch_config_find(const VpLaunchConfig *config,
                                             const char *platform);
bool vp_item_extension(const VpItem *item, char *destination, size_t capacity);
bool vp_item_download_path(const VpItem *item, const char *directory,
                           char *destination, size_t capacity);
bool vp_build_launch_uri(const VpLaunchProfile *profile, const char *rom_path,
                         char *destination, size_t capacity);

/* Percent-encodes bytes that RFC 3986 forbids in a URL (spaces, quotes, braces, controls,
 * non-ASCII). Existing %XX sequences are preserved so URLs are never double encoded.
 * Returns false when the escaped result does not fit in destination. */
bool vp_url_escape(const char *url, char *destination, size_t capacity);
bool vp_libretro_thumbnail_url(const VpItem *item, char *destination, size_t capacity);
bool vp_safe_cover_png(const unsigned char *data, size_t size);

bool vp_local_import_manifest(VpCatalog *catalog, const char *manifest);
bool vp_romm_import(VpCatalog *catalog, const char *json);
bool vp_romm_exchange_pair_code(VpRommConfig *config, const char *code,
                                VpRommPairBackend backend);
VpRommConnectionState vp_romm_check_connection(const VpRommConfig *config,
                                               VpRommProbeBackend backend);
void vp_romm_connection_reset(VpRommConnection *connection, const VpRommConfig *config);
bool vp_romm_connection_apply(VpRommConnection *connection, unsigned long revision,
                              VpRommConnectionState state);
bool vp_romm_pair_code_enabled(VpRommConnectionState state);
bool vp_romm_sync(VpCatalog *catalog, const VpRommConfig *config,
                  VpFetchBackend backend);
bool vp_cache_cover(const VpItem *item, const VpRommConfig *config, const char *destination,
                    VpCoverTransformBackend backend);
bool vp_homebrew_import(VpCatalog *catalog, const char *json);
bool vp_homebrew_sync(VpCatalog *catalog, const VpHomebrewConfig *config,
                      VpFetchBackend backend);
bool vp_queue_download(const VpItem *item, const VpRommConfig *config, const char *destination,
                       VpTransferBackend backend);
bool vp_install_homebrew(const VpItem *item, const char *destination,
                         VpTransferBackend backend);

#endif
