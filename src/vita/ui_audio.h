#ifndef VP_UI_AUDIO_H
#define VP_UI_AUDIO_H

#include <stdbool.h>

typedef enum {
    VP_UI_SOUND_NAVIGATION,
    VP_UI_SOUND_TAB,
    VP_UI_SOUND_ENTER,
    VP_UI_SOUND_BACK,
    VP_UI_SOUND_DOWNLOAD_START,
    VP_UI_SOUND_DOWNLOAD_COMPLETE
} VpUiSound;

#define VP_UI_SOUND_COUNT 6

bool vp_ui_audio_init(void);
void vp_ui_audio_play(VpUiSound sound);
void vp_ui_audio_shutdown(void);

#endif
