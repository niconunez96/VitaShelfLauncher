"""Execute the real publication branch with changed configuration/title snapshots."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ArtworkWorkerTests(unittest.TestCase):
    def test_stale_results_cannot_update_connections_or_title_artwork(self):
        source = (ROOT / "src/vita/main.c").read_text()
        start = source.index("    if (kind==VP_JOB_CHECK_STEAMGRID", source.index("static void background_job_poll("))
        end = source.index("    if (kind==VP_JOB_SYNC_COVERS)", start)
        branch = source[start:end]
        prefix = r'''
#include "vitashelflauncher.h"
#include "integrations.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum { VP_JOB_CHECK_STEAMGRID,VP_JOB_CHECK_IMAGES,VP_JOB_SYNC_STEAMGRID };
static struct {
    char steamgrid_key[VP_TEXT];
    unsigned long integration_revision;
    VpConnectionState connection_state;
    VpCatalog catalog;
    size_t covers_total,covers_missing;
} background_job;
static unsigned long integration_revision;
static VpConnectionState image_service_state,steamgrid_state;
static bool cover_cache_dirty;
static int notifications;
static bool start_next_download(void) { return false; }
static const char *sync_job_title(int kind) {(void)kind;return "Sync";}
static void show_notification(VpApp *app,const char *format,...) { (void)app;(void)format;notifications++; }
static void publish(VpApp *app,int kind) {
    bool succeeded=true;
    size_t covers_completed=1;
'''
        suffix = r'''
}
int main(void) {
    static VpApp app;
    VpItem item={0};
    strcpy(item.id,"local:TEST00001"); strcpy(item.title,"Original title"); item.source=VP_SOURCE_LOCAL;
    assert(vp_catalog_add(&app.catalog,&item));
    strcpy(item.cover_url,"https://cdn.example/art.png"); strcpy(item.remote_id,"123");
    assert(vp_catalog_add(&background_job.catalog,&item));
    integration_revision=2; background_job.integration_revision=1;
    background_job.connection_state=VP_CONNECTION_INVALID_KEY;
    steamgrid_state=VP_CONNECTION_NOT_CHECKED;
    strcpy(background_job.steamgrid_key,"old-key");
    publish(&app,VP_JOB_SYNC_STEAMGRID);
    assert(steamgrid_state==VP_CONNECTION_NOT_CHECKED && !app.catalog.items[0].cover_url[0]);
    assert(!background_job.steamgrid_key[0] && !cover_cache_dirty && notifications==0);
    background_job.integration_revision=2;
    background_job.connection_state=VP_CONNECTION_CONNECTED;
    strcpy(app.catalog.items[0].title,"A different title");
    publish(&app,VP_JOB_SYNC_STEAMGRID);
    assert(!app.catalog.items[0].cover_url[0] && notifications==1 && cover_cache_dirty);
    strcpy(app.catalog.items[0].title,"Original title");
    publish(&app,VP_JOB_SYNC_STEAMGRID);
    assert(!strcmp(app.catalog.items[0].cover_url,"https://cdn.example/art.png"));
    assert(!strcmp(app.catalog.items[0].remote_id,"123"));
    publish(&app,VP_JOB_CHECK_IMAGES);
    assert(image_service_state==VP_CONNECTION_CONNECTED);
    puts("Artwork publication rejects stale configuration and changed title identities: PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix="vitashelf-artwork-worker-") as temporary:
            probe = Path(temporary) / "publication"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Iinclude",
                            "-x", "c", "-", "src/core/vitashelflauncher.c", "-o", str(probe)],
                           cwd=ROOT, input=prefix + branch + suffix, text=True, check=True)
            subprocess.run([str(probe)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
