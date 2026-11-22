"""Exercise the Vita controller handler with host queues and isolated UI stubs."""
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class SettingsNavigationTests(unittest.TestCase):
    def test_appearance_round_trip_preserves_connections_and_failed_save(self):
        source = (ROOT / "src/vita/main.c").read_text()

        def body(signature):
            start = source.index(signature)
            return source[start:source.index("\n}", start) + 2]

        prefix = r'''
#include "vitashelflauncher.h"
#include "ui_presentation.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define VP_CONFIG_PATH "config.ini"
static VpRommConfig romm_config;
static VpHomebrewConfig homebrew_config;
static VpLaunchConfig launch_config;
static VpUiPreferences ui_preferences=VP_UI_PREFERENCES_DEFAULT;
static VpApp app;
static char active_theme[VP_TEXT]="default";
static void load_theme_file(VpApp *app,const char *path) { (void)path; app->active_view=VP_VIEW_GRID; }
static int sceIoMkdir(const char *path,int mode) { (void)path; (void)mode; return 0; }
static int sceIoRemove(const char *path) { return remove(path); }
static int sceIoRename(const char *from,const char *to) { return rename(from,to); }
'''
        suffix = r'''
int main(void) {
    char saved[4096],after[4096];
    FILE *file=fopen(VP_CONFIG_PATH,"w");
    assert(file);
    fputs("romm_url=http://127.0.0.1:1\nromm_token=test-fixture\n"
          "image_proxy_url=http://127.0.0.1:2/image\nhomebrew_catalog=http://127.0.0.1:3/catalog\n"
          "active_theme=default\nactive_view=list\nlaunch_psx=RETROVITA:pcsx_rearmed\n",file);
    assert(!fclose(file));
    vp_launch_config_init(&launch_config);
    load_providers(&app);
    assert(app.active_view==VP_VIEW_LIST); /* user view wins over the theme default */
    assert(ui_preferences.wallpaper==0 && ui_preferences.text_color==VP_UI_TEXT_COLOR_WHITE && ui_preferences.animated);
    assert(vp_ui_preferences_set(&ui_preferences,"ui_wallpaper","violet"));
    assert(vp_ui_preferences_set(&ui_preferences,"ui_text_color","dark"));
    assert(vp_ui_preferences_set(&ui_preferences,"ui_wallpaper_animated","0"));
    assert(save_romm_config());
    memset(&romm_config,0,sizeof(romm_config));
    memset(&homebrew_config,0,sizeof(homebrew_config));
    vp_launch_config_init(&launch_config);
    ui_preferences=(VpUiPreferences)VP_UI_PREFERENCES_DEFAULT;
    app.active_view=VP_VIEW_GRID;
    load_providers(&app);
    assert(app.active_view==VP_VIEW_LIST);
    assert(ui_preferences.wallpaper==1 && ui_preferences.text_color==VP_UI_TEXT_COLOR_DARK && !ui_preferences.animated);
    assert(!strcmp(romm_config.token,"test-fixture"));
    assert(!strcmp(romm_config.image_proxy_url,"http://127.0.0.1:2/image"));
    assert(!strcmp(homebrew_config.catalog_url,"http://127.0.0.1:3/catalog"));
    const VpLaunchProfile *profile=vp_launch_config_find(&launch_config,"psx");
    assert(profile && !strcmp(profile->titleid,"RETROVITA") && !strcmp(profile->core,"pcsx_rearmed"));
    app.active_view=VP_VIEW_CAROUSEL;
    assert(save_romm_config()); /* RomM saves keep the appearance selection. */
    file=fopen(VP_CONFIG_PATH,"r"); assert(file);
    size_t length=fread(saved,1,sizeof(saved)-1,file); fclose(file);
    saved[length]='\0';
    assert(strstr(saved,"active_view=carousel\n"));
    assert(strstr(saved,"ui_wallpaper=violet\n"));
    assert(strstr(saved,"ui_text_color=dark\n"));
    assert(!symlink("/dev/full",VP_CONFIG_PATH ".tmp"));
    assert(!save_romm_config());
    file=fopen(VP_CONFIG_PATH,"r"); assert(file);
    assert(fread(after,1,sizeof(after),file)==length); fclose(file);
    assert(!memcmp(saved,after,length));
    assert(access(VP_CONFIG_PATH ".tmp",F_OK)!=0);
    assert(!rename(VP_CONFIG_PATH,VP_CONFIG_PATH ".bak")); /* interrupted save leaves no config */
    memset(&romm_config,0,sizeof(romm_config));
    vp_launch_config_init(&launch_config);
    load_providers(&app);
    assert(!strcmp(romm_config.token,"test-fixture"));
    assert(app.active_view==VP_VIEW_CAROUSEL && ui_preferences.wallpaper==1 && ui_preferences.text_color==VP_UI_TEXT_COLOR_DARK);
    assert(access(VP_CONFIG_PATH ".bak",F_OK)!=0);
    puts("View and appearance persistence, backup recovery and failed-save recovery: PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix="vitashelf-preferences-") as temporary:
            probe = pathlib.Path(temporary) / "preferences"
            subprocess.run(
                ["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-Iinclude", "-Isrc/vita", "-x", "c", "-", "src/core/vitashelflauncher.c",
                 "src/vita/ui_presentation.c", "-lm", "-o", str(probe)],
                cwd=ROOT, input=prefix + body("static void trim(") + body("static void load_providers(")
                + body("static bool save_romm_config(") + suffix, text=True, check=True,
            )
            subprocess.run([str(probe)], cwd=temporary, check=True, timeout=5)

    def test_sections_nested_romm_back_and_download_actions(self):
        source = (ROOT / "src/vita/main.c").read_text()
        start = source.index("static void handle_settings_input(")
        end = source.index("\ntypedef struct", start)
        handler = source[start:end]
        touch_start = source.index("static unsigned int touch_navigation_pressed(")
        touch = source[touch_start:source.index("\nstatic unsigned int touch_tap_pressed(", touch_start)]
        prefix = r'''
