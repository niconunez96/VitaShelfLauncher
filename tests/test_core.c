#include "vitashelflauncher.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int calls;
    VpTransferAction last_action;
    char url[VP_URL];
    char bearer[VP_TEXT];
    char destination[VP_PATH];
} TransferLog;

typedef struct {
    int calls;
    char source_url[VP_URL];
    char bearer[VP_TEXT];
    char service_url[VP_URL];
    char destination[VP_PATH];
} CoverTransformLog;

typedef struct {
    int calls;
    char url[VP_URL];
    char urls[4][VP_URL];
    char bearer[VP_TEXT];
    const char *payload;
    const char *pages[4];
    size_t page_count;
    int fail_at_call;
} FetchLog;

typedef struct {
    int calls;
    char url[VP_URL];
    char code[VP_TEXT];
    const char *response;
    bool succeed;
} PairLog;

static bool fake_pair(const char *url, const char *code,
                      char *response, size_t response_capacity, void *context) {
    PairLog *log = context;
    log->calls++;
    (void)snprintf(log->url, sizeof(log->url), "%s", url);
    (void)snprintf(log->code, sizeof(log->code), "%s", code);
    if (!log->succeed) return false;
    (void)snprintf(response, response_capacity, "%s", log->response);
    return true;
}

static bool fake_fetch(const char *url, const char *bearer_token,
                       char *response, size_t response_capacity, void *context) {
    FetchLog *log = context;
    const char *payload = log->payload != NULL ? log->payload :
        "{\"total\":1,\"items\":[{\"id\":99,\"name\":\"F-Zero\",\"platform_slug\":\"snes\","
        "\"url_cover\":\"/assets/fzero.png\",\"download_url\":\"/api/file/fzero.sfc\"}]}";
    if ((size_t)log->calls < log->page_count) payload = log->pages[log->calls];
    if (log->fail_at_call > 0 && log->calls == log->fail_at_call) {
        log->calls++;
        return false;
    }
    log->calls++;
    (void)snprintf(log->url, sizeof(log->url), "%s", url);
    if (log->calls <= (int)(sizeof(log->urls) / sizeof(log->urls[0])))
        (void)snprintf(log->urls[log->calls - 1], sizeof(log->urls[0]), "%s", url);
    (void)snprintf(log->bearer, sizeof(log->bearer), "%s", bearer_token);
    (void)snprintf(response, response_capacity, "%s", payload);
    return true;
}

typedef struct {
    FetchLog fetch;
    long status;
} ProbeLog;

static long fake_probe(const char *url, const char *token, char *response,
                       size_t capacity, void *context) {
    ProbeLog *log = context;
    (void)fake_fetch(url, token, response, capacity, &log->fetch);
    return log->status;
}

static void test_romm_connection(void) {
    ProbeLog log = {.status = 200};
    VpRommProbeBackend backend = {fake_probe, &log};
    VpRommConfig config = {0}, before;
    VpRommConnection connection = {0};
    unsigned long revision;

    assert(vp_romm_check_connection(&config, backend) == VP_ROMM_UNCONFIGURED);
    assert(log.fetch.calls == 0);
    strcpy(config.base_url, "https://romm.local/");
    assert(vp_romm_check_connection(&config, backend) == VP_ROMM_UNCONFIGURED);
    strcpy(config.token, "rmm_existing");
    before = config;
    assert(vp_romm_check_connection(&config, backend) == VP_ROMM_CONNECTED);
    assert(strcmp(log.fetch.url, "https://romm.local/api/roms?limit=1&offset=0&with_char_index=false&with_filter_values=false&with_rom_id_index=false") == 0);
    assert(strcmp(log.fetch.bearer, config.token) == 0);
    log.fetch.payload = "{\"total\":0,\"items\":[]}";
    assert(vp_romm_check_connection(&config, backend) == VP_ROMM_CONNECTED);
    log.status = 401;
    assert(vp_romm_check_connection(&config, backend) == VP_ROMM_INVALID_TOKEN);
    log.status = 403;
    assert(vp_romm_check_connection(&config, backend) == VP_ROMM_FORBIDDEN);
    {
        const long failures[] = {0, 301, 404, 500, 503};
        size_t i;
        for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
            log.status = failures[i];
            assert(vp_romm_check_connection(&config, backend) == VP_ROMM_UNREACHABLE);
        }
    }
    log.status = 200;
    {
        const char *invalid[] = {"", "<html>Login</html>", "{}", "{\"items\":[}",
                                 "{\"items\":[]", "{\"items\":[]} trailing"};
        size_t i;
        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            log.fetch.payload = invalid[i];
            assert(vp_romm_check_connection(&config, backend) == VP_ROMM_UNREACHABLE);
        }
    }
    assert(memcmp(&config, &before, sizeof(config)) == 0);

    vp_romm_connection_reset(&connection, &config);
    revision = connection.revision;
    assert(connection.state == VP_ROMM_CHECKING);
    assert(!vp_romm_pair_code_enabled(connection.state));
    assert(vp_romm_connection_apply(&connection, revision, VP_ROMM_CONNECTED));
    assert(!vp_romm_pair_code_enabled(connection.state));
    vp_romm_connection_reset(&connection, &config);
    assert(!vp_romm_connection_apply(&connection, revision, VP_ROMM_CONNECTED));
    assert(connection.state == VP_ROMM_CHECKING);
    assert(vp_romm_connection_apply(&connection, connection.revision, VP_ROMM_INVALID_TOKEN));
    assert(vp_romm_pair_code_enabled(connection.state));
    assert(vp_romm_pair_code_enabled(VP_ROMM_FORBIDDEN));
    assert(vp_romm_pair_code_enabled(VP_ROMM_UNREACHABLE));
    config.token[0] = '\0';
    vp_romm_connection_reset(&connection, &config);
    assert(connection.state == VP_ROMM_UNCONFIGURED);
    assert(vp_romm_pair_code_enabled(connection.state));
}

static bool fake_transfer(const char *url, const char *bearer_token, const char *destination,
                          VpTransferAction action, void *context) {
    TransferLog *log = context;
    log->calls++;
    log->last_action = action;
    (void)snprintf(log->url, sizeof(log->url), "%s", url);
    (void)snprintf(log->bearer, sizeof(log->bearer), "%s", bearer_token != NULL ? bearer_token : "");
    (void)snprintf(log->destination, sizeof(log->destination), "%s", destination);
    return true;
}

static bool fake_cover_transform(const char *source_url, const char *bearer_token,
                                 const char *service_url, const char *destination, void *context) {
    CoverTransformLog *log = context;
    log->calls++;
    (void)snprintf(log->source_url, sizeof(log->source_url), "%s", source_url);
    (void)snprintf(log->bearer, sizeof(log->bearer), "%s", bearer_token != NULL ? bearer_token : "");
    (void)snprintf(log->service_url, sizeof(log->service_url), "%s", service_url);
    (void)snprintf(log->destination, sizeof(log->destination), "%s", destination);
    return true;
}

static VpItem make_item(const char *id, const char *title) {
    VpItem item;
    memset(&item, 0, sizeof(item));
    (void)snprintf(item.id, sizeof(item.id), "%s", id);
    (void)snprintf(item.title, sizeof(item.title), "%s", title);
    (void)snprintf(item.platform, sizeof(item.platform), "SNES");
    item.source = VP_SOURCE_LOCAL;
    item.kind = VP_KIND_ROM;
    return item;
}

