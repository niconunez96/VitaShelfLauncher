#include "ui_presentation.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Keep this target independent of the core translation units. */
static size_t presentation_core_count(void) { return 10; }

static void test_sync_feedback(void) {
    char text[128];
    VpUiSyncStatus status = {.state=VP_SYNC_RUNNING, .operation=VP_SYNC_METADATA,
        .stage=VP_SYNC_PROCESSING, .processed=3, .total=8};
    assert(vp_ui_sync_percent(&status)==37);
    vp_ui_sync_text(&status,text,sizeof(text));
    assert(strstr(text,"RomM: metadata") && strstr(text,"37% (3/8)"));
    status.processed=8;
    assert(vp_ui_sync_percent(&status)==99);
    status.state=VP_SYNC_SUCCEEDED;
    assert(vp_ui_sync_percent(&status)==100);
    vp_ui_sync_text(&status,text,sizeof(text));
    assert(strstr(text,"RomM: metadata") && strstr(text,"completado") && strstr(text,"100%"));
    status.state=VP_SYNC_FAILED; status.processed=2;
    vp_ui_sync_text(&status,text,sizeof(text));
    assert(strstr(text,"RomM: metadata") && strstr(text,"fallido") && strstr(text,"25%"));
    status.state=VP_SYNC_RUNNING; status.total=0;
    vp_ui_sync_text(&status,text,sizeof(text));
    assert(!strchr(text,'%') && strstr(text,"RomM: metadata") && strstr(text,"2 procesados"));
    status.stage=VP_SYNC_WAITING;status.operation=VP_SYNC_CATALOG;
    vp_ui_sync_text(&status,text,sizeof(text));
    assert(strstr(text,"RomM: catálogo") && strstr(text,"en cola") && !strchr(text,'%'));
    status.operation=VP_SYNC_STEAMGRID; status.stage=VP_SYNC_PROCESSING;
    status.processed=3; status.total=8;
    vp_ui_sync_text(&status,text,sizeof(text));
    assert(strstr(text,"SteamGridDB: carátulas") && strstr(text,"37% (3/8)"));
    assert(!strstr(text,"Metadata"));
    status.processed=0; status.state=VP_SYNC_SUCCEEDED;
    assert(vp_ui_sync_percent(&status)==100);
}

static void test_console_motion(void) {
    VpUiConsoleMotion m={0};
    VpUiRect focused,plain,before,after;
    size_t i;
    vp_ui_console_motion_update(&m,3,12,1000000,180);
    focused=vp_ui_console_motion_tile(&m,3);
    plain=vp_ui_console_tile(3);
    assert(focused.w==150 && focused.h==187 && focused.y<plain.y);
    assert(vp_ui_console_motion_touch(&m,focused.x,focused.y)==3);
    assert(vp_ui_console_motion_touch(&m,focused.x+focused.w-1,focused.y+focused.h-1)==3);
    assert(vp_ui_console_motion_touch(&m,focused.x,focused.y-1)==-1);
    assert(vp_ui_console_motion_touch(&m,focused.x+focused.w,focused.y)==-1);
    vp_ui_console_motion_update(&m,4,12,1100000,180);
    before=vp_ui_console_motion_tile(&m,3);
    assert(!memcmp(&before,&focused,sizeof(before)));
    vp_ui_console_motion_update(&m,4,12,1190000,180);
    assert(m.scroll>0 && m.scroll<1 && m.lift[3]>0 && m.lift[3]<1 && m.lift[4]>0);
    before=vp_ui_console_motion_tile(&m,4);
    assert(vp_ui_console_motion_touch(&m,before.x+before.w/2,before.y+before.h/2)==4);
    vp_ui_console_motion_update(&m,2,12,1190000,180);
    after=vp_ui_console_motion_tile(&m,4);
    assert(!memcmp(&before,&after,sizeof(before)));
    vp_ui_console_motion_update(&m,2,12,1370000,180);
    assert(m.scroll==0 && m.lift[2]==1 && m.lift[3]==0 && m.lift[4]==0);
    /* A direct tap grows its target, never intermediate consoles. */
    vp_ui_console_motion_update(&m,5,12,1400000,180);
    vp_ui_console_motion_update(&m,5,12,1490000,180);
    assert(m.lift[2]>0 && m.lift[5]>0 && m.lift[3]==0 && m.lift[4]==0);
    vp_ui_console_motion_update(&m,11,12,1500000,0);
    focused=vp_ui_console_motion_tile(&m,11);
    assert(m.scroll==6 && focused.x>=24 && focused.x+focused.w<=936);
    assert(!vp_ui_console_motion_visible(&m,5) && vp_ui_console_motion_visible(&m,6));
    vp_ui_console_motion_update(&m,0,12,1600000,180);
    assert(m.scroll==0);
    vp_ui_console_motion_update(&m,11,12,1700000,180);
    assert(m.scroll==6);
    for (i=0;i<=180;i+=15) {
        size_t j;
        vp_ui_console_motion_update(&m,11,12,1700000+i*1000,180);
        for (j=0;j<12;j++) if (vp_ui_console_motion_visible(&m,j)) {
            VpUiRect r=vp_ui_console_motion_tile(&m,j);
            assert(r.y>=210 && r.y+r.h<444 && r.w>=136 && r.w<=150);
        }
    }
    vp_ui_console_motion_update(&m,0,1,2000000,180);
    assert(m.count==1 && m.scroll==0 && m.lift[0]==1);
    vp_ui_console_motion_update(&m,0,0,2100000,180);
    assert(vp_ui_console_motion_touch(&m,100,300)==-1);
    vp_ui_console_motion_update(&m,VP_MAX_LIBRARY_GROUPS-1,VP_MAX_LIBRARY_GROUPS,2200000,0);
    assert(m.count==VP_MAX_LIBRARY_GROUPS && m.lift[VP_MAX_LIBRARY_GROUPS-1]==1);
    vp_ui_console_motion_update(&m,0,VP_MAX_LIBRARY_GROUPS+1,2250000,180);
    assert(m.count==0);
    vp_ui_console_motion_update(&m,4,12,2300000,180);
    vp_ui_console_motion_drag(&m,3.5f,2310000);
    assert(fabsf(m.scroll-3.5f)<.001f && fabsf(m.from_scroll-3.5f)<.001f);
}

