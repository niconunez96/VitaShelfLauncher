#include "ui_presentation.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    VpUiAppearance appearance = {180, 16, 130};
    const char *aliases[][2] = {
        {"SNES", "snes"}, {"megadrive", "genesis"}, {"mega-drive", "genesis"},
        {"playstation", "psx"}, {"PS Vita", "vita"},
        {"turbografx-16", "pce"}, {"sms", "mastersystem"}
    };
    size_t i;
    const char *requested[][2] = {
        {"arcade", "platform_arcade"}, {"dreamcast", "platform_dreamcast"}, {"dc", "platform_dreamcast"},
        {"gba", "platform_gba"}, {"3ds", "platform_n3ds"}, {"n3ds", "platform_n3ds"},
        {"nintendo-3ds", "platform_n3ds"}, {"nds", "platform_nds"}, {"ds", "platform_nds"},
        {"gamecube", "platform_gamecube"}, {"ngc", "platform_gamecube"}, {"nintendo-gamecube", "platform_gamecube"},
        {"ps1", "platform_psx"}, {"psx", "platform_psx"}, {"ps2", "platform_ps2"},
        {"playstation2", "platform_ps2"}, {"playstation-2", "platform_ps2"}, {"psp", "platform_psp"},
        {"genesis", "platform_genesis"}, {"snes", "platform_snes"}, {"wii", "platform_wii"},
        {"nintendo-wii", "platform_wii"}, {"Dreamcast", "platform_dreamcast"}
    };
    for (i = 0; i < sizeof(requested) / sizeof(requested[0]); ++i)
        assert(strcmp(vp_ui_platform(requested[i][0])->asset, requested[i][1]) == 0);
    for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i)
        assert(strcmp(vp_ui_platform(aliases[i][0])->asset, vp_ui_platform(aliases[i][1])->asset) == 0);
    assert(strcmp(vp_ui_platform("unknown")->asset, "platform_generic") == 0);
    assert(strcmp(vp_ui_platform(NULL)->asset, "platform_generic") == 0);
    assert(strcmp(vp_ui_platform("gbc")->asset,"platform_gameboycolor")==0);
    assert(strcmp(vp_ui_platform("neogeo")->asset,"platform_neogeo")==0);
    for (i = 0; i < VP_UI_GRID_PAGE; ++i) {
        VpUiRect r = vp_ui_tile(i);
        assert(r.x >= 24 && r.y >= 140 && r.x + r.w <= 936 && r.y + r.h <= 480);
        assert(vp_ui_touch_tile_slot(r.x, r.y) == (int)i);
        assert(vp_ui_touch_tile_slot(r.x + r.w - 1, r.y + r.h - 1) == (int)i);
    }
    assert(VP_UI_CONSOLE_PAGE==6);
    for (i=0;i<VP_UI_CONSOLE_PAGE;i++) {
        VpUiRect r=vp_ui_console_tile(i);
        assert(r.w==136 && r.h==170);
        assert(r.x>=24 && r.x+r.w<=936 && r.y>=158 && r.y+r.h<=440);
        assert(vp_ui_touch_console_slot(r.x+1,r.y+1)==(int)i);
        assert(vp_ui_touch_console_slot(r.x+r.w-1,r.y+r.h-1)==(int)i);
        assert(vp_ui_touch_console_slot(r.x+r.w,r.y)==-1);
    }
    for (i=0;i<VP_UI_LIST_PAGE;i++) {
        VpUiRect r=vp_ui_list_row(i);
        assert(vp_ui_touch_list_slot(r.x,r.y)==(int)i);
        assert(vp_ui_touch_list_slot(r.x+r.w,r.y)==-1);
    }
    assert(VP_SETTINGS_INTEGRATIONS==0 && VP_SETTINGS_DOWNLOADS==1 && VP_SETTINGS_ABOUT==3);
    assert(VP_SETTINGS_COUNT==4);
    for (i=0;i<VP_SETTINGS_COUNT;i++) {
        VpUiRect r=vp_ui_settings_row(i);
        assert(vp_ui_touch_settings_slot(r.x,r.y)==(int)i);
        assert(vp_ui_touch_settings_slot(r.x+r.w-1,r.y+r.h-1)==(int)i);
        assert(vp_ui_touch_settings_slot(r.x+r.w,r.y)==-1);
    }
    assert(vp_ui_touch_settings_slot(310,348)==-1);
    for (i=0;i<VP_UI_APPEARANCE_SLOT_COUNT;i++) {
        VpUiRect r=i<VP_UI_WALLPAPER_COUNT?vp_ui_wallpaper_tile(i):
            i==VP_UI_APPEARANCE_TEXT_COLOR_SLOT?vp_ui_text_color_row():vp_ui_wallpaper_motion_row();
        assert(r.x>=310 && r.x+r.w<=910 && r.y>=100 && r.y+r.h<=490);
        assert(vp_ui_touch_appearance_slot(r.x,r.y)==(int)i);
        assert(vp_ui_touch_appearance_slot(r.x+r.w-1,r.y+r.h-1)==(int)i);
        assert(vp_ui_touch_appearance_slot(r.x+r.w,r.y)==-1);
        assert(vp_ui_touch_appearance_slot(r.x,r.y+r.h)==-1);
    }
    assert(vp_ui_text_color_row().y < vp_ui_wallpaper_motion_row().y);
    {
        VpUiPreferences preferences=VP_UI_PREFERENCES_DEFAULT;
        assert(preferences.wallpaper==0 && preferences.text_color==VP_UI_TEXT_COLOR_WHITE && preferences.animated);
        assert(!strcmp(vp_ui_wallpaper(-1)->id,"dark-blue"));
        assert(!strcmp(vp_ui_wallpaper(VP_UI_WALLPAPER_COUNT)->id,"dark-blue"));
        assert(!strcmp(vp_ui_text_color(-1)->id,"white"));
        assert(!strcmp(vp_ui_text_color(VP_UI_TEXT_COLOR_COUNT)->id,"white"));
        for (i=0;i<VP_UI_WALLPAPER_COUNT;i++) {
            assert(vp_ui_preferences_set(&preferences,"ui_wallpaper",vp_ui_wallpaper((int)i)->id));
            assert(preferences.wallpaper==(int)i);
        }
        assert(!vp_ui_preferences_set(&preferences,"ui_wallpaper","../../other"));
        assert(preferences.wallpaper==VP_UI_WALLPAPER_COUNT-1);
        for (i=0;i<VP_UI_TEXT_COLOR_COUNT;i++) {
            assert(vp_ui_preferences_set(&preferences,"ui_text_color",vp_ui_text_color((int)i)->id));
            assert(preferences.text_color==(int)i);
        }
        assert(!vp_ui_preferences_set(&preferences,"ui_text_color","rainbow"));
        assert(preferences.text_color==VP_UI_TEXT_COLOR_COUNT-1);
        assert(vp_ui_preferences_set(&preferences,"ui_wallpaper_animated","0") && !preferences.animated);
        assert(!vp_ui_preferences_set(&preferences,"ui_wallpaper_animated","2") && !preferences.animated);
        assert(!vp_ui_preferences_set(&preferences,"unrelated","1"));
        assert(vp_ui_preferences_set(&preferences,"ui_wallpaper_animated","1") && preferences.animated);
    }
    for (i=0;i<=180000;i+=17) {
        unsigned long long now=(unsigned long long)i*1000;
        VpUiBackdropTransform t=vp_ui_backdrop_transform(now,true),still=vp_ui_backdrop_transform(now,false);
        assert(t.scale>=1.019f && t.scale<=1.031f);
        assert(t.x<=0 && t.y<=0 && t.x+960*t.scale>=960 && t.y+544*t.scale>=544);
        assert(still.x==0 && still.y==0 && still.scale==1);
    }
    {
        VpUiBackdropTransform a=vp_ui_backdrop_transform(179999999,true),b=vp_ui_backdrop_transform(180000000,true);
        assert(fabsf(a.x-b.x)<.001f && fabsf(a.y-b.y)<.001f && fabsf(a.scale-b.scale)<.001f);
    }
    {
        VpUiRect r=vp_ui_settings_content_card();
        assert(r.x==310 && r.y==148 && r.w==600 && r.h==270);
        assert(r.x+r.w==910);
    }
    assert(vp_ui_console_start(0,12)==0 && vp_ui_console_start(11,12)==6);
    assert(vp_ui_console_start(3,4)==0);
    assert(vp_ui_console_start(0,0)==0);
    for (i=1;i<=25;i++) {
        size_t selected;
        for (selected=0;selected<i;selected++) {
            size_t start=vp_ui_console_start(selected,i);
            assert(start<=selected && selected<start+VP_UI_CONSOLE_PAGE);
            assert(i<=VP_UI_CONSOLE_PAGE?start==0:start+VP_UI_CONSOLE_PAGE<=i);
        }
    }
    assert(vp_ui_touch_tile_slot(47,158)==-1);
    assert(vp_ui_touch_tile_slot(210,158)==-1);
    assert(vp_ui_touch_dock_screen(68,20)==0);
    assert(vp_ui_touch_dock_screen(218,20)==1);
    assert(vp_ui_touch_dock_screen(330,20)==2);
    assert(vp_ui_touch_dock_screen(454,20)==-1);
    assert(vp_ui_touch_dock_screen(90,500)==-1);
    assert(vp_ui_touch_view_toggle(824,513));
    assert(!vp_ui_touch_view_toggle(803,513));
    assert(vp_ui_touch_back(613,513));
    assert(!vp_ui_touch_back(599,513));
    assert(vp_ui_touch_carousel_offset(480,220)==0);
    assert(vp_ui_touch_carousel_offset(260,220)==-1);
    assert(vp_ui_touch_carousel_offset(20,100)==99);
    assert(vp_ui_ease(-1) == 0 && vp_ui_ease(1) == 1 && vp_ui_ease(2) == 1);
    assert(vp_ui_ease(.5f) > .5f);
    vp_ui_appearance_load(&appearance, "id=legacy\nbackground=#101624\n");
    assert(appearance.motion_ms == 180 && appearance.radius == 16 && appearance.wallpaper_alpha == 130);
    vp_ui_appearance_load(&appearance, "ui_motion_ms=0\nui_radius=12\nui_wallpaper_alpha=85\n");
    assert(appearance.motion_ms == 0 && appearance.radius == 12 && appearance.wallpaper_alpha == 85);
    vp_ui_appearance_load(&appearance, "ui_motion_ms=999999999999\nui_radius=1\nui_wallpaper_alpha=-1\n");
    assert(appearance.motion_ms == 0 && appearance.radius == 12 && appearance.wallpaper_alpha == 85);
    puts("presentation tests passed");
    return 0;
}
