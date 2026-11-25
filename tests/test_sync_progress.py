"""Run the actual artwork loops and status publication with deterministic host I/O."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SyncProgressTests(unittest.TestCase):
    def test_processed_counts_results_and_configuration_revisions(self):
        source = (ROOT / "src/vita/main.c").read_text()
        state = source[source.index("typedef enum {\n    VP_JOB_NONE"):
                       source.index("static VpDownloadQueue download_queue;")]
        worker = source[source.index("static void *background_job_run("):]
        covers = worker[worker.index("    } else if (kind == VP_JOB_SYNC_COVERS) {"):
                        worker.index("    } else if (kind == VP_JOB_SCAN_LOCAL) {")]
        steamgrid = worker[worker.index("    } else if (kind == VP_JOB_SYNC_STEAMGRID) {"):
                           worker.index("    } else {\n        VpTransferBackend")]
        prefix = r'''
#include "vitashelflauncher.h"
#include "ui_presentation.h"
#include "integrations.h"
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static VpRommConnection romm_connection={.revision=1};
static unsigned long integration_revision=1;
static void safe_id(const char *id,char *dest,size_t n);
#define VP_ARTWORK_CACHE "cache"
'''
        backends = r'''
static VpUiSyncStatus status_snapshot(int integration) {
    VpJobStatusSnapshot snapshot = {0};
    snapshot.active = background_job.active;
    snapshot.kind = background_job.kind;
    snapshot.sync = background_job.sync;
    snapshot.romm_revision = background_job.romm_revision;
    snapshot.integration_revision = background_job.integration_revision;
    return sync_status_snapshot(&snapshot, integration);
}
static int requests, rename_result, removals, cleared;
static bool transform_ok=true, background_ok=true, save_ok=true, cached;
static VpConnectionState connection=VP_CONNECTION_CONNECTED;
static VpArtworkResult lookup=VP_ARTWORK_FOUND;
static void safe_id(const char *id,char *dest,size_t n) {snprintf(dest,n,"%s",id);}
static void cover_path_for(const char *id,char *out,size_t n) {snprintf(out,n,"%s.png",id);}
static void artwork_path_for(const char *id,char *out,size_t n) {cover_path_for(id,out,n);}
static bool cached_cover_valid(const char *path) {(void)path;return cached;}
static int sceIoRemove(const char *path) {(void)path;removals++;return 0;}
static int sceIoRename(const char *a,const char *b) {(void)a;(void)b;return rename_result;}
static bool vp_vita_transform_cover(const char *a,const char *b,const char *c,const char *d,void *e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;return transform_ok;
}
#define vp_vita_transform_background vp_vita_transform_cover
bool vp_cache_cover(const VpItem *item,const VpRommConfig *config,const char *path,VpCoverTransformBackend backend) {
    (void)config;(void)path;(void)backend;
    return !strcmp(item->cover_url,"background")?background_ok:transform_ok;
}
static long vp_vita_probe_service(const char *a,const char *b,char *c,size_t d,void *e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;return 200;
}
VpConnectionState vp_steamgrid_check(const char *key,VpRommProbeBackend backend) {
    (void)key;(void)backend;return connection;
}
VpArtworkResult vp_steamgrid_cover(VpItem *item,const char *key,VpRommProbeBackend backend,VpConnectionState *state) {
    (void)item;(void)key;(void)backend;(void)state;requests++;return lookup;
}
static bool vp_local_artwork_save(const VpItem *item,const char *path) {(void)item;(void)path;return save_ok;}
static bool vp_local_artwork_clear(VpItem *item,const char *path) {
    (void)path;cleared++;item->remote_id[0]=0;item->cover_url[0]=0;return true;
}
static void run(VpJobKind kind) {
    VpBackgroundJob *job=&background_job;
    const VpCatalog *catalog=&job->catalog;
    VpRommConfig config={0};
    bool succeeded=false;
    if (false) {
'''
        cases = r'''
    }
finish:
    job->succeeded=succeeded;
    sync_result_publish(job);
    job->active=false;
}
static void setup(VpSyncOperation operation) {
    VpBackgroundJob *job=&background_job;
    job->active=true; job->covers_completed=job->covers_missing=0;
    removals=cleared=0;
    job->romm_revision=romm_connection.revision;job->integration_revision=integration_revision;
    job->catalog.count=2;
    for (size_t i=0;i<2;i++) {
        VpItem *item=&job->catalog.items[i];
        memset(item,0,sizeof(*item));
        item->source=operation==VP_SYNC_STEAMGRID?VP_SOURCE_LOCAL:VP_SOURCE_ROMM;
        strcpy(item->id,"game");strcpy(item->platform,"psvita");
        strcpy(item->cover_url,"cover");strcpy(item->remote_id,"123");
    }
    job->sync=(VpUiSyncStatus){.state=VP_SYNC_RUNNING,.operation=operation,.total=2};
    transform_ok=background_ok=save_ok=true;cached=false;rename_result=0;requests=0;
    lookup=VP_ARTWORK_FOUND;connection=VP_CONNECTION_CONNECTED;
}
int main(void) {
    VpUiSyncStatus status;
    assert(!pending_user_job.active);
    assert(strstr(sync_job_title(VP_JOB_SYNC_STEAMGRID),"SteamGridDB"));
    background_job.active=true;background_job.kind=VP_JOB_SYNC_STEAMGRID;
    assert(!strcmp(background_job_active_label(),"sincronización de SteamGridDB"));
    setup(VP_SYNC_STEAMGRID); lookup=VP_ARTWORK_NOT_FOUND;
    run(VP_JOB_SYNC_STEAMGRID);
    status=status_snapshot( VP_INTEGRATION_STEAMGRID);
    assert(status.state==VP_SYNC_SUCCEEDED && status.processed==2 && status.missing==2 && status.ready==0);
    assert(strstr(status.detail,"SteamGridDB: carátulas — completado"));
    assert(cleared==2 && removals==4);
    assert(!background_job.catalog.items[0].cover_url[0] && !background_job.catalog.items[0].remote_id[0]);
    assert(!background_job.catalog.items[1].cover_url[0] && !background_job.catalog.items[1].remote_id[0]);
    assert(status_snapshot( VP_INTEGRATION_ROMM).state==VP_SYNC_IDLE);
    /* An unrelated subsequent job cannot erase the retained outcome. */
    background_job.sync=(VpUiSyncStatus){0};background_job.active=true;
    sync_result_publish(&background_job);
    assert(status_snapshot( VP_INTEGRATION_STEAMGRID).missing==2);

    setup(VP_SYNC_STEAMGRID); cached=true;
    run(VP_JOB_SYNC_STEAMGRID);
    assert(requests==0 && background_job.sync.processed==2 && background_job.sync.ready==2);
    setup(VP_SYNC_STEAMGRID); transform_ok=false;
    run(VP_JOB_SYNC_STEAMGRID);
    status=status_snapshot( VP_INTEGRATION_STEAMGRID);
    assert(status.state==VP_SYNC_FAILED && status.processed==1 && status.failed==1);
    setup(VP_SYNC_STEAMGRID); save_ok=false;
    run(VP_JOB_SYNC_STEAMGRID);
    assert(status_snapshot( VP_INTEGRATION_STEAMGRID).state==VP_SYNC_FAILED && background_job.covers_completed==0);
    setup(VP_SYNC_STEAMGRID); connection=VP_CONNECTION_INVALID_KEY;
    run(VP_JOB_SYNC_STEAMGRID);
    status=status_snapshot( VP_INTEGRATION_STEAMGRID);
    assert(!requests && status.state==VP_SYNC_FAILED && strstr(status.detail,"API key"));
    assert(strstr(status.detail,"SteamGridDB: carátulas — fallido"));

    setup(VP_SYNC_METADATA);strcpy(background_job.catalog.items[0].background_url,"background");background_ok=false;
    run(VP_JOB_SYNC_COVERS);
    status=status_snapshot( VP_INTEGRATION_ROMM);
    assert(status.state==VP_SYNC_FAILED && status.processed==2 && status.ready==1 && status.failed==1);
    assert(strstr(status.detail,"RomM: metadata — fallido"));
    setup(VP_SYNC_METADATA);rename_result=-1;
    run(VP_JOB_SYNC_COVERS);
    assert(background_job.sync.ready==0 && background_job.sync.failed==2);
    setup(VP_SYNC_METADATA);background_job.catalog.count=0;background_job.sync.total=0;
    run(VP_JOB_SYNC_COVERS);
    assert(status_snapshot( VP_INTEGRATION_ROMM).state==VP_SYNC_SUCCEEDED);

    setup(VP_SYNC_CATALOG);sync_progress(1,4,&background_job);
    assert(status_snapshot( VP_INTEGRATION_ROMM).processed==1);
    background_job.succeeded=true;sync_result_publish(&background_job);background_job.active=false;
    status=status_snapshot( VP_INTEGRATION_ROMM);
    assert(status.state==VP_SYNC_SUCCEEDED && status.total==4);
    assert(strstr(status.detail,"RomM: catálogo — completado"));
    romm_connection.revision++;
    assert(status_snapshot( VP_INTEGRATION_ROMM).state==VP_SYNC_IDLE);
    sync_result_publish(&background_job);
    assert(status_snapshot( VP_INTEGRATION_ROMM).state==VP_SYNC_IDLE);
    setup(VP_SYNC_METADATA);integration_revision++;
    assert(status_snapshot( VP_INTEGRATION_ROMM).state==VP_SYNC_IDLE);
    run(VP_JOB_SYNC_COVERS);
    assert(status_snapshot( VP_INTEGRATION_ROMM).state==VP_SYNC_IDLE);
    puts("Sync counts, partial failures, retained results and revision guards: PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix="vitashelf-sync-") as temporary:
            binary = Path(temporary) / "sync"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-Iinclude", "-Isrc/vita", "-x", "c", "-", "-pthread", "-o", str(binary)],
                           cwd=ROOT, input=prefix + state + backends + covers + steamgrid + cases,
                           text=True, check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
