#include "catalog_cache.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char id[VP_TEXT],title[VP_TEXT],platform[VP_TEXT],platform_display[VP_TEXT],description[VP_TEXT];
    char launch_target[VP_PATH],remote_id[32],fs_name[VP_TEXT],download_url[VP_URL],cover_url[VP_URL];
    VpSource source;
    VpKind kind;
    bool installed,favorite,multi_file;
    unsigned long last_played;
} V1Item;
typedef struct { V1Item items[VP_MAX_ITEMS]; size_t count; } V1Catalog;

typedef struct {
    char id[VP_TEXT],title[VP_TEXT],platform[VP_TEXT],platform_display[VP_TEXT],description[VP_TEXT];
    char launch_target[VP_PATH],remote_id[32],fs_name[VP_TEXT],download_url[VP_URL],cover_url[VP_URL];
    VpSource source;
    VpKind kind;
    bool installed,favorite,multi_file;
    unsigned long last_played;
    char background_url[VP_URL];
} V2Item;
typedef struct { V2Item items[VP_MAX_ITEMS]; size_t count; } V2Catalog;

int main(void) {
    static VpCatalog old,new;
    static V1Catalog legacy;
    static V2Catalog artwork;
    FILE *file;
    size_t i;
    old.count=2;
    (void)snprintf(old.items[0].id,sizeof(old.items[0].id),"romm:12");
    (void)snprintf(old.items[0].title,sizeof(old.items[0].title),"An offline game");
    old.items[0].favorite=true; old.items[0].installed=true;
    (void)snprintf(old.items[1].cover_url,sizeof(old.items[1].cover_url),"https://example.test/cover.png");
    assert(sizeof(V1Item)==offsetof(VpItem,background_url));
    legacy.count=old.count;
    for (i=0;i<VP_MAX_ITEMS;i++) memcpy(&legacy.items[i],&old.items[i],sizeof(V1Item));
    file=tmpfile(); assert(file);
    assert(fwrite(&legacy,sizeof(legacy),1,file)==1);
    rewind(file); assert(vp_catalog_cache_read(file,1,&new)); fclose(file);
    assert(new.count==2 && !memcmp(&old,&new,sizeof(old)));

    old.items[0].hidden=true;
    (void)snprintf(old.items[0].background_url,sizeof(old.items[0].background_url),"https://example.test/hero.png");
    assert(sizeof(V2Item)==offsetof(VpItem,hidden));
    artwork.count=old.count;
    for (i=0;i<VP_MAX_ITEMS;i++) memcpy(&artwork.items[i],&old.items[i],sizeof(V2Item));
    file=tmpfile(); assert(file); assert(fwrite(&artwork,sizeof(artwork),1,file)==1);
    rewind(file); assert(vp_catalog_cache_read(file,2,&new)); fclose(file);
    assert(new.count==2 && !new.items[0].hidden);
    assert(!memcmp(&old,&new,sizeof(V2Item)));

    file=tmpfile(); assert(file); assert(fwrite(&old,sizeof(old),1,file)==1);
    rewind(file); assert(vp_catalog_cache_read(file,3,&new)); assert(!memcmp(&old,&new,sizeof(old)));
    rewind(file); assert(!vp_catalog_cache_read(file,99,&new)); fclose(file);
    file=tmpfile(); assert(file); assert(fwrite(&old,1,100,file)==100);
    rewind(file); assert(!vp_catalog_cache_read(file,1,&new));
    rewind(file); assert(!vp_catalog_cache_read(file,2,&new));
    rewind(file); assert(!vp_catalog_cache_read(file,3,&new)); fclose(file);
    old.count=VP_MAX_ITEMS+1;
    file=tmpfile(); assert(file); assert(fwrite(&old,sizeof(old),1,file)==1);
    rewind(file); assert(!vp_catalog_cache_read(file,3,&new)); fclose(file);
    puts("catalog cache: offline V1/V2 migration, V3 flags, truncated/invalid payloads passed");
    return 0;
}
