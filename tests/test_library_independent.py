"""Host regressions for persisted library state and filtered Vita navigation."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/vita/main.c").read_text()


def body(signature):
    start = SOURCE.index(signature)
    return SOURCE[start:SOURCE.index("\n}", start) + 2]


PREFIX = r"""
#include "library_preferences.h"
#include "vita_titles.h"
#include "ui_presentation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VP_LIBRARY_PREFERENCES_PATH "library.ini"
#define VP_LIBRARY_PREFERENCES_PARTIAL "library.ini.part"
#define VP_LIBRARY_PREFERENCES_LINE 1024
static VpApp app;
static VpRommConfig romm_config;
static bool rename_fails, catalog_view_cache_dirty=true;
static VpLibraryLevel cached_library_level;
static char cached_platform[VP_TEXT];
static VpPlatformGroup platform_groups[VP_MAX_LIBRARY_GROUPS];
static size_t platform_group_count, visible_count_cache, visible_indexes[VP_MAX_ITEMS];
static VpLibraryModal library_modal;
static size_t library_modal_row;
static unsigned long long library_modal_opened_at;
enum { SCE_CTRL_UP=1, SCE_CTRL_DOWN=2, SCE_CTRL_CROSS=4, SCE_CTRL_CIRCLE=8,
 SCE_CTRL_SQUARE=16,SCE_CTRL_TRIANGLE=32,SCE_CTRL_SELECT=256,
 VP_UI_SOUND_NAVIGATION,VP_UI_SOUND_BACK,VP_UI_SOUND_ENTER,VP_UI_SOUND_DOWNLOAD_COMPLETE };
