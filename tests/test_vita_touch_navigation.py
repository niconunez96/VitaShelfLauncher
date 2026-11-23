"""Front-touch wiring and host-executed gesture/navigation regressions."""
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "vita" / "main.c").read_text()
CMAKE = (ROOT / "CMakeLists.txt").read_text()


class VitaTouchNavigationTests(unittest.TestCase):
    def test_front_touch_is_started_and_read_from_the_vita_api(self):
        self.assertIn("#include <psp2/touch.h>", MAIN)
        self.assertIn("sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT", MAIN)
        self.assertIn("sceTouchPeek(SCE_TOUCH_PORT_FRONT", MAIN)
        self.assertIn("SceTouchData", MAIN)
        self.assertIn("SceTouch_stub", CMAKE)

    def test_touch_gestures_preserve_taps_and_reuse_controller_actions(self):
        self.assertIn("static unsigned int touch_gesture_pressed", MAIN)
        self.assertIn("VP_TOUCH_SWIPE_SLOP", MAIN)
        self.assertIn("VP_TOUCH_AXIS_HORIZONTAL", MAIN)
        self.assertIn("VP_TOUCH_AXIS_VERTICAL", MAIN)
        self.assertIn("touch_scroll_release(gesture)", MAIN)
        self.assertIn("vp_app_move(&app,(int)target-(int)before,0,columns)", MAIN)
        self.assertIn("vp_ui_touch_dock_screen", MAIN)
        self.assertIn("vp_ui_touch_tile_slot", MAIN)
        self.assertIn("vp_ui_touch_list_slot", MAIN)
        self.assertIn("SCE_CTRL_CROSS", MAIN)
        self.assertIn("SCE_CTRL_TRIANGLE", MAIN)

    def test_raw_touch_coordinates_are_normalized_to_aurora_viewport(self):
        self.assertIn("VP_TOUCH_WIDTH", MAIN)
        self.assertIn("VP_TOUCH_HEIGHT", MAIN)
        self.assertIn("* 960 / VP_TOUCH_WIDTH", MAIN)
        self.assertIn("* 544 / VP_TOUCH_HEIGHT", MAIN)

    def test_gestures_route_by_view_and_settle_at_dragged_distance(self):
        state_start = MAIN.index("typedef enum { VP_TOUCH_AXIS_NONE")
        state_end = MAIN.index("\ntypedef enum {", state_start)
        handlers = MAIN[MAIN.index("static int touch_coordinate_x("):MAIN.index("static void handle_app_events(")]
        prefix = r'''
#include "ui_presentation.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VP_TOUCH_WIDTH 1920
#define VP_TOUCH_HEIGHT 1088
#define VP_TOUCH_TAP_SLOP 32
#define VP_TOUCH_SWIPE_SLOP 32
#define VP_UI_SOUND_NAVIGATION 1
typedef struct { int reportNum; struct { int x,y; } report[2]; } SceTouchData;
static VpApp app;
static size_t platform_group_count,visible_count_cache,visible_indexes[VP_MAX_ITEMS];
static unsigned long long now=1000000;
static float origin;
static int taps,sounds;
static unsigned long long sceKernelGetProcessTimeWide(void) { return now; }
static float vp_ui_scroll_origin(VpLibraryLevel level) { (void)level; return origin; }
static void vp_ui_audio_play(int sound) { (void)sound; sounds++; }
static unsigned int touch_navigation_pressed(int x,int y) { (void)x; (void)y; taps++; return 4; }
static size_t visible_position(const size_t *visible,size_t count,size_t selected) {
    for (size_t i=0;i<count;i++) if (visible[i]==selected) return i;
    return 0;
}
'''
        suffix = r'''
static void sample(int x,int y,int fingers) {
    SceTouchData touch={0}; touch.reportNum=fingers;
    touch.report[0].x=x*2; touch.report[0].y=y*2;
    (void)touch_gesture_pressed(&touch,&touch_gesture);
}
static void setup(VpView view,size_t selected) {
    vp_app_init(&app);
    for (size_t i=0;i<20;i++) {
        VpItem item={0};
        (void)snprintf(item.id,sizeof(item.id),"test:%02u",(unsigned)i);
        (void)snprintf(item.title,sizeof(item.title),"Game %02u",(unsigned)i);
        strcpy(item.platform,"psvita");
        vp_catalog_add(&app.catalog,&item);
    }
    app.library_level=VP_LEVEL_ROMS; strcpy(app.active_platform,"psvita"); app.active_view=view;
    visible_count_cache=vp_app_visible(&app,visible_indexes,VP_MAX_ITEMS);
    assert(visible_count_cache==20);
    app.selected_item=visible_indexes[selected];
    memset(&touch_gesture,0,sizeof(touch_gesture));
    list_start=selected/5*5; list_selected=VP_MAX_ITEMS; origin=(float)selected; taps=sounds=0;
    grid_start=selected/VP_UI_GRID_COLUMNS; grid_selected=VP_MAX_ITEMS;
}
int main(void) {
    setup(VP_VIEW_GRID,0);
    sample(200,410,1); sample(200,170,1);
    assert(touch_gesture.dragging && touch_gesture.position==1.5f && app.selected_item==visible_indexes[0]);
    sample(0,0,0);
    assert(grid_start==2 && app.selected_item==visible_indexes[0] && sounds==0 && !taps);
    now+=180000; touch_scroll_tick(&touch_gesture,now);
    assert(!touch_gesture.settling && touch_gesture.position==2);
    /* A second swipe starts at the scrolled rows, not the selected game's page. */
    sample(200,170,1); sample(200,330,1); sample(0,0,0);
    assert(grid_start==1 && app.selected_item==visible_indexes[0]);
    now+=180000; touch_scroll_tick(&touch_gesture,now);
    sample(200,250,1); sample(201,251,1); sample(0,0,0); assert(taps==1);
    setup(VP_VIEW_GRID,0);
    sample(200,250,1); sample(500,250,1); sample(0,0,0);
    assert(!touch_gesture.settling && grid_start==0 && !taps);
    assert(!touch_drag_allowed(400,200,VP_TOUCH_AXIS_VERTICAL)); /* heading */
    assert(!touch_drag_allowed(400,1026,VP_TOUCH_AXIS_VERTICAL)); /* footer */
    sample(200,200,1); sample(200,450,1); sample(0,0,0);
    assert(grid_start==0); /* bounded at the top */
    setup(VP_VIEW_GRID,0);
    visible_count_cache=11;
    sample(200,450,1); sample(200,150,1); sample(0,0,0);
    assert(grid_start==1 && app.selected_item==visible_indexes[0]); /* partial final row */
    setup(VP_VIEW_GRID,0);
    visible_count_cache=4;
    sample(200,450,1); sample(200,150,1); sample(0,0,0);
    assert(grid_start==0 && !taps); /* one short row cannot overscroll */

    setup(VP_VIEW_LIST,0);
    assert(!touch_drag_allowed(1000,500,VP_TOUCH_AXIS_VERTICAL)); /* hero area */
    assert(!touch_drag_allowed(400,500,VP_TOUCH_AXIS_HORIZONTAL));
    sample(200,400,1); sample(200,130,1);
    assert(touch_gesture.dragging && touch_gesture.position==5 && app.selected_item==visible_indexes[0]);
    sample(0,0,0);
    assert(list_start==5 && app.selected_item==visible_indexes[0] && sounds==0 && !taps);
    now+=180000; touch_scroll_tick(&touch_gesture,now);
    assert(!touch_gesture.settling && touch_gesture.position==5);
    /* Continued drags are anchored to the current list viewport. */
    sample(200,400,1); sample(200,184,1); sample(0,0,0);
    assert(list_start==9 && app.selected_item==visible_indexes[0]);

    setup(VP_VIEW_CAROUSEL,2);
    sample(900,260,1); sample(240,270,1);
    assert(touch_gesture.dragging && touch_gesture.position==7);
    assert(app.selected_item==visible_indexes[2]);
    sample(0,0,0);
    assert(app.selected_item==visible_indexes[7] && sounds==1 && !taps);
    now+=180000; touch_scroll_tick(&touch_gesture,now);
    origin=7; sample(200,260,1); sample(860,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[2]);

    setup(VP_VIEW_CAROUSEL,18);
    sample(900,260,1); sample(240,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[3] && touch_gesture.target==23);
    now+=180000; touch_scroll_tick(&touch_gesture,now);
    origin=3;
    sample(900,260,1); sample(240,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[8]); /* continue after crossing the seam */
    setup(VP_VIEW_CAROUSEL,0);
    sample(200,260,1); sample(860,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[15] && touch_gesture.target==-5);
    setup(VP_VIEW_CAROUSEL,19);
    sample(500,260,1); sample(368,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[0] && touch_gesture.position==20);
    setup(VP_VIEW_CAROUSEL,0);
    sample(500,260,1); sample(632,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[19] && touch_gesture.position==-1);
    setup(VP_VIEW_CAROUSEL,2); visible_count_cache=3;
    sample(900,260,1); sample(240,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[1] && touch_gesture.target==7);
    setup(VP_VIEW_CAROUSEL,0); visible_count_cache=2;
    sample(200,260,1); sample(860,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[1] && touch_gesture.target==-5);
    setup(VP_VIEW_CAROUSEL,0); visible_count_cache=1;
    sample(900,260,1); sample(240,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[0] && touch_gesture.position==0 && !taps);
    setup(VP_VIEW_CAROUSEL,0); visible_count_cache=0;
    sample(900,260,1); sample(240,260,1); sample(0,0,0);
    assert(!touch_gesture.settling && !taps);
    setup(VP_VIEW_CAROUSEL,5);
    sample(500,260,1); sample(480,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[5] && !taps); /* sub-half-card returns to origin */

    setup(VP_VIEW_CAROUSEL,5);
    sample(500,260,1); sample(200,260,1); sample(500,260,1); sample(0,0,0);
    assert(app.selected_item==visible_indexes[5] && !taps); /* drag back is never a tap */
    setup(VP_VIEW_CAROUSEL,5);
    sample(500,30,1); sample(200,260,1); sample(0,0,0); assert(!sounds && !taps);
    sample(500,513,1); sample(200,260,1); sample(0,0,0); assert(!sounds && !taps);
    sample(500,260,1); sample(200,260,2); sample(0,0,0); assert(!sounds && !taps);
    setup(VP_VIEW_CAROUSEL,5);
    sample(500,260,1); sample(200,260,1);
    app.active_view=VP_VIEW_GRID; touch_scroll_tick(&touch_gesture,now); sample(0,0,0);
    assert(!touch_gesture.dragging && !touch_gesture.settling && !sounds && !taps);

    setup(VP_VIEW_CAROUSEL,0);
    for (size_t i=0;i<20;i++)
        (void)snprintf(app.catalog.items[i].platform,sizeof(app.catalog.items[i].platform),"system-%02u",(unsigned)(i%12));
    app.library_level=VP_LEVEL_PLATFORMS; app.active_platform[0]='\0';
    app.selected_platform=2; platform_group_count=12; origin=0;
    sample(900,300,1); sample(160,300,1);
    assert(touch_gesture.position==5);
    sample(0,0,0);
    assert(app.selected_platform==7 && !taps && sounds==1);
    puts("Grid/list content-only scrolling, multi-item carousel, taps, boundaries and cancellation: PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix="vitashelf-touch-") as temporary:
            probe = pathlib.Path(temporary) / "touch"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Iinclude", "-Isrc/vita",
                 "-x", "c", "-", "src/core/vitashelflauncher.c", "src/vita/ui_presentation.c", "-lm", "-o", str(probe)],
                cwd=ROOT, input=prefix + MAIN[state_start:state_end] + handlers + suffix, text=True, check=True,
            )
            subprocess.run([str(probe)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
