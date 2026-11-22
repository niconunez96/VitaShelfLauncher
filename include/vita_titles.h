#ifndef VP_VITA_TITLES_H
#define VP_VITA_TITLES_H
#include "vitashelflauncher.h"

bool vp_vita_title_id_valid(const char *title_id);
bool vp_vita_title_from_sfo(VpItem *item, const void *data, size_t size);
bool vp_vita_title_launch_uri(const VpItem *item, char *uri, size_t capacity);
bool vp_safe_vita_icon_png(const unsigned char *header, size_t size);
/* Replace one source without allowing a remote refresh to evict local titles. */
size_t vp_catalog_replace_source(VpCatalog *catalog, const VpCatalog *incoming, VpSource source);
bool vp_catalog_source_matches(const VpCatalog *catalog, const VpCatalog *incoming, VpSource source);
size_t vp_catalog_replace_providers(VpCatalog *catalog, const VpCatalog *incoming);
#endif
