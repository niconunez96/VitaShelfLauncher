#include "local_library.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool read_title(const char *path, VpItem *item) {
    unsigned char *data;
    FILE *file=fopen(path,"rb");
    long size;
    bool valid=false;
    if (!file) return false;
    if (fseek(file,0,SEEK_END) || (size=ftell(file))<20 || size>128*1024 || fseek(file,0,SEEK_SET)) {
        fclose(file); return false;
    }
    data=malloc((size_t)size);
    if (data) {
        if (fread(data,1,(size_t)size,file)==(size_t)size) valid=vp_vita_title_from_sfo(item,data,(size_t)size);
        free(data);
    }
    fclose(file);
    return valid;
}

bool vp_local_scan(VpCatalog *catalog, const char *apps, const char *appmeta, size_t *omitted) {
    DIR *dir;
    struct dirent *entry;
    if (!catalog || !apps || !appmeta || !omitted) return false;
    *omitted=0;
    dir=opendir(apps);
    if (!dir) return false;
    vp_catalog_init(catalog);
    for (;;) {
        VpItem item;
        char path[VP_PATH];
        errno=0;
        entry=readdir(dir);
        if (!entry) {
            bool complete=errno==0;
            closedir(dir);
            return complete;
        }
        if (!vp_vita_title_id_valid(entry->d_name)) continue;
        if (snprintf(path,sizeof(path),"%s/%s/sce_sys/param.sfo",apps,entry->d_name)>=(int)sizeof(path)) continue;
        if (!read_title(path,&item)) {
            if (snprintf(path,sizeof(path),"%s/%s/param.sfo",appmeta,entry->d_name)>=(int)sizeof(path) ||
                !read_title(path,&item)) continue;
        }
        if (strcmp(item.launch_target,entry->d_name)) continue;
        if (snprintf(item.fs_name,sizeof(item.fs_name),"%s/%s/sce_sys/icon0.png",apps,entry->d_name)>=(int)sizeof(item.fs_name)) continue;
        {
            FILE *icon=fopen(item.fs_name,"rb");
            if (icon) fclose(icon);
            else if (snprintf(item.fs_name,sizeof(item.fs_name),"%s/%.9s/icon0.png",appmeta,entry->d_name)>=(int)sizeof(item.fs_name)) continue;
        }
        if (!vp_catalog_add(catalog,&item)) (*omitted)++;
    }
}

static bool artwork_path(const VpItem *item, const char *cache, char *path, size_t capacity) {
    return item && cache && item->source==VP_SOURCE_LOCAL && vp_vita_title_id_valid(item->launch_target) &&
        snprintf(path,capacity,"%s/steamgrid-%s.txt",cache,item->launch_target)<(int)capacity;
}

bool vp_local_artwork_load(VpItem *item, const char *cache) {
    char path[VP_PATH], title[VP_TEXT+2], id[32], url[VP_URL+2];
    FILE *file;
    bool valid;
    if (!artwork_path(item,cache,path,sizeof(path)) || !(file=fopen(path,"r"))) return false;
    valid=fgets(title,sizeof(title),file) && fgets(id,sizeof(id),file) && fgets(url,sizeof(url),file);
    fclose(file);
    if (!valid) return false;
    title[strcspn(title,"\r\n")]=0; id[strcspn(id,"\r\n")]=0; url[strcspn(url,"\r\n")]=0;
    if (strcmp(title,item->title) || !id[0] || strspn(id,"0123456789")!=strlen(id) ||
        strncmp(url,"https://",8) || strlen(url)>=sizeof(item->cover_url)) return false;
    (void)snprintf(item->remote_id,sizeof(item->remote_id),"%s",id);
    memcpy(item->cover_url,url,strlen(url)+1);
    return true;
}

bool vp_local_artwork_save(const VpItem *item, const char *cache) {
    char path[VP_PATH], temp[VP_PATH+8];
    FILE *file;
    bool valid;
    if (!artwork_path(item,cache,path,sizeof(path)) || !item->remote_id[0] || !item->cover_url[0]) return false;
    (void)snprintf(temp,sizeof(temp),"%s.tmp",path);
    file=fopen(temp,"w");
    if (!file) return false;
    valid=fprintf(file,"%s\n%s\n%s\n",item->title,item->remote_id,item->cover_url)>0;
    if (fclose(file)) valid=false;
    if (valid) {
        (void)remove(path);
        valid=rename(temp,path)==0;
    }
    if (!valid) (void)remove(temp);
    return valid;
}