static int sceIoMkdir(const char *p,int m) { (void)p;(void)m;return 0; }
static int sceIoRemove(const char *p) { return remove(p); }
static int sceIoRename(const char *a,const char *b) {
    if (rename_fails && !strcmp(a,VP_LIBRARY_PREFERENCES_PARTIAL)) return -1;
    return rename(a,b);
}
static unsigned long long sceKernelGetProcessTimeWide(void) { return 1; }
static void show_notification(VpApp *v,const char *s,...) { (void)v;(void)s; }
static void vp_ui_audio_play(int s) { (void)s; }
static bool save_romm_config(void) { return true; }
static bool start_next_download(void) { return false; }
static bool settings_ime_active;
static size_t list_start,grid_start;
static int vp_ui_touch_platform(int x,int y) { (void)x;(void)y;return -1; }
static float library_modal_progress(void) { return 1.f; }
#define VP_UI_SOUND_TAB 50
#define SCE_CTRL_LEFT 64
#define SCE_CTRL_RIGHT 128
"""

CASES = r"""
static void seed(void) {
    vp_app_init(&app); strcpy(romm_config.base_url,"https://example.test");
    app.catalog.count=2;
    strcpy(app.catalog.items[0].id,"local:PCSA00001");
    strcpy(app.catalog.items[0].platform,"psvita");
    strcpy(app.catalog.items[0].title,"Alpha");
    strcpy(app.catalog.items[0].launch_target,"PCSA00001");
    app.catalog.items[0].source=VP_SOURCE_LOCAL;
    app.catalog.items[0].installed=true;
    strcpy(app.catalog.items[1].id,"romm:7");
    strcpy(app.catalog.items[1].platform,"snes");
    strcpy(app.catalog.items[1].title,"Zelda");
    strcpy(app.catalog.items[1].launch_target,"downloads/zelda.sfc");
    app.catalog.items[1].source=VP_SOURCE_ROMM;
}
int main(int argc,char **argv) {
    assert(argc==2); seed();
    if (!strcmp(argv[1],"roundtrip")) {
        app.catalog.items[0].favorite=true; app.catalog.items[1].hidden=true;
        app.library_filters=(VpLibraryFilters){true,true}; assert(save_library_preferences(&app));
        static VpCatalog fresh; fresh=app.catalog;
        fresh.items[1].hidden=false; fresh.items[1].favorite=false;
        vp_catalog_replace_providers(&app.catalog,&fresh); assert(load_library_preferences(&app));
        assert(app.catalog.items[0].favorite && app.catalog.items[1].hidden);
        assert(app.library_filters.show_hidden && app.library_filters.downloaded_only);
        vp_library_preferences_reset(&app.catalog); assert(load_library_preferences(&app));
        assert(app.catalog.items[0].favorite && app.catalog.items[1].hidden);
        size_t ix[VP_MAX_ITEMS]; VpPlatformGroup g[VP_MAX_LIBRARY_GROUPS];
        assert(vp_catalog_library_platforms(&app.catalog,&app.library_filters,g,VP_MAX_LIBRARY_GROUPS)==2);
        assert(!strcmp(g[0].platform,VP_FAVORITES_PLATFORM) && g[0].count==1);
        assert(vp_catalog_library_items(&app.catalog,g[0].platform,&app.library_filters,ix,VP_MAX_ITEMS)==1 && ix[0]==0);
        assert(!strcmp(app.catalog.items[0].launch_target,"PCSA00001"));
        app.catalog.items[1].favorite=true; app.catalog.items[1].installed=true;
        assert(vp_catalog_library_platforms(&app.catalog,&app.library_filters,g,VP_MAX_LIBRARY_GROUPS)==3);
        assert(g[0].count==2);
        assert(vp_catalog_library_items(&app.catalog,VP_FAVORITES_PLATFORM,&app.library_filters,ix,VP_MAX_ITEMS)==2);
        assert(ix[0]==0 && ix[1]==1);
        assert(!strcmp(app.catalog.items[1].platform,"snes"));
        assert(!strcmp(app.catalog.items[1].launch_target,"downloads/zelda.sfc"));
        static VpCatalog empty;
        assert(vp_catalog_library_platforms(&empty,&app.library_filters,g,VP_MAX_LIBRARY_GROUPS)==1 && g[0].count==0);
        app.library_filters=(VpLibraryFilters){false,false};
        assert(!vp_library_item_visible(&app.catalog.items[1],&app.library_filters));
        assert(vp_catalog_library_items(&app.catalog,"snes",&app.library_filters,ix,0)==0);
        puts("roundtrip: PASS");
    } else if (!strcmp(argv[1],"save")) {
        app.catalog.items[0].favorite=true; assert(save_library_preferences(&app));
    } else if (!strcmp(argv[1],"rename-failure")) {
        rename_fails=true; app.library_filters.show_hidden=true;
        printf("save=%d\n",save_library_preferences(&app));
    } else if (!strcmp(argv[1],"startup")) {
        app.catalog.items[0].favorite=true; app.catalog.items[0].hidden=true;
        assert(save_library_preferences(&app));
        VpItem local=app.catalog.items[0];
        app.catalog.items[0]=app.catalog.items[1]; app.catalog.count=1;
        assert(load_library_preferences(&app));
        app.library_filters.show_hidden=true; assert(save_library_preferences(&app));
        local.favorite=false; local.hidden=false; assert(vp_catalog_add(&app.catalog,&local));
        assert(load_library_preferences(&app));
        printf("favorite=%d hidden=%d\n",app.catalog.items[1].favorite,app.catalog.items[1].hidden);
    } else if (!strcmp(argv[1],"selection")) {
        app.selected_platform=2; refresh_catalog_view_cache(&app); assert(platform_group_count==3);
        library_modal=VP_LIBRARY_MODAL_FILTERS; library_modal_row=1;
        handle_library_modal_input(SCE_CTRL_CROSS);
        printf("selected=%zu groups=%zu\n",app.selected_platform,platform_group_count);
    } else if (!strcmp(argv[1],"empty-actions")) {
        app.library_level=VP_LEVEL_ROMS; strcpy(app.active_platform,"snes"); app.selected_item=1;
        refresh_catalog_view_cache(&app); assert(visible_count_cache==1);
        library_modal=VP_LIBRARY_MODAL_ACTIONS; library_modal_row=1;
        handle_library_modal_input(SCE_CTRL_CROSS); assert(!visible_count_cache);
        shortcuts(SCE_CTRL_SQUARE);
        if (library_modal!=VP_LIBRARY_MODAL_NONE) handle_library_modal_input(SCE_CTRL_CROSS);
        printf("unselected-local-favorite=%d\n",app.catalog.items[0].favorite);
    } else if (!strcmp(argv[1],"download")) {
        app.library_filters.downloaded_only=true; refresh_catalog_view_cache(&app);
        assert(platform_group_count==2); complete_download(); refresh_catalog_view_cache(&app);
        printf("groups=%zu installed=%d\n",platform_group_count,app.catalog.items[1].installed);
    } else if (!strcmp(argv[1],"touch")) {
        app.active_screen=VP_SCREEN_LIBRARY;
        assert(touch_navigation_pressed(744,513)==0);
        assert(touch_navigation_pressed(745,492)==SCE_CTRL_SELECT);
        assert(touch_navigation_pressed(879,533)==SCE_CTRL_SELECT);
        assert(touch_navigation_pressed(880,513)==0);
        app.library_level=VP_LEVEL_ROMS;
        assert(touch_navigation_pressed(366,513)==SCE_CTRL_TRIANGLE);
        assert(touch_navigation_pressed(510,513)==SCE_CTRL_SQUARE);
        assert(touch_navigation_pressed(675,513)==SCE_CTRL_SELECT);
        assert(touch_navigation_pressed(790,513)==SCE_CTRL_CIRCLE);
        puts("touch: PASS");
    } else if (!strcmp(argv[1],"settings-touch")) {
        printf("save-icon-touch=%d\n",vp_ui_touch_view_toggle(815,513));
    } else if (!strcmp(argv[1],"capacity")) {
        strcpy(app.catalog.items[1].platform,"psvita");
        size_t ix[2]={99,12345};
        size_t n=vp_catalog_library_items(&app.catalog,"psvita",&app.library_filters,ix,1);
        printf("count=%zu guard=%zu\n",n,ix[1]);
    }
    return 0;
}
"""


class LibraryStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="vitashelf-library-state-")
        cls.directory = Path(cls.temporary.name)
        start = SOURCE.index("if (app.library_level != VP_LEVEL_ROMS)", SOURCE.index("int main(void)"))
        end = SOURCE.index("\n                if (pressed & SCE_CTRL_CIRCLE)", start)
        shortcuts = "static void shortcuts(unsigned int pressed) {\n" + SOURCE[start:end] + "\n}\n"
        functions = "\n".join(body(s) for s in (
            "static int library_modal_rows(", "static bool write_library_preference_line(",
            "static bool save_library_preferences(", "static bool load_library_preferences(",
            "static void refresh_catalog_view_cache(", "static void close_library_modal(",
            "static void open_library_modal(", "static void persist_library_preferences(",
            "static void handle_library_modal_input("))
        start = SOURCE.index("        VpItem *completed = vp_catalog_find(&app->catalog, item.id);")
        end = SOURCE.index("    } else if (kind == VP_JOB_INSTALL_HOMEBREW)", start)
        download = "static void complete_download(void) { VpApp *value=&app; VpItem item=app.catalog.items[1]; const char *destination=\"downloads/zelda.sfc\"; char resolved[VP_PATH]; (void)snprintf(resolved,sizeof(resolved),\"%s\",destination);\n" + SOURCE[start:end].replace("app->", "value->").replace("show_notification(app,", "show_notification(value,") + "\n}\n"
        start = SOURCE.index("static unsigned int touch_navigation_pressed(")
        end = SOURCE.index("    if (app.active_screen != VP_SCREEN_SETTINGS)", start)
        touch = SOURCE[start:end] + "return 0;\n}\n"
        cls.binary = cls.directory / "library-state"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Iinclude", "-Isrc/vita",
                        "-x", "c", "-", "src/core/vitashelflauncher.c", "src/core/vita_titles.c",
                        "src/core/library_preferences.c", "src/vita/ui_presentation.c", "-lm", "-o", str(cls.binary)],
                       cwd=ROOT, input=PREFIX + functions + shortcuts + download + touch + CASES, text=True, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], cwd=self.directory, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        return result.stdout

    def test_roundtrip_resync_and_filters(self):
        self.assertEqual(self.run_case("roundtrip"), "roundtrip: PASS\n")

    def test_failed_save_preserves_bytes(self):
        self.run_case("save")
        path = self.directory / "library.ini"
        before = hashlib.sha256(path.read_bytes()).hexdigest()
        self.assertEqual(self.run_case("rename-failure"), "save=0\n")
        after = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else "MISSING"
        self.assertEqual(after, before)

    def test_startup_filter_save_preserves_local_preferences(self):
        self.assertEqual(self.run_case("startup"), "favorite=1 hidden=1\n")

    def test_filtered_platform_selection_stays_valid(self):
        selected, groups = self.run_case("selection").strip().split()
        self.assertLess(int(selected.split("=")[1]), int(groups.split("=")[1]))

    def test_empty_collection_cannot_edit_another_game(self):
        self.assertEqual(self.run_case("empty-actions"), "unselected-local-favorite=0\n")

    def test_download_completion_updates_filters(self):
        self.assertEqual(self.run_case("download"), "groups=3 installed=1\n")

    def test_existing_settings_save_icon_remains_touchable(self):
        self.assertEqual(self.run_case("settings-touch"), "save-icon-touch=1\n")

    def test_bounded_item_output(self):
        self.assertEqual(self.run_case("capacity"), "count=1 guard=12345\n")

    def test_touch_entry_points(self):
        self.assertEqual(self.run_case("touch"), "touch: PASS\n")


if __name__ == "__main__":
    unittest.main()
