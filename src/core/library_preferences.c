#include "library_preferences.h"

#include <stdio.h>
#include <string.h>

#define VP_LIBRARY_PREFERENCE_LINE (VP_TEXT * 2 + 192)

static unsigned int vp_library_hash(const char *value) {
    unsigned int hash = 2166136261U;
    size_t index;
    if (value == NULL) return 0;
    for (index = 0; value[index] != '\0'; index++) {
        hash ^= (unsigned char)value[index];
        hash *= 16777619U;
    }
    return hash;
}

static void vp_library_hex(const char *value, char *destination, size_t capacity) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    size_t used = 0;
    if (destination == NULL || capacity == 0) return;
    for (index = 0; value[index] != '\0' && used + 2 < capacity; index++) {
        unsigned char byte = (unsigned char)value[index];
        destination[used++] = digits[byte >> 4];
        destination[used++] = digits[byte & 15];
    }
    destination[used] = '\0';
}

static bool vp_library_bool(const char *value, bool *destination) {
    if (value == NULL || destination == NULL) return false;
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        *destination = true;
        return true;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
        *destination = false;
        return true;
    }
    return false;
}

void vp_library_preferences_reset(VpCatalog *catalog) {
    size_t index;
    if (catalog == NULL) return;
    for (index = 0; index < catalog->count && index < VP_MAX_ITEMS; index++) {
        catalog->items[index].favorite = false;
        catalog->items[index].hidden = false;
    }
}

void vp_library_item_key(const VpItem *item, const char *romm_base_url,
                         char *destination, size_t capacity) {
    int prefix;
    if (item == NULL || destination == NULL || capacity == 0) return;
    if (item->source == VP_SOURCE_ROMM)
        prefix = snprintf(destination, capacity, "r%08x:", vp_library_hash(romm_base_url));
    else
        prefix = snprintf(destination, capacity, "s%d:", (int)item->source);
    if (prefix < 0 || (size_t)prefix >= capacity) {
        destination[0] = '\0';
        return;
    }
    vp_library_hex(item->id, destination + prefix, capacity - (size_t)prefix);
}

bool vp_library_preferences_parse_line(const char *line, VpCatalog *catalog,
                                       VpLibraryFilters *filters, const char *romm_base_url) {
    char text[VP_LIBRARY_PREFERENCE_LINE];
    char key[VP_LIBRARY_PREFERENCE_LINE];
    char *equals;
    size_t length;
    unsigned long bits;
    size_t index;

    if (line == NULL || catalog == NULL) return false;
    (void)snprintf(text, sizeof(text), "%s", line);
    length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r'))
        text[--length] = '\0';
    if (length == 0 || text[0] == '#') return false;
    equals = strchr(text, '=');
    if (equals == NULL || equals == text) return false;
    *equals++ = '\0';
    if (strcmp(text, "filter.hidden") == 0)
        return filters != NULL && vp_library_bool(equals, &filters->show_hidden);
    if (strcmp(text, "filter.downloaded") == 0)
        return filters != NULL && vp_library_bool(equals, &filters->downloaded_only);
    if (strncmp(text, "item.", 5) != 0) return false;
    if (sscanf(equals, "%lu", &bits) != 1) return false;
    (void)snprintf(key, sizeof(key), "%s", text + 5);
    for (index = 0; index < catalog->count && index < VP_MAX_ITEMS; index++) {
        char generated[VP_LIBRARY_PREFERENCE_LINE];
        vp_library_item_key(&catalog->items[index], romm_base_url,
                            generated, sizeof(generated));
        if (strcmp(generated, key) != 0) continue;
        catalog->items[index].favorite = (bits & 1U) != 0;
        catalog->items[index].hidden = (bits & 2U) != 0;
        return true;
    }
    return false;
}

bool vp_library_preferences_serialize(const VpCatalog *catalog, const VpLibraryFilters *filters,
                                      const char *romm_base_url,
                                      VpLibraryPreferenceLineFn write_line, void *context) {
    char line[VP_LIBRARY_PREFERENCE_LINE];
    size_t index;

    if (catalog == NULL || filters == NULL || write_line == NULL) return false;
    if (!write_line("library_version=1", context)) return false;
    (void)snprintf(line, sizeof(line), "filter.hidden=%d", filters->show_hidden ? 1 : 0);
    if (!write_line(line, context)) return false;
    (void)snprintf(line, sizeof(line), "filter.downloaded=%d", filters->downloaded_only ? 1 : 0);
    if (!write_line(line, context)) return false;
    for (index = 0; index < catalog->count && index < VP_MAX_ITEMS; index++) {
        const VpItem *item = &catalog->items[index];
        unsigned int bits = (item->favorite ? 1U : 0U) | (item->hidden ? 2U : 0U);
        char key[VP_TEXT * 2 + 48];
        if (bits == 0) continue;
        vp_library_item_key(item, romm_base_url, key, sizeof(key));
        (void)snprintf(line, sizeof(line), "item.%s=%u", key, bits);
        if (!write_line(line, context)) return false;
    }
    return true;
}
