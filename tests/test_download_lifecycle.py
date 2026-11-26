"""Source-level contract for Vita download pause, resume and removal."""
import pathlib
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include" / "vitashelflauncher.h").read_text()
CORE = (ROOT / "src" / "core" / "vitashelflauncher.c").read_text()
MAIN = (ROOT / "src" / "vita" / "main.c").read_text()
RENDERER = (ROOT / "src" / "vita" / "ui_renderer.c").read_text()
README = (ROOT / "README.md").read_text()


def function_source(source, name):
    start = source.rindex(name)
    end = source.index("\n}\n", start) + 2
    return source[start:end]


class VitaDownloadLifecycleTests(unittest.TestCase):
    def test_core_offers_pause_retry_and_remove(self):
        self.assertIn("VP_DOWNLOAD_PAUSED", HEADER)
        self.assertRegex(HEADER, r"bool\s+vp_download_queue_pause\s*\(")
        self.assertRegex(HEADER, r"bool\s+vp_download_queue_remove\s*\(")

        pause = function_source(CORE, "bool vp_download_queue_pause")
        self.assertIn("VP_DOWNLOAD_PAUSED", pause)

        retry = function_source(CORE, "bool vp_download_queue_retry")
        self.assertIn("VP_DOWNLOAD_PAUSED", retry)

        remove = function_source(CORE, "bool vp_download_queue_remove")
        self.assertRegex(remove, r"queue->count--")

    def test_main_polls_app_events_and_pauses_active_download(self):
        self.assertIn("#include <psp2/appmgr.h>", MAIN)
        self.assertIn("sceAppMgrReceiveSystemEvent", MAIN)
        self.assertIn("SCE_APPMGR_SYSTEMEVENT_ON_DEACTIVATE", MAIN)
        self.assertIn("SCE_APPMGR_SYSTEMEVENT_REQUEST_QUIT", MAIN)
        self.assertIn("pause_requested", MAIN)

        progress = function_source(MAIN, "static bool download_progress")
        self.assertRegex(progress, r"canceled\s*=.*cancel_requested\s*\|\|\s*[^\n]*pause_requested")

        pause_active = function_source(MAIN, "static bool pause_active_download")
        self.assertIn("vp_download_queue_pause", pause_active)

        poll = function_source(MAIN, "static void background_job_poll")
        self.assertIn("paused", poll)
        self.assertIn("return;", poll)

    def test_ui_exposes_resume_and_delete(self):
        self.assertIn("SCE_CTRL_SQUARE", MAIN)
        self.assertIn("vp_download_queue_remove", MAIN)
        self.assertIn("Pausada", RENDERER)
        self.assertIn("Reintentar", RENDERER)
        self.assertIn("Quitar", RENDERER)
        self.assertIn("Cuadrado", README)


class VitaPspRomStagingTests(unittest.TestCase):
    def test_psp_roms_are_staged_in_adrenaline_iso_directory(self):
        self.assertIn('#define VP_PSP_ISO_ROOT "ux0:pspemu/ISO"', MAIN)
        activate = function_source(MAIN, "static void activate_selected")
        self.assertIn("vp_item_install_path(", activate)
        self.assertIn("VP_PSP_ISO_ROOT", activate)
        refresh = function_source(MAIN, "static bool installed_item_path")
        self.assertIn('"iso"', refresh)
        self.assertIn('"cso"', refresh)

    def test_extracted_target_survives_worker_to_launch_target(self):
        worker = function_source(MAIN, "static void *background_job_run")
        self.assertIn("vp_vita_extract_psp_archive(", worker)
        self.assertIn('strcmp(extension, "zip") == 0', worker)
        poll = function_source(MAIN, "static void background_job_poll")
        self.assertIn("resolved", poll)
        self.assertIn("completed->launch_target", poll)
        self.assertIn("resolved", poll[poll.index("completed->launch_target") - 200:])


