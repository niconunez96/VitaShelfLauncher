"""Source-level contract for Vita thread priority ordering during downloads."""
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "vita" / "main.c").read_text()
AUDIO = (ROOT / "src" / "vita" / "ui_audio.c").read_text()
HEADER = (ROOT / "src" / "vita" / "vita_thread_priority.h").read_text()


def function_body(source, signature):
    start = source.index(signature)
    return source[start : source.index("\n}", start) + 2]


def priority(sources, name):
    match = re.search(r"#define\s+%s\s+(\d+)" % name, sources)
    return int(match.group(1))


class VitaThreadPriorityTests(unittest.TestCase):
    def test_priorities_invert_the_vita_scheduler_direction(self):
        self.assertLess(
            priority(HEADER, "VP_VITA_THREAD_PRIORITY_AUDIO"),
            priority(HEADER, "VP_VITA_THREAD_PRIORITY_BACKGROUND"),
        )

    def test_ui_thread_runs_below_audio_but_above_background(self):
        ui = priority(HEADER, "VP_VITA_THREAD_PRIORITY_UI")
        self.assertLess(priority(HEADER, "VP_VITA_THREAD_PRIORITY_AUDIO"), ui)
        self.assertLess(ui, priority(HEADER, "VP_VITA_THREAD_PRIORITY_BACKGROUND"))

    def test_ui_thread_sets_priority_before_startup(self):
        call = "vp_vita_thread_set_priority(VP_VITA_THREAD_PRIORITY_UI);"
        main_start = MAIN.index("int main(void) {")
        self.assertIn(call, MAIN[main_start:])
        self.assertLess(MAIN.index(call), MAIN.index("curl_global_init(", main_start))
        self.assertLess(MAIN.index(call), MAIN.index("for (;;) {", main_start))

    def test_priority_is_set_from_inside_the_thread(self):
        self.assertIn("sceKernelGetThreadId()", HEADER)
        self.assertIn("sceKernelChangeThreadPriority(", HEADER)
        self.assertIn(
            "sceKernelChangeThreadPriority(sceKernelGetThreadId(), priority)", HEADER
        )
        sources = "\n".join(
            (ROOT / "src" / "vita" / path).read_text()
            for path in ("main.c", "ui_audio.c")
        )
        for forbidden in ("pthread_attr_setschedparam", "pthread_setschedparam", "sched_setscheduler"):
            self.assertNotIn(forbidden, sources)

    def test_audio_thread_runs_above_the_download_workers(self):
        self.assertIn('#include "vita_thread_priority.h"', AUDIO)
        body = function_body(AUDIO, "static void *audio_thread(")
        call = "vp_vita_thread_set_priority(VP_VITA_THREAD_PRIORITY_AUDIO);"
        self.assertIn(call, body)
        self.assertLess(body.index(call), body.index("sceAudioOutOpenPort("))

    def test_background_workers_yield_to_render_and_audio(self):
        self.assertIn('#include "vita_thread_priority.h"', MAIN)
        call = "vp_vita_thread_set_priority(VP_VITA_THREAD_PRIORITY_BACKGROUND);"
        worker = function_body(MAIN, "static void *background_job_run(")
        decoder = function_body(MAIN, "static void *cover_decode_thread(")
        self.assertIn(call, worker)
        self.assertIn(call, decoder)
        self.assertLess(worker.index(call), worker.index("pthread_mutex_lock("))
        self.assertLess(decoder.index(call), decoder.index("pthread_mutex_lock("))


if __name__ == "__main__":
    unittest.main()
