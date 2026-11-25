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

static void set_icon_path(VpItem *item, const char *path) {
    size_t length=strlen(path);
    if (length<sizeof(item->fs_name)) memcpy(item->fs_name,path,length+1);
    else item->fs_name[0]=0;
}

static bool icon_header_valid(const char *path) {
    unsigned char header[29];
    size_t count;
    FILE *icon=fopen(path,"rb");
    if (!icon) return false;
    count=fread(header,1,sizeof(header),icon);
    fclose(icon);
    return vp_safe_vita_icon_png(header,count);
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
        {
            char app_icon[VP_PATH], meta_icon[VP_PATH];
            if (snprintf(app_icon,sizeof(app_icon),"%s/%s/sce_sys/icon0.png",apps,entry->d_name)>=(int)sizeof(app_icon) ||
                snprintf(meta_icon,sizeof(meta_icon),"%s/%.9s/icon0.png",appmeta,entry->d_name)>=(int)sizeof(meta_icon)) continue;
            /* Installed game icons are often encrypted; appmeta holds the decoded copy. */
            if (icon_header_valid(meta_icon)) set_icon_path(&item,meta_icon);
            else if (icon_header_valid(app_icon)) set_icon_path(&item,app_icon);
            else item.fs_name[0]=0; /* Keep the title, but do not promise an undecodable icon. */
        }
        if (!vp_catalog_add(catalog,&item)) (*omitted)++;
    }
}

bool vp_local_icon_path(const VpItem *item, char *path, size_t capacity) {
    if (!item || !path || !capacity || item->source!=VP_SOURCE_LOCAL ||
        strcmp(item->platform,"psvita") || !vp_vita_title_id_valid(item->launch_target) ||
        !item->fs_name[0]) return false;
    return snprintf(path,capacity,"%s",item->fs_name)<(int)capacity;
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

bool vp_local_artwork_clear(VpItem *item, const char *cache) {
    char path[VP_PATH];
    if (!item || !artwork_path(item,cache,path,sizeof(path))) return false;
    item->remote_id[0]=0;
    item->cover_url[0]=0;
    errno=0;
    (void)remove(path);
    return errno==0 || errno==ENOENT;
}
