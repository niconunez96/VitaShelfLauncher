"""Host probes for T2 error and cover-cache snapshots."""
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
BACKEND = (ROOT / "src/vita/vita_backend.c").read_text()
MAIN = (ROOT / "src/vita/main.c").read_text()


def body(src, signature):
    start = src.index(signature)
    i = src.index("{", start)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[start:j + 1]
    raise AssertionError(signature)


def run_c(code, name):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / name
        path.write_text(textwrap.dedent(code))
        exe = path.with_suffix(".out")
        cc = subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             str(path), "-pthread", "-o", str(exe)],
            capture_output=True, text=True)
        if cc.returncode:
            raise AssertionError(cc.stdout + cc.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        if run.returncode:
            raise AssertionError(run.stdout + run.stderr)


class T2ConcurrencyProbe(unittest.TestCase):
    def test_last_error_snapshot_is_caller_owned(self):
        functions = "\n".join(body(BACKEND, sig) for sig in (
            "void vp_vita_last_error(char *destination, size_t capacity)",
            "static void store_last_error(",
            "void vp_vita_set_error("))
        code = r"""
        #include <assert.h>
        #include <pthread.h>
        #include <stdbool.h>
        #include <stdint.h>
        #include <stdio.h>
        #include <string.h>
        #define VP_TEXT 192
        static char last_error[VP_TEXT];
        static pthread_mutex_t last_error_mutex = PTHREAD_MUTEX_INITIALIZER;
        /*PARTS*/
        static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
        static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
        static int stage, bad_a, bad_b;

        static void wait_for(int wanted) {
            while (stage < wanted) pthread_cond_wait(&changed, &gate);
        }
        static void advance(int next) {
            stage = next;
            pthread_cond_broadcast(&changed);
        }
        static void *reader(void *arg) {
            int id = (int)(intptr_t)arg;
            char buffer[VP_TEXT];
            pthread_mutex_lock(&gate);
            if (id == 0) {
                wait_for(1);
                vp_vita_last_error(buffer, sizeof(buffer));
                advance(2);
                wait_for(4);
                if (strcmp(buffer, "AAAAAAAA")) bad_a = 1;
            } else {
                wait_for(3);
                vp_vita_last_error(buffer, sizeof(buffer));
                advance(4);
                if (strcmp(buffer, "BBBBBBBB")) bad_b = 1;
            }
            pthread_mutex_unlock(&gate);
            return NULL;
        }
        int main(void) {
            pthread_t a, b;
            vp_vita_set_error("AAAAAAAA");
            pthread_create(&a, NULL, reader, (void *)(intptr_t)0);
            pthread_create(&b, NULL, reader, (void *)(intptr_t)1);
            pthread_mutex_lock(&gate);
            advance(1);
            wait_for(2);
            vp_vita_set_error("BBBBBBBB");
            advance(3);
            wait_for(4);
            pthread_mutex_unlock(&gate);
            pthread_join(a, NULL);
            pthread_join(b, NULL);
            printf("bad_a=%d bad_b=%d\n", bad_a, bad_b);
            return bad_a || bad_b;
        }
        """.replace("/*PARTS*/", functions)
        run_c(code, "last_error_probe.c")

    def test_cover_path_invalidation_reprobes(self):
        functions = "\n".join(body(MAIN, sig) for sig in (
            "static void invalidate_cover_path(",
            "static bool cached_cover_path(",
            "static bool cached_artwork_path("))
        code = r"""
        #include <assert.h>
        #include <stdbool.h>
        #include <stddef.h>
        #include <stdio.h>
        #include <string.h>
        #define VP_TEXT 192
        #define VP_PATH 512
        #define VP_COVER_PATH_CACHE 32
        #define SCE_O_RDONLY 0
        typedef int SceUID;
        typedef struct { char id[VP_TEXT]; } VpItem;
        static bool exists;
        static int opens;
        static int sceIoOpen(const char *p, int m, int f) {
            (void)p; (void)m; (void)f; opens++;
            return exists ? 7 : -1;
        }
        static void sceIoClose(int fd) { (void)fd; }
        static void cover_path_for(const char *id, char *p, size_t n) {
            snprintf(p, n, "cover:%s", id);
        }
        static void artwork_path_for(const char *id, char *p, size_t n) {
            snprintf(p, n, "art:%s", id);
        }
        static void safe_id(const char *id, char *d, size_t n) {
            snprintf(d, n, "%s", id);
        }
        static bool vp_local_icon_path(const VpItem *item, char *p, size_t n) {
            (void)item; (void)p; (void)n; return false;
        }
        typedef struct { char id[VP_TEXT]; char path[VP_PATH]; } VpCoverPathEntry;
        static VpCoverPathEntry cover_path_cache[VP_COVER_PATH_CACHE];
        static size_t cover_path_cache_used;
        static char cached_artwork_id[VP_TEXT];
        static char cached_artwork_path_value[VP_PATH];
        /*PARTS*/
        int main(void) {
            (void)cached_artwork_path;
            char path[VP_PATH];
            VpItem item = {"probe"};
            exists = true;
            assert(cached_cover_path(&item, path, sizeof(path)));
            exists = false;
            invalidate_cover_path("probe");
            assert(!cached_cover_path(&item, path, sizeof(path)));
            exists = true;
            invalidate_cover_path("probe");
            int before = opens;
            bool ok = cached_cover_path(&item, path, sizeof(path));
            printf("ok=%d opens_after=%d before=%d\n", (int)ok, opens, before);
            return !(ok && opens == before + 1);
        }
        """.replace("/*PARTS*/", functions)
        run_c(code, "cover_cache_probe.c")


if __name__ == "__main__":
    unittest.main()
