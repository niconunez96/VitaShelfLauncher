#include "vitashelflauncher.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Vita frontend is single-threaded; keeping the remote payload off its small stack avoids
 * a startup crash when providers are configured. One RomM ROM object can exceed 30 KiB, so the
 * buffer is sized for a single metadata-heavy page instead of the bare minimum. */
static char vp_fetch_response[VP_FETCH_RESPONSE_SIZE];
/* The Vita frontend is single-threaded; keeping these scratch buffers off its small stack
 * avoids a startup crash while grouping a large catalog. */
static size_t vp_visible_scratch[VP_MAX_ITEMS];
static VpPlatformGroup vp_platform_scratch[VP_MAX_LIBRARY_GROUPS];

static int vp_compare_ci(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        int left = tolower((unsigned char)*a);
        int right = tolower((unsigned char)*b);
        if (left != right) return left - right;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static void vp_copy(char *destination, size_t capacity, const char *source) {
    if (capacity == 0) return;
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf(destination, capacity, "%s", source);
}

static bool vp_contains_ci(const char *text, const char *needle) {
    size_t i;
    size_t needle_len;
    if (needle == NULL || needle[0] == '\0') return true;
    if (text == NULL) return false;
    needle_len = strlen(needle);
    for (i = 0; text[i] != '\0'; i++) {
        size_t j = 0;
        while (j < needle_len && text[i + j] != '\0' &&
               tolower((unsigned char)text[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == needle_len) return true;
    }
    return false;
}

static const char *vp_skip_space(const char *value) {
    while (value != NULL && *value != '\0' && isspace((unsigned char)*value)) value++;
    return value;
}

static bool vp_json_value(const char *object, const char *object_end,
                          const char *key, char *destination, size_t capacity) {
    char pattern[VP_TEXT + 8];
    const char *match;
    const char *value;
    size_t used = 0;

    if (object == NULL || object_end == NULL || key == NULL || capacity == 0) return false;
    (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    match = strstr(object, pattern);
    if (match == NULL || match >= object_end) return false;
    value = strchr(match + strlen(pattern), ':');
    if (value == NULL || value >= object_end) return false;
    value = vp_skip_space(value + 1);
    if (value == NULL || value >= object_end) return false;

    if (*value == '"') {
        value++;
        while (value < object_end && *value != '"' && used + 1 < capacity) {
            if (*value == '\\' && value + 1 < object_end) value++;
            destination[used++] = *value++;
        }
    } else {
        while (value < object_end && *value != ',' && *value != '}' &&
               !isspace((unsigned char)*value) && used + 1 < capacity) {
            destination[used++] = *value++;
        }
        destination[used] = '\0';
        if (strcmp(destination, "null") == 0) {
            destination[0] = '\0';
            return false;
        }
    }
    destination[used] = '\0';
    return used > 0;
}

static bool vp_json_flag(const char *object, const char *object_end, const char *key) {
    char value[8];
    if (!vp_json_value(object, object_end, key, value, sizeof(value))) return false;
    return strcmp(value, "true") == 0;
}

/* RomM exposes its already-hosted game artwork as merged_screenshots. Only
 * explicit URL fields/strings are accepted; missing metadata keeps the fallback. */
static void vp_romm_artwork(const char *object,const char *end,VpItem *item) {
    char value[VP_URL];
    const char *match,*cursor;
    size_t used=0;
    if (vp_json_value(object,end,"background_url",value,sizeof(value)) &&
        (value[0]=='/' || !strncmp(value,"https://",8) || !strncmp(value,"http://",7))) {
        vp_copy(item->background_url,sizeof(item->background_url),value);
        return;
    }
    match=strstr(object,"\"merged_screenshots\"");
    if (!match || match>=end) return;
    cursor=strchr(match+strlen("\"merged_screenshots\""),':');
    if (!cursor || cursor>=end) return;
    cursor=vp_skip_space(cursor+1);
    if (cursor>=end || *cursor!='[') return;
    cursor=vp_skip_space(cursor+1);
    if (cursor>=end || *cursor!='"') return;
    ++cursor;
    while (cursor<end && *cursor!='"' && used+1<sizeof(value)) {
        if (*cursor=='\\' && cursor+1<end && (cursor[1]=='/' || cursor[1]=='\\' || cursor[1]=='"')) ++cursor;
        value[used++]=*cursor++;
    }
    value[used]='\0';
    if (cursor<end && *cursor=='"' && (value[0]=='/' || !strncmp(value,"https://",8) || !strncmp(value,"http://",7)))
        vp_copy(item->background_url,sizeof(item->background_url),value);
}

static const char *vp_object_end(const char *begin) {
    int depth = 0;
    bool quoted = false;
    const char *cursor;
    for (cursor = begin; *cursor != '\0'; cursor++) {
        if (*cursor == '"' && (cursor == begin || cursor[-1] != '\\')) quoted = !quoted;
        if (quoted) continue;
        if (*cursor == '{') depth++;
        if (*cursor == '}') {
            depth--;
            if (depth == 0) return cursor + 1;
        }
    }
    return NULL;
}

static void vp_default_item(VpItem *item) {
    memset(item, 0, sizeof(*item));
    item->source = VP_SOURCE_LOCAL;
    item->kind = VP_KIND_GAME;
}

void vp_catalog_init(VpCatalog *catalog) {
    if (catalog != NULL) memset(catalog, 0, sizeof(*catalog));
}

void vp_download_queue_init(VpDownloadQueue *queue) {
    if (queue != NULL) memset(queue, 0, sizeof(*queue));
}

VpDownloadEntry *vp_download_queue_find(VpDownloadQueue *queue, const char *item_id) {
    size_t index;
    if (queue == NULL || item_id == NULL) return NULL;
    for (index = 0; index < queue->count; index++) {
        if (strcmp(queue->entries[index].item.id, item_id) == 0) return &queue->entries[index];
    }
    return NULL;
}

bool vp_download_queue_enqueue(VpDownloadQueue *queue, const VpItem *item,
                               const char *destination, const char *partial) {
    VpDownloadEntry *entry;
    if (queue == NULL || item == NULL || item->id[0] == '\0' || destination == NULL ||
        partial == NULL || queue->count >= VP_MAX_DOWNLOAD_QUEUE ||
        vp_download_queue_find(queue, item->id) != NULL) return false;
    entry = &queue->entries[queue->count++];
    memset(entry, 0, sizeof(*entry));
    entry->item = *item;
    vp_copy(entry->destination, sizeof(entry->destination), destination);
    vp_copy(entry->partial, sizeof(entry->partial), partial);
    entry->state = VP_DOWNLOAD_PENDING;
    return true;
}

VpDownloadEntry *vp_download_queue_start_next(VpDownloadQueue *queue) {
    size_t index;
    if (queue == NULL) return NULL;
    for (index = 0; index < queue->count; index++) {
        VpDownloadEntry *entry = &queue->entries[index];
        if (entry->state == VP_DOWNLOAD_PENDING) {
            entry->state = VP_DOWNLOAD_DOWNLOADING;
            entry->percent = 0;
            entry->error[0] = '\0';
            return entry;
        }
    }
    return NULL;
}

void vp_download_queue_progress(VpDownloadEntry *entry,
                                unsigned long long downloaded, unsigned long long total) {
    if (entry == NULL || entry->state != VP_DOWNLOAD_DOWNLOADING) return;
    if (total == 0) {
        entry->percent = 0;
        return;
    }
    entry->percent = (unsigned int)((downloaded * 100U) / total);
    if (entry->percent > 100U) entry->percent = 100U;
}

void vp_download_queue_finish(VpDownloadEntry *entry, bool succeeded, const char *error) {
    if (entry == NULL) return;
    if (succeeded) {
        entry->state = VP_DOWNLOAD_COMPLETED;
        entry->percent = 100;
        entry->error[0] = '\0';
        return;
    }
    entry->state = VP_DOWNLOAD_FAILED;
    vp_copy(entry->error, sizeof(entry->error), error);
}

bool vp_download_queue_cancel(VpDownloadQueue *queue, const char *item_id) {
    VpDownloadEntry *entry = vp_download_queue_find(queue, item_id);
    if (entry == NULL || (entry->state != VP_DOWNLOAD_PENDING &&
                          entry->state != VP_DOWNLOAD_DOWNLOADING)) return false;
    entry->state = VP_DOWNLOAD_CANCELED;
    entry->percent = 0;
    entry->error[0] = '\0';
    return true;
}

bool vp_download_queue_pause(VpDownloadQueue *queue, const char *item_id) {
    VpDownloadEntry *entry = vp_download_queue_find(queue, item_id);
    if (entry == NULL || entry->state != VP_DOWNLOAD_DOWNLOADING) return false;
    entry->state = VP_DOWNLOAD_PAUSED;
    entry->error[0] = '\0';
    return true;
}

bool vp_download_queue_retry(VpDownloadQueue *queue, const char *item_id) {
    VpDownloadEntry *entry = vp_download_queue_find(queue, item_id);
    if (entry == NULL || (entry->state != VP_DOWNLOAD_FAILED &&
                          entry->state != VP_DOWNLOAD_CANCELED &&
                          entry->state != VP_DOWNLOAD_PAUSED)) return false;
    entry->state = VP_DOWNLOAD_PENDING;
    entry->percent = 0;
    entry->error[0] = '\0';
    return true;
}

bool vp_download_queue_remove(VpDownloadQueue *queue, const char *item_id) {
    size_t index;
    size_t position;
    if (queue == NULL || item_id == NULL) return false;
    for (index = 0; index < queue->count; index++) {
        if (strcmp(queue->entries[index].item.id, item_id) != 0) continue;
        for (position = index; position + 1 < queue->count; position++)
            queue->entries[position] = queue->entries[position + 1];
        queue->count--;
        memset(&queue->entries[queue->count], 0, sizeof(queue->entries[queue->count]));
        return true;
    }
    return false;
}

VpItem *vp_catalog_add(VpCatalog *catalog, const VpItem *item) {
    VpItem *existing;
    if (catalog == NULL || item == NULL || item->id[0] == '\0') return NULL;
    existing = vp_catalog_find(catalog, item->id);
    if (existing != NULL) {
        *existing = *item;
        return existing;
    }
    if (catalog->count >= VP_MAX_ITEMS) return NULL;
    catalog->items[catalog->count] = *item;
    catalog->count++;
    return &catalog->items[catalog->count - 1];
}

VpItem *vp_catalog_find(VpCatalog *catalog, const char *id) {
    size_t index;
    if (catalog == NULL || id == NULL) return NULL;
    for (index = 0; index < catalog->count; index++) {
        if (strcmp(catalog->items[index].id, id) == 0) return &catalog->items[index];
    }
    return NULL;
}

size_t vp_catalog_search(const VpCatalog *catalog, const char *query,
                         size_t *indexes, size_t capacity) {
    size_t index;
    size_t found = 0;
    if (catalog == NULL || indexes == NULL) return 0;
    for (index = 0; index < catalog->count && found < capacity; index++) {
        const VpItem *item = &catalog->items[index];
        if (vp_contains_ci(item->title, query) || vp_contains_ci(item->platform, query)) {
            indexes[found++] = index;
        }
    }
    return found;
}

static int vp_compare_item(const VpItem *a, const VpItem *b) {
    int result = vp_compare_ci(a->title, b->title);
    if (result != 0) return result;
    return strcmp(a->id, b->id);
}

static size_t vp_app_platform_count(const VpApp *app) {
    if (app == NULL) return 0;
    return vp_catalog_library_platforms(&app->catalog, &app->library_filters,
                                        vp_platform_scratch, VP_MAX_LIBRARY_GROUPS);
}

size_t vp_catalog_platforms(const VpCatalog *catalog, VpPlatformGroup *groups, size_t capacity) {
    size_t index;
    size_t found = 0;
    if (catalog == NULL || groups == NULL || capacity == 0) return 0;
    for (index = 0; index < catalog->count; index++) {
        const VpItem *item = &catalog->items[index];
        size_t cursor;
        size_t insertion;
        if (item->platform[0] == '\0') continue;
        for (cursor = 0; cursor < found; cursor++) {
            if (strcmp(groups[cursor].platform, item->platform) == 0) break;
        }
        if (cursor < found) {
            groups[cursor].count++;
            if (groups[cursor].display[0] == '\0' ||
                strcmp(groups[cursor].display, groups[cursor].platform) == 0) {
                if (item->platform_display[0] != '\0')
                    vp_copy(groups[cursor].display, sizeof(groups[cursor].display),
                            item->platform_display);
            }
            continue;
        }
        if (found >= capacity) continue;
        memset(&groups[found], 0, sizeof(groups[found]));
        vp_copy(groups[found].platform, sizeof(groups[found].platform), item->platform);
        vp_copy(groups[found].display, sizeof(groups[found].display),
                item->platform_display[0] != '\0' ? item->platform_display : item->platform);
        groups[found].count = 1;
        insertion = found;
        while (insertion > 0 &&
               vp_compare_ci(groups[insertion - 1].display, groups[insertion].display) > 0) {
            VpPlatformGroup temp = groups[insertion - 1];
            groups[insertion - 1] = groups[insertion];
            groups[insertion] = temp;
            insertion--;
        }
        found++;
    }
    return found;
}

bool vp_library_item_visible(const VpItem *item, const VpLibraryFilters *filters) {
    if (item == NULL) return false;
    if (filters == NULL) return !item->hidden;
    if (item->hidden && !filters->show_hidden) return false;
    if (filters->downloaded_only && !item->installed) return false;
    return true;
}

size_t vp_catalog_library_platforms(const VpCatalog *catalog, const VpLibraryFilters *filters,
                                    VpPlatformGroup *groups, size_t capacity) {
    size_t index;
    size_t found;
    size_t favorites = 0;
    if (catalog == NULL || groups == NULL || capacity == 0) return 0;
    memset(&groups[0], 0, sizeof(groups[0]));
    vp_copy(groups[0].platform, sizeof(groups[0].platform), VP_FAVORITES_PLATFORM);
    vp_copy(groups[0].display, sizeof(groups[0].display), VP_FAVORITES_DISPLAY);
    for (index = 0; index < catalog->count; index++) {
        if (vp_library_item_visible(&catalog->items[index], filters) &&
            catalog->items[index].favorite) favorites++;
    }
    groups[0].count = favorites;
    found = 1;
    for (index = 0; index < catalog->count; index++) {
        const VpItem *item = &catalog->items[index];
        size_t cursor;
        size_t insertion;
        if (!vp_library_item_visible(item, filters) || item->platform[0] == '\0') continue;
        for (cursor = 1; cursor < found; cursor++) {
            if (strcmp(groups[cursor].platform, item->platform) == 0) break;
        }
        if (cursor < found) {
            groups[cursor].count++;
            if (groups[cursor].display[0] == '\0' ||
                strcmp(groups[cursor].display, groups[cursor].platform) == 0) {
                if (item->platform_display[0] != '\0')
                    vp_copy(groups[cursor].display, sizeof(groups[cursor].display),
                            item->platform_display);
            }
            continue;
        }
        if (found >= capacity) continue;
        memset(&groups[found], 0, sizeof(groups[found]));
        vp_copy(groups[found].platform, sizeof(groups[found].platform), item->platform);
        vp_copy(groups[found].display, sizeof(groups[found].display),
                item->platform_display[0] != '\0' ? item->platform_display : item->platform);
        groups[found].count = 1;
        insertion = found;
        while (insertion > 1 &&
               vp_compare_ci(groups[insertion - 1].display, groups[insertion].display) > 0) {
            VpPlatformGroup temp = groups[insertion - 1];
            groups[insertion - 1] = groups[insertion];
            groups[insertion] = temp;
            insertion--;
        }
        found++;
    }
    return found;
}

size_t vp_catalog_platform_items(const VpCatalog *catalog, const char *platform,
                                 size_t *indexes, size_t capacity) {
    size_t index;
    size_t found = 0;
    if (catalog == NULL || platform == NULL || indexes == NULL || capacity == 0) return 0;
    for (index = 0; index < catalog->count; index++) {
        size_t insertion;
        if (strcmp(catalog->items[index].platform, platform) != 0) continue;
        if (found >= capacity) continue;
        insertion = found;
        while (insertion > 0 &&
               vp_compare_item(&catalog->items[indexes[insertion - 1]],
                               &catalog->items[index]) > 0) {
            indexes[insertion] = indexes[insertion - 1];
            insertion--;
        }
        indexes[insertion] = index;
        found++;
    }
    return found;
}

size_t vp_catalog_library_items(const VpCatalog *catalog, const char *platform,
                                const VpLibraryFilters *filters, size_t *indexes, size_t capacity) {
    size_t index;
    size_t found = 0;
    bool favorites;
    if (catalog == NULL || platform == NULL || indexes == NULL || capacity == 0) return 0;
    favorites = strcmp(platform, VP_FAVORITES_PLATFORM) == 0;
    for (index = 0; index < catalog->count && found < capacity; index++) {
        const VpItem *item = &catalog->items[index];
        size_t insertion;
        if (!vp_library_item_visible(item, filters)) continue;
        if (favorites ? !item->favorite : strcmp(item->platform, platform) != 0) continue;
        insertion = found;
        while (insertion > 0 &&
               vp_compare_item(&catalog->items[indexes[insertion - 1]], item) > 0) {
            indexes[insertion] = indexes[insertion - 1];
            insertion--;
        }
        indexes[insertion] = index;
        found++;
    }
    return found;
}

size_t vp_catalog_recent(const VpCatalog *catalog, size_t *indexes, size_t capacity) {
    size_t index;
    size_t found = 0;
    if (catalog == NULL || indexes == NULL) return 0;
    for (index = 0; index < catalog->count && found < capacity; index++) {
        size_t insertion = found;
        size_t cursor;
        if (catalog->items[index].last_played == 0) continue;
        for (cursor = 0; cursor < found; cursor++) {
            if (catalog->items[index].last_played > catalog->items[indexes[cursor]].last_played) {
                insertion = cursor;
                break;
            }
        }
        if (found < capacity) found++;
        for (cursor = found - 1; cursor > insertion; cursor--) indexes[cursor] = indexes[cursor - 1];
        indexes[insertion] = index;
    }
    return found;
}

void vp_item_toggle_favorite(VpItem *item) {
    if (item != NULL) item->favorite = !item->favorite;
}

void vp_item_toggle_hidden(VpItem *item) {
    if (item != NULL) item->hidden = !item->hidden;
}

void vp_item_mark_played(VpItem *item, unsigned long timestamp) {
    if (item != NULL) item->last_played = timestamp;
}

static void vp_safe_component(const char *source, char *destination, size_t capacity) {
    size_t used = 0;
    size_t index;
    if (destination == NULL || capacity == 0) return;
    if (source != NULL) {
        for (index = 0; source[index] != '\0' && used + 1 < capacity; index++) {
            unsigned char value = (unsigned char)source[index];
            destination[used++] = (isalnum(value) || value == '-' || value == '_')
                                      ? (char)value
                                      : '_';
        }
    }
    destination[used] = '\0';
    if (destination[0] == '\0') vp_copy(destination, capacity, "item");
}

static void vp_append_download_slug(const char *source, bool strip_extension,
                                    char *destination, size_t capacity, size_t *used) {
    size_t index;
    size_t end;
    if (source == NULL || destination == NULL || capacity == 0 || used == NULL) return;
    end = strlen(source);
    if (strip_extension) {
        const char *name = source;
        const char *dot = NULL;
        const char *cursor;
        for (cursor = source; *cursor != '\0'; cursor++) {
            if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
        }
        for (cursor = name; *cursor != '\0'; cursor++) {
            if (*cursor == '.') dot = cursor;
        }
        if (dot != NULL && dot > name) end = (size_t)(dot - source);
    }
    for (index = 0; source[index] != '\0' && index < end && *used + 1 < capacity; index++) {
        unsigned char value = (unsigned char)source[index];
        if (isspace(value)) continue;
        if (value >= 'A' && value <= 'Z') value = (unsigned char)(value - 'A' + 'a');
        if (isalnum(value) || value == '-' || value == '_')
            destination[(*used)++] = (char)value;
    }
}

static void vp_append_download_id(const VpItem *item, char *destination,
                                  size_t capacity, size_t *used) {
    const char *source = NULL;
    const char *cursor;
    size_t index;
    if (item == NULL || destination == NULL || capacity == 0 || used == NULL) return;
    if (item->remote_id[0] != '\0') source = item->remote_id;
    else if (item->id[0] != '\0') {
        source = item->id;
        cursor = strchr(item->id, ':');
        if (cursor != NULL && cursor[1] != '\0') source = cursor + 1;
    }
    if (source != NULL) {
        for (index = 0; source[index] != '\0' && *used + 1 < capacity; index++) {
            unsigned char value = (unsigned char)source[index];
            if (value >= 'A' && value <= 'Z') value = (unsigned char)(value - 'A' + 'a');
            if (isalnum(value) || value == '-' || value == '_')
                destination[(*used)++] = (char)value;
        }
    }
    if (*used == 0) vp_append_download_slug("item", false, destination, capacity, used);
}

static void vp_item_download_basename(const VpItem *item, char *destination, size_t capacity) {
    size_t used = 0;
    size_t id_start;
    size_t id_length;
    char id_buffer[VP_TEXT];
    size_t id_used = 0;
    if (destination == NULL || capacity == 0) return;
    destination[0] = '\0';
    id_buffer[0] = '\0';
    vp_append_download_id(item, id_buffer, sizeof(id_buffer), &id_used);
    id_buffer[id_used] = '\0';
    id_length = id_used;
    if (item != NULL && item->title[0] != '\0')
        vp_append_download_slug(item->title, false, destination, capacity, &used);
    if (used == 0 && item != NULL && item->fs_name[0] != '\0')
        vp_append_download_slug(item->fs_name, true, destination, capacity, &used);
    if (used == 0 && item != NULL && item->remote_id[0] != '\0')
        vp_append_download_slug(item->remote_id, false, destination, capacity, &used);
    if (used == 0) {
        vp_copy(destination, capacity, id_buffer[0] != '\0' ? id_buffer : "item");
        return;
    }
    if (id_length == 0) {
        destination[used] = '\0';
        return;
    }
    if (used + 1 + id_length + 1 > capacity) {
        size_t keep = capacity > (1 + id_length + 1) ? capacity - (1 + id_length + 1) : 0;
        if (keep > used) keep = used;
        used = keep;
    }
    id_start = used;
    destination[used++] = '_';
    {
        size_t index;
        for (index = 0; id_buffer[index] != '\0' && used + 1 < capacity; index++)
            destination[used++] = id_buffer[index];
    }
    destination[used] = '\0';
    (void)id_start;
}

static void vp_extension_from(const char *source, char *destination, size_t capacity) {
    const char *end;
    const char *cursor;
    const char *name;
    const char *dot = NULL;
    size_t length;
    size_t index;

    if (source == NULL || destination == NULL || capacity == 0) return;
    end = source;
    while (*end != '\0' && *end != '?' && *end != '#') end++;
    name = source;
    for (cursor = source; cursor < end; cursor++) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
    }
    for (cursor = name; cursor < end; cursor++) {
        if (*cursor == '.') dot = cursor;
    }
    if (dot == NULL || dot + 1 >= end) return;
    length = (size_t)(end - (dot + 1));
    if (length + 1 > capacity) return;
    for (index = 0; index < length; index++) {
        if (!isalnum((unsigned char)dot[1 + index])) return;
    }
    for (index = 0; index < length; index++) {
        destination[index] = (char)tolower((unsigned char)dot[1 + index]);
    }
    destination[length] = '\0';
}

bool vp_item_extension(const VpItem *item, char *destination, size_t capacity) {
    if (item == NULL || destination == NULL || capacity == 0) return false;
    destination[0] = '\0';
    if (item->fs_name[0] != '\0') vp_extension_from(item->fs_name, destination, capacity);
    if (destination[0] == '\0' && item->download_url[0] != '\0')
        vp_extension_from(item->download_url, destination, capacity);
    if (destination[0] == '\0') {
        if (capacity < 4) return false;
        vp_copy(destination, capacity, "rom");
    }
    return true;
}

bool vp_item_download_path(const VpItem *item, const char *directory,
                            char *destination, size_t capacity) {
    char basename[VP_TEXT];
    char platform[VP_TEXT];
    char extension[VP_DOWNLOAD_EXTENSION];
    size_t directory_length;
    int written;

    if (item == NULL || item->id[0] == '\0' || directory == NULL || directory[0] == '\0' ||
        destination == NULL || capacity == 0) return false;
    vp_item_download_basename(item, basename, sizeof(basename));
    vp_safe_component(item->platform, platform, sizeof(platform));
    if (!vp_item_extension(item, extension, sizeof(extension))) return false;
    directory_length = strlen(directory);
    while (directory_length > 0 && directory[directory_length - 1] == '/') directory_length--;
    written = snprintf(destination, capacity, "%.*s/%s/%s.%s", (int)directory_length, directory,
                       platform, basename, extension);
    return written > 0 && (size_t)written < capacity;
}

bool vp_platform_is_psp(const char *platform) {
    return vp_compare_ci(platform, "psp") == 0 ||
           vp_compare_ci(platform, "playstation portable") == 0;
}

bool vp_item_psp_image_path(const VpItem *item, const char *psp_root,
                            const char *extension, char *destination, size_t capacity) {
    char basename[VP_TEXT];
    char normalized[VP_DOWNLOAD_EXTENSION];
    size_t index = 0;
    size_t root_length;
    int written;

    if (item == NULL || item->id[0] == '\0' || psp_root == NULL || psp_root[0] == '\0' ||
        destination == NULL || capacity == 0) return false;
    vp_item_download_basename(item, basename, sizeof(basename));
    if (extension != NULL && extension[0] != '\0') {
        if (*extension == '.') extension++;
        while (extension[index] != '\0') {
            unsigned char value = (unsigned char)extension[index];
            if (!isalnum(value)) return false;
            if (index + 1 >= sizeof(normalized)) return false;
            normalized[index] = (char)tolower(value);
            index++;
        }
        normalized[index] = '\0';
    } else if (!vp_item_extension(item, normalized, sizeof(normalized))) {
        return false;
    }
    root_length = strlen(psp_root);
    while (root_length > 0 && psp_root[root_length - 1] == '/') root_length--;
    written = snprintf(destination, capacity, "%.*s/%s.%s", (int)root_length, psp_root,
                       basename, normalized);
    return written > 0 && (size_t)written < capacity;
}

bool vp_item_install_path(const VpItem *item, const char *directory, const char *psp_root,
                          char *destination, size_t capacity) {
    if (vp_platform_is_psp(item != NULL ? item->platform : NULL))
        return vp_item_psp_image_path(item, psp_root, NULL, destination, capacity);
    return vp_item_download_path(item, directory, destination, capacity);
}

typedef struct {
    const char *platform;
    const char *core;
} VpDefaultLaunchProfile;

static const VpDefaultLaunchProfile vp_default_launch_profiles[] = {
    {"snes", "app0:/snes9x2005_libretro.self"},
    {"nes", "app0:/fceumm_libretro.self"},
    {"famicom", "app0:/fceumm_libretro.self"},
    {"gba", "app0:/gpsp_libretro.self"},
    {"gb", "app0:/gambatte_libretro.self"},
    {"gbc", "app0:/gambatte_libretro.self"},
    {"genesis", "app0:/genesis_plus_gx_libretro.self"},
    {"mega-drive", "app0:/genesis_plus_gx_libretro.self"},
    {"megadrive", "app0:/genesis_plus_gx_libretro.self"},
    {"mastersystem", "app0:/genesis_plus_gx_libretro.self"},
    {"sms", "app0:/genesis_plus_gx_libretro.self"},
    {"gamegear", "app0:/genesis_plus_gx_libretro.self"},
    {"psx", "app0:/pcsx_rearmed_libretro.self"},
    {"playstation", "app0:/pcsx_rearmed_libretro.self"},
    {"arcade", "app0:/fbneo_libretro.self"},
    {"neogeo", "app0:/fbneo_libretro.self"},
    {"pce", "app0:/mednafen_pce_fast_libretro.self"},
    {"pc-engine", "app0:/mednafen_pce_fast_libretro.self"},
    {"turbografx-16", "app0:/mednafen_pce_fast_libretro.self"},
    {"atari2600", "app0:/stella2014_libretro.self"},
    {"wonderswan", "app0:/mednafen_wswan_libretro.self"}
};

static bool vp_valid_titleid(const char *titleid) {
    const char *cursor;
    if (titleid == NULL || titleid[0] == '\0') return false;
    for (cursor = titleid; *cursor != '\0'; cursor++) {
        if (!isalnum((unsigned char)*cursor) && *cursor != '-' && *cursor != '_') return false;
    }
    return true;
}

static bool vp_launch_profile_append(VpLaunchConfig *config, const char *platform,
                                     const char *titleid, const char *core) {
    VpLaunchProfile *profile;
    if (config == NULL || platform == NULL || platform[0] == '\0' ||
        config->count >= VP_MAX_LAUNCH_PROFILES) return false;
    profile = &config->profiles[config->count];
    memset(profile, 0, sizeof(*profile));
    vp_copy(profile->platform, sizeof(profile->platform), platform);
    vp_copy(profile->titleid, sizeof(profile->titleid), titleid);
    vp_copy(profile->core, sizeof(profile->core), core);
    config->count++;
    return true;
}

void vp_launch_config_init(VpLaunchConfig *config) {
    size_t index;
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    for (index = 0;
         index < sizeof(vp_default_launch_profiles) / sizeof(vp_default_launch_profiles[0]);
         index++) {
        (void)vp_launch_profile_append(config, vp_default_launch_profiles[index].platform,
                                       "RETROVITA", vp_default_launch_profiles[index].core);
    }
}

const VpLaunchProfile *vp_launch_config_find(const VpLaunchConfig *config,
                                             const char *platform) {
    size_t index;
    if (config == NULL || platform == NULL) return NULL;
    for (index = 0; index < config->count; index++) {
        if (vp_compare_ci(config->profiles[index].platform, platform) == 0)
            return &config->profiles[index];
    }
    return NULL;
}

bool vp_launch_config_set(VpLaunchConfig *config, const char *platform, const char *value) {
    const char *colon;
    size_t titleid_length;
    char titleid[32];
    const char *core;
    VpLaunchProfile *profile;
    size_t index;

    if (config == NULL || platform == NULL || platform[0] == '\0' || value == NULL) return false;
    colon = strchr(value, ':');
    titleid_length = colon != NULL ? (size_t)(colon - value) : strlen(value);
    if (titleid_length == 0 || titleid_length >= sizeof(titleid)) return false;
    memcpy(titleid, value, titleid_length);
    titleid[titleid_length] = '\0';
    if (!vp_valid_titleid(titleid)) return false;
    core = colon != NULL ? colon + 1 : "";
    for (index = 0; index < config->count; index++) {
        profile = &config->profiles[index];
        if (vp_compare_ci(profile->platform, platform) != 0) continue;
        vp_copy(profile->titleid, sizeof(profile->titleid), titleid);
        vp_copy(profile->core, sizeof(profile->core), core);
        return true;
    }
    return vp_launch_profile_append(config, platform, titleid, core);
}

bool vp_build_launch_uri(const VpLaunchProfile *profile, const char *rom_path,
                         char *destination, size_t capacity) {
    int written;
    if (profile == NULL || rom_path == NULL || rom_path[0] == '\0' ||
        destination == NULL || capacity == 0 || !vp_valid_titleid(profile->titleid))
        return false;
    if (profile->core[0] != '\0') {
        written = snprintf(destination, capacity, "psgm:play?titleid=%s&param=%s&param2=%s",
                           profile->titleid, profile->core, rom_path);
    } else {
        written = snprintf(destination, capacity, "psgm:play?titleid=%s&param=%s",
                           profile->titleid, rom_path);
    }
    return written > 0 && (size_t)written < capacity;
}

const char *vp_source_name(VpSource source) {
    switch (source) {
        case VP_SOURCE_LOCAL: return "local";
        case VP_SOURCE_ROMM: return "romm";
        case VP_SOURCE_HOMEBREW: return "homebrew";
        case VP_SOURCE_OFFICIAL_SHORTCUT: return "official-shortcut";
        default: return "unknown";
    }
}

void vp_app_init(VpApp *app) {
    if (app == NULL) return;
    memset(app, 0, sizeof(*app));
    vp_catalog_init(&app->catalog);
    vp_copy(app->theme.id, sizeof(app->theme.id), "default");
    vp_copy(app->theme.name, sizeof(app->theme.name), "VitaShelf Launcher Default");
    vp_copy(app->theme.background, sizeof(app->theme.background), "#11141c");
    vp_copy(app->theme.surface, sizeof(app->theme.surface), "#202938");
    vp_copy(app->theme.accent, sizeof(app->theme.accent), "#5bc0be");
    vp_copy(app->theme.text, sizeof(app->theme.text), "#eef2f6");
    vp_copy(app->theme.muted, sizeof(app->theme.muted), "#9ca9b8");
    app->theme.view = VP_VIEW_GRID;
    app->theme.card_width = 154;
    app->theme.card_height = 216;
    app->active_view = VP_VIEW_GRID;
    app->active_screen = VP_SCREEN_LIBRARY;
    app->library_level = VP_LEVEL_PLATFORMS;
    app->active_platform[0] = '\0';
    app->selected_platform = 0;
    app->selected_item = 0;
}

const char *vp_screen_name(VpScreen screen) {
    switch (screen) {
        case VP_SCREEN_LIBRARY: return "Biblioteca";
        case VP_SCREEN_STORES: return "Tiendas";
        case VP_SCREEN_SETTINGS: return "Ajustes";
        default: return "Biblioteca";
    }
}

void vp_app_cycle_screen(VpApp *app, int direction) {
    int value;
    if (app == NULL || direction == 0) return;
    value = (int)app->active_screen + (direction > 0 ? 1 : -1);
    if (value < (int)VP_SCREEN_LIBRARY) value = (int)VP_SCREEN_SETTINGS;
    if (value > (int)VP_SCREEN_SETTINGS) value = (int)VP_SCREEN_LIBRARY;
    app->active_screen = (VpScreen)value;
}

void vp_app_cycle_view(VpApp *app) {
    if (app == NULL) return;
    app->active_view = (VpView)(((int)app->active_view + 1) % 3);
}

static size_t vp_wrap_add(size_t position, long delta, size_t count) {
    long value;
    if (count == 0) return 0;
    value = (long)position + delta;
    while (value < 0) value += (long)count;
    return (size_t)value % count;
}

size_t vp_app_visible(VpApp *app, size_t *indexes, size_t capacity) {
    size_t count;
    size_t cursor;
    if (app == NULL || indexes == NULL) return 0;
    if (app->library_level != VP_LEVEL_ROMS) return 0;
    count = vp_catalog_library_items(&app->catalog, app->active_platform,
                                     &app->library_filters, indexes, capacity);
    if (count == 0) {
        app->selected_item = 0;
        return 0;
    }
    for (cursor = 0; cursor < count; cursor++) {
        if (indexes[cursor] == app->selected_item) return count;
    }
    app->selected_item = indexes[0];
    return count;
}

void vp_app_select(VpApp *app, int direction) {
    size_t count;
    size_t position = 0;
    size_t cursor;
    if (app == NULL || direction == 0) return;
    if (app->library_level == VP_LEVEL_PLATFORMS) {
        count = vp_app_platform_count(app);
        if (count == 0) {
            app->selected_platform = 0;
            return;
        }
        if (direction > 0) app->selected_platform = (app->selected_platform + 1) % count;
        else app->selected_platform = (app->selected_platform + count - 1) % count;
        app->installation_armed = false;
        return;
    }
    count = vp_app_visible(app, vp_visible_scratch, VP_MAX_ITEMS);
    if (count == 0) {
        app->selected_item = 0;
        return;
    }
    for (cursor = 0; cursor < count; cursor++) {
        if (vp_visible_scratch[cursor] == app->selected_item) {
            position = cursor;
            break;
        }
    }
    position = vp_wrap_add(position, direction, count);
    app->selected_item = vp_visible_scratch[position];
    app->installation_armed = false;
}

static void vp_move_platforms(VpApp *app, int dx, int dy, int columns) {
    size_t count = vp_app_platform_count(app);
    size_t rows;
    size_t row;
    size_t column;
    size_t target;
    if (count == 0) {
        app->selected_platform = 0;
        return;
    }
    if (app->selected_platform >= count) app->selected_platform = count - 1;
    if (columns < 1) columns = 1;
    rows = (count + (size_t)columns - 1) / (size_t)columns;
    if (rows <= 1) {
        long delta = dy != 0 ? dy : dx;
        if (delta != 0) app->selected_platform = vp_wrap_add(app->selected_platform, delta, count);
        return;
    }
    row = app->selected_platform / (size_t)columns;
    column = app->selected_platform % (size_t)columns;
    if (dy != 0) {
        long new_row = (long)row + dy;
        while (new_row < 0) new_row += (long)rows;
        new_row %= (long)rows;
        target = (size_t)new_row * (size_t)columns + column;
        if (target >= count) target = count - 1;
    } else if (dx != 0) {
        long new_column = (long)column + dx;
        while (new_column < 0) new_column += columns;
        new_column %= columns;
        target = row * (size_t)columns + (size_t)new_column;
        if (target >= count) target = 0;
    } else {
        return;
    }
    app->selected_platform = target;
}

static void vp_move_items(VpApp *app, int dx, int dy, int columns) {
    size_t count = vp_app_visible(app, vp_visible_scratch, VP_MAX_ITEMS);
    size_t position = 0;
    size_t cursor;
    long delta;
    if (count == 0) {
        app->selected_item = 0;
        return;
    }
    for (cursor = 0; cursor < count; cursor++) {
        if (vp_visible_scratch[cursor] == app->selected_item) {
            position = cursor;
            break;
        }
    }
    if (columns < 1) columns = 1;
    delta = dy != 0 ? (long)dy * (long)columns : dx;
    if (delta == 0) return;
    position = vp_wrap_add(position, delta, count);
    app->selected_item = vp_visible_scratch[position];
}

void vp_app_move(VpApp *app, int dx, int dy, int columns) {
    if (app == NULL || (dx == 0 && dy == 0)) return;
    if (columns < 1) columns = 1;
    if (app->library_level == VP_LEVEL_PLATFORMS) vp_move_platforms(app, dx, dy, columns);
    else vp_move_items(app, dx, dy, columns);
    app->installation_armed = false;
}

void vp_app_open(VpApp *app) {
    size_t count;
    if (app == NULL || app->library_level == VP_LEVEL_ROMS) return;
    count = vp_catalog_library_platforms(&app->catalog, &app->library_filters,
                                         vp_platform_scratch, VP_MAX_LIBRARY_GROUPS);
    if (count == 0 || app->selected_platform >= count) return;
    vp_copy(app->active_platform, sizeof(app->active_platform),
            vp_platform_scratch[app->selected_platform].platform);
    app->library_level = VP_LEVEL_ROMS;
    app->selected_item = 0;
    (void)vp_app_visible(app, vp_visible_scratch, VP_MAX_ITEMS);
    app->installation_armed = false;
}

void vp_app_back(VpApp *app) {
    if (app == NULL || app->library_level != VP_LEVEL_ROMS) return;
    app->library_level = VP_LEVEL_PLATFORMS;
    app->active_platform[0] = '\0';
    app->selected_item = 0;
    app->installation_armed = false;
}

bool vp_view_from_string(const char *value, VpView *view) {
    VpView parsed;
    if (value == NULL || view == NULL) return false;
    if (strcmp(value, "grid") == 0) parsed = VP_VIEW_GRID;
    else if (strcmp(value, "list") == 0) parsed = VP_VIEW_LIST;
    else if (strcmp(value, "carousel") == 0) parsed = VP_VIEW_CAROUSEL;
    else return false;
    *view = parsed;
    return true;
}

const char *vp_view_name(VpView view) {
    switch (view) {
        case VP_VIEW_GRID: return "grid";
        case VP_VIEW_LIST: return "list";
        case VP_VIEW_CAROUSEL: return "carousel";
        default: return "grid";
    }
}

bool vp_theme_load(VpTheme *theme, const char *ini_text) {
    char buffer[2048];
    char *line;
    char *cursor;
    if (theme == NULL || ini_text == NULL || strlen(ini_text) >= sizeof(buffer)) return false;
    vp_copy(buffer, sizeof(buffer), ini_text);
    cursor = buffer;
    while (cursor != NULL && *cursor != '\0') {
        char *equals;
        char *next = strchr(cursor, '\n');
        if (next != NULL) *next++ = '\0';
        line = cursor;
        cursor = next;
        if (*line == '#' || *line == '\0') continue;
        equals = strchr(line, '=');
        if (equals == NULL) continue;
        *equals++ = '\0';
        if (strcmp(line, "id") == 0) vp_copy(theme->id, sizeof(theme->id), equals);
        else if (strcmp(line, "name") == 0) vp_copy(theme->name, sizeof(theme->name), equals);
        else if (strcmp(line, "background") == 0) vp_copy(theme->background, sizeof(theme->background), equals);
        else if (strcmp(line, "surface") == 0) vp_copy(theme->surface, sizeof(theme->surface), equals);
        else if (strcmp(line, "accent") == 0) vp_copy(theme->accent, sizeof(theme->accent), equals);
        else if (strcmp(line, "text") == 0) vp_copy(theme->text, sizeof(theme->text), equals);
        else if (strcmp(line, "muted") == 0) vp_copy(theme->muted, sizeof(theme->muted), equals);
        else if (strcmp(line, "view") == 0 && !vp_view_from_string(equals, &theme->view)) return false;
        else if (strcmp(line, "card_width") == 0) theme->card_width = atoi(equals);
        else if (strcmp(line, "card_height") == 0) theme->card_height = atoi(equals);
    }
    return theme->id[0] != '\0' && theme->name[0] != '\0' &&
           theme->card_width > 0 && theme->card_height > 0;
}

static void vp_resolve_romm_download(VpItem *item) {
    int written;
    if (item == NULL || item->source != VP_SOURCE_ROMM ||
        item->fs_name[0] == '\0' || item->remote_id[0] == '\0') return;
    /* RomM exposes no downloadable URL in its ROM schema: the supported endpoint is
     * synthesized from the ROM id and file name. Overwrite any stray value because a
     * recursive "url" match can otherwise point at cover metadata. */
    written = snprintf(item->download_url, sizeof(item->download_url), "/api/roms/%s/content/%s",
                       item->remote_id, item->fs_name);
    if (written < 0 || (size_t)written >= sizeof(item->download_url)) item->download_url[0] = '\0';
}

static bool vp_import_items(VpCatalog *catalog, const char *json, VpSource source,
                            VpKind kind, const char *default_platform) {
    const char *cursor = json;
    size_t imported = 0;
    if (catalog == NULL || json == NULL) return false;
    while ((cursor = strchr(cursor, '{')) != NULL) {
        const char *end = vp_object_end(cursor);
        VpItem item;
        char remote_id[VP_TEXT - 24];
        if (end == NULL) return false;
        vp_default_item(&item);
        item.source = source;
        item.kind = kind;
        if (!vp_json_value(cursor, end, "id", remote_id, sizeof(remote_id))) {
            cursor = end;
            continue;
        }
        (void)snprintf(item.id, sizeof(item.id), "%s:%s", vp_source_name(source), remote_id);
        vp_copy(item.remote_id, sizeof(item.remote_id), remote_id);
        if (!vp_json_value(cursor, end, "name", item.title, sizeof(item.title))) {
            (void)vp_json_value(cursor, end, "title", item.title, sizeof(item.title));
        }
        if (item.title[0] == '\0') {
            cursor = end;
            continue;
        }
        if (!vp_json_value(cursor, end, "platform", item.platform, sizeof(item.platform))) {
            if (!vp_json_value(cursor, end, "platform_slug", item.platform, sizeof(item.platform))) {
                vp_copy(item.platform, sizeof(item.platform), default_platform);
            }
        }
        (void)vp_json_value(cursor, end, "platform_display_name", item.platform_display,
                            sizeof(item.platform_display));
        if (item.platform_display[0] == '\0')
            vp_copy(item.platform_display, sizeof(item.platform_display), item.platform);
        if (!vp_json_value(cursor, end, "summary", item.description, sizeof(item.description))) {
            (void)vp_json_value(cursor, end, "description", item.description, sizeof(item.description));
        }
        if (!vp_json_value(cursor, end, "download_url", item.download_url, sizeof(item.download_url)) &&
            source != VP_SOURCE_ROMM) {
            (void)vp_json_value(cursor, end, "url", item.download_url, sizeof(item.download_url));
        }
        (void)vp_json_value(cursor, end, "fs_name", item.fs_name, sizeof(item.fs_name));
        item.multi_file = vp_json_flag(cursor, end, "has_multiple_files");
        vp_resolve_romm_download(&item);
        if (!vp_json_value(cursor, end, "path_cover_small", item.cover_url, sizeof(item.cover_url)) &&
            !vp_json_value(cursor, end, "path_cover_large", item.cover_url, sizeof(item.cover_url)) &&
            !vp_json_value(cursor, end, "cover_url", item.cover_url, sizeof(item.cover_url))) {
            (void)vp_json_value(cursor, end, "url_cover", item.cover_url, sizeof(item.cover_url));
        }
        if (source==VP_SOURCE_ROMM) vp_romm_artwork(cursor,end,&item);
        if (vp_catalog_add(catalog, &item) != NULL) imported++;
        cursor = end;
    }
    return imported > 0;
}

bool vp_local_import_manifest(VpCatalog *catalog, const char *manifest) {
    char buffer[4096];
    char *cursor;
    size_t imported = 0;
    if (catalog == NULL || manifest == NULL || strlen(manifest) >= sizeof(buffer)) return false;
    vp_copy(buffer, sizeof(buffer), manifest);
    cursor = buffer;
    while (cursor != NULL && *cursor != '\0') {
        char *next = strchr(cursor, '\n');
        char *id;
        char *title;
        char *platform;
        char *target;
        VpItem item;
        if (next != NULL) *next++ = '\0';
        if (*cursor == '\0' || *cursor == '#') {
            cursor = next;
            continue;
        }
        id = cursor;
        title = strchr(id, '|');
        if (title == NULL) {
            cursor = next;
            continue;
        }
        *title++ = '\0';
        platform = strchr(title, '|');
        if (platform == NULL) {
            cursor = next;
            continue;
        }
        *platform++ = '\0';
        target = strchr(platform, '|');
        if (target == NULL) {
            cursor = next;
            continue;
        }
        *target++ = '\0';
        vp_default_item(&item);
        (void)snprintf(item.id, sizeof(item.id), "local:%.180s", id);
        vp_copy(item.title, sizeof(item.title), title);
        vp_copy(item.platform, sizeof(item.platform), platform);
        vp_copy(item.launch_target, sizeof(item.launch_target), target);
        item.installed = true;
        item.source = VP_SOURCE_LOCAL;
        item.kind = VP_KIND_GAME;
        if (item.title[0] != '\0' && vp_catalog_add(catalog, &item) != NULL) imported++;
        cursor = next;
    }
    return imported > 0;
}

bool vp_romm_import(VpCatalog *catalog, const char *json) {
    return vp_import_items(catalog, json, VP_SOURCE_ROMM, VP_KIND_ROM, "romm");
}

static bool vp_join_url(char *destination, size_t capacity,
                        const char *base_url, const char *path) {
    size_t base_length;
    size_t path_offset = 0;
    size_t path_length;
    if (destination == NULL || base_url == NULL || path == NULL || capacity == 0) return false;
    base_length = strlen(base_url);
    while (base_length > 0 && base_url[base_length - 1] == '/') base_length--;
    while (path[path_offset] == '/') path_offset++;
    path_length = strlen(path + path_offset);
    if (base_length + 1 + path_length + 1 > capacity) return false;
    memcpy(destination, base_url, base_length);
    destination[base_length] = '/';
    memcpy(destination + base_length + 1, path + path_offset, path_length + 1);
    return true;
}

bool vp_url_escape(const char *url, char *destination, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *cursor;
    size_t used = 0;
    if (url == NULL || destination == NULL || capacity == 0) return false;
    destination[0] = '\0';
    for (cursor = (const unsigned char *)url; *cursor != '\0'; cursor++) {
        char character = (char)*cursor;
        bool escape = *cursor <= 0x20 || *cursor >= 0x7F ||
                      strchr("\"<>\\^`{|}", character) != NULL;
        if (used + (escape ? 3 : 1) + 1 > capacity) {
            destination[0] = '\0';
            return false;
        }
        if (escape) {
            destination[used++] = '%';
            destination[used++] = hex[*cursor >> 4];
            destination[used++] = hex[*cursor & 0x0F];
        } else {
            destination[used++] = character;
        }
    }
    destination[used] = '\0';
    return true;
}

static const char *vp_libretro_system(const char *platform) {
    static const struct {
        const char *platform;
        const char *system;
    } systems[] = {
        {"nes", "Nintendo - Nintendo Entertainment System"},
        {"snes", "Nintendo - Super Nintendo Entertainment System"},
        {"gb", "Nintendo - Game Boy"},
        {"gbc", "Nintendo - Game Boy Color"},
        {"gba", "Nintendo - Game Boy Advance"},
        {"nds", "Nintendo - Nintendo DS"},
        {"genesis", "Sega - Mega Drive - Genesis"},
        {"mega-drive", "Sega - Mega Drive - Genesis"},
        {"megadrive", "Sega - Mega Drive - Genesis"},
        {"mastersystem", "Sega - Master System - Mark III"},
        {"sms", "Sega - Master System - Mark III"},
        {"gamegear", "Sega - Game Gear"},
        {"psx", "Sony - PlayStation"},
        {"playstation", "Sony - PlayStation"},
        {"psp", "Sony - PlayStation Portable"},
        {"pce", "NEC - PC Engine - TurboGrafx 16"},
        {"pc-engine", "NEC - PC Engine - TurboGrafx 16"},
        {"turbografx-16", "NEC - PC Engine - TurboGrafx 16"}
    };
    size_t index;
    if (platform == NULL) return NULL;
    for (index = 0; index < sizeof(systems) / sizeof(systems[0]); index++) {
        if (vp_compare_ci(platform, systems[index].platform) == 0) return systems[index].system;
    }
    return NULL;
}

bool vp_libretro_thumbnail_url(const VpItem *item, char *destination, size_t capacity) {
    char unescaped[VP_URL];
    char title[VP_TEXT];
    char *extension;
    const char *source_title;
    const char *system;
    size_t index;
    if (item == NULL || destination == NULL || capacity == 0 || item->title[0] == '\0') return false;
    system = vp_libretro_system(item->platform);
    if (system == NULL) return false;
    source_title = item->fs_name[0] != '\0' ? item->fs_name : item->title;
    vp_copy(title, sizeof(title), source_title);
    if (item->fs_name[0] != '\0') {
        extension = strrchr(title, '.');
        if (extension != NULL) *extension = '\0';
    }
    for (index = 0; title[index] != '\0'; index++) {
        if (title[index] == '/' || title[index] == '\\') title[index] = '_';
    }
    if (snprintf(unescaped, sizeof(unescaped),
                 "https://thumbnails.libretro.com/%s/Named_Boxarts/%s.png", system, title) >=
        (int)sizeof(unescaped)) return false;
    return vp_url_escape(unescaped, destination, capacity);
}

bool vp_safe_cover_png(const unsigned char *data, size_t size) {
    unsigned int width;
    unsigned int height;
    static const unsigned char signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data == NULL || size < 29 || memcmp(data, signature, sizeof(signature)) != 0 ||
        data[8] != 0 || data[9] != 0 || data[10] != 0 || data[11] != 13 ||
        memcmp(data + 12, "IHDR", 4) != 0) return false;
    width = ((unsigned int)data[16] << 24) | ((unsigned int)data[17] << 16) |
            ((unsigned int)data[18] << 8) | data[19];
    height = ((unsigned int)data[20] << 24) | ((unsigned int)data[21] << 16) |
             ((unsigned int)data[22] << 8) | data[23];
    return width > 0 && height > 0 && width <= 512 && height <= 512 &&
           data[24] == 8 && (data[25] == 2 || data[25] == 6) &&
           data[26] == 0 && data[27] == 0 && data[28] == 0;
}

static const char *vp_array_end(const char *begin) {
    int depth = 0;
    bool quoted = false;
    const char *cursor;
    for (cursor = begin; *cursor != '\0'; cursor++) {
        if (*cursor == '"' && (cursor == begin || cursor[-1] != '\\')) quoted = !quoted;
        if (quoted) continue;
        if (*cursor == '[') depth++;
        if (*cursor == ']') {
            depth--;
            if (depth == 0) return cursor + 1;
        }
    }
    return NULL;
}

static bool vp_romm_import_page(VpCatalog *catalog, const char *json, size_t *item_count) {
    const char *items_key;
    const char *cursor;
    const char *end;
    size_t imported = 0;
    size_t found = 0;
    if (catalog == NULL || json == NULL || item_count == NULL) return false;
    *item_count = 0;
    items_key = strstr(json, "\"items\"");
    if (items_key == NULL || (cursor = strchr(items_key, '[')) == NULL) return false;
    end = vp_array_end(cursor);
    if (end == NULL) return false;
    while ((cursor = strchr(cursor, '{')) != NULL && cursor < end) {
        const char *object_end = vp_object_end(cursor);
        VpItem item;
        char remote_id[VP_TEXT - 24];
        if (object_end == NULL || object_end > end) return false;
        found++;
        vp_default_item(&item);
        item.source = VP_SOURCE_ROMM;
        item.kind = VP_KIND_ROM;
        if (!vp_json_value(cursor, object_end, "id", remote_id, sizeof(remote_id))) {
            cursor = object_end;
            continue;
        }
        (void)snprintf(item.id, sizeof(item.id), "romm:%s", remote_id);
        vp_copy(item.remote_id, sizeof(item.remote_id), remote_id);
        if (!vp_json_value(cursor, object_end, "name", item.title, sizeof(item.title)))
            (void)vp_json_value(cursor, object_end, "title", item.title, sizeof(item.title));
        if (item.title[0] == '\0') {
            cursor = object_end;
            continue;
        }
        if (!vp_json_value(cursor, object_end, "platform", item.platform, sizeof(item.platform)) &&
            !vp_json_value(cursor, object_end, "platform_slug", item.platform, sizeof(item.platform)))
            vp_copy(item.platform, sizeof(item.platform), "romm");
        (void)vp_json_value(cursor, object_end, "platform_display_name", item.platform_display,
                            sizeof(item.platform_display));
        if (item.platform_display[0] == '\0')
            vp_copy(item.platform_display, sizeof(item.platform_display), item.platform);
        if (!vp_json_value(cursor, object_end, "summary", item.description, sizeof(item.description)))
            (void)vp_json_value(cursor, object_end, "description", item.description, sizeof(item.description));
        (void)vp_json_value(cursor, object_end, "download_url", item.download_url,
                            sizeof(item.download_url));
        (void)vp_json_value(cursor, object_end, "fs_name", item.fs_name, sizeof(item.fs_name));
        item.multi_file = vp_json_flag(cursor, object_end, "has_multiple_files");
        vp_resolve_romm_download(&item);
        if (!vp_json_value(cursor, object_end, "path_cover_small", item.cover_url, sizeof(item.cover_url)) &&
            !vp_json_value(cursor, object_end, "path_cover_large", item.cover_url, sizeof(item.cover_url)) &&
            !vp_json_value(cursor, object_end, "cover_url", item.cover_url, sizeof(item.cover_url)))
            (void)vp_json_value(cursor, object_end, "url_cover", item.cover_url, sizeof(item.cover_url));
        vp_romm_artwork(cursor,object_end,&item);
        if (vp_catalog_add(catalog, &item) != NULL) imported++;
        cursor = object_end;
    }
    *item_count = found;
    return imported > 0 || found == 0;
}

static void vp_romm_normalize_urls(VpCatalog *catalog, const char *base_url) {
    size_t index;
    for (index = 0; index < catalog->count; index++) {
        VpItem *item = &catalog->items[index];
        char absolute[VP_URL];
        if (item->source != VP_SOURCE_ROMM) continue;
        if (item->cover_url[0] == '/' && vp_join_url(absolute, sizeof(absolute), base_url, item->cover_url))
            vp_copy(item->cover_url, sizeof(item->cover_url), absolute);
        if (item->download_url[0] == '/' && vp_join_url(absolute, sizeof(absolute), base_url, item->download_url))
            vp_copy(item->download_url, sizeof(item->download_url), absolute);
        if (item->background_url[0] == '/' && vp_join_url(absolute,sizeof(absolute),base_url,item->background_url))
            vp_copy(item->background_url,sizeof(item->background_url),absolute);
    }
}

bool vp_romm_exchange_pair_code(VpRommConfig *config, const char *code,
                                VpRommPairBackend backend) {
    char url[VP_URL];
    char token[VP_TEXT];
    const char *cursor;
    if (config == NULL || config->base_url[0] == '\0' || code == NULL || code[0] == '\0' ||
        backend.execute == NULL) return false;
    for (cursor = code; *cursor != '\0'; cursor++) {
        if (!isalnum((unsigned char)*cursor) && *cursor != '-') return false;
    }
    if (!vp_join_url(url, sizeof(url), config->base_url, "/api/client-tokens/exchange") ||
        !backend.execute(url, code, vp_fetch_response, sizeof(vp_fetch_response), backend.context) ||
        !vp_json_value(vp_fetch_response, vp_fetch_response + strlen(vp_fetch_response),
                       "raw_token", token, sizeof(token))) return false;
    vp_copy(config->token, sizeof(config->token), token);
    return true;
}

VpRommConnectionState vp_romm_check_connection(const VpRommConfig *config,
                                               VpRommProbeBackend backend) {
    char url[VP_URL];
    long status;
    const char *root, *end, *items, *array_end;
    if (config == NULL || config->base_url[0] == '\0' || config->token[0] == '\0')
        return VP_ROMM_UNCONFIGURED;
    if (backend.execute == NULL ||
        !vp_join_url(url, sizeof(url), config->base_url,
                     "/api/roms?limit=1&offset=0&with_char_index=false"
                     "&with_filter_values=false&with_rom_id_index=false"))
        return VP_ROMM_UNREACHABLE;
    vp_fetch_response[0] = '\0';
    status = backend.execute(url, config->token, vp_fetch_response,
                             sizeof(vp_fetch_response), backend.context);
    if (status == 401) return VP_ROMM_INVALID_TOKEN;
    if (status == 403) return VP_ROMM_FORBIDDEN;
    if (status != 200) return VP_ROMM_UNREACHABLE;
    /* Reject login pages and incomplete/unexpected envelopes. An empty items array
     * is a valid authenticated response; catalog population is unrelated. */
    root = vp_skip_space(vp_fetch_response);
    end = *root == '{' ? vp_object_end(root) : NULL;
    if (end == NULL || *vp_skip_space(end) != '\0') return VP_ROMM_UNREACHABLE;
    items = strstr(root, "\"items\"");
    if (items == NULL || (items = vp_skip_space(items + 7)) == NULL || *items != ':')
        return VP_ROMM_UNREACHABLE;
    items = vp_skip_space(items + 1);
    if (*items != '[' || (array_end = vp_array_end(items)) == NULL || array_end >= end)
        return VP_ROMM_UNREACHABLE;
    return VP_ROMM_CONNECTED;
}

void vp_romm_connection_reset(VpRommConnection *connection, const VpRommConfig *config) {
    if (connection == NULL) return;
    connection->revision++;
    connection->state = config != NULL && config->base_url[0] && config->token[0]
                            ? VP_ROMM_CHECKING : VP_ROMM_UNCONFIGURED;
}

bool vp_romm_connection_apply(VpRommConnection *connection, unsigned long revision,
                              VpRommConnectionState state) {
    if (connection == NULL || connection->revision != revision) return false;
    connection->state = state;
    return true;
}

bool vp_romm_pair_code_enabled(VpRommConnectionState state) {
    return state != VP_ROMM_CONNECTED && state != VP_ROMM_CHECKING;
}

static void vp_romm_sync_import_count(size_t processed, size_t total, void *context) {
    (void)total;
    *(size_t *)context = processed;
}

bool vp_romm_sync(VpCatalog *catalog, const VpRommConfig *config,
                  VpFetchBackend backend) {
    size_t imported = 0;
    VpSyncProgressBackend progress = {vp_romm_sync_import_count, &imported};
    if (!vp_romm_sync_with_progress(catalog, config, backend, progress)) return false;
    return imported > 0;
}

bool vp_romm_sync_with_progress(VpCatalog *catalog, const VpRommConfig *config,
                                VpFetchBackend backend, VpSyncProgressBackend progress) {
    char url[VP_URL];
    size_t offset;
    size_t processed = 0;
    size_t capacity;
    if (catalog == NULL || config == NULL || config->base_url[0] == '\0' ||
        config->token[0] == '\0' || backend.execute == NULL) return false;
    capacity = VP_MAX_ITEMS - catalog->count;
    if (progress.execute != NULL) progress.execute(0, 0, progress.context);
    for (offset = 0; offset < VP_MAX_ITEMS && catalog->count < VP_MAX_ITEMS; offset++) {
        char path[VP_URL];
        char total_text[32];
        size_t page_items;
        unsigned long total = 0;
        /* Vita's snprintf does not support the C99 z length modifier. */
        (void)snprintf(path, sizeof(path),
                       "/api/roms?limit=1&offset=%u&with_char_index=false"
                       "&with_filter_values=false&with_rom_id_index=false",
                       (unsigned)offset);
        if (!vp_join_url(url, sizeof(url), config->base_url, path)) return false;
        if (!backend.execute(url, config->token, vp_fetch_response, sizeof(vp_fetch_response), backend.context)) return false;
        if (!vp_romm_import_page(catalog, vp_fetch_response, &page_items)) return false;
        processed += page_items;
        vp_romm_normalize_urls(catalog, config->base_url);
        if (vp_json_value(vp_fetch_response, vp_object_end(vp_fetch_response), "total", total_text, sizeof(total_text)))
            total = strtoul(total_text, NULL, 10);
        if (progress.execute != NULL) {
            size_t effective_total = total > capacity ? capacity : (size_t)total;
            if (effective_total && effective_total < processed) effective_total = processed;
            progress.execute(processed, effective_total, progress.context);
        }
        if (page_items == 0) break;
        if (total > 0 && offset + 1 >= total) break;
    }
    return true;
}

static const char *vp_romm_token_for_url(const VpItem *item, const VpRommConfig *config,
                                         const char *url) {
    size_t base_length;
    if (item == NULL || config == NULL || url == NULL || item->source != VP_SOURCE_ROMM ||
        config->token[0] == '\0' || config->base_url[0] == '\0') return NULL;
    base_length = strlen(config->base_url);
    while (base_length > 0 && config->base_url[base_length - 1] == '/') base_length--;
    if (strncmp(url, config->base_url, base_length) != 0 ||
        (url[base_length] != '\0' && url[base_length] != '/')) return NULL;
    return config->token;
}

static bool vp_image_proxy_url(const VpRommConfig *config, char *destination, size_t capacity) {
    if (config == NULL || destination == NULL || capacity == 0) return false;
    if (config->image_proxy_url[0] != '\0') {
        vp_copy(destination, capacity, config->image_proxy_url);
        return destination[0] != '\0';
    }
    return vp_join_url(destination, capacity, config->base_url, "/image");
}

bool vp_cache_cover(const VpItem *item, const VpRommConfig *config, const char *destination,
                    VpCoverTransformBackend backend) {
    char service_url[VP_URL];
    if (item == NULL || destination == NULL || item->cover_url[0] == '\0' ||
        config == NULL || backend.execute == NULL ||
        !vp_image_proxy_url(config, service_url, sizeof(service_url))) return false;
    return backend.execute(item->cover_url, vp_romm_token_for_url(item, config, item->cover_url),
                           service_url, destination, backend.context);
}

bool vp_homebrew_import(VpCatalog *catalog, const char *json) {
    return vp_import_items(catalog, json, VP_SOURCE_HOMEBREW, VP_KIND_APP, "PS Vita");
}

bool vp_homebrew_sync(VpCatalog *catalog, const VpHomebrewConfig *config,
                      VpFetchBackend backend) {
    if (catalog == NULL || config == NULL || config->catalog_url[0] == '\0' ||
        backend.execute == NULL) return false;
    if (!backend.execute(config->catalog_url, "", vp_fetch_response, sizeof(vp_fetch_response), backend.context)) return false;
    return vp_homebrew_import(catalog, vp_fetch_response);
}

bool vp_queue_download(const VpItem *item, const VpRommConfig *config, const char *destination,
                       VpTransferBackend backend) {
    if (item == NULL || destination == NULL || item->download_url[0] == '\0' ||
        backend.execute == NULL) return false;
    return backend.execute(item->download_url,
                           vp_romm_token_for_url(item, config, item->download_url),
                           destination, VP_TRANSFER_DOWNLOAD, backend.context);
}

bool vp_install_homebrew(const VpItem *item, const char *destination,
                         VpTransferBackend backend) {
    if (item == NULL || item->source != VP_SOURCE_HOMEBREW || destination == NULL ||
        item->download_url[0] == '\0' || backend.execute == NULL) return false;
    return backend.execute(item->download_url, NULL, destination, VP_TRANSFER_INSTALL_VPK, backend.context);
}
