"""Source-level contract for Vita UI sound packaging and action mapping."""
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import wave


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOUNDS = {
    "navigation": "01-navegacion-alternativa.wav",
    "tab": "02-cambio-tab.wav",
    "enter": "03-entrar.wav",
    "back": "04-volver-salir.wav",
    "download_start": "05-iniciar-descarga.wav",
    "download_complete": "06-descarga-completa.wav",
}


class VitaAudioTests(unittest.TestCase):
    def test_cues_match_the_player_format_and_have_safe_short_envelopes(self):
        for action, filename in SOUNDS.items():
            with self.subTest(action=action):
                with wave.open(str(ROOT / "assets/audio" / filename), "rb") as cue:
                    self.assertEqual(cue.getnchannels(), 1)
                    self.assertEqual(cue.getsampwidth(), 2)
                    self.assertEqual(cue.getframerate(), 48000)
                    self.assertEqual(cue.getcomptype(), "NONE")
                    duration = cue.getnframes() / cue.getframerate()
                    self.assertGreater(duration, 0.04)
                    self.assertLessEqual(duration, 0.10 if action == "navigation" else 0.60)
                    pcm = cue.readframes(cue.getnframes())
                samples = [value[0] / 32768 for value in struct.iter_unpack("<h", pcm)]
                peak = max(abs(value) for value in samples)
                self.assertGreater(peak, 0.10, "Cue must not be silent")
                self.assertLessEqual(peak, 0.40, "Keep headroom for the Vita output")
                self.assertLess(abs(sum(samples) / len(samples)), 0.002, "DC offset")
                self.assertEqual(samples[0], 0.0)
                self.assertEqual(samples[-1], 0.0)
                self.assertLess(max(abs(b - a) for a, b in zip(samples, samples[1:])), 0.07,
                                "Abrupt waveform steps can produce clicks")

    def test_shipped_cues_are_reproducible_from_the_original_synthesis(self):
        with tempfile.TemporaryDirectory() as output:
            subprocess.run([sys.executable, str(ROOT / "scripts/generate-ui-audio.py"),
                            "--output-dir", output], check=True, capture_output=True)
            for filename in SOUNDS.values():
                with self.subTest(filename=filename):
                    self.assertEqual((pathlib.Path(output) / filename).read_bytes(),
                                     (ROOT / "assets/audio" / filename).read_bytes())

    def test_pcm_sounds_are_packaged_in_the_vpk(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        for filename in SOUNDS.values():
            asset = ROOT / "assets" / "audio" / filename
            self.assertTrue(asset.is_file(), filename)
            self.assertEqual(asset.read_bytes()[:4], b"RIFF")
            self.assertIn(f"assets/audio/{filename}", cmake)

    def test_ui_action_calls_are_mapped_to_the_approved_sound_set(self):
        main = (ROOT / "src" / "vita" / "main.c").read_text()
        audio = (ROOT / "src" / "vita" / "ui_audio.c").read_text()
        self.assertIn('#include "ui_audio.h"', main)
        self.assertRegex(audio, r"VP_UI_SOUND_NAVIGATION.*01-navegacion-alternativa.wav")
        self.assertRegex(audio, r"VP_UI_SOUND_TAB.*02-cambio-tab.wav")
        self.assertRegex(audio, r"VP_UI_SOUND_ENTER.*03-entrar.wav")
        self.assertRegex(audio, r"VP_UI_SOUND_BACK.*04-volver-salir.wav")
        self.assertRegex(audio, r"VP_UI_SOUND_DOWNLOAD_START.*05-iniciar-descarga.wav")
        self.assertRegex(audio, r"VP_UI_SOUND_DOWNLOAD_COMPLETE.*06-descarga-completa.wav")
        self.assertIn("vp_ui_audio_play(VP_UI_SOUND_DOWNLOAD_START);", main)
        self.assertIn("vp_ui_audio_play(VP_UI_SOUND_DOWNLOAD_COMPLETE);", main)
        self.assertGreaterEqual(main.count("vp_ui_audio_play(VP_UI_SOUND_NAVIGATION);"), 2)
        self.assertIn("vp_ui_audio_play(VP_UI_SOUND_TAB);", main)
        self.assertIn("vp_ui_audio_play(VP_UI_SOUND_ENTER);", main)
        self.assertIn("vp_ui_audio_play(VP_UI_SOUND_BACK);", main)

    def test_audio_is_non_blocking_and_uses_the_vita_audio_port(self):
        audio = (ROOT / "src" / "vita" / "ui_audio.c").read_text()
        self.assertIn("pthread_create", audio)
        self.assertIn("sceAudioOutOpenPort", audio)
        self.assertIn("sceAudioOutOutput", audio)
        self.assertIn("sceAudioOutReleasePort", audio)
        self.assertIn("SCE_AUDIO_OUT_MODE_MONO", audio)

    def test_pcm_buffer_start_address_is_64_byte_aligned(self):
        audio = (ROOT / "src" / "vita" / "ui_audio.c").read_text()
        self.assertRegex(
            audio,
            r"_Alignas\(64\)\s+static\s+int16_t\s+samples\s*\[VP_UI_AUDIO_BUFFER_COUNT\]"
            r"\[VP_UI_AUDIO_BUFFER_SAMPLES\]",
        )

    def test_volume_sets_both_channels_even_for_a_mono_port(self):
        audio = (ROOT / "src" / "vita" / "ui_audio.c").read_text()
        self.assertRegex(audio, r"int\s+volumes\[2\]\s*=\s*\{\s*volume\s*,\s*volume\s*\}")
        self.assertIn("SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH", audio)

    def test_output_buffers_rotate_and_drain_only_on_wrap(self):
        audio = (ROOT / "src" / "vita" / "ui_audio.c").read_text()
        self.assertIn("#define VP_UI_AUDIO_BUFFER_COUNT 4", audio)
        read = "fread(samples[index]"
        sent = "sceAudioOutOutput(port, samples[index])"
        advance = "index = (index + 1) % VP_UI_AUDIO_BUFFER_COUNT"
        drained = "if (index == 0 && sceAudioOutOutput(port, NULL) < 0)"
        self.assertIn(read, audio)
        self.assertIn(sent, audio)
        self.assertIn(advance, audio)
        self.assertIn(drained, audio)
        self.assertLess(audio.index(read), audio.index(sent))
        self.assertLess(audio.index(sent), audio.index(advance))
        self.assertLess(audio.index(advance), audio.index(drained))
        self.assertNotIn("sceAudioOutOutput(port, samples)", audio)
        self.assertEqual(audio.count("sceAudioOutOutput(port, NULL)"), 2)


if __name__ == "__main__":
    unittest.main()