static void test_shell_catalog(void) {
    VpApp app;
    VpItem *item;
    VpPlatformGroup groups[4];
    size_t indexes[4];

    vp_app_init(&app);
    assert(app.active_view == VP_VIEW_GRID);
    assert(app.active_screen == VP_SCREEN_LIBRARY);
    assert(app.library_level == VP_LEVEL_PLATFORMS);
    assert(strcmp(vp_screen_name(VP_SCREEN_LIBRARY), "Biblioteca") == 0);
    assert(strcmp(vp_screen_name(VP_SCREEN_STORES), "Tiendas") == 0);
    assert(strcmp(vp_screen_name(VP_SCREEN_SETTINGS), "Ajustes") == 0);
    vp_app_cycle_screen(&app, -1);
    assert(app.active_screen == VP_SCREEN_SETTINGS);
    vp_app_cycle_screen(&app, 1);
    assert(app.active_screen == VP_SCREEN_LIBRARY);
    vp_app_cycle_screen(&app, 1);
    assert(app.active_screen == VP_SCREEN_STORES);
    vp_app_cycle_screen(&app, 1);
    assert(app.active_screen == VP_SCREEN_SETTINGS);
    vp_app_cycle_view(&app);
    assert(app.active_view == VP_VIEW_LIST);
    assert(vp_catalog_add(&app.catalog, &(VpItem){0}) == NULL);

    item = vp_catalog_add(&app.catalog, &(VpItem){
        .id = "local:chrono", .title = "Chrono Trigger", .platform = "SNES",
        .source = VP_SOURCE_LOCAL, .kind = VP_KIND_ROM, .installed = true
    });
    assert(item != NULL);
    assert(vp_catalog_add(&app.catalog, &(VpItem){
        .id = "local:metroid", .title = "Metroid Fusion", .platform = "GBA",
        .source = VP_SOURCE_LOCAL, .kind = VP_KIND_ROM, .installed = true
    }) != NULL);
    assert(vp_catalog_search(&app.catalog, "chrono", indexes, 4) == 1);
    assert(indexes[0] == 0);
    vp_item_toggle_favorite(item);
    assert(item->favorite);
    assert(vp_catalog_library_platforms(&app.catalog, NULL, groups, 4) == 3);
    assert(strcmp(groups[0].platform, VP_FAVORITES_PLATFORM) == 0);
    assert(groups[0].count == 1);
    vp_item_mark_played(item, 100);
    vp_item_mark_played(&app.catalog.items[1], 200);
    assert(vp_catalog_recent(&app.catalog, indexes, 4) == 2);
    assert(indexes[0] == 1);
    /* At the platform level the vertical selector moves between console groups,
     * not between individual catalog items. */
    vp_app_select(&app, 1);
    assert(app.selected_platform == 1);
    vp_app_select(&app, 1);
    assert(app.selected_platform == 2);
    assert(strcmp(vp_screen_name(VP_SCREEN_STORES), "Tiendas") == 0);
}

static void test_platform_grouping(void) {
    VpCatalog catalog;
    VpPlatformGroup groups[8];
    size_t indexes[8];
    VpItem item;

    vp_catalog_init(&catalog);
    item = make_item("romm:1", "Zelda");
    (void)snprintf(item.platform, sizeof(item.platform), "nds");
    (void)snprintf(item.platform_display, sizeof(item.platform_display), "Nintendo DS");
    assert(vp_catalog_add(&catalog, &item) != NULL);
    item = make_item("romm:2", "Chrono Trigger");
    (void)snprintf(item.platform, sizeof(item.platform), "snes");
    (void)snprintf(item.platform_display, sizeof(item.platform_display), "Super Nintendo");
    assert(vp_catalog_add(&catalog, &item) != NULL);
    item = make_item("romm:3", "Patapon");
    (void)snprintf(item.platform, sizeof(item.platform), "psp");
    assert(vp_catalog_add(&catalog, &item) != NULL);
    item = make_item("romm:4", "Advance Wars");
    (void)snprintf(item.platform, sizeof(item.platform), "nds");
    assert(vp_catalog_add(&catalog, &item) != NULL);

    assert(vp_catalog_platforms(&catalog, groups, 8) == 3);
    /* Groups are sorted by display name: Nintendo DS, PSP, Super Nintendo. */
    assert(strcmp(groups[0].platform, "nds") == 0);
    assert(strcmp(groups[0].display, "Nintendo DS") == 0);
    assert(groups[0].count == 2);
    assert(strcmp(groups[1].platform, "psp") == 0);
    assert(groups[1].count == 1);
    assert(strcmp(groups[2].platform, "snes") == 0);
    assert(groups[2].count == 1);

    /* Items inside a console are sorted by title. */
    assert(vp_catalog_platform_items(&catalog, "nds", indexes, 8) == 2);
    assert(strcmp(catalog.items[indexes[0]].title, "Advance Wars") == 0);
    assert(strcmp(catalog.items[indexes[1]].title, "Zelda") == 0);
}

static void test_library_navigation(void) {
    VpApp app;
    VpItem item;
    size_t visible[8];
    size_t index;

    vp_app_init(&app);
    for (index = 0; index < 5; index++) {
        char title[16];
        (void)snprintf(title, sizeof(title), "Game%u", (unsigned)index);
        item = make_item("romm:nav", title);
        (void)snprintf(item.id, sizeof(item.id), "romm:nav%u", (unsigned)index);
        (void)snprintf(item.platform, sizeof(item.platform), "nds");
        assert(vp_catalog_add(&app.catalog, &item) != NULL);
    }
    item = make_item("romm:psp", "Patapon");
    (void)snprintf(item.platform, sizeof(item.platform), "psp");
    assert(vp_catalog_add(&app.catalog, &item) != NULL);

    assert(vp_app_visible(&app, visible, 8) == 0);
    assert(app.selected_platform == 0);
    vp_app_move(&app, 0, 1, 4);
    assert(app.selected_platform == 1);
    vp_app_move(&app, 0, -1, 4);
    assert(app.selected_platform == 0);
    vp_app_move(&app, -1, 0, 4);
    assert(app.selected_platform == 2);

    app.selected_platform = 1;
    vp_app_open(&app);
    assert(app.library_level == VP_LEVEL_ROMS);
    assert(strcmp(app.active_platform, "nds") == 0);
    assert(vp_app_visible(&app, visible, 8) == 5);
    assert(app.selected_item == visible[0]);

    vp_app_move(&app, 0, 1, 3);
    assert(app.selected_item == visible[3]);
    vp_app_move(&app, 0, -1, 3);
    assert(app.selected_item == visible[0]);
    vp_app_move(&app, 1, 0, 3);
    assert(app.selected_item == visible[1]);
    vp_app_move(&app, -1, 0, 3);
    assert(app.selected_item == visible[0]);
    vp_app_move(&app, -1, 0, 3);
    assert(app.selected_item == visible[4]);
    vp_app_move(&app, 1, 0, 3);
    assert(app.selected_item == visible[0]);

    /* A one-column list maps left/right onto vertical movement so every arrow works. */
    vp_app_move(&app, 1, 0, 1);
    assert(app.selected_item == visible[1]);

    vp_app_back(&app);
    assert(app.library_level == VP_LEVEL_PLATFORMS);
}

