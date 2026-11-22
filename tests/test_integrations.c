#include "integrations.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { long status; const char *search, *grids; int calls; bool health; } Fixture;
static long probe(const char *url, const char *key, char *out, size_t capacity, void *context) {
    Fixture *f=context;
    const char *reply;
    f->calls++;
    if (f->health) {
        assert(!strcmp(url,"http://images.example:8090/health")); assert(key==NULL);
        reply=f->search;
    } else {
        assert(!strncmp(url,"https://www.steamgriddb.com/api/v2/",strlen("https://www.steamgriddb.com/api/v2/")));
        assert(key && !strcmp(key,"fixture-key"));
        reply=strstr(url,"/grids/")?f->grids:f->search;
        if (strstr(url,"/grids/")) {
            assert(strstr(url,"types=static") && strstr(url,"mimes=image/png,image/jpeg"));
            assert(strstr(url,"/game/123?"));
        }
    }
    (void)snprintf(out,capacity,"%s",reply?reply:"");
    return f->status;
}
int main(void) {
    char url[VP_URL]; VpConnectionState state;
    Fixture f={200,"{\"success\":true,\"data\":[{\"id\":123,\"name\":\"Persona 4 Golden\"}]}",
        "{\"success\":true,\"data\":[{\"id\":5,\"author\":{\"name\":\"Artist\"},\"mime\":\"image/jpeg\",\"url\":\"https:\\/\\/cdn.example\\/cover.jpg\"}]}",0,false};
    VpRommProbeBackend backend={probe,&f};
    VpItem item={0}; item.source=VP_SOURCE_LOCAL; strcpy(item.title,"Persona 4 Golden");
    assert(vp_image_service_endpoint("images.example:8090/","image",url,sizeof(url)));
    assert(!strcmp(url,"http://images.example:8090/image"));
    assert(vp_image_service_endpoint("https://images.example/prefix/image/","health",url,sizeof(url)));
    assert(!strcmp(url,"https://images.example/prefix/health"));
    assert(!vp_image_service_endpoint("https://user:secret@images.example","image",url,sizeof(url)));
    assert(!vp_image_service_endpoint("file:///tmp/image","image",url,sizeof(url)));
    assert(!vp_image_service_endpoint("http://","image",url,sizeof(url)));
    assert(!vp_image_service_endpoint("http://x","image",url,5));
    assert(vp_steamgrid_check("",backend)==VP_CONNECTION_UNCONFIGURED && f.calls==0);
    assert(vp_steamgrid_check("fixture-key",backend)==VP_CONNECTION_CONNECTED);
    f.status=401; assert(vp_steamgrid_check("fixture-key",backend)==VP_CONNECTION_INVALID_KEY);
    f.status=429; assert(vp_steamgrid_check("fixture-key",backend)==VP_CONNECTION_RATE_LIMITED);
    f.status=0; assert(vp_steamgrid_check("fixture-key",backend)==VP_CONNECTION_UNREACHABLE);
    f.status=200;
    assert(vp_steamgrid_cover(&item,"fixture-key",backend,&state)==VP_ARTWORK_FOUND);
    assert(!strcmp(item.remote_id,"123") && !strcmp(item.cover_url,"https://cdn.example/cover.jpg"));
    item.remote_id[0]=0;
    f.search="{\"success\":true,\"data\":[{\"id\":123,\"name\":\"Persona 4 Golden\"},{\"id\":124,\"name\":\"Persona 4 Golden\"}]}";
    assert(vp_steamgrid_cover(&item,"fixture-key",backend,&state)==VP_ARTWORK_NOT_FOUND);
    f.search="{\"success\":true,\"data\":[{\"id\":123,\"name\":\"Persona 4\"}]}";
    assert(vp_steamgrid_cover(&item,"fixture-key",backend,&state)==VP_ARTWORK_NOT_FOUND);
    f.search="{\"success\":true,\"data\":[{\"id\":123,\"name\":\"Persona 4 Golden\\u2122\"}]}";
    assert(vp_steamgrid_cover(&item,"fixture-key",backend,&state)==VP_ARTWORK_FOUND);
    strcpy(item.title,"Pokémon"); item.remote_id[0]=0;
    f.search="{\"success\":true,\"data\":[{\"id\":123,\"name\":\"Pok\\u00e9mon\"}]}";
    assert(vp_steamgrid_cover(&item,"fixture-key",backend,&state)==VP_ARTWORK_FOUND);
    f.search="{\"success\":false,\"data\":[]}";
    assert(vp_steamgrid_check("fixture-key",backend)==VP_CONNECTION_INVALID_RESPONSE);
    f.search="{\"success\":true,\"data\":[{\"name\":\"bad\\uD800\"}]}";
    assert(vp_steamgrid_check("fixture-key",backend)==VP_CONNECTION_INVALID_RESPONSE);
    f.search="<html>proxy error</html>";
    assert(vp_steamgrid_check("fixture-key",backend)==VP_CONNECTION_INVALID_RESPONSE);
    f.health=true; f.search="ok\n";
    assert(vp_image_service_check("http://images.example:8090/image",backend)==VP_CONNECTION_CONNECTED);
    f.search="<html>OK</html>";
    assert(vp_image_service_check("http://images.example:8090",backend)==VP_CONNECTION_INVALID_RESPONSE);
    puts("SteamGridDB matching, Unicode, authentication, image endpoints and health checks: PASS");
}
