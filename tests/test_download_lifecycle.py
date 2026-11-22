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


class VitaAppEventPollingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory(prefix="vp-app-events-")
        cls.addClassCleanup(scratch.cleanup)
        cls.probe = pathlib.Path(scratch.name) / "app_events"
        prefix = r'''
#include "vitashelflauncher.h"
#include <stdio.h>
#include <stdlib.h>
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


if __name__ == "__main__":
    unittest.main()
