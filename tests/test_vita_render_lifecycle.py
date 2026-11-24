"""Source-level lifecycle guards; these do not emulate the Vita GPU or PNG decoder."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/vita/main.c").read_text()


def function_body(signature):
    start = SOURCE.index(signature)
    return SOURCE[start : SOURCE.index("\n}", start) + 2]


class VitaRenderLifecycleTests(unittest.TestCase):
    def test_romm_validation_stays_in_the_worker(self):
        worker = function_body("static void *background_job_run(")
        self.assertIn("vp_romm_check_connection(", worker)
        for signature in ("static void prepare_shell(", "static bool settings_start_ime(",
                          "static void handle_settings_input("):
            self.assertNotIn("vp_romm_check_connection(", function_body(signature))
        self.assertIn("vp_romm_pair_code_enabled(", function_body("static bool settings_start_ime("))
        self.assertIn("vp_romm_pair_code_enabled(", function_body("static void handle_settings_input("))

    def test_romm_results_are_revision_guarded_before_publication(self):
        poll = function_body("static void background_job_poll(")
        guard = poll.index("vp_romm_connection_apply(")
        self.assertLess(guard, poll.index("romm_config = config;"))
        self.assertLess(guard, poll.index("apply_provider_catalog("))

    def test_frame_waits_before_preparation_and_pool_reuse(self):
        body = function_body("int main(void)")
        loop = body[body.index("for (;;) {") : body.index("\n    vp_ui_shutdown();")]
        calls = [
            "vita2d_wait_rendering_done();",
            "prepare_shell(&frame, font, &app);",
            "vita2d_start_drawing();",
            "vita2d_clear_screen();",
            "vp_ui_render(&frame);",
            "vita2d_end_drawing();",
            "vita2d_swap_buffers();",
        ]
        # Ignore the existing dialog-only swap: normal UI frames follow this order.
        normal_frame = loop[loop.index("vita2d_wait_rendering_done();") :]
        positions = [normal_frame.index(call) for call in calls]
        self.assertEqual(positions, sorted(positions))

    def test_texture_preparation_does_not_draw(self):
        body = function_body("static void prepare_shell(")
        self.assertIn("covers_step(app, cover_indexes, cover_count);", body)
        for call in ("vita2d_start_drawing(", "vita2d_clear_screen(", "vp_ui_render("):
            self.assertNotIn(call, body)

    def test_notifications_are_ephemeral_and_not_retriggered_by_navigation(self):
        main = (ROOT / "src/vita/main.c").read_text()
        renderer = (ROOT / "src/vita/ui_renderer.c").read_text()
        header = (ROOT / "src/vita/ui_renderer.h").read_text()
        self.assertNotIn("Biblioteca cargada desde cache", main)
        self.assertNotIn("refresh_notice", header)
        self.assertIn("notice_id", header)
        self.assertIn("f->notice_id != last_notice_id", renderer)
        self.assertIn("if (age < 4)", renderer)
        self.assertIn("round_rect(636,64,296,54", renderer)

    def test_background_texture_is_resolved_after_slot_eviction(self):
        body = function_body("static void prepare_shell(")
        prepare = body.index("covers_step(app, cover_indexes, cover_count);")
        resolve = body.index("background_texture = cover_texture_for(")
        self.assertLess(prepare, resolve)
        self.assertIn("frame->background = background_texture;", body)

    def test_cover_diagnostics_bracket_decode_and_close_the_log(self):
        body = function_body("static vita2d_texture *load_texture_file(")
        start = body.index('record_cover_event("load-start", id, path);')
        decode = body.index("vita2d_load_PNG_file(path)")
        done = body.index('record_cover_event(texture != NULL ? "load-ok" : "load-failed"')
        self.assertLess(start, decode)
        self.assertLess(decode, done)
        for event in ("load-open-failed", "load-size-rejected", "load-header-rejected"):
            self.assertIn(event, body)
        log = function_body("static void record_cover_event(")
        self.assertIn("fclose(log);", log)
        self.assertIn("64 * 1024", log)
        self.assertNotIn("cover_url", log)

    def test_cover_decoding_runs_on_a_dedicated_worker(self):
        step = function_body("static void covers_step(")
        self.assertIn("cover_decode_collect();", step)
        self.assertIn("cover_decode_request(item->id, path)", step)
        self.assertNotIn("vita2d_load_PNG_file", step)
        worker = function_body("static void *cover_decode_thread(")
        self.assertIn("load_texture_file(id, path)", worker)
        init = function_body("static bool cover_decoder_init(")
        self.assertIn("pthread_create", init)
        main_body = function_body("int main(void)")
        self.assertIn("cover_decoder_init();", main_body)
        self.assertIn("cover_decoder_shutdown();", main_body)

    def test_manual_cover_sync_is_the_only_fetch_path(self):
        self.assertNotIn("VP_JOB_FETCH_COVER", SOURCE)
        self.assertIn("VP_JOB_SYNC_COVERS", SOURCE)
        self.assertIn("static bool sync_cover_metadata(", SOURCE)
        step = function_body("static void covers_step(")
        self.assertNotIn("background_job_start", step)

    def test_psp_x_launch_uses_the_bundled_plugin_without_a_bubble(self):
        activate = function_body("static void activate_selected")
        for call in ("vp_platform_is_psp", "vp_vita_psp_runtime_status",
                     "vp_vita_psp_plugin_install", "vp_vita_write_psp_boot_request",
                     'psgm:play?titleid=PSPEMUCFW'):
            self.assertIn(call, activate)
        self.assertIn("Presiona X otra vez para instalar el complemento PSP", activate)
        self.assertIn("Instala Adrenaline (PSPEMUCFW)", activate)
        psp = activate[activate.index("if (vp_platform_is_psp") :
                       activate.index("if (item->installed && item->launch_target[0]")]
        self.assertNotIn("snprintf(app->notice", psp)
        for call in ("show_notification(app, \"%.150s\", error)",
                     "show_notification(app, \"%.150s\", vp_vita_last_error())",
                     "show_notification(app, \"No se pudo abrir Adrenaline"):
            self.assertIn(call, psp)


if __name__ == "__main__":
    unittest.main()
