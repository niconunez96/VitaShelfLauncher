"""Exercise the Vita controller handler with host queues and isolated UI stubs."""
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class SettingsNavigationTests(unittest.TestCase):
    def test_sections_nested_romm_back_and_download_actions(self):
        source = (ROOT / "src/vita/main.c").read_text()
        start = source.index("static void handle_settings_input(")
        end = source.index("\ntypedef struct", start)
        handler = source[start:end]
        prefix = r'''
#include "vitashelflauncher.h"
#include "ui_presentation.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
enum { SCE_CTRL_UP=1, SCE_CTRL_DOWN=2, SCE_CTRL_CROSS=4, SCE_CTRL_CIRCLE=8,
       SCE_CTRL_SQUARE=16, SCE_CTRL_TRIANGLE=32 };
enum { VP_UI_SOUND_NAVIGATION, VP_UI_SOUND_ENTER, VP_UI_SOUND_BACK,
       VP_JOB_DOWNLOAD_ROM, VP_JOB_PAIR_AND_SYNC };
static int settings_section, settings_field, checks, syncs;
static size_t download_selection;
static bool settings_detail_active, settings_romm_active;
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
static void request_romm_check(void) { checks++; }
static void show_notification(VpApp *app, const char *format, ...) { (void)app; (void)format; }
static bool start_next_download(void) { return false; }
static bool sync_romm_catalog(VpApp *app) { (void)app; syncs++; return true; }
static bool sync_cover_metadata(VpApp *app) { (void)app; return true; }
static bool settings_start_ime(void) { return true; }
static bool save_romm_config(void) { return true; }
static bool background_job_start(int kind, const void *a, const void *b, const void *c) {
    (void)kind; (void)a; (void)b; (void)c; return true;
}
'''
        suffix = r'''
int main(void) {
    static VpApp app;
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
    handle_settings_input(&app,SCE_CTRL_DOWN);
    assert(settings_section==VP_SETTINGS_ABOUT);
    handle_settings_input(&app,SCE_CTRL_CROSS);
    handle_settings_input(&app,SCE_CTRL_TRIANGLE);
    assert(settings_detail_active && !settings_romm_active && syncs==1 && checks==1);
    handle_settings_input(&app,SCE_CTRL_CIRCLE);
    assert(!settings_detail_active);
    puts("Settings controller navigation, nested RomM and download actions: PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix="vitashelf-settings-") as temporary:
            probe = pathlib.Path(temporary) / "navigation"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-Iinclude", "-Isrc/vita", "-x", "c", "-", "src/core/vitashelflauncher.c",
                 "-pthread", "-o", str(probe)],
                cwd=ROOT, input=prefix + handler + suffix, text=True, check=True,
            )
            subprocess.run([str(probe)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
