#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui_audio.h"
#include "vita_thread_priority.h"

#define VP_UI_AUDIO_SAMPLE_RATE 48000
#define VP_UI_AUDIO_BUFFER_SAMPLES 1024
#define VP_UI_AUDIO_BUFFER_COUNT 4
/* The longest shipped cue is 560 ms at 48 kHz (26880 frames). */
#define VP_UI_AUDIO_MAX_CUE_FRAMES 27000
#define VP_UI_AUDIO_QUEUED_FRAMES (VP_UI_AUDIO_BUFFER_SAMPLES * (VP_UI_AUDIO_BUFFER_COUNT - 1))

typedef struct {
    const char *path;
} VpUiAudioAsset;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool running;
    bool pending;
    VpUiSound sound;
} VpUiAudioPlayer;

/* UI sounds are serialized on a dedicated thread. Keeping only the newest
 * pending cue prevents rapid directional input from accumulating stale
 * navigation clicks. */
static VpUiAudioPlayer player = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER
};

/* sceAudioOutOutput DMAs directly from these buffers: each start address and
 * size must be 64-byte aligned, otherwise the audio is corrupted on hardware.
 * A stack array has no such guarantee, and a static one also keeps them off the
 * small Vita thread stack. Each buffer is 1024 frames * 2 bytes = 2048 bytes.
 * Cues are decoded once at init so the audio thread never waits on storage or
 * libc stdio locks while a download is writing catalog data. Only the audio
 * thread calls play_sound. */
_Alignas(64) static int16_t samples[VP_UI_AUDIO_BUFFER_COUNT][VP_UI_AUDIO_BUFFER_SAMPLES];
_Alignas(64) static int16_t cues[VP_UI_SOUND_COUNT][VP_UI_AUDIO_MAX_CUE_FRAMES];
static size_t cue_frames[VP_UI_SOUND_COUNT];

static const VpUiAudioAsset assets[] = {
    [VP_UI_SOUND_NAVIGATION] = {"app0:assets/audio/01-navegacion-alternativa.wav"},
    [VP_UI_SOUND_TAB] = {"app0:assets/audio/02-cambio-tab.wav"},
    [VP_UI_SOUND_ENTER] = {"app0:assets/audio/03-entrar.wav"},
    [VP_UI_SOUND_BACK] = {"app0:assets/audio/04-volver-salir.wav"},
    [VP_UI_SOUND_DOWNLOAD_START] = {"app0:assets/audio/05-iniciar-descarga.wav"},
    [VP_UI_SOUND_DOWNLOAD_COMPLETE] = {"app0:assets/audio/06-descarga-completa.wav"}
};

static unsigned int read_u32_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8) |
           ((unsigned int)bytes[2] << 16) | ((unsigned int)bytes[3] << 24);
}

static unsigned int read_u16_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
}

static bool seek_pcm_data(FILE *file) {
    unsigned char riff[12];
    bool saw_format = false;
    if (fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;
    for (;;) {
        unsigned char chunk[8];
        unsigned int size;
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) return false;
        size = read_u32_le(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            unsigned char format[16];
            if (size < sizeof(format) || fread(format, 1, sizeof(format), file) != sizeof(format) ||
                read_u16_le(format) != 1 || read_u16_le(format + 2) != 1 ||
                read_u32_le(format + 4) != VP_UI_AUDIO_SAMPLE_RATE || read_u16_le(format + 14) != 16)
                return false;
            if (size > sizeof(format) && fseek(file, (long)(size - sizeof(format)), SEEK_CUR) != 0)
                return false;
            saw_format = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            return saw_format;
        } else if (fseek(file, (long)size, SEEK_CUR) != 0) {
            return false;
        }
        if ((size & 1U) != 0U && fseek(file, 1, SEEK_CUR) != 0) return false;
    }
}

static void load_cue(VpUiSound sound) {
    FILE *file;
    size_t frames;
    if ((unsigned int)sound >= VP_UI_SOUND_COUNT) return;
    file = fopen(assets[sound].path, "rb");
    if (file == NULL || !seek_pcm_data(file)) {
        if (file != NULL) fclose(file);
        cue_frames[sound] = 0;
        return;
    }
    frames = fread(cues[sound], sizeof(cues[sound][0]), VP_UI_AUDIO_MAX_CUE_FRAMES, file);
    cue_frames[sound] = frames;
    if (frames < VP_UI_AUDIO_MAX_CUE_FRAMES)
        memset(cues[sound] + frames, 0,
               (VP_UI_AUDIO_MAX_CUE_FRAMES - frames) * sizeof(cues[sound][0]));
    fclose(file);
}