static void test_library_filters(void) {
    VpApp app;
    VpCatalog full;
    VpPlatformGroup groups[VP_MAX_LIBRARY_GROUPS];
    size_t visible[VP_MAX_ITEMS];
    size_t index;
    VpItem item;
    VpItem *hidden_favorite;

    vp_app_init(&app);
    item = make_item("romm:zelda", "Zelda");
    (void)snprintf(item.platform, sizeof(item.platform), "snes");
    (void)snprintf(item.platform_display, sizeof(item.platform_display), "Super Nintendo");
    item.installed = true;
    item.favorite = true;
    assert(vp_catalog_add(&app.catalog, &item) != NULL);

    item = make_item("romm:secret", "Hidden Secret");
    (void)snprintf(item.platform, sizeof(item.platform), "gba");
    (void)snprintf(item.platform_display, sizeof(item.platform_display), "Game Boy Advance");
    item.installed = true;
    item.favorite = true;
    item.hidden = true;
    assert(vp_catalog_add(&app.catalog, &item) != NULL);

    item = make_item("romm:patapon", "Patapon");
    (void)snprintf(item.platform, sizeof(item.platform), "psp");
    (void)snprintf(item.platform_display, sizeof(item.platform_display), "PlayStation Portable");
    assert(vp_catalog_add(&app.catalog, &item) != NULL);

    item = make_item("romm:download", "Hidden Download");
    (void)snprintf(item.platform, sizeof(item.platform), "nds");
    (void)snprintf(item.platform_display, sizeof(item.platform_display), "Nintendo DS");
    item.installed = true;
    item.hidden = true;
    assert(vp_catalog_add(&app.catalog, &item) != NULL);

    assert(vp_catalog_library_platforms(&app.catalog, NULL, groups, VP_MAX_LIBRARY_GROUPS) == 3);
    assert(strcmp(groups[0].platform, VP_FAVORITES_PLATFORM) == 0);
    assert(groups[0].count == 1);
    assert(strcmp(groups[1].platform, "psp") == 0);
    assert(strcmp(groups[2].platform, "snes") == 0);
    assert(vp_catalog_library_items(&app.catalog, VP_FAVORITES_PLATFORM, NULL,
                                    visible, VP_MAX_ITEMS) == 1);
    assert(strcmp(app.catalog.items[visible[0]].title, "Zelda") == 0);

    app.library_filters.show_hidden = true;
    assert(vp_catalog_library_platforms(&app.catalog, &app.library_filters,
                                        groups, VP_MAX_LIBRARY_GROUPS) == 5);
    assert(groups[0].count == 2);
    assert(strcmp(groups[1].platform, "gba") == 0);
    assert(strcmp(groups[2].platform, "nds") == 0);
    assert(vp_catalog_library_items(&app.catalog, VP_FAVORITES_PLATFORM,
                                    &app.library_filters, visible, VP_MAX_ITEMS) == 2);
    assert(strcmp(app.catalog.items[visible[0]].title, "Hidden Secret") == 0);
    assert(strcmp(app.catalog.items[visible[1]].title, "Zelda") == 0);

    app.library_filters.downloaded_only = true;
    app.library_filters.show_hidden = false;
    assert(vp_catalog_library_platforms(&app.catalog, &app.library_filters,
                                        groups, VP_MAX_LIBRARY_GROUPS) == 2);
    assert(groups[0].count == 1);
    assert(strcmp(groups[1].platform, "snes") == 0);

    app.library_filters.downloaded_only = false;
    app.library_filters.show_hidden = true;
    app.selected_platform = 0;
    vp_app_open(&app);
    assert(app.library_level == VP_LEVEL_ROMS);
    assert(strcmp(app.active_platform, VP_FAVORITES_PLATFORM) == 0);
    assert(vp_app_visible(&app, visible, VP_MAX_ITEMS) == 2);
    assert(app.selected_item == visible[1]);
    vp_app_select(&app, 1);
    assert(app.selected_item == visible[0]);
    app.library_filters.show_hidden = false;
    assert(vp_app_visible(&app, visible, VP_MAX_ITEMS) == 1);
    assert(strcmp(app.catalog.items[app.selected_item].title, "Zelda") == 0);

    hidden_favorite = vp_catalog_find(&app.catalog, "romm:secret");
    assert(hidden_favorite != NULL && hidden_favorite->hidden);
    vp_item_toggle_hidden(hidden_favorite);
    assert(!hidden_favorite->hidden);
    assert(vp_catalog_library_platforms(&app.catalog, NULL, groups, VP_MAX_LIBRARY_GROUPS) == 4);
    assert(groups[0].count == 2);

    vp_catalog_init(&full);
    for (index = 0; index < VP_MAX_PLATFORMS; index++) {
        char platform[16];
        (void)snprintf(platform, sizeof(platform), "p%03u", (unsigned)index);
        item = make_item("romm:slot", platform);
        (void)snprintf(item.id, sizeof(item.id), "romm:slot%u", (unsigned)index);
        (void)snprintf(item.platform, sizeof(item.platform), "%s", platform);
        assert(vp_catalog_add(&full, &item) != NULL);
    }
    assert(vp_catalog_library_platforms(&full, NULL, groups, VP_MAX_LIBRARY_GROUPS) ==
           VP_MAX_LIBRARY_GROUPS);
    assert(strcmp(groups[0].platform, VP_FAVORITES_PLATFORM) == 0);
    assert(groups[VP_MAX_LIBRARY_GROUPS - 1].count == 1);
}

static void test_romm_metadata(void) {
    VpCatalog catalog;
    VpItem *item;
    const char *json = "{\"items\":[{\"id\":7,\"name\":\"Metroid\","
                       "\"platform_slug\":\"nds\",\"platform_display_name\":\"Nintendo DS\","
                       "\"summary\":\"A sci-fi adventure\","
                       "\"path_cover_small\":\"/assets/romm/resources/roms/7/7/cover/small.png\","
                       "\"path_cover_large\":\"/assets/romm/resources/roms/7/7/cover/big.png\","
                       "\"url_cover\":\"https://provider.example/cover.jpg\","
                       "\"download_url\":\"/api/roms/7/download\"}]}";

    vp_catalog_init(&catalog);
    assert(vp_romm_import(&catalog, json));
    item = vp_catalog_find(&catalog, "romm:7");
    assert(item != NULL);
    assert(strcmp(item->platform_display, "Nintendo DS") == 0);
    assert(strcmp(item->description, "A sci-fi adventure") == 0);
    assert(strcmp(item->cover_url, "/assets/romm/resources/roms/7/7/cover/small.png") == 0);
}

static void test_romm_null_json_values(void) {
    VpCatalog catalog;
    VpItem *item;
    const char *json = "{\"items\":["
                       "{\"id\":8,\"name\":\"Null cover\",\"platform_slug\":\"nds\","
                       "\"summary\":null,\"description\":\"Fallback summary\","
                       "\"path_cover_small\":null,\"path_cover_large\":\"/assets/romm/8/big.png\","
                       "\"download_url\":\"/api/roms/8/download\"},"
                       "{\"id\":null,\"name\":\"Skipped\",\"platform_slug\":\"nds\"}]}";

    vp_catalog_init(&catalog);
    assert(vp_romm_import(&catalog, json));
    assert(catalog.count == 1);
    item = vp_catalog_find(&catalog, "romm:8");
    assert(item != NULL);
    assert(strcmp(item->description, "Fallback summary") == 0);
    assert(strcmp(item->cover_url, "/assets/romm/8/big.png") == 0);
}

