#define _POSIX_C_SOURCE 200809L
#include "local_library.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static VpCatalog catalog, incoming;
static void put32(unsigned char *p, uint32_t n) {
    p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8); p[2]=(unsigned char)(n>>16); p[3]=(unsigned char)(n>>24);
}
static size_t sfo(unsigned char *data, const char *id) {
    static const char keys[]="TITLE_ID\0TITLE\0CATEGORY\0";
    size_t lengths[]={strlen(id)+1,strlen("Persona 4 Golden")+1,3}, offsets[]={0,9,15}, i, value=0;
    const char *values[]={id,"Persona 4 Golden","gd"};
    memset(data,0,256); memcpy(data,"\0PSF",4);
    put32(data+8,68); put32(data+12,100); put32(data+16,3);
    memcpy(data+68,keys,sizeof(keys));
    for (i=0;i<3;i++) {
        unsigned char *entry=data+20+16*i;
        entry[0]=(unsigned char)offsets[i]; entry[2]=4; entry[3]=2;
        put32(entry+4,(uint32_t)lengths[i]); put32(entry+8,(uint32_t)lengths[i]); put32(entry+12,(uint32_t)value);
        memcpy(data+100+value,values[i],lengths[i]); value+=lengths[i];
    }
    return 100+value;
}
int main(void) {
    unsigned char data[256];
    VpItem item;
    char uri[64], temporary[]="/tmp/vitashelf-local-XXXXXX";
    size_t size=sfo(data,"PCSE00120"), omitted, i;
    FILE *file;
    char *root=mkdtemp(temporary);
    assert(root);
    {
        unsigned char icon[29]={137,'P','N','G',13,10,26,10,0,0,0,13,'I','H','D','R',0,0,0,128,0,0,0,128,8,3,0,0,0};
        assert(vp_safe_vita_icon_png(icon,sizeof(icon)));
        assert(!vp_safe_cover_png(icon,sizeof(icon))); /* remote covers keep their strict format */
        icon[19]=255; assert(!vp_safe_vita_icon_png(icon,sizeof(icon)));
    }
    assert(vp_vita_title_from_sfo(&item,data,size));
    assert(!strcmp(item.title,"Persona 4 Golden") && !strcmp(item.id,"local:PCSE00120") && item.installed);
    assert(vp_vita_title_launch_uri(&item,uri,sizeof(uri)));
    assert(!strcmp(uri,"psgm:play?titleid=PCSE00120"));
    assert(!vp_vita_title_launch_uri(&item,uri,8));
    assert(!vp_vita_title_id_valid("PCSE/0120"));
    for (i=0;i<size;i++) assert(!vp_vita_title_from_sfo(&item,data,i));
    put32(data+12,0xffffffffU);
    assert(!vp_vita_title_from_sfo(&item,data,size));
    size=sfo(data,"VSHELF001"); assert(!vp_vita_title_from_sfo(&item,data,size));
    size=sfo(data,"NPXS10000"); assert(!vp_vita_title_from_sfo(&item,data,size));
    size=sfo(data,"PCSE00120"); assert(vp_vita_title_from_sfo(&item,data,size));
    assert(!chdir(root));
    assert(!mkdir("apps",0700)); assert(!mkdir("meta",0700));
    assert(!mkdir("apps/PCSE00120",0700)); assert(!mkdir("meta/PCSE00120",0700));
    file=fopen("meta/PCSE00120/param.sfo","wb"); assert(file);
    assert(fwrite(data,1,size,file)==size); assert(!fclose(file));
    assert(vp_local_scan(&catalog,"apps","meta",&omitted));
    assert(catalog.count==1 && omitted==0 && !strcmp(catalog.items[0].fs_name,"meta/PCSE00120/icon0.png"));
    incoming=catalog;
    assert(vp_catalog_source_matches(&catalog,&incoming,VP_SOURCE_LOCAL));
    strcpy(incoming.items[0].title,"Updated title");
    assert(!vp_catalog_source_matches(&catalog,&incoming,VP_SOURCE_LOCAL));
    assert(!vp_local_scan(&catalog,"missing","meta",&omitted));
    assert(catalog.count==1); /* Failed discovery preserves the snapshot. */
    strcpy(item.remote_id,"123"); strcpy(item.cover_url,"https://cdn.example/cover.png");
    assert(vp_local_artwork_save(&item,"meta"));
    item.remote_id[0]=0; item.cover_url[0]=0;
    assert(vp_local_artwork_load(&item,"meta"));
    assert(!strcmp(item.remote_id,"123") && !strcmp(item.cover_url,"https://cdn.example/cover.png"));
    strcpy(item.title,"Another game"); assert(!vp_local_artwork_load(&item,"meta"));
    incoming=catalog; vp_catalog_init(&catalog);
    for (i=0;i<VP_MAX_ITEMS;i++) {
        VpItem remote={0}; remote.source=VP_SOURCE_ROMM;
        (void)snprintf(remote.id,sizeof(remote.id),"romm:%u",(unsigned)i);
        assert(vp_catalog_add(&catalog,&remote));
    }
    assert(vp_catalog_replace_source(&catalog,&incoming,VP_SOURCE_LOCAL)==1);
    assert(catalog.count==VP_MAX_ITEMS && vp_catalog_find(&catalog,"local:PCSE00120"));
    assert(vp_catalog_replace_source(&catalog,&incoming,VP_SOURCE_LOCAL)==0);
    vp_catalog_init(&incoming);
    assert(vp_catalog_replace_source(&catalog,&incoming,VP_SOURCE_LOCAL)==0);
    assert(!vp_catalog_find(&catalog,"local:PCSE00120"));
    vp_catalog_init(&catalog); vp_catalog_init(&incoming);
    assert(vp_catalog_add(&catalog,&item)); /* native title must survive a provider refresh */
    for (i=0;i<VP_MAX_ITEMS-1;i++) {
        VpItem remote={0}; remote.source=VP_SOURCE_HOMEBREW;
        (void)snprintf(remote.id,sizeof(remote.id),"homebrew:%u",(unsigned)i);
        assert(vp_catalog_add(&catalog,&remote));
        remote.source=VP_SOURCE_ROMM;
        (void)snprintf(remote.id,sizeof(remote.id),"romm:%u",(unsigned)i);
        assert(vp_catalog_add(&incoming,&remote));
    }
    assert(vp_catalog_replace_providers(&catalog,&incoming)==0);
    assert(catalog.count==VP_MAX_ITEMS && vp_catalog_find(&catalog,"local:PCSE00120"));
    assert(vp_catalog_find(&catalog,"romm:254") && !vp_catalog_find(&catalog,"homebrew:0"));
    assert(!remove("meta/steamgrid-PCSE00120.txt"));
    assert(!remove("meta/PCSE00120/param.sfo"));
    assert(!rmdir("meta/PCSE00120")); assert(!rmdir("apps/PCSE00120"));
    assert(!rmdir("meta")); assert(!rmdir("apps"));
    assert(!chdir("/tmp")); assert(!rmdir(root));
    puts("Local Vita metadata, discovery, launch, artwork persistence and catalog capacity: PASS");
}
