#include "library_preferences.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char text[64 * 1024];
    size_t used;
} PreferenceSink;

static bool append_line(const char *line, void *context) {
    PreferenceSink *sink = context;
    int written;
    size_t remaining;
    if (sink == NULL || line == NULL) return false;
    remaining = sizeof(sink->text) - sink->used;
    if (remaining == 0) return false;
    written = snprintf(sink->text + sink->used, remaining, "%s\n", line);
    if (written < 0 || (size_t)written >= remaining) return false;
    sink->used += (size_t)written;
    return true;
}

static VpItem make_item(const char *id, VpSource source, bool favorite, bool hidden) {
    VpItem item;
    memset(&item, 0, sizeof(item));
    (void)snprintf(item.id, sizeof(item.id), "%s", id);
    item.source = source;
    item.favorite = favorite;
    item.hidden = hidden;
    return item;
}

static void apply_text(VpCatalog *catalog, VpLibraryFilters *filters,
                       const PreferenceSink *sink, const char *base_url) {
    char text[64 * 1024];
    char *cursor;
    (void)snprintf(text, sizeof(text), "%s", sink->text);
    cursor = text;
    vp_library_preferences_reset(catalog);
    filters->show_hidden = false;
    filters->downloaded_only = false;
    while (*cursor != '\0') {
        char *next = strchr(cursor, '\n');
        if (next != NULL) *next = '\0';
        (void)vp_library_preferences_parse_line(cursor, catalog, filters, base_url);
        if (next == NULL) break;
        cursor = next + 1;
    }
}

int main(void) {
    VpCatalog catalog;
    VpItem items[3];
    VpLibraryFilters filters = {true, false};
    VpLibraryFilters restored;
    PreferenceSink sink;
    char key1[VP_TEXT * 2];
    char key2[VP_TEXT * 2];
    char other[VP_TEXT * 2];
    char line[VP_TEXT * 3];

    vp_catalog_init(&catalog);
    items[0] = make_item("romm:12", VP_SOURCE_ROMM, true, false);
    items[1] = make_item("local:chrono", VP_SOURCE_LOCAL, false, true);
    items[2] = make_item("romm:99", VP_SOURCE_ROMM, false, false);
    assert(vp_catalog_add(&catalog, &items[0]) != NULL);
    assert(vp_catalog_add(&catalog, &items[1]) != NULL);
    assert(vp_catalog_add(&catalog, &items[2]) != NULL);

    vp_library_item_key(&catalog.items[0], "https://romm.example", key1, sizeof(key1));
    vp_library_item_key(&catalog.items[0], "https://other.example", other, sizeof(other));
    assert(strcmp(key1, other) != 0);
    assert(strncmp(key1, "r", 1) == 0);
    vp_library_item_key(&catalog.items[1], "https://romm.example", key2, sizeof(key2));
    assert(strncmp(key2, "s0:", 3) == 0);

    memset(&sink, 0, sizeof(sink));
    assert(vp_library_preferences_serialize(&catalog, &filters, "https://romm.example",
                                            append_line, &sink));
    assert(strstr(sink.text, "filter.hidden=1\n") != NULL);
    assert(strstr(sink.text, "filter.downloaded=0\n") != NULL);
    (void)snprintf(line, sizeof(line), "item.%s=1\n", key1);
    assert(strstr(sink.text, line) != NULL);
    (void)snprintf(line, sizeof(line), "item.%s=2\n", key2);
    assert(strstr(sink.text, line) != NULL);

    apply_text(&catalog, &restored, &sink, "https://romm.example");
    assert(restored.show_hidden && !restored.downloaded_only);
    assert(catalog.items[0].favorite && !catalog.items[0].hidden);
    assert(!catalog.items[1].favorite && catalog.items[1].hidden);
    assert(!catalog.items[2].favorite && !catalog.items[2].hidden);

    catalog.items[0].favorite = false;
    catalog.items[1].hidden = false;
    memset(&sink, 0, sizeof(sink));
    assert(vp_library_preferences_serialize(&catalog, &filters, "https://romm.example",
                                            append_line, &sink));
    apply_text(&catalog, &restored, &sink, "https://romm.example");
    assert(!catalog.items[0].favorite);
    assert(!catalog.items[1].hidden);
    puts("library preferences: scoped keys, round trip and unset flags passed");
    return 0;
}