static void test_romm_quoted_null_strings_kept(void) {
    VpCatalog catalog;
    VpItem *item;

    vp_catalog_init(&catalog);
    assert(vp_romm_import(&catalog,
        "{\"id\":\"null\",\"name\":\"Quoted id\",\"platform_slug\":\"nds\","
        "\"path_cover_small\":\"/c.png\"}"));
    assert(catalog.count == 1);
    item = vp_catalog_find(&catalog, "romm:null");
    assert(item != NULL);
    assert(strcmp(item->title, "Quoted id") == 0);
    assert(strcmp(item->cover_url, "/c.png") == 0);

    vp_catalog_init(&catalog);
    assert(vp_romm_import(&catalog,
        "{\"id\":31,\"name\":\"null\",\"platform_slug\":\"nds\"}"));
    assert(catalog.count == 1);
    item = vp_catalog_find(&catalog, "romm:31");
    assert(item != NULL);
    assert(strcmp(item->title, "null") == 0);

    vp_catalog_init(&catalog);
    assert(vp_romm_import(&catalog,
        "{\"id\":32,\"name\":\"S\",\"summary\":\"null\","
        "\"description\":\"fallback\",\"path_cover_small\":\"null\","
        "\"path_cover_large\":\"/big.png\"}"));
    assert(catalog.count == 1);
    item = vp_catalog_find(&catalog, "romm:32");
    assert(item != NULL);
    assert(strcmp(item->description, "null") == 0);
    assert(strcmp(item->cover_url, "null") == 0);
}

static void test_romm_null_cover_chain_clears(void) {
    VpCatalog catalog;
    VpItem *item;

    vp_catalog_init(&catalog);
    assert(vp_romm_import(&catalog,
        "{\"id\":33,\"name\":\"C\",\"path_cover_small\":null,"
        "\"path_cover_large\":null,\"url_cover\":\"/uc.png\"}"));
    assert(catalog.count == 1);
    item = vp_catalog_find(&catalog, "romm:33");
    assert(item != NULL);
    assert(strcmp(item->cover_url, "/uc.png") == 0);
}

static void test_download_paths(void) {
    VpItem item;
    char extension[16];
    char path[VP_PATH];

    memset(&item, 0, sizeof(item));
    item.source = VP_SOURCE_ROMM;
    (void)snprintf(item.id, sizeof(item.id), "%s", "romm:42");
    (void)snprintf(item.remote_id, sizeof(item.remote_id), "%s", "42");
    (void)snprintf(item.platform, sizeof(item.platform), "%s", "snes");
    (void)snprintf(item.title, sizeof(item.title), "%s", "Super Metroid (USA)");
    (void)snprintf(item.fs_name, sizeof(item.fs_name), "%s", "Super Metroid (USA).sfc");

    assert(vp_item_extension(&item, extension, sizeof(extension)));
    assert(strcmp(extension, "sfc") == 0);
    assert(vp_item_download_path(&item, "ux0:data/vitashelflauncher/downloads", path, sizeof(path)));
    assert(strcmp(path, "ux0:data/vitashelflauncher/downloads/snes/supermetroidusa_42.sfc") == 0);
    assert(vp_item_download_path(&item, "ux0:data/vitashelflauncher/downloads/", path, sizeof(path)));
    assert(strcmp(path, "ux0:data/vitashelflauncher/downloads/snes/supermetroidusa_42.sfc") == 0);

    /* RomM does not always expose fs_name; the content URL still carries the extension. */
    item.fs_name[0] = '\0';
    (void)snprintf(item.download_url, sizeof(item.download_url), "%s",
                   "https://romm.local/api/roms/42/content/game.zip?token=1");
    assert(vp_item_extension(&item, extension, sizeof(extension)));
    assert(strcmp(extension, "zip") == 0);
    assert(vp_item_download_path(&item, "ux0:data/vitashelflauncher/downloads", path, sizeof(path)));
    assert(strcmp(path, "ux0:data/vitashelflauncher/downloads/snes/supermetroidusa_42.zip") == 0);

    (void)snprintf(item.platform, sizeof(item.platform), "%s", "Mega Drive / Genesis");
    assert(vp_item_download_path(&item, "ux0:data/vitashelflauncher/downloads", path, sizeof(path)));
    assert(strcmp(path, "ux0:data/vitashelflauncher/downloads/Mega_Drive___Genesis/supermetroidusa_42.zip") == 0);

    (void)snprintf(item.download_url, sizeof(item.download_url), "%s",
                   "https://romm.local/api/roms/42/content/7");
    assert(vp_item_extension(&item, extension, sizeof(extension)));
    assert(strcmp(extension, "rom") == 0);

    assert(!vp_item_download_path(&item, "ux0:data/vitashelflauncher/downloads", path, 8));

    /* Fallback to fs_name stem when title is unavailable. */
    item.title[0] = '\0';
    item.download_url[0] = '\0';
    (void)snprintf(item.fs_name, sizeof(item.fs_name), "%s", "Donkey Kong Country (USA).sfc");
    (void)snprintf(item.platform, sizeof(item.platform), "%s", "snes");
    assert(vp_item_download_path(&item, "ux0:data/vitashelflauncher/downloads", path, sizeof(path)));
    assert(strcmp(path, "ux0:data/vitashelflauncher/downloads/snes/donkeykongcountryusa_42.sfc") == 0);

    /* Different IDs keep same-title ROMs unique. */
    (void)snprintf(item.title, sizeof(item.title), "%s", "Metroid");
    item.fs_name[0] = '\0';
    (void)snprintf(item.download_url, sizeof(item.download_url), "%s",
                   "https://romm.local/api/roms/45/content/game.zip");
    (void)snprintf(item.id, sizeof(item.id), "%s", "romm:45");
    (void)snprintf(item.remote_id, sizeof(item.remote_id), "%s", "45");
    assert(vp_item_download_path(&item, "ux0:data/vitashelflauncher/downloads", path, sizeof(path)));
    assert(strcmp(path, "ux0:data/vitashelflauncher/downloads/snes/metroid_45.zip") == 0);
}

static void test_launch_profiles(void) {
    VpLaunchConfig config;
    const VpLaunchProfile *profile;
    char uri[VP_LAUNCH_URI];

    vp_launch_config_init(&config);
    profile = vp_launch_config_find(&config, "snes");
    assert(profile != NULL);
    assert(strcmp(profile->titleid, "RETROVITA") == 0);
    assert(strcmp(profile->core, "app0:/snes9x2005_libretro.self") == 0);
    assert(vp_build_launch_uri(profile, "ux0:data/vitashelflauncher/downloads/snes/supermetroid_42.sfc",
                               uri, sizeof(uri)));
    assert(strcmp(uri, "psgm:play?titleid=RETROVITA&param=app0:/snes9x2005_libretro.self"
                       "&param2=ux0:data/vitashelflauncher/downloads/snes/supermetroid_42.sfc") == 0);

    assert(vp_launch_config_set(&config, "nds", "DSVITA000:"));
    profile = vp_launch_config_find(&config, "nds");
    assert(profile != NULL);
    assert(strcmp(profile->titleid, "DSVITA000") == 0);
    assert(profile->core[0] == '\0');
    assert(vp_build_launch_uri(profile, "ux0:data/vitashelflauncher/downloads/nds/metroid_7.nds",
                               uri, sizeof(uri)));
    assert(strcmp(uri, "psgm:play?titleid=DSVITA000&param=ux0:data/vitashelflauncher/downloads/nds/metroid_7.nds") == 0);

    assert(vp_launch_config_set(&config, "PSP", "VSCU00001:"));
    assert(vp_launch_config_find(&config, "psp") != NULL);
    assert(vp_launch_config_find(&config, "unknown-platform") == NULL);
    assert(!vp_launch_config_set(&config, "snes", ""));
    assert(!vp_build_launch_uri(profile, "ux0:data/vitashelflauncher/downloads/nds/metroid_7.nds", uri, 16));
}

