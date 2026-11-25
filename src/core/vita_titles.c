#include "vita_titles.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

bool vp_vita_title_id_valid(const char *id) {
    size_t i;
    if (!id || strlen(id)!=9) return false;
    for (i=0;i<9;i++) if (!((id[i]>='A' && id[i]<='Z') || (id[i]>='0' && id[i]<='9'))) return false;
    return true;
}

static bool sfo_string(const unsigned char *data, size_t size, const char *key,
                       char *output, size_t capacity) {
    uint32_t keys, values, count, i;
    if (size<20 || memcmp(data,"\0PSF",4)!=0) return false;
    keys=u32(data+8); values=u32(data+12); count=u32(data+16);
    if (keys>size || values>size || keys<20 || values<keys || count>(size-20)/16) return false;
    for (i=0;i<count;i++) {
        const unsigned char *entry=data+20+i*16;
        size_t k=(size_t)entry[0]+((size_t)entry[1]<<8), v=u32(entry+12), length=u32(entry+4);
        const unsigned char *end;
        if (k>=values-keys || !memchr(data+keys+k,0,values-keys-k)) return false;
        if (strcmp((const char *)data+keys+k,key)) continue;
        if (entry[2]!=4 || entry[3]!=2 || v>size-values || length>size-values-v || !length) return false;
        end=memchr(data+values+v,0,length);
        if (!end || (size_t)(end-(data+values+v))>=capacity) return false;
        memcpy(output,data+values+v,(size_t)(end-(data+values+v))+1);
        return true;
    }
    return false;
}

bool vp_vita_title_from_sfo(VpItem *item, const void *data, size_t size) {
    char id[16], title[VP_TEXT], category[8];
    size_t i;
    if (!item || !data || !sfo_string(data,size,"TITLE_ID",id,sizeof(id)) ||
        !vp_vita_title_id_valid(id) || !strcmp(id,"VSHELF001") || !strncmp(id,"NPXS",4) ||
        !sfo_string(data,size,"TITLE",title,sizeof(title)) || !title[0] ||
        !sfo_string(data,size,"CATEGORY",category,sizeof(category)) || strcmp(category,"gd")) return false;
    for (i=0;title[i];i++) if ((unsigned char)title[i]<32) title[i]=' ';
    memset(item,0,sizeof(*item));
    (void)snprintf(item->id,sizeof(item->id),"local:%s",id);
    (void)snprintf(item->title,sizeof(item->title),"%s",title);
    (void)snprintf(item->launch_target,sizeof(item->launch_target),"%s",id);
    (void)snprintf(item->platform,sizeof(item->platform),"psvita");
    (void)snprintf(item->platform_display,sizeof(item->platform_display),"PS Vita");
    item->source=VP_SOURCE_LOCAL; item->kind=VP_KIND_GAME; item->installed=true;
    return true;
}

bool vp_vita_title_launch_uri(const VpItem *item, char *uri, size_t capacity) {
    int written;
    if (!item || !uri || !capacity || item->source!=VP_SOURCE_LOCAL ||
        strcmp(item->platform,"psvita") || !vp_vita_title_id_valid(item->launch_target)) return false;
    written=snprintf(uri,capacity,"psgm:play?titleid=%s",item->launch_target);
    return written>0 && (size_t)written<capacity;
}

bool vp_safe_vita_icon_png(const unsigned char *header, size_t size) {
    unsigned char expanded[29];
    if (!header || size<sizeof(expanded)) return false;
    memcpy(expanded,header,sizeof(expanded));
    /* Local icon0 files can be indexed, grayscale, interlaced, or use a
     * 16-bit depth; libpng/vita2d expands them to 8-bit RGB/RGBA on load. */
    if (expanded[25]==0 || expanded[25]==3 || expanded[25]==4)
        expanded[25]=(expanded[25]==0) ? 2 : 6;
    if (expanded[25]==2 || expanded[25]==6) {
        expanded[24]=8;
        expanded[28]=0;
    }
    return vp_safe_cover_png(expanded,sizeof(expanded));
}

size_t vp_catalog_replace_source(VpCatalog *catalog, const VpCatalog *incoming, VpSource source) {
    size_t i=0, omitted=0;
    if (!catalog || !incoming || catalog==incoming) return 0;
    while (i<catalog->count) {
        if (catalog->items[i].source==source) {
            memmove(&catalog->items[i],&catalog->items[i+1],(catalog->count-i-1)*sizeof(VpItem));
            catalog->count--;
        } else i++;
    }
    for (i=0;i<incoming->count;i++) {
        if (incoming->items[i].source!=source) continue;
        /* Native titles take priority over remote entries at the fixed capacity. */
        if (catalog->count==VP_MAX_ITEMS && source==VP_SOURCE_LOCAL) {
            size_t last=catalog->count;
            while (last && catalog->items[last-1].source==VP_SOURCE_LOCAL) last--;
            if (last) {
                memmove(&catalog->items[last-1],&catalog->items[last],(catalog->count-last)*sizeof(VpItem));
                catalog->count--; omitted++;
            }
        }
        if (!vp_catalog_add(catalog,&incoming->items[i])) omitted++;
    }
    return omitted;
}

bool vp_catalog_source_matches(const VpCatalog *catalog, const VpCatalog *incoming, VpSource source) {
    size_t count=0, matched=0, i, j;
    if (!catalog || !incoming) return false;
    for (i=0;i<catalog->count;i++) if (catalog->items[i].source==source) count++;
    for (i=0;i<incoming->count;i++) {
        const VpItem *item=&incoming->items[i];
        if (item->source!=source) continue;
        for (j=0;j<catalog->count;j++) {
            const VpItem *existing=&catalog->items[j];
            if (existing->source==source && !strcmp(existing->id,item->id) && !memcmp(existing,item,sizeof(*item))) break;
        }
        if (j==catalog->count) return false;
        matched++;
    }
    return matched==count;
}

size_t vp_catalog_replace_providers(VpCatalog *catalog, const VpCatalog *incoming) {
    size_t i=0, omitted=0;
    if (!catalog || !incoming || catalog==incoming) return 0;
    /* Release both old provider sources before applying the new shared snapshot. */
    while (i<catalog->count) {
        VpSource source=catalog->items[i].source;
        if (source==VP_SOURCE_ROMM || source==VP_SOURCE_HOMEBREW) {
            memmove(&catalog->items[i],&catalog->items[i+1],(catalog->count-i-1)*sizeof(VpItem));
            catalog->count--;
        } else i++;
    }
    for (i=0;i<incoming->count;i++) {
        VpSource source=incoming->items[i].source;
        if ((source==VP_SOURCE_ROMM || source==VP_SOURCE_HOMEBREW) &&
            !vp_catalog_add(catalog,&incoming->items[i])) omitted++;
    }
    return omitted;
}