class VitaNdsRomStagingTests(unittest.TestCase):
    def test_nds_roms_are_staged_in_dsvita(self):
        self.assertIn('#define VP_DSVITA_ROOT "ux0:data/dsvita"', MAIN)
        activate = function_source(MAIN, "static void activate_selected")
        self.assertIn("vp_platform_is_nds(", activate)
        self.assertIn("vp_vita_path_exists(VP_DSVITA_ROOT)", activate)
        self.assertIn("ux0:data/dsvita para descargar juegos NDS", activate)
        self.assertRegex(activate, r"vp_item_install_path\([^;]*VP_DSVITA_ROOT")
        refresh = function_source(MAIN, "static bool installed_item_path")
        self.assertIn("vp_platform_is_nds(", refresh)
        self.assertIn("VP_DSVITA_ROOT", refresh)
        self.assertIn('"nds"', refresh)

    def test_nds_zip_is_extracted_before_launch_target_is_set(self):
        worker = function_source(MAIN, "static void *background_job_run")
        self.assertIn("vp_vita_extract_nds_archive(", worker)
        self.assertIn("vp_platform_is_nds(item.platform)", worker)
        self.assertIn("VP_DSVITA_ROOT", worker)
        poll = function_source(MAIN, "static void background_job_poll")
        self.assertIn("completed->launch_target", poll)
        self.assertIn("resolved", poll[poll.index("completed->launch_target") - 200:])


class VitaAppEventPollingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory(prefix="vp-app-events-")
        cls.addClassCleanup(scratch.cleanup)
        cls.probe = pathlib.Path(scratch.name) / "app_events"
        prefix = r'''
#include "vitashelflauncher.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
typedef enum { VP_JOB_SCAN_LOCAL } VpProbeJobKind;
static struct { pthread_mutex_t mutex; bool active; VpProbeJobKind kind; } background_job =
    { .mutex = PTHREAD_MUTEX_INITIALIZER };
static struct { bool active; } pending_user_job;
typedef struct { int systemEvent; } SceAppMgrSystemEvent;
enum { SCE_APPMGR_SYSTEMEVENT_ON_DEACTIVATE = 1,
       SCE_APPMGR_SYSTEMEVENT_ON_RESUME = 2,
       SCE_APPMGR_SYSTEMEVENT_REQUEST_QUIT = 3 };
static struct { size_t count; VpDownloadEntry entries[1]; } download_queue;
static int scenario, polls, pauses, exit_pauses;
static bool local_scan_pending;
static int sceAppMgrReceiveSystemEvent(SceAppMgrSystemEvent *event) {
    polls++;
    if (scenario == 0) return -1;
    if (scenario == 1) return 0; /* Vita3K's unimplemented success-only stub. */
    if (scenario == 2) {
        event->systemEvent = SCE_APPMGR_SYSTEMEVENT_ON_DEACTIVATE;
        return 0;
    }
    if (scenario == 3) {
        if (polls > 3) return -1;
        event->systemEvent = polls;
        return 0;
    }
    if (scenario == 4) {
        if (polls == 1) event->systemEvent = SCE_APPMGR_SYSTEMEVENT_ON_DEACTIVATE;
        return 0; /* Later calls leave the previous event untouched. */
    }
    event->systemEvent = 99;
    return 0;
}
static bool pause_active_download(VpApp *app, bool exiting) {
    (void)app;
    if (exiting) exit_pauses++;
    else pauses++;
    return true;
}
'''
        suffix = r'''
static VpApp app;
int main(int argc, char **argv) {
    bool quit = false;
    (void)argc;
    scenario = atoi(argv[1]);
    download_queue.count = 1;
    download_queue.entries[0].state = VP_DOWNLOAD_PAUSED;
    handle_app_events(&app, &quit);
    if (scenario==3 && !local_scan_pending) return 2;
    printf("%d|%d|%d|%d|%s", polls, pauses, exit_pauses, quit, app.notice);
    return 0;
}
'''
        handler = function_source(MAIN, "static void handle_app_events")
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0",
             "-I", str(ROOT / "include"), "-x", "c", "-", "-o", str(cls.probe)],
            input=prefix + handler + suffix, text=True, capture_output=True, check=True,
        )

    def poll(self, scenario):
        result = subprocess.run(
            [str(self.probe), str(scenario)], text=True, capture_output=True, timeout=1,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        fields = result.stdout.split("|", 4)
        return [int(value) for value in fields[:4]], fields[4]

    def test_success_without_event_returns_before_rendering(self):
        self.assertEqual(self.poll(1), ([1, 0, 0, 0], ""))

    def test_repeated_valid_events_have_a_per_frame_budget(self):
        counts, notice = self.poll(2)
        self.assertGreater(counts[0], 0)
        self.assertLessEqual(counts[0], 16)
        self.assertEqual(counts, [counts[0], counts[0], 0, 0])
        self.assertEqual(notice, "")

    def test_suspend_resume_and_quit_keep_their_effects(self):
        self.assertEqual(
            self.poll(3),
            ([3, 1, 1, 1], "Descarga en pausa; X para reanudar"),
        )

    def test_failed_receive_leaves_the_application_running(self):
        self.assertEqual(self.poll(0), ([1, 0, 0, 0], ""))

    def test_each_poll_starts_with_a_cleared_event(self):
        self.assertEqual(self.poll(4), ([2, 1, 0, 0], ""))

    def test_unknown_event_does_not_trigger_lifecycle_actions(self):
        self.assertEqual(self.poll(5), ([1, 0, 0, 0], ""))


class VitaDownloadKeepAwakeTests(unittest.TestCase):
    """Keep-awake must follow an active download/install, not idle browsing."""

    @classmethod
    def setUpClass(cls):
        if "static bool download_job_in_progress(void)" not in MAIN:
            raise AssertionError("missing static bool download_job_in_progress(void)")
        scratch = tempfile.TemporaryDirectory(prefix="vp-download-keep-awake-")
        cls.addClassCleanup(scratch.cleanup)
        cls.probe = pathlib.Path(scratch.name) / "keep_awake"
        prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
typedef enum {
    VP_JOB_NONE, VP_JOB_SYNC_PROVIDERS, VP_JOB_PAIR_AND_SYNC, VP_JOB_VALIDATE_ROMM,
    VP_JOB_DOWNLOAD_ROM, VP_JOB_INSTALL_HOMEBREW, VP_JOB_SYNC_COVERS, VP_JOB_SCAN_LOCAL,
    VP_JOB_CHECK_STEAMGRID, VP_JOB_CHECK_IMAGES, VP_JOB_SYNC_STEAMGRID
} VpProbeJobKind;
static struct {
    pthread_mutex_t mutex;
    bool active;
    bool cancel_requested;
    bool pause_requested;
    VpProbeJobKind kind;
} background_job = { .mutex = PTHREAD_MUTEX_INITIALIZER };
'''
        suffix = r'''
int main(void) {
    background_job.active = false;
    background_job.kind = VP_JOB_DOWNLOAD_ROM;
    assert(!download_job_in_progress());
    background_job.active = true;
    background_job.kind = VP_JOB_SCAN_LOCAL;
    assert(!download_job_in_progress());
    background_job.kind = VP_JOB_DOWNLOAD_ROM;
    assert(download_job_in_progress());
    background_job.pause_requested = true;
    assert(!download_job_in_progress());
    background_job.pause_requested = false;
    background_job.cancel_requested = true;
    assert(!download_job_in_progress());
    background_job.cancel_requested = false;
    background_job.kind = VP_JOB_INSTALL_HOMEBREW;
    assert(download_job_in_progress());
    background_job.active = false;
    assert(!download_job_in_progress());
    puts("Gated download keep-awake predicate: PASS");
    return 0;
}
'''
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
             "-x", "c", "-", "-o", str(cls.probe)],
            input=prefix + function_source(MAIN, "static bool download_job_in_progress") + suffix,
            text=True, capture_output=True, check=True,
        )

    def test_gated_tick_contract(self):
        predicate = function_source(MAIN, "static bool download_job_in_progress")
        self.assertIn("VP_JOB_DOWNLOAD_ROM", predicate)
        self.assertIn("VP_JOB_INSTALL_HOMEBREW", predicate)
        self.assertIn("!background_job.cancel_requested", predicate)
        self.assertIn("!background_job.pause_requested", predicate)
        loop = MAIN[MAIN.index("int main(void) {"):]
        loop = loop[:loop.index("if (settings_ime_active)")]
        self.assertLess(loop.index("download_job_in_progress()"), loop.index("sceKernelPowerTick"))
        self.assertIn("SCE_KERNEL_POWER_TICK_DEFAULT", loop)
        self.assertIn("SceProcessmgr_stub", (ROOT / "CMakeLists.txt").read_text())
        self.assertNotIn("sceShellUtilLock", MAIN)

    def test_predicate_follows_active_download_and_install_only(self):
        result = subprocess.run(
            [str(self.probe)], text=True, capture_output=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