static void test_romm_content_download(void) {
    VpCatalog catalog;
    FetchLog fetch_log;
    TransferLog transfer_log;
    VpFetchBackend fetch_backend;
    VpTransferBackend transfer_backend;
    VpRommConfig config;
    VpItem *item;
    char path[VP_PATH];

    memset(&fetch_log, 0, sizeof(fetch_log));
    memset(&transfer_log, 0, sizeof(transfer_log));
    memset(&config, 0, sizeof(config));
    fetch_log.payload = "{\"total\":1,\"items\":[{\"id\":42,\"name\":\"Super Metroid\","
                        "\"platform_slug\":\"snes\",\"fs_name\":\"Super Metroid (USA).sfc\"}]}";
    (void)snprintf(config.base_url, sizeof(config.base_url), "http://romm.local/");
    (void)snprintf(config.token, sizeof(config.token), "rmm_test_token");
    fetch_backend.execute = fake_fetch;
    fetch_backend.context = &fetch_log;
    transfer_backend.execute = fake_transfer;
    transfer_backend.context = &transfer_log;
    vp_catalog_init(&catalog);

    assert(vp_romm_sync(&catalog, &config, fetch_backend));
    item = vp_catalog_find(&catalog, "romm:42");
    assert(item != NULL);
    assert(strcmp(item->fs_name, "Super Metroid (USA).sfc") == 0);
    assert(strcmp(item->download_url,
                  "http://romm.local/api/roms/42/content/Super Metroid (USA).sfc") == 0);
    assert(vp_item_download_path(item, "ux0:data/vitashelflauncher/downloads", path, sizeof(path)));
    assert(strcmp(path, "ux0:data/vitashelflauncher/downloads/snes/supermetroid_42.sfc") == 0);
    assert(vp_queue_download(item, &config, path, transfer_backend));
    assert(strcmp(transfer_log.url,
                  "http://romm.local/api/roms/42/content/Super Metroid (USA).sfc") == 0);
    assert(strcmp(transfer_log.destination,
                   "ux0:data/vitashelflauncher/downloads/snes/supermetroid_42.sfc") == 0);
    assert(strcmp(transfer_log.bearer, "rmm_test_token") == 0);
    assert(!item->multi_file);

    {
        VpCatalog nested_url;
        vp_catalog_init(&nested_url);
        assert(vp_romm_import(&nested_url,
                              "{\"id\":86,\"name\":\"Castlevania\","
                              "\"platform_slug\":\"gba\","
                              "\"fs_name\":\"Castlevania.zip\","
                              "\"launchbox_metadata\":{\"images\":[{\"url\":\""
                              "https://images.launchbox-app.com/cover.jpg\"}]}}"));
        item = vp_catalog_find(&nested_url, "romm:86");
        assert(item != NULL);
        assert(strcmp(item->download_url, "/api/roms/86/content/Castlevania.zip") == 0);
    }

    {
        VpCatalog multi;
        vp_catalog_init(&multi);
        assert(vp_romm_import(&multi,
                              "{\"id\":7,\"name\":\"Final Fantasy VII\","
                              "\"platform_slug\":\"psx\",\"fs_name\":\"Final Fantasy VII\","
                              "\"has_multiple_files\":true}"));
        item = vp_catalog_find(&multi, "romm:7");
        assert(item != NULL);
        assert(item->multi_file);
        assert(strcmp(item->download_url,
                      "/api/roms/7/content/Final Fantasy VII") == 0);
    }
}

static void test_url_escape(void) {
    char escaped[VP_URL * 3];

    /* RomM emits cover timestamps with a literal space, which libcurl rejects outright. */
    assert(vp_url_escape("http://romm.local/assets/1/cover/small.png?ts=2026-10-06 23:56:16",
                         escaped, sizeof(escaped)));
    assert(strcmp(escaped,
                  "http://romm.local/assets/1/cover/small.png?ts=2026-10-06%2023:56:16") == 0);
    /* Already-encoded URLs must not be double encoded. */
    assert(vp_url_escape("http://romm.local/a.png?ts=2026-10-06%2023:56:16",
                         escaped, sizeof(escaped)));
    assert(strcmp(escaped, "http://romm.local/a.png?ts=2026-10-06%2023:56:16") == 0);
    assert(vp_url_escape("http://romm.local/a b/{c}.png", escaped, sizeof(escaped)));
    assert(strcmp(escaped, "http://romm.local/a%20b/%7Bc%7D.png") == 0);
    assert(!vp_url_escape("http://romm.local/a b.png", escaped, 5));
    assert(escaped[0] == '\0');
}

static void test_theme(void) {
    VpTheme theme;
    VpView view = VP_VIEW_GRID;
    const char *ini = "id=amber\nname=Amber Pulse\nbackground=#000000\nsurface=#111111\n"
                      "accent=#ffaa00\ntext=#ffffff\nmuted=#999999\nview=carousel\n"
                      "card_width=170\ncard_height=220\n";
    memset(&theme, 0, sizeof(theme));
    assert(vp_theme_load(&theme, ini));
    assert(strcmp(theme.id, "amber") == 0);
    assert(theme.view == VP_VIEW_CAROUSEL);
    assert(theme.card_width == 170);
    assert(!vp_theme_load(&theme, "id=x\nname=x\nview=broken\n"));
    assert(vp_view_from_string("list", &view) && view == VP_VIEW_LIST);
    assert(strcmp(vp_view_name(view), "list") == 0);
    assert(strcmp(vp_view_name(VP_VIEW_CAROUSEL), "carousel") == 0);
    assert(!vp_view_from_string("broken", &view) && view == VP_VIEW_LIST);
}

static void test_local_library(void) {
    VpCatalog catalog;
    const char *manifest = "pcse00120|Persona 4 Golden|PS Vita|PCSE00120\n"
                           "snes-smw|Super Mario World|SNES|ux0:data/roms/snes/smw.sfc\n";
    vp_catalog_init(&catalog);
    assert(vp_local_import_manifest(&catalog, manifest));
    assert(catalog.count == 2);
    assert(vp_catalog_find(&catalog, "local:pcse00120") != NULL);
    assert(vp_catalog_find(&catalog, "local:snes-smw")->installed);
}