#include "vitashelflauncher.h"
#include "ui_presentation.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
enum { SCE_CTRL_UP=1, SCE_CTRL_DOWN=2, SCE_CTRL_CROSS=4, SCE_CTRL_CIRCLE=8,
       SCE_CTRL_SQUARE=16, SCE_CTRL_TRIANGLE=32, SCE_CTRL_LEFT=64, SCE_CTRL_RIGHT=128 };
enum { VP_UI_SOUND_NAVIGATION, VP_UI_SOUND_ENTER, VP_UI_SOUND_BACK, VP_UI_SOUND_TAB,
       VP_JOB_DOWNLOAD_ROM, VP_JOB_PAIR_AND_SYNC };
static int settings_section, settings_field, checks, syncs;
static size_t download_selection;
static bool settings_detail_active, settings_romm_active;
static bool settings_ime_active;
static VpApp app;
static size_t platform_group_count,visible_count_cache,visible_indexes[256];
static VpUiPreferences ui_preferences=VP_UI_PREFERENCES_DEFAULT;
static bool save_ok=true;
static int saves;
static char romm_pair_code[VP_TEXT];
static VpRommConnection romm_connection;
static VpDownloadQueue download_queue;
static struct {
    pthread_mutex_t mutex;
    bool active, cancel_requested, pause_requested;
    int kind;
    VpItem item;
} background_job = {.mutex=PTHREAD_MUTEX_INITIALIZER};
static void vp_ui_audio_play(int sound) { (void)sound; }
static void refresh_catalog_view_cache(VpApp *value) { (void)value; }
static size_t visible_position(const size_t *indexes,size_t count,size_t item) {
    size_t i;
    for (i=0;i<count;i++) if (indexes[i]==item) return i;
    return 0;
}
static void request_romm_check(void) { checks++; }
static void show_notification(VpApp *app, const char *format, ...) { (void)app; (void)format; }
static bool start_next_download(void) { return false; }
static bool sync_romm_catalog(VpApp *app) { (void)app; syncs++; return true; }
static bool sync_cover_metadata(VpApp *app) { (void)app; return true; }
static bool settings_start_ime(void) { return true; }
static bool save_romm_config(void) { saves++; return save_ok; }
static bool background_job_start(int kind, const void *a, const void *b, const void *c) {
    (void)kind; (void)a; (void)b; (void)c; return true;
}
'''
        suffix = r'''