static void test_carousel_motion(void) {
    VpUiCarouselMotion m={0};
    VpUiRect center,side,before,after,r;
    int i;
    vp_ui_carousel_motion_update(&m,3,12,1000000,180);
    assert(m.scroll==3 && m.pop==0 && m.count==12);
    center=vp_ui_carousel_motion_tile(&m,3);
    side=vp_ui_carousel_motion_tile(&m,2);
    assert(center.x==370 && center.y==150 && center.w==220 && center.h==246);
    assert(side.x==228 && side.y==190 && side.w==122 && side.h==168);
    assert(vp_ui_carousel_motion_focus(&m,3)==1 && vp_ui_carousel_motion_focus(&m,4)==0);
    vp_ui_carousel_motion_update(&m,4,12,1000000,180);
    assert(m.scroll==3 && m.pop==0);
    vp_ui_carousel_motion_update(&m,4,12,1090000,180);
    assert(m.scroll>3 && m.scroll<4 && m.pop>0);
    r=vp_ui_carousel_motion_tile(&m,4);
    assert(r.x>370 && r.x<604 && r.w>180 && r.w<240);
    assert(vp_ui_carousel_motion_focus(&m,4)>0 && vp_ui_carousel_motion_focus(&m,3)>0);
    before=vp_ui_carousel_motion_tile(&m,4);
    vp_ui_carousel_motion_update(&m,2,12,1090000,180);
    after=vp_ui_carousel_motion_tile(&m,4);
    assert(!memcmp(&before,&after,sizeof(before)));
    assert(m.scroll>3 && m.pop>0);
    vp_ui_carousel_motion_update(&m,2,12,1180000,180);
    assert(m.scroll>2 && m.scroll<3 && m.pop>0);
    vp_ui_carousel_motion_update(&m,2,12,1270000,180);
    assert(m.scroll==2 && m.pop==0);
    r=vp_ui_carousel_motion_tile(&m,2);
    assert(!memcmp(&center,&r,sizeof(r)));
    vp_ui_carousel_motion_update(&m,7,12,1300000,0);
    assert(m.scroll==7 && m.pop==0);
    /* A catalog wrap keeps the pose and slides one card, like any other step. */
    vp_ui_carousel_motion_update(&m,11,12,1400000,180);
    vp_ui_carousel_motion_update(&m,11,12,1580000,180);
    assert(m.scroll==11 && m.pop==0);
    before=vp_ui_carousel_motion_tile(&m,11);
    vp_ui_carousel_motion_update(&m,0,12,1580000,180);
    after=vp_ui_carousel_motion_tile(&m,-1);
    assert(!memcmp(&before,&after,sizeof(before))); /* virtual -1 is the last game */
    assert(m.scroll==-1 && m.from_scroll==-1 && m.pop==0);
    vp_ui_carousel_motion_update(&m,0,12,1670000,180);
    assert(m.scroll>-1 && m.scroll<0 && m.pop>0);
    assert(vp_ui_carousel_motion_tile(&m,-1).x<370); /* outgoing card leaves the center */
    assert(vp_ui_carousel_motion_tile(&m,0).x>370 && vp_ui_carousel_motion_tile(&m,0).x<604);
    for (i=0;i<=180;i+=15) {
        int position;
        vp_ui_carousel_motion_update(&m,0,12,1700000+i*1000,180);
        for (position=-VP_UI_CAROUSEL_REACH;position<=VP_UI_CAROUSEL_REACH;position++) {
            float offset=(float)position-m.scroll;
            VpUiRect tile;
            if (fabsf(offset)>VP_UI_CAROUSEL_PAGE/2.f+.5f) continue;
            tile=vp_ui_carousel_motion_tile(&m,position);
            assert(tile.x>=-200 && tile.x<=1000 && tile.y>=130 && tile.y+tile.h<=420);
        }
    }
    assert(m.scroll==0 && m.pop==0);
    before=vp_ui_carousel_motion_tile(&m,0);
    vp_ui_carousel_motion_update(&m,11,12,1900000,180);
    after=vp_ui_carousel_motion_tile(&m,12);
    assert(!memcmp(&before,&after,sizeof(before)));
    assert(m.scroll==12 && m.from_scroll==12 && m.pop==0);
    vp_ui_carousel_motion_update(&m,11,12,1990000,180);
    assert(m.scroll>11 && m.scroll<12 && m.pop>0);
    vp_ui_carousel_motion_update(&m,11,12,2080000,180);
    assert(m.scroll==11 && m.pop==0);
    vp_ui_carousel_motion_update(&m,5,13,2200000,180);
    assert(m.count==13 && m.scroll==5 && m.pop==0);
    vp_ui_carousel_motion_update(&m,0,0,2300000,180);
    assert(m.count==0 && m.scroll==0);
    vp_ui_carousel_motion_update(&m,0,1,2400000,180);
    assert(m.count==1 && m.scroll==0 && m.pop==0);
}