static void test_romm_pair_code_exchange(void) {
    PairLog pair_log;
    VpRommPairBackend backend;
    VpRommConfig config;

    memset(&pair_log, 0, sizeof(pair_log));
    memset(&config, 0, sizeof(config));
    (void)snprintf(config.base_url, sizeof(config.base_url), "https://romm.local/");
    (void)snprintf(config.token, sizeof(config.token), "rmm_existing_token");
    pair_log.succeed = true;
    pair_log.response = "{\"id\":4,\"raw_token\":\"rmm_paired_token\"}";
    backend.execute = fake_pair;
    backend.context = &pair_log;

    assert(vp_romm_exchange_pair_code(&config, "ABCD-1234", backend));
    assert(pair_log.calls == 1);
    assert(strcmp(pair_log.url, "https://romm.local/api/client-tokens/exchange") == 0);
    assert(strcmp(pair_log.code, "ABCD-1234") == 0);
    assert(strcmp(config.token, "rmm_paired_token") == 0);

    pair_log.succeed = false;
    assert(!vp_romm_exchange_pair_code(&config, "WXYZ-9999", backend));
    assert(strcmp(config.token, "rmm_paired_token") == 0);
    assert(!vp_romm_exchange_pair_code(&config, "invalid code!", backend));
    assert(pair_log.calls == 2);
}

static void test_romm_sync(void) {
    VpCatalog catalog;
    FetchLog fetch_log;
    TransferLog transfer_log;
    CoverTransformLog cover_transform_log;
    VpFetchBackend fetch_backend;
    VpTransferBackend transfer_backend;
    VpCoverTransformBackend cover_transform_backend;
    VpRommConfig config;
    VpItem *item;

    memset(&fetch_log, 0, sizeof(fetch_log));
    memset(&transfer_log, 0, sizeof(transfer_log));
    memset(&cover_transform_log, 0, sizeof(cover_transform_log));
    memset(&config, 0, sizeof(config));
    (void)snprintf(config.base_url, sizeof(config.base_url), "http://romm.local/");
    (void)snprintf(config.token, sizeof(config.token), "rmm_test_token");
    fetch_backend.execute = fake_fetch;
    fetch_backend.context = &fetch_log;
    transfer_backend.execute = fake_transfer;
    transfer_backend.context = &transfer_log;
    cover_transform_backend.execute = fake_cover_transform;
    cover_transform_backend.context = &cover_transform_log;
    vp_catalog_init(&catalog);
    assert(vp_romm_sync(&catalog, &config, fetch_backend));
    assert(fetch_log.calls == 1);
    assert(strcmp(fetch_log.url, "http://romm.local/api/roms?limit=1&offset=0&with_char_index=false&with_filter_values=false&with_rom_id_index=false") == 0);
    assert(strcmp(fetch_log.bearer, "rmm_test_token") == 0);
    item = vp_catalog_find(&catalog, "romm:99");
    assert(item != NULL);
    assert(strcmp(item->cover_url, "http://romm.local/assets/fzero.png") == 0);
    assert(vp_cache_cover(item, &config, "ux0:data/vitashelflauncher/cache/fzero.png", cover_transform_backend));
    assert(cover_transform_log.calls == 1);
    assert(strcmp(cover_transform_log.source_url, "http://romm.local/assets/fzero.png") == 0);
    assert(strcmp(cover_transform_log.bearer, "rmm_test_token") == 0);
    assert(strcmp(cover_transform_log.service_url, "http://romm.local/image") == 0);
    assert(vp_queue_download(item, &config, "ux0:data/vitashelflauncher/downloads/fzero.sfc", transfer_backend));
    assert(transfer_log.calls == 1);
    assert(strcmp(transfer_log.url, "http://romm.local/api/file/fzero.sfc") == 0);
    assert(strcmp(transfer_log.bearer, "rmm_test_token") == 0);
    (void)snprintf(item->cover_url, sizeof(item->cover_url), "https://cdn.example/fzero.png");
    (void)snprintf(config.image_proxy_url, sizeof(config.image_proxy_url), "http://romm.local:8090/image");
    assert(vp_cache_cover(item, &config, "ux0:data/vitashelflauncher/cache/fzero.png", cover_transform_backend));
    assert(strcmp(cover_transform_log.bearer, "") == 0);
    assert(strcmp(cover_transform_log.service_url, "http://romm.local:8090/image") == 0);
    (void)snprintf(item->download_url, sizeof(item->download_url), "https://cdn.example/fzero.sfc");
    assert(vp_queue_download(item, &config, "ux0:data/vitashelflauncher/downloads/fzero.sfc", transfer_backend));
    assert(strcmp(transfer_log.bearer, "") == 0);
    item->source=VP_SOURCE_LOCAL;
    config.base_url[0]='\0';
    assert(vp_cache_cover(item,&config,"ux0:data/vitashelflauncher/cache/local.png",cover_transform_backend));
    assert(cover_transform_log.bearer[0]=='\0');
    assert(!strcmp(cover_transform_log.service_url,"http://romm.local:8090/image"));
}

static void test_libretro_thumbnail_safety(void) {
    VpItem item = make_item("romm:99", "F-Zero");
    char url[VP_URL];
    const unsigned char valid_png[] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x80,
        0x08, 0x06, 0x00, 0x00, 0x00
    };
    unsigned char unsafe_png[sizeof(valid_png)];

    (void)snprintf(item.platform, sizeof(item.platform), "snes");
    assert(vp_libretro_thumbnail_url(&item, url, sizeof(url)));
    assert(strcmp(url, "https://thumbnails.libretro.com/Nintendo%20-%20Super%20Nintendo%20Entertainment%20System/Named_Boxarts/F-Zero.png") == 0);
    (void)snprintf(item.fs_name, sizeof(item.fs_name), "F-Zero (USA).sfc");
    assert(vp_libretro_thumbnail_url(&item, url, sizeof(url)));
    assert(strcmp(url, "https://thumbnails.libretro.com/Nintendo%20-%20Super%20Nintendo%20Entertainment%20System/Named_Boxarts/F-Zero%20(USA).png") == 0);
    assert(vp_safe_cover_png(valid_png, sizeof(valid_png)));
    memcpy(unsafe_png, valid_png, sizeof(unsafe_png));
    unsafe_png[16] = 0x03;
    assert(!vp_safe_cover_png(unsafe_png, sizeof(unsafe_png)));
    (void)snprintf(item.platform, sizeof(item.platform), "unknown");
    assert(!vp_libretro_thumbnail_url(&item, url, sizeof(url)));
}

typedef struct {
    size_t calls, processed[VP_MAX_ITEMS + 2], total[VP_MAX_ITEMS + 2];
} SyncProgressLog;

static void record_sync_progress(size_t processed, size_t total, void *context) {
    SyncProgressLog *log = context;
    assert(log->calls < VP_MAX_ITEMS + 2);
    if (log->calls) assert(processed >= log->processed[log->calls - 1]);
    assert(!total || processed <= total);
    log->processed[log->calls] = processed;
    log->total[log->calls++] = total;
}