/* sceAudioOutOutput only waits until a buffer is accepted, not until it has
 * played. Wait until the oldest ring buffer is no longer queued before reusing
 * it; a negative count means the firmware does not report queue depth, so the
 * blocking output call is the only pacing available. */
static bool wait_for_room(int port) {
    while (sceAudioOutGetRestSample(port) > VP_UI_AUDIO_QUEUED_FRAMES)
        sceKernelDelayThread(100);
    return true;
}

static void wait_for_idle(int port) {
    while (sceAudioOutGetRestSample(port) > 0) sceKernelDelayThread(100);
}

static void play_sound(int port, VpUiSound sound) {
    const int16_t *pcm;
    size_t remaining, index = 0;
    if ((unsigned int)sound >= VP_UI_SOUND_COUNT || cue_frames[sound] == 0) return;
    wait_for_idle(port);
    pcm = cues[sound];
    remaining = cue_frames[sound];
    while (remaining > 0) {
        size_t count = remaining < VP_UI_AUDIO_BUFFER_SAMPLES
                       ? remaining : VP_UI_AUDIO_BUFFER_SAMPLES;
        if (!wait_for_room(port)) return;
        memcpy(samples[index], pcm, count * sizeof(samples[index][0]));
        if (count < VP_UI_AUDIO_BUFFER_SAMPLES)
            memset(samples[index] + count, 0,
                   (VP_UI_AUDIO_BUFFER_SAMPLES - count) * sizeof(samples[index][0]));
        if (sceAudioOutOutput(port, samples[index]) < 0) return;
        pcm += count;
        remaining -= count;
        index = (index + 1) % VP_UI_AUDIO_BUFFER_COUNT;
    }
    wait_for_idle(port);
}

static void *audio_thread(void *context) {
    VpUiAudioPlayer *audio = context;
    vp_vita_thread_set_priority(VP_VITA_THREAD_PRIORITY_AUDIO);
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VP_UI_AUDIO_BUFFER_SAMPLES,
                                   VP_UI_AUDIO_SAMPLE_RATE, SCE_AUDIO_OUT_MODE_MONO);
    if (port >= 0) {
        int volume = SCE_AUDIO_OUT_MAX_VOL / 2;
        int volumes[2] = { volume, volume };
        (void)sceAudioOutSetVolume(port,
                                   SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                                   volumes);
    }
    for (;;) {
        VpUiSound sound;
        pthread_mutex_lock(&audio->mutex);
        while (audio->running && !audio->pending)
            (void)pthread_cond_wait(&audio->condition, &audio->mutex);
        if (!audio->running) {
            pthread_mutex_unlock(&audio->mutex);
            break;
        }
        sound = audio->sound;
        audio->pending = false;
        pthread_mutex_unlock(&audio->mutex);
        if (port >= 0) play_sound(port, sound);
    }
    if (port >= 0) (void)sceAudioOutReleasePort(port);
    return NULL;
}

bool vp_ui_audio_init(void) {
    int result;
    unsigned int sound;
    pthread_mutex_lock(&player.mutex);
    if (player.running) {
        pthread_mutex_unlock(&player.mutex);
        return true;
    }
    for (sound = 0; sound < VP_UI_SOUND_COUNT; sound++) load_cue((VpUiSound)sound);
    player.pending = false;
    player.running = true;
    result = pthread_create(&player.thread, NULL, audio_thread, &player);
    if (result != 0) player.running = false;
    pthread_mutex_unlock(&player.mutex);
    return result == 0;
}

void vp_ui_audio_play(VpUiSound sound) {
    pthread_mutex_lock(&player.mutex);
    if (player.running) {
        player.sound = sound;
        player.pending = true;
        (void)pthread_cond_signal(&player.condition);
    }
    pthread_mutex_unlock(&player.mutex);
}

void vp_ui_audio_shutdown(void) {
    bool join = false;
    pthread_mutex_lock(&player.mutex);
    if (player.running) {
        player.running = false;
        (void)pthread_cond_signal(&player.condition);
        join = true;
    }
    pthread_mutex_unlock(&player.mutex);
    if (join) (void)pthread_join(player.thread, NULL);
}