int main(void) {
    test_sync_feedback();
    VpUiAppearance appearance = {180, 16, 130};
    const char *aliases[][2] = {
        {"SNES", "snes"}, {"megadrive", "genesis"}, {"mega-drive", "genesis"},
        {"playstation", "psx"}, {"PS Vita", "vita"},
        {"turbografx-16", "pce"}, {"sms", "mastersystem"}
    };
    size_t i;
    test_console_motion();
    test_carousel_motion();
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
    {
        float min_scale=0,max_scale=0,min_x=0,max_x=0,min_y=0,max_y=0;
        for (i=0;i<=16000;i+=13) {
            unsigned long long now=(unsigned long long)i*1000;
            VpUiBackdropTransform t=vp_ui_backdrop_transform(now,true),still=vp_ui_backdrop_transform(now,false);
            if (!i || t.scale<min_scale) min_scale=t.scale;
            if (!i || t.scale>max_scale) max_scale=t.scale;
            if (!i || t.x<min_x) min_x=t.x;
            if (!i || t.x>max_x) max_x=t.x;
            if (!i || t.y<min_y) min_y=t.y;
            if (!i || t.y>max_y) max_y=t.y;
            assert(t.scale>=1.029f && t.scale<=1.071f);
            assert(t.x<=0 && t.y<=0 && t.x+960*t.scale>=960 && t.y+544*t.scale>=544);
            assert(still.x==0 && still.y==0 && still.scale==1);
        }
        assert(max_scale-min_scale>.035f && max_x-min_x>18.f && max_y-min_y>10.f);
    }
    {
        VpUiBackdropTransform a=vp_ui_backdrop_transform(15999999,true),b=vp_ui_backdrop_transform(16000000,true);
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
    assert(vp_ui_scroll_clamp(-5,20,1)==0 && vp_ui_scroll_clamp(25,20,1)==19);
    assert(vp_ui_wrap_index(-1,20)==19 && vp_ui_wrap_index(20,20)==0);
    assert(vp_ui_wrap_index(-45,20)==15 && vp_ui_wrap_index(65,20)==5);
    assert(vp_ui_wrap_index(-9,1)==0 && vp_ui_wrap_index(9,0)==0);
    {
        VpUiCarouselMotion m={0};
        VpUiRect before,after;
        vp_ui_carousel_motion_update(&m,0,20,1000000,0);
        vp_ui_carousel_motion_drag(&m,60.25f,1100000);
        assert(m.scroll==60.25f);
        before=vp_ui_carousel_motion_tile(&m,60);
        vp_ui_carousel_motion_rebase(&m);
        after=vp_ui_carousel_motion_tile(&m,0);
        assert(m.scroll==.25f && !memcmp(&before,&after,sizeof(before)));
        vp_ui_carousel_motion_update(&m,19,20,1200000,0);
        vp_ui_carousel_motion_drag(&m,-.75f,1300000);
        before=vp_ui_carousel_motion_tile(&m,-1);
        vp_ui_carousel_motion_rebase(&m);
        after=vp_ui_carousel_motion_tile(&m,19);
        assert(m.scroll==19.25f && !memcmp(&before,&after,sizeof(before)));
    }
    assert(vp_ui_scroll_clamp(4,3,VP_UI_LIST_PAGE)==0);
    assert(vp_ui_list_start(7,20,2)==3 && vp_ui_list_start(1,20,5)==1);
    assert(vp_ui_scrolled_list_row(5,4.5f).y==197);
    assert(vp_ui_grid_rows(0)==0 && vp_ui_grid_rows(5)==1 && vp_ui_grid_rows(11)==3);
    assert(vp_ui_grid_start(11,12,0)==1 && vp_ui_grid_start(0,12,1)==0);
    assert(vp_ui_grid_start(255,256,0)==50 && vp_ui_grid_start(4,5,3)==0);
    {
        VpUiRect r=vp_ui_scrolled_grid_tile(12,1.5f);
        assert(r.x==400 && r.y==238 && r.w==160 && r.h==146);
        for (size_t slot=0;slot<VP_UI_GRID_PAGE;slot++) {
            VpUiRect scrolled=vp_ui_scrolled_grid_tile(slot+10,2),original=vp_ui_tile(slot);
            assert(!memcmp(&scrolled,&original,sizeof(original)));
        }
    }
    {
        VpUiCarouselMotion m={0};
        VpUiRect before,after;
        vp_ui_carousel_motion_update(&m,3,10,1000000,180);
        before=vp_ui_carousel_motion_tile(&m,3);
        assert(before.w==220 && before.h==246);
        vp_ui_carousel_motion_drag(&m,2.5f,1010000);
        after=vp_ui_carousel_motion_tile(&m,3);
        assert(m.scroll==2.5f && after.x>before.x && after.w<before.w);
        vp_ui_carousel_motion_update(&m,4,10,1020000,180);
        assert(m.from_scroll==2.5f);
    }
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
    assert(vp_ui_touch_view_toggle(815,513));
    assert(!vp_ui_touch_view_toggle(803,513));
    {
        VpUiRect closed=vp_ui_library_panel(0.f),open=vp_ui_library_panel(1.f);
        assert(closed.x>=960 && open.x>480 && open.x+open.w<=936);
        assert(vp_ui_library_panel(-1.f).x==closed.x);
        assert(vp_ui_library_panel(2.f).x==open.x);
        for (int step=0;step<=4;step++) {
            float progress=(float)step/4;
            VpUiRect panel=vp_ui_library_panel(progress);
            assert(panel.x<=closed.x && panel.x>=open.x);
            for (size_t row=0;row<VP_LIBRARY_ACTIONS_ARCADE_ROWS;row++) {
                VpUiRect r=vp_ui_library_panel_row(row,progress);
                assert(r.x>=panel.x+24 && r.x+r.w<=panel.x+panel.w-24);
                assert(r.y>=panel.y+90 && r.y+r.h<panel.y+panel.h-70);
                assert(vp_ui_touch_library_panel_row(progress,r.x,r.y,VP_LIBRARY_MODAL_ACTIONS,
                                                     VP_LIBRARY_ACTIONS_ARCADE_ROWS)==(int)row);
                assert(vp_ui_touch_library_panel_row(progress,r.x+r.w-1,r.y+r.h-1,VP_LIBRARY_MODAL_ACTIONS,
                                                     VP_LIBRARY_ACTIONS_ARCADE_ROWS)==(int)row);
                assert(vp_ui_touch_library_panel_row(progress,r.x+r.w,r.y,VP_LIBRARY_MODAL_ACTIONS,
                                                     VP_LIBRARY_ACTIONS_ARCADE_ROWS)==-1);
                assert(vp_ui_touch_library_panel_row(progress,r.x,r.y+r.h,VP_LIBRARY_MODAL_ACTIONS,
                                                     VP_LIBRARY_ACTIONS_ARCADE_ROWS)==-1);
            }
            assert(vp_ui_touch_library_panel_row(progress,vp_ui_library_panel_row(3,progress).x,
                                                 vp_ui_library_panel_row(3,progress).y,
                                                 VP_LIBRARY_MODAL_ACTIONS,VP_LIBRARY_ACTIONS_ROWS)==-1);
        }
        assert(vp_ui_touch_library_panel_row(1.f,50,160,VP_LIBRARY_MODAL_ACTIONS,
                                             VP_LIBRARY_ACTIONS_ARCADE_ROWS)==-1);
        for (size_t row=0;row<presentation_core_count();row++) {
            VpUiRect r=vp_ui_library_panel_core_row(row,1.f);
            assert(r.y>=open.y+64 && r.y+r.h<open.y+open.h-30);
            assert(vp_ui_touch_library_panel_row(1.f,r.x,r.y,VP_LIBRARY_MODAL_CORES,
                                                 (int)presentation_core_count())==(int)row);
        }
    }
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