static void test_romm_pagination(void) {
    VpCatalog catalog;
    FetchLog fetch_log;
    VpFetchBackend backend;
    VpRommConfig config;
    SyncProgressLog progress_log = {0};
    VpSyncProgressBackend progress = {record_sync_progress, &progress_log};

    memset(&fetch_log, 0, sizeof(fetch_log));
    memset(&config, 0, sizeof(config));
    fetch_log.page_count = 2;
    fetch_log.pages[0] = "{\"total\":2,\"items\":[{\"id\":1,\"name\":\"First\"}]}";
    fetch_log.pages[1] = "{\"total\":2,\"items\":[{\"id\":2,\"name\":\"Second\"}]}";
    (void)snprintf(config.base_url, sizeof(config.base_url), "https://romm.local");
    (void)snprintf(config.token, sizeof(config.token), "rmm_test_token");
    backend.execute = fake_fetch;
    backend.context = &fetch_log;
    vp_catalog_init(&catalog);

    assert(vp_romm_sync_with_progress(&catalog, &config, backend, progress));
    assert(progress_log.calls == 3);
    assert(progress_log.processed[0] == 0 && progress_log.total[0] == 0);
    assert(progress_log.processed[1] == 1 && progress_log.total[1] == 2);
    assert(progress_log.processed[2] == 2 && progress_log.total[2] == 2);
    assert(catalog.count == 2);
    assert(fetch_log.calls == 2);
    assert(strcmp(fetch_log.urls[0], "https://romm.local/api/roms?limit=1&offset=0&with_char_index=false&with_filter_values=false&with_rom_id_index=false") == 0);
    assert(strcmp(fetch_log.urls[1], "https://romm.local/api/roms?limit=1&offset=1&with_char_index=false&with_filter_values=false&with_rom_id_index=false") == 0);
    assert(strcmp(fetch_log.bearer, "rmm_test_token") == 0);
    assert(vp_catalog_find(&catalog, "romm:1") != NULL);
    assert(vp_catalog_find(&catalog, "romm:2") != NULL);

    /* Unknown totals stay indeterminate until an empty page ends the sync. */
    fetch_log.calls = 0;
    fetch_log.pages[0] = "{\"items\":[{\"id\":1,\"name\":\"First\"}]}";
    fetch_log.pages[1] = "{\"items\":[]}";
    progress_log = (SyncProgressLog){0};
    vp_catalog_init(&catalog);
    assert(vp_romm_sync_with_progress(&catalog, &config, backend, progress));
    assert(progress_log.processed[2] == 1 && progress_log.total[2] == 0);

    fetch_log.calls = 0;
    fetch_log.page_count = 1;
    fetch_log.pages[0] = "{\"total\":0,\"items\":[]}";
    progress_log = (SyncProgressLog){0};
    vp_catalog_init(&catalog);
    assert(vp_romm_sync_with_progress(&catalog, &config, backend, progress));
    assert(catalog.count == 0 && progress_log.calls == 2);

    /* A large server library reports the bounded work this client can import. */
    fetch_log.calls = 0;
    fetch_log.page_count = 2;
    fetch_log.pages[0] = "{\"total\":1000,\"items\":[{\"id\":1,\"name\":\"First\"}]}";
    fetch_log.pages[1] = "{\"total\":1000,\"items\":[]}";
    progress_log = (SyncProgressLog){0};
    vp_catalog_init(&catalog);
    assert(vp_romm_sync_with_progress(&catalog, &config, backend, progress));
    assert(progress_log.total[1] == VP_MAX_ITEMS);

    /* The legacy wrapper keeps reporting whether anything was imported. */
    fetch_log.calls = 0;
    fetch_log.page_count = 1;
    fetch_log.pages[0] = "{\"total\":0,\"items\":[]}";
    vp_catalog_init(&catalog);
    assert(!vp_romm_sync(&catalog, &config, backend));

    fetch_log.calls = 0;
    fetch_log.pages[0] = "{\"total\":1,\"items\":[{\"id\":1,\"name\":\"First\"}]}";
    vp_catalog_init(&catalog);
    assert(vp_romm_sync(&catalog, &config, backend));
}

static void test_romm_sync_fails_after_partial_page(void) {
    VpCatalog catalog;
    FetchLog fetch_log;
    VpFetchBackend backend;
    VpRommConfig config;
    SyncProgressLog progress_log = {0};
    VpSyncProgressBackend progress = {record_sync_progress, &progress_log};

    memset(&fetch_log, 0, sizeof(fetch_log));
    memset(&config, 0, sizeof(config));
    fetch_log.fail_at_call = 1;
    fetch_log.page_count = 1;
    fetch_log.pages[0] = "{\"total\":2,\"items\":[{\"id\":1,\"name\":\"First\"}]}";
    (void)snprintf(config.base_url, sizeof(config.base_url), "https://romm.local");
    (void)snprintf(config.token, sizeof(config.token), "rmm_test_token");
    backend.execute = fake_fetch;
    backend.context = &fetch_log;
    vp_catalog_init(&catalog);

    assert(!vp_romm_sync_with_progress(&catalog, &config, backend, progress));
    assert(progress_log.calls == 2);
    assert(progress_log.processed[1] == 1 && progress_log.total[1] == 2);
    assert(catalog.count == 1);
    assert(fetch_log.calls == 2);
}

static void test_homebrew_store_sync(void) {
    VpCatalog catalog;
    FetchLog fetch_log;
    VpFetchBackend fetch_backend;
    VpHomebrewConfig config;
    VpItem *item;

    memset(&fetch_log, 0, sizeof(fetch_log));
    memset(&config, 0, sizeof(config));
    fetch_log.payload = "{\"apps\":[{\"id\":\"vitaftp\",\"name\":\"Vita FTP\","
                        "\"description\":\"FTP server\",\"url\":\"https://example.org/vitaftp.vpk\"}]}";
    (void)snprintf(config.catalog_url, sizeof(config.catalog_url),
                   "https://catalog.example.org/vita.json");
    fetch_backend.execute = fake_fetch;
    fetch_backend.context = &fetch_log;
    vp_catalog_init(&catalog);
    assert(vp_homebrew_sync(&catalog, &config, fetch_backend));
    assert(fetch_log.calls == 1);
    assert(fetch_log.bearer[0] == '\0');
    item = vp_catalog_find(&catalog, "homebrew:vitaftp");
    assert(item != NULL);
    assert(item->kind == VP_KIND_APP);
    assert(strcmp(item->description, "FTP server") == 0);
}

static void test_providers_and_transfers(void) {
    VpCatalog catalog;
    TransferLog log;
    VpTransferBackend backend;
    VpItem *rom;
    VpItem *homebrew;
    const char *romm = "{\"items\":[{\"id\":42,\"name\":\"Super Metroid\","
                       "\"platform_slug\":\"snes\",\"url_cover\":\"http://romm/cover.png\","
                       "\"download_url\":\"http://romm/game.sfc\"}]}";
    const char *store = "[{\"id\":\"vitashelflauncher\",\"name\":\"VitaShelf Launcher\","
                        "\"description\":\"Custom shell\",\"url\":\"https://example.org/vitashelflauncher.vpk\"}]";

    memset(&log, 0, sizeof(log));
    backend.execute = fake_transfer;
    backend.context = &log;
    vp_catalog_init(&catalog);
    assert(vp_romm_import(&catalog, romm));
    assert(vp_homebrew_import(&catalog, store));
    assert(catalog.count == 2);
    rom = vp_catalog_find(&catalog, "romm:42");
    homebrew = vp_catalog_find(&catalog, "homebrew:vitashelflauncher");
    assert(rom != NULL && homebrew != NULL);
    assert(strcmp(rom->platform, "snes") == 0);
    assert(vp_queue_download(rom, NULL, "ux0:data/vitashelflauncher/downloads/snes/game.sfc", backend));
    assert(log.calls == 1 && log.last_action == VP_TRANSFER_DOWNLOAD);
    assert(log.bearer[0] == '\0');
    assert(vp_install_homebrew(homebrew, "ux0:data/vitashelflauncher/downloads/VitaShelf Launcher.vpk", backend));
    assert(log.calls == 2 && log.last_action == VP_TRANSFER_INSTALL_VPK);
    assert(!vp_install_homebrew(rom, "ux0:data/vitashelflauncher/x", backend));
}

