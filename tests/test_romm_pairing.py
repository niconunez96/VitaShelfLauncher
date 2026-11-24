"""Exercise the real pairing queue and worker admission without network or Vita SDK."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RommPairingTests(unittest.TestCase):
    def test_pairing_waits_for_worker_and_precedes_automatic_jobs(self):
        source = (ROOT / "src/vita/main.c").read_text()

        def body(signature):
            pos = 0
            while True:
                start = source.index(signature, pos)
                depth = 1
                i = start + len(signature)
                while depth:
                    if source[i] == '(':
                        depth += 1
                    elif source[i] == ')':
                        depth -= 1
                    i += 1
                j = i
                while source[j] in " \t\r\n":
                    j += 1
                if source[j] == '{':
                    break
                pos = start + 1
            return source[start:source.index("\n}", start) + 2]

        state = source[source.index("typedef enum {\n    VP_JOB_NONE"):
                       source.index("static VpDownloadQueue download_queue;")]
        prefix = r'''
#include "vitashelflauncher.h"
#include "ui_presentation.h"
#include "integrations.h"
#include <pthread.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static bool sync_cover_metadata(VpApp *app);
static VpApp app;
static VpRommConfig romm_config;
static VpHomebrewConfig homebrew_config;
static VpRommConnection romm_connection={.revision=7,.state=VP_ROMM_INVALID_TOKEN};
static VpDownloadQueue download_queue;
static unsigned long integration_revision=1;
static bool romm_check_pending,local_scan_pending;
static VpConnectionState steamgrid_state,image_service_state;
static char steamgrid_api_key[VP_TEXT],romm_pair_code[VP_TEXT];
static int launches,create_error;
static void show_notification(VpApp *app,const char *format,...) {
    va_list args;va_start(args,format);
    vsnprintf(app->notice,sizeof(app->notice),format,args);
    va_end(args);app->notice_id++;
}
static int fake_pthread_create(pthread_t *thread,const pthread_attr_t *attrs,void *(*run)(void *),void *ctx) {
    (void)thread;(void)attrs;(void)run;(void)ctx;
    launches++;return create_error;
}
#define pthread_create fake_pthread_create
'''
        worker = r'''
static void *background_job_run(void *context) {
    /* Referenced but never run by the fake thread creator. */
    sync_progress(0,0,context);sync_item_finished(context,true,false,false);
    return NULL;
}
'''
        cases = r'''
int main(void) {
    assert(strstr(sync_job_title(VP_JOB_SYNC_PROVIDERS),"RomM"));
    strcpy(romm_config.base_url,"https://romm.example.test");strcpy(romm_config.token,"expired-fixture");
    strcpy(romm_pair_code,"NEW12345");
    background_job.active=true;background_job.kind=VP_JOB_SCAN_LOCAL;
    romm_check_pending=true;
    request_romm_pairing(&app);
    assert(romm_pairing.active && !launches && !romm_check_pending);
    assert(!strcmp(background_job_active_label(),"búsqueda de juegos instalados"));
    assert(strstr(app.notice,"en cola") && strstr(app.notice,"juegos instalados"));
    VpUiSyncStatus status=sync_status_snapshot(VP_INTEGRATION_ROMM);
    assert(status.state==VP_SYNC_RUNNING && status.stage==VP_SYNC_WAITING);
    assert(strstr(status.detail,"juegos instalados") && !strstr(status.detail,"NEW12345"));
    assert(!strstr(app.notice,"NEW12345"));

    /* A second explicit request fails honestly instead of replacing the queued one. */
    memset(&pending_user_job,0,sizeof(pending_user_job));
    assert(request_user_job(&app,VP_JOB_SYNC_PROVIDERS));
    assert(strstr(app.notice,"en cola"));
    assert(!request_user_job(&app,VP_JOB_CHECK_IMAGES));
    assert(pending_user_job.active && pending_user_job.kind==VP_JOB_SYNC_PROVIDERS);
    assert(strstr(app.notice,"fallido") && !strstr(app.notice,"en cola"));
    memset(&pending_user_job,0,sizeof(pending_user_job));
    assert(request_user_job(&app,VP_JOB_CHECK_STEAMGRID));
    assert(!sync_cover_metadata(&app));
    assert(pending_user_job.active && pending_user_job.kind==VP_JOB_CHECK_STEAMGRID);
    assert(strstr(app.notice,"fallido") && !strstr(app.notice,"en cola"));
    memset(&pending_user_job,0,sizeof(pending_user_job));
    (void)start_pending_user_job();

    request_romm_check();assert(!romm_check_pending);

    /* A finished old check may clear the UI code; the queued snapshot must survive. */
    background_job.active=false;romm_pair_code[0]='\0';
    download_queue.count=1;download_queue.entries[0].state=VP_DOWNLOAD_PENDING;
    assert(start_next_download());
    assert(launches==1 && background_job.active && background_job.kind==VP_JOB_PAIR_AND_SYNC);
    assert(!strcmp(background_job.pair_code,"NEW12345") && !romm_pairing.active && !romm_pairing.code[0]);
    assert(download_queue.entries[0].state==VP_DOWNLOAD_PENDING);
    assert(strstr(app.notice,"Canjeando"));
    strcpy(romm_pair_code,"NEW12345");request_romm_pairing(&app);
    assert(launches==1 && !romm_pairing.active && strstr(app.notice,"ya está en curso"));

    background_job.active=false;strcpy(romm_pair_code,"IDLE1234");
    request_romm_pairing(&app);
    assert(launches==2 && !romm_pairing.active && !strcmp(background_job.pair_code,"IDLE1234"));

    /* Completed-but-uncollected jobs still own the slot until the normal poll joins. */
    background_job.active=true;background_job.completed=true;background_job.kind=VP_JOB_CHECK_IMAGES;
    strcpy(romm_pair_code,"WAIT1234");request_romm_pairing(&app);
    assert(romm_pairing.active && launches==2);
    background_job.active=false;assert(start_next_download());
    assert(launches==3 && !strcmp(background_job.pair_code,"WAIT1234"));

    background_job.active=true;background_job.kind=VP_JOB_SYNC_COVERS;
    request_romm_pairing(&app);assert(romm_pairing.active);
    romm_connection.revision++;
    assert(sync_status_snapshot(VP_INTEGRATION_ROMM).stage!=VP_SYNC_WAITING);
    assert(!start_pending_romm_pairing());
    assert(!romm_pairing.active && !romm_pairing.code[0] && launches==3);

    background_job.active=false;create_error=11;
    request_romm_pairing(&app);
    assert(launches==4 && !background_job.active && !romm_pairing.active);
    assert(strstr(app.notice,"No se pudo iniciar el canje"));
    assert(sync_status_snapshot(VP_INTEGRATION_ROMM).state==VP_SYNC_FAILED);

    /* A queued validation must not claim RomM is being checked before it owns the worker. */
    background_job.active=true;background_job.kind=VP_JOB_SCAN_LOCAL;
    romm_connection.state=VP_ROMM_INVALID_TOKEN;romm_check_pending=false;
    request_romm_check();
    assert(romm_check_pending && romm_connection.state==VP_ROMM_INVALID_TOKEN);
    puts("RomM pairing: busy queue, priority, code snapshot, duplicate, stale, thread failure and honest check queue: PASS");
}
'''
        code = prefix + state + worker + "\n".join(body(signature) for signature in (
            "static void request_romm_check(", "static bool background_job_start(",
            "static bool request_user_job(", "static bool start_pending_user_job(",
            "static bool sync_cover_metadata(", "static bool start_pending_romm_pairing(",
            "static void request_romm_pairing(",
            "static bool start_next_download(")) + cases
        with tempfile.TemporaryDirectory(prefix="vitashelf-romm-pairing-") as temporary:
            binary = Path(temporary) / "pairing"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-Iinclude", "-Isrc/vita", "-x", "c", "-", "src/core/vitashelflauncher.c",
                            "-pthread", "-o", str(binary)], cwd=ROOT, input=code, text=True, check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