int main(void) {
    handle_settings_input(&app,SCE_CTRL_UP);
    assert(settings_section==VP_SETTINGS_INTEGRATIONS);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(settings_detail_active && !settings_romm_active && checks==0);
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_field==0);
    handle_settings_input(&app,SCE_CTRL_TRIANGLE);
    assert(syncs==0 && !settings_romm_active);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(settings_romm_active && checks==1);
    handle_settings_input(&app,SCE_CTRL_DOWN);
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_field==2);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(syncs==1);
    handle_settings_input(&app,SCE_CTRL_CIRCLE);
    assert(settings_detail_active && !settings_romm_active);
    handle_settings_input(&app,SCE_CTRL_CIRCLE);
    assert(!settings_detail_active);
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_section==VP_SETTINGS_DOWNLOADS);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    download_queue.count=1;
    strcpy(download_queue.entries[0].item.id,"romm:1");
    download_queue.entries[0].state=VP_DOWNLOAD_PAUSED;
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(download_queue.entries[0].state==VP_DOWNLOAD_PENDING);
    handle_settings_input(&app,SCE_CTRL_SQUARE);
    assert(download_queue.count==0);
    handle_settings_input(&app,SCE_CTRL_CIRCLE);
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_section==VP_SETTINGS_APPEARANCE);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(settings_detail_active && settings_field==0);
    handle_settings_input(&app,SCE_CTRL_RIGHT);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(ui_preferences.wallpaper==1 && saves==1);
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_field==3);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(ui_preferences.wallpaper==3 && saves==2);
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_field==VP_UI_APPEARANCE_TEXT_COLOR_SLOT);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(ui_preferences.text_color==VP_UI_TEXT_COLOR_LIGHT && saves==3);
    handle_settings_input(&app,SCE_CTRL_LEFT);
    assert(ui_preferences.text_color==VP_UI_TEXT_COLOR_WHITE && saves==4);
    save_ok=false;
    handle_settings_input(&app,SCE_CTRL_RIGHT);
    assert(ui_preferences.text_color==VP_UI_TEXT_COLOR_WHITE && saves==5);
    save_ok=true;
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_field==VP_UI_APPEARANCE_MOTION_SLOT);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(!ui_preferences.animated && saves==6);
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_field==0);
    handle_settings_input(&app,SCE_CTRL_CIRCLE);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    assert(settings_field==3);
    handle_settings_input(&app,SCE_CTRL_CIRCLE);
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_section==VP_SETTINGS_ABOUT);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    handle_settings_input(&app,SCE_CTRL_TRIANGLE);
    assert(settings_detail_active && !settings_romm_active && syncs==1 && checks==1);
    handle_settings_input(&app,SCE_CTRL_CIRCLE);
    assert(!settings_detail_active);
    app.active_screen=VP_SCREEN_SETTINGS;
    VpUiRect row=vp_ui_settings_row(VP_SETTINGS_APPEARANCE);
    handle_settings_input(&app,touch_navigation_pressed(row.x+1,row.y+1));
    assert(settings_detail_active && settings_section==VP_SETTINGS_APPEARANCE);
    for (int i=0;i<VP_UI_WALLPAPER_COUNT;i++) {
        VpUiRect tile=vp_ui_wallpaper_tile((size_t)i);
        handle_settings_input(&app,touch_navigation_pressed(tile.x+1,tile.y+1));
        assert(ui_preferences.wallpaper==i && settings_field==i);
        assert(touch_navigation_pressed(tile.x+tile.w,tile.y)==0);
        assert(settings_field==i);
    }
    row=vp_ui_text_color_row();
    int text_color=ui_preferences.text_color;
    handle_settings_input(&app,touch_navigation_pressed(row.x+1,row.y+1));
    assert(ui_preferences.text_color==(text_color+1)%VP_UI_TEXT_COLOR_COUNT);
    row=vp_ui_wallpaper_motion_row();
    bool animated=ui_preferences.animated;
    handle_settings_input(&app,touch_navigation_pressed(row.x+1,row.y+1));
    assert(ui_preferences.animated!=animated);
    handle_settings_input(&app,touch_navigation_pressed(630,513));
    assert(!settings_detail_active);
    puts("Settings controller/touch navigation, appearance, nested RomM and download actions: PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix="vitashelf-settings-") as temporary:
            probe = pathlib.Path(temporary) / "navigation"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-Iinclude", "-Isrc/vita", "-x", "c", "-", "src/core/vitashelflauncher.c",
                 "src/vita/ui_presentation.c", "-lm", "-pthread", "-o", str(probe)],
                cwd=ROOT, input=prefix + handler + touch + suffix, text=True, check=True,
            )
            subprocess.run([str(probe)], check=True, timeout=5)

    def test_triangle_view_change_saves_and_rolls_back(self):
        source = (ROOT / "src/vita/main.c").read_text()
        start = source.index("if (pressed & SCE_CTRL_TRIANGLE) {", source.index("int main(void)"))
        end = source.index("\n                if ((pressed & SCE_CTRL_SQUARE)", start)
        toggle = source[start:end]
        prefix = r'''
#include "vitashelflauncher.h"
#include <assert.h>
#include <stdio.h>
enum { SCE_CTRL_TRIANGLE=32 };
static VpApp app;
static unsigned int pressed;
static bool save_ok=true;
static int saves,notices;
static void show_notification(VpApp *value,const char *format,...) { (void)value; (void)format; notices++; }
static bool save_romm_config(void) { saves++; return save_ok; }
static void apply_view_toggle(void) {
'''
        suffix = r'''
}
int main(void) {
    vp_app_init(&app);
    pressed=SCE_CTRL_TRIANGLE;
    apply_view_toggle();
    assert(app.active_view==VP_VIEW_LIST && saves==1 && notices==0);
    pressed=0; saves=0;
    apply_view_toggle();
    assert(app.active_view==VP_VIEW_LIST && saves==0 && notices==0);
    save_ok=false; pressed=SCE_CTRL_TRIANGLE; saves=0;
    apply_view_toggle();
    assert(app.active_view==VP_VIEW_LIST && saves==1 && notices==1);
    puts("Triangle view toggle save and rollback: PASS");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="vitashelf-view-toggle-") as temporary:
            probe = pathlib.Path(temporary) / "toggle"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-Iinclude", "-x", "c", "-", "src/core/vitashelflauncher.c", "-lm", "-o", str(probe)],
                cwd=ROOT, input=prefix + toggle + suffix, text=True, check=True,
            )
            subprocess.run([str(probe)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