static void test_download_queue(void) {
    VpDownloadQueue queue;
    VpItem first = make_item("romm:queue-1", "First");
    VpItem second = make_item("romm:queue-2", "Second");
    VpDownloadEntry *entry;

    vp_download_queue_init(&queue);
    assert(vp_download_queue_enqueue(&queue, &first, "ux0:data/first.sfc", "ux0:data/first.sfc.part"));
    assert(vp_download_queue_enqueue(&queue, &second, "ux0:data/second.sfc", "ux0:data/second.sfc.part"));
    assert(!vp_download_queue_enqueue(&queue, &first, "ux0:data/first.sfc", "ux0:data/first.sfc.part"));

    entry = vp_download_queue_start_next(&queue);
    assert(entry != NULL && strcmp(entry->item.id, "romm:queue-1") == 0);
    vp_download_queue_progress(entry, 50, 200);
    assert(entry->percent == 25);
    vp_download_queue_finish(entry, true, NULL);
    assert(entry->state == VP_DOWNLOAD_COMPLETED && entry->percent == 100);

    entry = vp_download_queue_start_next(&queue);
    assert(entry != NULL && strcmp(entry->item.id, "romm:queue-2") == 0);
    vp_download_queue_progress(entry, 25, 100);
    assert(entry->percent == 25);
    assert(vp_download_queue_pause(&queue, "romm:queue-2"));
    assert(entry->state == VP_DOWNLOAD_PAUSED && entry->percent == 25);
    assert(vp_download_queue_retry(&queue, "romm:queue-2"));
    assert(vp_download_queue_find(&queue, "romm:queue-2")->state == VP_DOWNLOAD_PENDING);
    vp_download_queue_finish(entry, false, "HTTP 500");
    assert(entry->state == VP_DOWNLOAD_FAILED);
    assert(vp_download_queue_retry(&queue, "romm:queue-2"));
    assert(vp_download_queue_find(&queue, "romm:queue-2")->state == VP_DOWNLOAD_PENDING);
    assert(vp_download_queue_cancel(&queue, "romm:queue-2"));
    assert(vp_download_queue_find(&queue, "romm:queue-2")->state == VP_DOWNLOAD_CANCELED);
    assert(!vp_download_queue_pause(&queue, "romm:queue-2"));
    assert(vp_download_queue_remove(&queue, "romm:queue-2"));
    assert(queue.count == 1);
    assert(vp_download_queue_find(&queue, "romm:queue-2") == NULL);
    assert(strcmp(vp_download_queue_find(&queue, "romm:queue-1")->item.id, "romm:queue-1") == 0);
    assert(!vp_download_queue_remove(&queue, "romm:queue-2"));
}

static void test_romm_game_artwork(void) {
    static VpCatalog catalog;
    VpRommConfig config={0};
    FetchLog log={0};
    VpFetchBackend backend={fake_fetch,&log};
    (void)snprintf(config.base_url,sizeof(config.base_url),"https://romm.example.test");
    (void)snprintf(config.token,sizeof(config.token),"test-token");
    log.payload="{\"total\":3,\"items\":["
        "{\"id\":1,\"name\":\"With artwork\",\"platform_slug\":\"psx\",\"merged_screenshots\":[\"/assets/romm/resources/roms/1/hero.png\"]},"
        "{\"id\":2,\"name\":\"No artwork\",\"platform_slug\":\"psx\",\"merged_screenshots\":[]},"
        "{\"id\":3,\"name\":\"Invalid artwork\",\"platform_slug\":\"psx\",\"background_url\":\"null\"}]}";
    vp_catalog_init(&catalog);
    assert(vp_romm_sync(&catalog,&config,backend));
    assert(catalog.count==3);
    assert(strcmp(catalog.items[0].background_url,"https://romm.example.test/assets/romm/resources/roms/1/hero.png")==0);
    assert(!catalog.items[1].background_url[0]);
    assert(!catalog.items[2].background_url[0]);
}

static void test_psp_install_paths(void) {
    VpItem item;
    char path[VP_PATH];
    assert(vp_platform_is_psp("PSP"));
    assert(vp_platform_is_psp("playstation portable"));
    assert(!vp_platform_is_psp("snes"));

    memset(&item, 0, sizeof(item));
    item.source = VP_SOURCE_ROMM;
    (void)snprintf(item.id, sizeof(item.id), "%s", "romm:42");
    (void)snprintf(item.remote_id, sizeof(item.remote_id), "%s", "42");
    (void)snprintf(item.platform, sizeof(item.platform), "%s", "PSP");
    (void)snprintf(item.title, sizeof(item.title), "%s", "God of War");
    (void)snprintf(item.fs_name, sizeof(item.fs_name), "%s", "God of War.iso");

    assert(vp_item_install_path(&item, "ux0:data/vitashelflauncher/downloads", "ux0:pspemu/ISO",
                                path, sizeof(path)));
    assert(strcmp(path, "ux0:pspemu/ISO/godofwar_42.iso") == 0);
    (void)snprintf(item.fs_name, sizeof(item.fs_name), "%s", "God of War.cso");
    assert(vp_item_psp_image_path(&item, "ux0:pspemu/ISO", "cso", path, sizeof(path)));
    assert(strcmp(path, "ux0:pspemu/ISO/godofwar_42.cso") == 0);
    (void)snprintf(item.download_url, sizeof(item.download_url), "%s",
                   "https://romm.local/api/roms/42/content/game.zip");
    item.fs_name[0] = '\0';
    assert(vp_item_install_path(&item, "ux0:data/vitashelflauncher/downloads", "ux0:pspemu/ISO",
                                path, sizeof(path)));
    assert(strcmp(path, "ux0:pspemu/ISO/godofwar_42.zip") == 0);
    assert(vp_item_psp_image_path(&item, "ux0:pspemu/ISO", "iso", path, sizeof(path)));
    assert(strcmp(path, "ux0:pspemu/ISO/godofwar_42.iso") == 0);

    (void)snprintf(item.platform, sizeof(item.platform), "%s", "snes");
    assert(vp_item_install_path(&item, "ux0:data/vitashelflauncher/downloads", "ux0:pspemu/ISO",
                                path, sizeof(path)));
    assert(strcmp(path, "ux0:data/vitashelflauncher/downloads/snes/godofwar_42.zip") == 0);
}

int main(void) {
    VpItem sample = make_item("local:sample", "Sample");
    assert(strcmp(sample.title, "Sample") == 0);
    test_shell_catalog();
    test_platform_grouping();
    test_library_navigation();
    test_library_filters();
    test_romm_metadata();
    test_romm_null_json_values();
    test_romm_quoted_null_strings_kept();
    test_romm_null_cover_chain_clears();
    test_download_paths();
    test_psp_install_paths();
    test_launch_profiles();
    test_romm_content_download();
    test_url_escape();
    test_theme();
    test_local_library();
    test_romm_pair_code_exchange();
    test_romm_connection();
    test_romm_sync();
    test_romm_game_artwork();
    test_libretro_thumbnail_safety();
    test_romm_pagination();
    test_romm_sync_fails_after_partial_page();
    test_homebrew_store_sync();
    test_providers_and_transfers();
    test_download_queue();
    puts("vitashelflauncher core tests: PASS");
    return 0;
}
