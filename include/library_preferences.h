#ifndef VP_LIBRARY_PREFERENCES_H
#define VP_LIBRARY_PREFERENCES_H

#include "vitashelflauncher.h"

typedef bool (*VpLibraryPreferenceLineFn)(const char *line, void *context);

void vp_library_preferences_reset(VpCatalog *catalog);
void vp_library_item_key(const VpItem *item, const char *romm_base_url,
                         char *destination, size_t capacity);
bool vp_library_preferences_parse_line(const char *line, VpCatalog *catalog,
                                       VpLibraryFilters *filters, const char *romm_base_url);
bool vp_library_preferences_serialize(const VpCatalog *catalog, const VpLibraryFilters *filters,
                                      const char *romm_base_url,
                                      VpLibraryPreferenceLineFn write_line, void *context);

#endif
