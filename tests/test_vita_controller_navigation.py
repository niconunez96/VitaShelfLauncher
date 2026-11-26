"""Host probe for left-analog navigation and hold-to-repeat in main.c."""
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "vita" / "main.c").read_text()
MARKER = "/* Controller navigation: analog directions and hold-to-repeat. */"


class VitaControllerNavigationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="vitashelf-controller-nav-")
        cls.directory = pathlib.Path(cls.temporary.name)
        start = MAIN.index(MARKER)
        end = MAIN.index("static void handle_settings_input(", start)
        navigation = MAIN[start:end]
        prefix = r'''
#include "vitashelflauncher.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned int buttons; unsigned char lx, ly, rx, ry; } SceCtrlData;
enum { SCE_CTRL_UP=1u, SCE_CTRL_DOWN=2u, SCE_CTRL_CROSS=4u, SCE_CTRL_CIRCLE=8u,
       SCE_CTRL_SQUARE=16u, SCE_CTRL_TRIANGLE=32u, SCE_CTRL_LEFT=64u, SCE_CTRL_RIGHT=128u,
       SCE_CTRL_SELECT=256u, SCE_CTRL_START=512u, SCE_CTRL_LTRIGGER=1024u,
       SCE_CTRL_RTRIGGER=2048u };
'''
        suffix = r'''
static SceCtrlData pad_at(unsigned int buttons, int lx, int ly) {
    SceCtrlData pad = {0};
    pad.buttons = buttons;
    pad.lx = (unsigned char)lx;
    pad.ly = (unsigned char)ly;
    return pad;
}
int main(void) {
    const unsigned long long t0 = 1000000ULL;
    VpNavRepeat state;
    SceCtrlData pad;

    vp_nav_repeat_reset(&nav_repeat);
    memset(&state, 0, sizeof(state));
    pad = pad_at(0, 128, 128);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == 0);
    assert(state.direction == 0 && state.next_at == 0);

    pad = pad_at(0, 200, 128);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == SCE_CTRL_RIGHT);
    assert(vp_controller_navigation(&pad, 0, t0 + 299999ULL, &state) == 0);
    assert(vp_controller_navigation(&pad, 0, t0 + 300000ULL, &state) == SCE_CTRL_RIGHT);
    assert(vp_controller_navigation(&pad, 0, t0 + 439999ULL, &state) == 0);
    assert(vp_controller_navigation(&pad, 0, t0 + 440000ULL, &state) == SCE_CTRL_RIGHT);

    pad = pad_at(0, 128, 128);
    assert(vp_controller_navigation(&pad, 0, t0 + 500000ULL, &state) == 0);
    assert(state.direction == 0 && state.next_at == 0);
    pad = pad_at(0, 200, 128);
    assert(vp_controller_navigation(&pad, 0, t0 + 500000ULL, &state) == SCE_CTRL_RIGHT);

    memset(&state, 0, sizeof(state));
    pad = pad_at(0, 56, 128);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == SCE_CTRL_LEFT);
    memset(&state, 0, sizeof(state));
    pad = pad_at(0, 128, 56);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == SCE_CTRL_UP);
    memset(&state, 0, sizeof(state));
    pad = pad_at(0, 128, 200);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == SCE_CTRL_DOWN);

    memset(&state, 0, sizeof(state));
    pad = pad_at(0, 200, 160);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == SCE_CTRL_RIGHT);
    memset(&state, 0, sizeof(state));
    pad = pad_at(0, 160, 200);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == SCE_CTRL_DOWN);

    memset(&state, 0, sizeof(state));
    pad = pad_at(0, 150, 128);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == 0);
    pad = pad_at(0, 200, 128);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == SCE_CTRL_RIGHT);
    pad = pad_at(0, 158, 128);
    assert(vp_controller_navigation(&pad, 0, t0 + 300000ULL, &state) == SCE_CTRL_RIGHT);
    pad = pad_at(0, 145, 128);
    assert(vp_controller_navigation(&pad, 0, t0 + 600000ULL, &state) == 0);
    assert(state.direction == 0);

    memset(&state, 0, sizeof(state));
    pad = pad_at(SCE_CTRL_RIGHT, 128, 128);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == SCE_CTRL_RIGHT);
    assert(vp_controller_navigation(&pad, 0, t0 + 299999ULL, &state) == 0);
    assert(vp_controller_navigation(&pad, 0, t0 + 300000ULL, &state) == SCE_CTRL_RIGHT);

    memset(&state, 0, sizeof(state));
    pad = pad_at(SCE_CTRL_LEFT, 200, 128);
    assert(vp_controller_navigation(&pad, 0, t0, &state) == SCE_CTRL_LEFT);

    memset(&state, 0, sizeof(state));
    pad = pad_at(0, 200, 128);
    assert(vp_controller_navigation(&pad, SCE_CTRL_CROSS, t0, &state) == 0);
    assert(state.direction == SCE_CTRL_RIGHT);
    assert(vp_controller_navigation(&pad, 0, t0 + 100000ULL, &state) == 0);
    assert(vp_controller_navigation(&pad, 0, t0 + 300000ULL, &state) == SCE_CTRL_RIGHT);
    assert(vp_controller_navigation(&pad, SCE_CTRL_CROSS, t0 + 440000ULL, &state) == 0);

    vp_nav_repeat_reset(&state);
    assert(state.direction == 0 && state.next_at == 0);
    puts("controller navigation: PASS");
    return 0;
}
'''
        cls.binary = cls.directory / "controller-navigation"
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Iinclude", "-x", "c", "-", "-o", str(cls.binary)],
            cwd=ROOT, input=prefix + navigation + suffix, text=True, check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_analog_directions_repeat_and_dpad_take_priority(self):
        result = subprocess.run([str(self.binary)], cwd=self.directory, text=True,
                                capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout, "controller navigation: PASS\n")

    def test_wiring_reads_analog_axes_and_routes_through_pressed_mask(self):
        self.assertIn("sceCtrlPeekBufferPositive(0, &pad, 1)", MAIN)
        self.assertIn("vp_controller_navigation(&pad", MAIN)
        self.assertIn("pad->lx", MAIN)
        self.assertIn("pad->ly", MAIN)
        self.assertNotIn("pad->rx", MAIN)
        self.assertIn("vp_nav_repeat_reset(&nav_repeat)", MAIN)


if __name__ == "__main__":
    unittest.main()
