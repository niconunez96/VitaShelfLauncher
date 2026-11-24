"""Source-level contract for Vita download transfer tuning."""
import hashlib
import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
BACKEND = (ROOT / "src" / "vita" / "vita_backend.c").read_text()


def assert_source_regex(test, source, pattern):
    test.assertTrue(re.search(pattern, source) is not None, pattern)


def function_source(name):
    start = BACKEND.index(name)
    end = BACKEND.index("\n}\n", start) + 2
    return BACKEND[start:end]


class VitaDownloadTuningTests(unittest.TestCase):
    def test_rom_transfer_requests_a_large_receive_buffer(self):
        transfer = function_source("bool vp_vita_transfer_with_progress")
        assert_source_regex(
            self, BACKEND, r"#define\s+VP_TRANSFER_BUFFER_SIZE\s+\(512\s*\*\s*1024\)"
        )
        assert_source_regex(
            self, transfer, r"CURLOPT_BUFFERSIZE,\s*\(long\)\s*VP_TRANSFER_BUFFER_SIZE"
        )

    def test_transfer_raises_the_socket_receive_window(self):
        transfer = function_source("bool vp_vita_transfer_with_progress")
        socket_cb = function_source("static int transfer_socket_options")
        self.assertIn("#include <sys/socket.h>", BACKEND)
        self.assertIn("CURLOPT_SOCKOPTFUNCTION, transfer_socket_options", transfer)
        assert_source_regex(
            self, socket_cb, r"setsockopt\(\(int\)fd,\s*SOL_SOCKET,\s*SO_RCVBUF,"
        )
        self.assertIn("VP_TRANSFER_BUFFER_SIZE", socket_cb)
        self.assertIn("return CURL_SOCKOPT_OK;", socket_cb)

    def test_progress_callback_is_throttled_without_losing_cancellation(self):
        progress = function_source("static int transfer_progress")
        assert_source_regex(
            self, BACKEND, r"#define\s+VP_TRANSFER_PROGRESS_INTERVAL_US\s+\(100\s*\*\s*1000ULL\)"
        )
        self.assertIn("sceKernelGetSystemTimeWide()", progress)
        self.assertIn("!progress->reported", progress)
        self.assertIn("last_report_us", progress)
        assert_source_regex(self, progress, r"return\s+progress->callback\(")

    def test_psp_zip_extraction_is_bounded_and_atomic(self):
        extract = function_source("bool vp_vita_extract_psp_archive")
        for call in ("zip_open", "zip_get_num_entries", "has_unsafe_path",
                     "psp_archive_extension", "sceIoRename", "sceIoRemove(partial_path)",
                     "sceIoRemove(archive_path)"):
            self.assertIn(call, extract)
        self.assertIn("VP_PSP_ARCHIVE_MAX_SIZE", extract)
        self.assertIn("El ZIP contiene mas de una imagen ISO/CSO", extract)

    def test_vitashelf_psp_plugin_request_matches_fork_contract(self):
        for define in ('#define VP_PSP_PLUGIN_SOURCE "app0:psp/vitashelflauncher_pspboot.prx"',
                       '#define VP_PSP_SE_ROOT "ux0:/pspemu/seplugins"',
                       '#define VP_PSP_PLUGIN_TARGET "ux0:/pspemu/seplugins/vitashelflauncher_pspboot.prx"',
                       '#define VP_PSP_PLUGIN_PSP_PATH "ms0:/seplugins/vitashelflauncher_pspboot.prx"',
                       '#define VP_PSP_PLUGIN_LINE_CLASSIC "ms0:/seplugins/vitashelflauncher_pspboot.prx 1"',
                       '#define VP_PSP_PLUGIN_LINE_EPI "xmb, ms0:/seplugins/vitashelflauncher_pspboot.prx, on"',
                       '#define VP_PSP_VSH_PLUGINS "ux0:/pspemu/seplugins/vsh.txt"',
                       '#define VP_PSP_EPI_PLUGINS "ux0:/pspemu/seplugins/EPIplugins.txt"',
                       '#define VP_PSP_SE_PLUGINS "ux0:/pspemu/seplugins/plugins.txt"',
                       '#define VP_PSP_BOOT_REQUEST "ux0:/pspemu/VitaShelfLauncher/boot.txt"',
                       '#define VP_PSP_PLUGIN_LOG "ux0:data/vitashelflauncher/psp-plugin.log"',
                       '#define VP_ADRENALINE_EBOOT "ux0:/app/PSPEMUCFW/eboot.bin"'):
            self.assertIn(define, BACKEND)
        install = function_source("bool vp_vita_psp_plugin_install")
        self.assertIn("VP_PSP_VSH_PLUGINS", install)
        self.assertIn("VP_PSP_EPI_PLUGINS", install)
        self.assertIn("VP_PSP_SE_PLUGINS", install)
        self.assertIn("VP_PSP_LIST_CLASSIC", install)
        self.assertIn("VP_PSP_LIST_EPI", install)
        self.assertIn("same_file_bytes", install)
        self.assertIn("ensure_psp_plugin_dirs", install)
        self.assertIn("copy_whole_file_with_status", install)
        self.assertIn("psp_plugin_copy_failed", install)
        dirs = function_source("static void ensure_psp_plugin_dirs")
        self.assertIn("sceIoMkdir(VP_PSP_SE_ROOT, 0777)", dirs)
        self.assertNotIn('sceIoMkdir("ms0:/seplugins", 0777)', dirs)
        copy = function_source("static bool copy_whole_file_with_status")
        self.assertIn("failure_code", copy)
        self.assertIn("sceIoOpen(destination", copy)
        self.assertNotIn('sceIoOpen("ms0:', BACKEND)
        self.assertNotIn('sceIoMkdir("ms0:', BACKEND)
        self.assertNotIn('"ms0:/PocketShelf', BACKEND)
        self.assertNotIn("pocketshelf.prx", BACKEND)
        enable = function_source("static bool write_plugin_enable_file")
        self.assertIn("VpPspPluginListFormat", enable)
        self.assertIn("VP_PSP_PLUGIN_LINE_CLASSIC", enable)
        self.assertIn("VP_PSP_PLUGIN_LINE_EPI", enable)
        enables = function_source("static bool line_enables_plugin")
        for enabled in ('", on"', '", enabled"', '", true"', '" 1"'):
            self.assertIn(enabled, enables)
        request = function_source("bool vp_vita_write_psp_boot_request")
        self.assertIn("sceRtcGetCurrentTick", request)
        self.assertIn("ms0:/ISO/%s", request)
        self.assertIn("EBOOT.BIN", request)
        self.assertIn("\n-\n", request.replace("\\n", "\n"))
        self.assertIn("now.tick", request)
        self.assertNotIn("now.tick64", request)

    def test_vitashelf_psp_assets_are_packaged_with_pinned_provenance(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        for entry in ("assets/psp/vitashelflauncher_pspboot.prx psp/vitashelflauncher_pspboot.prx",
                      "assets/psp/LICENSE.txt assets/licenses/VitaShelfLauncher-PSPBoot-LICENSE.txt",
                      "assets/psp/THIRD_PARTY_NOTICES.txt assets/licenses/VitaShelfLauncher-PSPBoot-THIRD_PARTY_NOTICES.txt",
                      "assets/psp/provenance.json assets/licenses/VitaShelfLauncher-PSPBoot-provenance.json"):
            self.assertIn(entry, cmake)
        prx = (ROOT / "assets/psp/vitashelflauncher_pspboot.prx").read_bytes()
        self.assertEqual(len(prx), 13158)
        self.assertEqual(hashlib.sha256(prx).hexdigest(),
                         "63065280e04be8434f36f018f45a3d367f005a99cb946fdfe6f9a44156b1ef48")
        provenance = json.loads((ROOT / "assets/psp/provenance.json").read_text())
        self.assertEqual(provenance["upstream_project"], "fthdgn-gh/pocket-shelf")
        self.assertEqual(provenance["upstream_tag"], "shelf-v0.6.1-beta.3")
        self.assertEqual(provenance["upstream_commit"],
                         "2af22cc2013707a9e1ab76cdfc32fb603cb92df6")
        self.assertEqual(provenance["artifact"], "vitashelflauncher_pspboot.prx")
        self.assertEqual(provenance["prx_sha256"], hashlib.sha256(prx).hexdigest())
        self.assertIn("MIT License", (ROOT / "assets/psp/LICENSE.txt").read_text())
        self.assertTrue((ROOT / "assets/psp/THIRD_PARTY_NOTICES.txt").stat().st_size > 0)

    def test_vendored_psp_boot_source_is_renamed_and_attributed(self):
        source = ROOT / "vendor" / "pocket-shelf-psp-boot"
        main = (source / "psp-boot" / "main.c").read_text()
        makefile = (source / "psp-boot" / "Makefile").read_text()
        exports = (source / "psp-boot" / "exports.exp").read_text()
        readme = (source / "README.md").read_text()
        license_text = (source / "LICENSE").read_text()
        build = (ROOT / "scripts" / "build-psp-plugin.sh").read_text()

        self.assertIn('PSP_MODULE_INFO("VitaShelfPspBoot", 0x1000, 1, 0);', main)
        self.assertIn('#define REQUEST_PATH "ms0:/VitaShelfLauncher/boot.txt"', main)
        self.assertIn('#define LOG_PATH "ms0:/VitaShelfLauncher/boot.log"', main)
        self.assertIn('sceKernelCreateThread("VitaShelfPspBoot"', main)
        self.assertNotIn("PocketShelfBoot", main)
        self.assertNotIn("ms0:/PocketShelf", main)
        self.assertIn("TARGET = vitashelflauncher_pspboot", makefile)
        self.assertIn("PRX_EXPORTS = exports.exp", makefile)
        self.assertIn("PSP_EXPORT_FUNC_HASH(module_start)", exports)
        self.assertIn("fthdgn-gh/pocket-shelf", readme)
        self.assertIn("shelf-v0.6.1-beta.3", readme)
        self.assertIn("2af22cc2013707a9e1ab76cdfc32fb603cb92df6", readme)
        self.assertIn("MIT License", license_text)
        self.assertIn("Copyright (c) 2026 fthdgn", license_text)
        self.assertIn("pspdev/pspdev", build)
        self.assertIn("assets/psp/vitashelflauncher_pspboot.prx", build)


if __name__ == "__main__":
    unittest.main()
