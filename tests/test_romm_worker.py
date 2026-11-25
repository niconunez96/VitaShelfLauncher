"""Execute the Vita worker/publication branches with deterministic host backends.

This checks credential-state integration, not Vita threading or SDK behavior.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RommWorkerTests(unittest.TestCase):
    def test_later_rejection_overrides_probe_and_stale_results_cannot_publish(self):
        source = (ROOT / "src/vita/main.c").read_text()
        worker = source[source.index("    if (kind == VP_JOB_VALIDATE_ROMM) {"):
                        source.index("    } else if (kind == VP_JOB_SYNC_COVERS) {")] + "    }\n"
        start = source.index("    if (kind == VP_JOB_VALIDATE_ROMM || kind == VP_JOB_SYNC_PROVIDERS || kind == VP_JOB_PAIR_AND_SYNC) {",
                             source.index("static void background_job_poll"))
        end = source.index("    if (kind == VP_JOB_SYNC_PROVIDERS || kind == VP_JOB_PAIR_AND_SYNC) {", start)
        poll = source[start:end]
        adapter = ""
        if "static bool fetch_romm_catalog(" in source:
            start = source.index("static bool fetch_romm_catalog(")
            adapter = source[start:source.index("\n}", start) + 2]
        prefix = r'''
#include "vitashelflauncher.h"
#include "ui_presentation.h"
#include <pthread.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
enum {VP_JOB_VALIDATE_ROMM, VP_JOB_SYNC_PROVIDERS, VP_JOB_PAIR_AND_SYNC,
      VP_JOB_SYNC_COVERS, VP_JOB_DOWNLOAD_ROM};
typedef struct {
    VpCatalog catalog; VpRommConnectionState romm_state; bool token_exchanged;
    pthread_mutex_t mutex; VpUiSyncStatus sync; bool succeeded;
} Job;
static Job storage={.mutex=PTHREAD_MUTEX_INITIALIZER};
#define background_job storage
static void sync_progress(size_t n,size_t total,void *context) {(void)n;(void)total;(void)context;}
static void sync_result_publish(Job *job) {(void)job;}
static VpRommConfig romm_config, saved;
static VpRommConnection romm_connection;
static char romm_pair_code[VP_TEXT];
static VpApp application;
static int saves, requests;
static bool cache_ok=true;
static long catalog_status;
static void vp_vita_last_error(char *destination, size_t capacity) {
    snprintf(destination, capacity, "%s", "catalog failure");
}
static void vp_vita_set_error(const char *error) {(void)error;}
static long vp_vita_probe_romm(const char *u,const char *t,char *r,size_t n,void *c) {
    (void)u;(void)t;(void)c;
    snprintf(r,n,"{\"items\":[],\"total\":0}");
    return requests++ == 0 ? 200 : catalog_status;
}
static bool vp_vita_fetch(const char *u,const char *t,char *r,size_t n,void *c) {
    (void)u;(void)t;(void)r;(void)n;(void)c; requests++;return false;
}
static bool vp_vita_exchange_pair_code(const char *u,const char *t,char *r,size_t n,void *c) {
    (void)u;(void)t;(void)c;snprintf(r,n,"{\"raw_token\":\"new-token\"}");return true;
}
static bool save_catalog_cache(const VpCatalog *c,const VpRommConfig *r) {(void)c;(void)r;return cache_ok;}
static bool save_romm_config(void) {saved=romm_config;saves++;return true;}
static bool start_next_download(void) {return false;}
static const char *sync_job_title(int kind) {(void)kind;return "RomM: catálogo";}
static void show_notification(VpApp *app,const char *format, ...) {
    va_list arguments; va_start(arguments,format);
    vsnprintf(app->notice,sizeof(app->notice),format,arguments);
    va_end(arguments); app->notice_id++;
}
'''
        run = r'''
static void run(int kind, unsigned long revision) {
    Job *job=&storage;
    VpRommConfig config=romm_config;
    VpHomebrewConfig homebrew={0};
    const char *pair_code="ABCD1234";
    bool succeeded=false,romm_requested=false,homebrew_requested=false;
    requests=0; job->token_exchanged=false;
'''
        finish = r'''
finish:;
    VpApp *app=&application;
    unsigned long romm_revision=revision;
    VpRommConnectionState romm_state=job->romm_state;
    bool token_exchanged=job->token_exchanged;
    char error[VP_TEXT];
    vp_vita_last_error(error, sizeof(error));
'''
        cases = r'''
}
int main(void) {
    VpRommConfig before;
    const long statuses[]={401,403,0,503};
    const VpRommConnectionState expected[]={VP_ROMM_INVALID_TOKEN,VP_ROMM_FORBIDDEN,
                                           VP_ROMM_UNREACHABLE,VP_ROMM_UNREACHABLE};
    size_t i;
    strcpy(romm_config.base_url,"https://a.test");strcpy(romm_config.token,"old-token");
    before=romm_config;
    for (i=0;i<sizeof(statuses)/sizeof(statuses[0]);i++) {
        catalog_status=statuses[i];
        vp_romm_connection_reset(&romm_connection,&romm_config);
        run(VP_JOB_SYNC_PROVIDERS,romm_connection.revision);
        assert(requests==2);
        assert(romm_connection.state==expected[i]);
        assert(vp_romm_pair_code_enabled(romm_connection.state));
        assert(memcmp(&before,&romm_config,sizeof(before))==0);
    }
    catalog_status=401;
    run(VP_JOB_PAIR_AND_SYNC,romm_connection.revision);
    assert(saves==1 && strcmp(saved.token,"new-token")==0);
    assert(romm_connection.state==VP_ROMM_INVALID_TOKEN);
    unsigned long stale=romm_connection.revision;
    strcpy(romm_config.base_url,"https://b.test");vp_romm_connection_reset(&romm_connection,&romm_config);
    strcpy(romm_config.base_url,"https://a.test");vp_romm_connection_reset(&romm_connection,&romm_config);
    before=romm_config;
    run(VP_JOB_PAIR_AND_SYNC,stale);
    assert(saves==1 && memcmp(&before,&romm_config,sizeof(before))==0);
    assert(romm_connection.state==VP_ROMM_CHECKING);
    catalog_status=200;cache_ok=false;
    run(VP_JOB_SYNC_PROVIDERS,romm_connection.revision);
    assert(strstr(application.notice,"RomM: catálogo — fallido"));
    puts("RomM worker rejection, persistence and stale-result probes: PASS");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="vitashelflauncher-romm-") as directory:
            probe = Path(directory) / "probe.c"
            binary = Path(directory) / "probe"
            probe.write_text(prefix + adapter + run + worker + finish + poll + cases)
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src/vita"), str(ROOT / "src/core/vitashelflauncher.c"),
                str(probe), "-pthread", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, cwd=directory)


if __name__ == "__main__":
    unittest.main()
