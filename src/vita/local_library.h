#ifndef VP_LOCAL_LIBRARY_H
#define VP_LOCAL_LIBRARY_H
#include "vita_titles.h"
/* False means discovery failed; callers must retain their previous local entries. */
bool vp_local_scan(VpCatalog *catalog, const char *apps, const char *appmeta, size_t *omitted);
bool vp_local_artwork_load(VpItem *item, const char *cache);
bool vp_local_artwork_save(const VpItem *item, const char *cache);
#endif
