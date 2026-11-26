#include "vita_backend.h"
#include "image_service.h"

#include <curl/curl.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/promoterutil.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <zip.h>

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#define VP_STAGE_DIR "ux0:data/vitashelflauncher/staging/latest"
#define VP_COVER_SOURCE_LIMIT (8 * 1024 * 1024)
#define VP_COVER_SOURCE_PATH "ux0:data/vitashelflauncher/cache/covers/.source-image"
/* Large libcurl and socket buffers keep the Vita's Wi-Fi link from stalling on big ROMs. */
#define VP_TRANSFER_BUFFER_SIZE (512 * 1024)
/* Progress callbacks are throttled so UI redraws do not slow the transfer down. */
#define VP_TRANSFER_PROGRESS_INTERVAL_US (100 * 1000ULL)
#define VP_ADRENALINE_EBOOT "ux0:/app/PSPEMUCFW/eboot.bin"
#define VP_PSP_PLUGIN_SOURCE "app0:psp/vitashelf.prx"
#define VP_PSP_SE_ROOT "ux0:/pspemu/seplugins"
#define VP_PSP_PLUGIN_TARGET "ux0:/pspemu/seplugins/vitashelf.prx"
#define VP_PSP_PLUGIN_PSP_PATH "ms0:/seplugins/vitashelf.prx"
#define VP_PSP_PLUGIN_LINE_CLASSIC "ms0:/seplugins/vitashelf.prx 1"
#define VP_PSP_PLUGIN_LINE_EPI "xmb, ms0:/seplugins/vitashelf.prx, on"
#define VP_PSP_PLUGIN_LEGACY_NAME "vitashelflauncher_pspboot.prx"
#define VP_PSP_PLUGIN_LEGACY_TARGET "ux0:/pspemu/seplugins/vitashelflauncher_pspboot.prx"
#define VP_PSP_VSH_PLUGINS "ux0:/pspemu/seplugins/vsh.txt"
#define VP_PSP_EPI_PLUGINS "ux0:/pspemu/seplugins/EPIplugins.txt"
#define VP_PSP_SE_PLUGINS "ux0:/pspemu/seplugins/plugins.txt"
#define VP_PSP_BOOT_REQUEST "ux0:/pspemu/VitaShelfLauncher/boot.txt"
#define VP_PSP_PLUGIN_LOG "ux0:data/vitashelflauncher/psp-plugin.log"

typedef struct {
    char *buffer;
    size_t capacity;
    size_t used;
    bool truncated;
} ResponseBuffer;

typedef struct {
    SceUID fd;
} DownloadFile;

typedef struct {
    SceUID fd;
    size_t used;
    size_t limit;
    bool exceeded;
} LimitedDownloadFile;

typedef struct {
    VpTransferProgressFn callback;
    void *context;
    uint64_t last_report_us;
    bool reported;
} TransferProgress;

/* Transfers run on the single UI thread, so this avoids an oversized Vita stack frame. */
static char extract_block[8192];
/* RomM cover paths carry a timestamp with a literal space; libcurl rejects such URLs. */
static char transfer_url[VP_URL * 3];
static char transform_url[VP_URL * 3];
static char last_error[VP_TEXT];
static pthread_mutex_t last_error_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Workers write errors while the UI thread formats notifications. Callers own
 * the destination, so two readers can never overwrite each other's text. */
void vp_vita_last_error(char *destination, size_t capacity) {
    if (destination == NULL || capacity == 0) return;
    pthread_mutex_lock(&last_error_mutex);
    (void)snprintf(destination, capacity, "%s",
                   last_error[0] != '\0' ? last_error : "sin detalle");
    pthread_mutex_unlock(&last_error_mutex);
}

static void store_last_error(const char *message) {
    pthread_mutex_lock(&last_error_mutex);
    if (message == NULL || message[0] == '\0') last_error[0] = '\0';
    else (void)snprintf(last_error, sizeof(last_error), "%.190s", message);
    pthread_mutex_unlock(&last_error_mutex);
}

void vp_vita_set_error(const char *message) {
    store_last_error(message);
}

static void record_psp_plugin_event(const char *step, int code) {
    FILE *log;
    (void)sceIoMkdir("ux0:data", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher", 0777);
    log = fopen(VP_PSP_PLUGIN_LOG, "a");
    if (log == NULL) return;
    (void)fprintf(log, "step=%s code=0x%08X source=%s target=%s\n",
                  step != NULL ? step : "?", (unsigned int)code,
                  VP_PSP_PLUGIN_SOURCE, VP_PSP_PLUGIN_TARGET);
    fclose(log);
}

static bool psp_plugin_copy_failed(const char *step, int code) {
    char message[VP_TEXT];
    record_psp_plugin_event(step, code);
    (void)snprintf(message, sizeof(message),
                   "No se pudo instalar el complemento PSP en seplugins: %s (0x%08X)",
                   step != NULL ? step : "?", (unsigned int)code);
    vp_vita_set_error(message);
    return false;
}

static void set_last_error(CURLcode result, long status, const char *response, bool truncated) {
    char message[VP_TEXT];
    const char *detail;
    message[0] = '\0';
    if (truncated) {
        (void)snprintf(message, sizeof(message),
                       "Respuesta demasiado grande (%u KiB); revisa network.log",
                       (unsigned)(VP_FETCH_RESPONSE_SIZE / 1024));
    } else if (result != CURLE_OK) {
        (void)snprintf(message, sizeof(message), "Red: %s", curl_easy_strerror(result));
    } else if (status == 422 && response != NULL &&
               (detail = strstr(response, "\"msg\":\"")) != NULL) {
        detail += strlen("\"msg\":\"");
        (void)snprintf(message, sizeof(message), "HTTP 422: %.96s", detail);
    } else if (status < 200 || status >= 300) {
        (void)snprintf(message, sizeof(message), "HTTP %ld", status);
    }
    vp_vita_set_error(message);
}

static void record_http_failure(const char *url, CURLcode result, long status, const char *response,
                                bool truncated) {
    FILE *log;
    if (result == CURLE_OK && status >= 200 && status < 300) return;
    (void)sceIoMkdir("ux0:data", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher", 0777);
    log = fopen("ux0:data/vitashelflauncher/network.log", "a");
    if (log == NULL) return;
    (void)fprintf(log, "url=%s\ncurl=%d (%s)\nhttp=%ld\ntruncated=%d\nbody=%.1024s\n---\n",
                  url != NULL ? url : "(null)", (int)result, curl_easy_strerror(result), status,
                  truncated ? 1 : 0, response != NULL ? response : "");
    fclose(log);
}

static bool valid_bearer_token(const char *token) {
    return token == NULL || (strchr(token, '\r') == NULL && strchr(token, '\n') == NULL);
}

static bool valid_pair_code(const char *code) {
    const char *cursor;
    if (code == NULL || code[0] == '\0') return false;
    for (cursor = code; *cursor != '\0'; cursor++) {
        if (!isalnum((unsigned char)*cursor) && *cursor != '-') return false;
    }
    return true;
}

static size_t response_write(char *data, size_t size, size_t count, void *context) {
    ResponseBuffer *buffer = context;
    size_t bytes = size * count;
    if (bytes == 0) return 0;
    if (buffer->used + bytes + 1 > buffer->capacity) {
        /* Returning a short count makes libcurl report CURLE_WRITE_ERROR, whose text talks
         * about writing to disk. Flag the real cause so the UI does not blame the network. */
        buffer->truncated = true;
        return 0;
    }
    memcpy(buffer->buffer + buffer->used, data, bytes);
    buffer->used += bytes;
    buffer->buffer[buffer->used] = '\0';
    return bytes;
}

static size_t file_write(char *data, size_t size, size_t count, void *context) {
    DownloadFile *file = context;
    size_t bytes = size * count;
    size_t written = 0;
    while (written < bytes) {
        int result = sceIoWrite(file->fd, data + written, bytes - written);
        if (result <= 0) return written;
        written += (size_t)result;
    }
    return bytes;
}

static size_t limited_file_write(char *data, size_t size, size_t count, void *context) {
    LimitedDownloadFile *file = context;
    size_t bytes = size * count;
    if (bytes > file->limit - file->used) {
        file->exceeded = true;
        return 0;
    }
    if (file_write(data, size, count, &(DownloadFile){file->fd}) != bytes) return 0;
    file->used += bytes;
    return bytes;
}

static size_t file_read(char *data, size_t size, size_t count, void *context) {
    DownloadFile *file = context;
    int result;
    size_t requested = size * count;
    if (requested == 0) return 0;
    result = sceIoRead(file->fd, data, requested);
    return result > 0 ? (size_t)result : 0;
}

/* Best effort: a smaller kernel buffer still downloads, just more slowly. */
static int transfer_socket_options(void *userdata, curl_socket_t fd, curlsocktype purpose) {
    int receive_buffer = VP_TRANSFER_BUFFER_SIZE;
    (void)userdata;
    (void)purpose;
    (void)setsockopt((int)fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                     sizeof(receive_buffer));
    return CURL_SOCKOPT_OK;
}

static int transfer_progress(void *context, curl_off_t total, curl_off_t downloaded,
                             curl_off_t upload_total, curl_off_t uploaded) {
    TransferProgress *progress = context;
    uint64_t now;
    (void)upload_total;
    (void)uploaded;
    if (progress == NULL || progress->callback == NULL) return 0;
    now = (uint64_t)sceKernelGetSystemTimeWide();
    if (!progress->reported ||
        now - progress->last_report_us >= VP_TRANSFER_PROGRESS_INTERVAL_US) {
        progress->reported = true;
        progress->last_report_us = now;
        return progress->callback(downloaded > 0 ? (unsigned long long)downloaded : 0,
                                  total > 0 ? (unsigned long long)total : 0,
                                  progress->context) ? 1 : 0;
    }
    return 0;
}

static bool has_unsafe_path(const char *path) {
    const char *cursor;
    if (path == NULL || path[0] == '/' || strchr(path, '\\') != NULL) return true;
    for (cursor = path; *cursor != '\0'; cursor++) {
        if (cursor[0] == '.' && cursor[1] == '.' &&
            (cursor == path || cursor[-1] == '/') &&
            (cursor[2] == '\0' || cursor[2] == '/')) return true;
    }
    return false;
}

static bool make_parent_dirs(const char *path) {
    char buffer[512];
    char *cursor;
    if (strlen(path) >= sizeof(buffer)) return false;
    (void)snprintf(buffer, sizeof(buffer), "%s", path);
    for (cursor = buffer + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            (void)sceIoMkdir(buffer, 0777);
            *cursor = '/';
        }
    }
    return true;
}

static bool extract_vpk(const char *vpk_path, const char *destination) {
    int error = 0;
    zip_t *archive = zip_open(vpk_path, ZIP_RDONLY, &error);
    zip_int64_t index;
    if (archive == NULL) return false;
    for (index = 0; index < zip_get_num_entries(archive, 0); index++) {
        const char *entry = zip_get_name(archive, (zip_uint64_t)index, 0);
        char output[512];
        zip_file_t *input;
        SceUID output_fd;
        if (entry == NULL || has_unsafe_path(entry) ||
            snprintf(output, sizeof(output), "%s/%s", destination, entry) >= (int)sizeof(output)) {
            zip_close(archive);
            return false;
        }
        if (entry[strlen(entry) - 1] == '/') {
            (void)sceIoMkdir(output, 0777);
            continue;
        }
        if (!make_parent_dirs(output)) {
            zip_close(archive);
            return false;
        }
        input = zip_fopen_index(archive, (zip_uint64_t)index, 0);
        output_fd = sceIoOpen(output, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (input == NULL || output_fd < 0) {
            if (input != NULL) zip_fclose(input);
            zip_close(archive);
            return false;
        }
        for (;;) {
            zip_int64_t read_count = zip_fread(input, extract_block, sizeof(extract_block));
            size_t offset = 0;
            if (read_count < 0) {
                zip_fclose(input);
                sceIoClose(output_fd);
                zip_close(archive);
                return false;
            }
            if (read_count == 0) break;
            while (offset < (size_t)read_count) {
                int write_count = sceIoWrite(output_fd, extract_block + offset, (size_t)read_count - offset);
                if (write_count <= 0) {
                    zip_fclose(input);
                    sceIoClose(output_fd);
                    zip_close(archive);
                    return false;
                }
                offset += (size_t)write_count;
            }
        }
        zip_fclose(input);
        sceIoClose(output_fd);
    }
    zip_close(archive);
    return true;
}

#define VP_PSP_ARCHIVE_MAX_SIZE (2ULL * 1024ULL * 1024ULL * 1024ULL)

static bool psp_archive_extension(const char *entry, char *extension, size_t capacity) {
    const char *name;
    const char *cursor;
    const char *dot = NULL;
    const char *end;
    size_t length;
    size_t index;
    if (entry == NULL || extension == NULL || capacity < 4) return false;
    name = entry;
    for (cursor = entry; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
        if (*cursor < 0x20 || *cursor == ':') return false;
    }
    end = name + strlen(name);
    for (cursor = name; cursor < end; cursor++) if (*cursor == '.') dot = cursor;
    if (dot == NULL || dot + 1 >= end) return false;
    length = (size_t)(end - (dot + 1));
    if (length != 3 || length + 1 > capacity) return false;
    for (index = 0; index < length; index++) {
        unsigned char value = (unsigned char)dot[1 + index];
        if (!isalnum(value)) return false;
        extension[index] = (char)tolower(value);
    }
    extension[length] = '\0';
    return strcmp(extension, "iso") == 0 || strcmp(extension, "cso") == 0;
}

bool vp_vita_extract_psp_archive(const VpItem *item, const char *archive_path,
                                 const char *psp_root, char *resolved,
                                 size_t resolved_capacity) {
    int error = 0;
    zip_t *archive;
    zip_int64_t count;
    zip_int64_t selected = -1;
    zip_file_t *input = NULL;
    SceUID output_fd = -1;
    char selected_extension[8] = "";
    char final_path[VP_PATH];
    char partial_path[VP_PATH + 8];
    zip_stat_t info;

    vp_vita_set_error(NULL);
    if (item == NULL || archive_path == NULL || psp_root == NULL ||
        resolved == NULL || resolved_capacity == 0) {
        vp_vita_set_error("No se pudo preparar la extraccion PSP");
        return false;
    }
    archive = zip_open(archive_path, ZIP_RDONLY, &error);
    if (archive == NULL) {
        vp_vita_set_error("No se pudo abrir el ZIP de la ROM PSP");
        return false;
    }
    count = zip_get_num_entries(archive, 0);
    if (count <= 0 || count > 128) {
        zip_close(archive);
        vp_vita_set_error("El ZIP de la ROM PSP tiene una cantidad de archivos invalida");
        return false;
    }
    for (zip_int64_t index = 0; index < count; index++) {
        const char *entry = zip_get_name(archive, (zip_uint64_t)index, 0);
        char extension[8];
        if (entry == NULL || entry[0] == '\0' || has_unsafe_path(entry) ||
            strncmp(entry, "__MACOSX/", 9) == 0 || entry[0] == '.') continue;
        if (entry[strlen(entry) - 1] == '/') continue;
        if (!psp_archive_extension(entry, extension, sizeof(extension))) continue;
        if (zip_stat_index(archive, (zip_uint64_t)index, 0, &info) == 0 &&
            info.size > VP_PSP_ARCHIVE_MAX_SIZE) {
            zip_close(archive);
            vp_vita_set_error("La imagen PSP del ZIP es demasiado grande");
            return false;
        }
        if (selected >= 0) {
            zip_close(archive);
            vp_vita_set_error("El ZIP contiene mas de una imagen ISO/CSO");
            return false;
        }
        selected = index;
        (void)snprintf(selected_extension, sizeof(selected_extension), "%s", extension);
    }
    if (selected < 0) {
        zip_close(archive);
        vp_vita_set_error("El ZIP no contiene una imagen ISO/CSO");
        return false;
    }
    if (!vp_item_psp_image_path(item, psp_root, selected_extension,
                                final_path, sizeof(final_path)) ||
        snprintf(partial_path, sizeof(partial_path), "%s.part", final_path) >=
            (int)sizeof(partial_path)) {
        zip_close(archive);
        vp_vita_set_error("No se pudo preparar la ruta de extraccion PSP");
        return false;
    }
    if (!make_parent_dirs(partial_path)) {
        zip_close(archive);
        vp_vita_set_error("No se pudo crear el directorio ISO de PSP");
        return false;
    }
    input = zip_fopen_index(archive, (zip_uint64_t)selected, 0);
    output_fd = sceIoOpen(partial_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (input == NULL || output_fd < 0) {
        if (input != NULL) (void)zip_fclose(input);
        if (output_fd >= 0) (void)sceIoClose(output_fd);
        zip_close(archive);
        (void)sceIoRemove(partial_path);
        vp_vita_set_error("No se pudo iniciar la extraccion PSP");
        return false;
    }
    for (;;) {
        zip_int64_t read_count = zip_fread(input, extract_block, sizeof(extract_block));
        size_t offset = 0;
        if (read_count < 0) {
            (void)zip_fclose(input);
            (void)sceIoClose(output_fd);
            zip_close(archive);
            (void)sceIoRemove(partial_path);
            vp_vita_set_error("Falló la lectura del ZIP PSP");
            return false;
        }
        if (read_count == 0) break;
        while (offset < (size_t)read_count) {
            int write_count = sceIoWrite(output_fd, extract_block + offset,
                                         (size_t)read_count - offset);
            if (write_count <= 0) {
                (void)zip_fclose(input);
                (void)sceIoClose(output_fd);
                zip_close(archive);
                (void)sceIoRemove(partial_path);
                vp_vita_set_error("No se pudo escribir la imagen PSP extraida");
                return false;
            }
            offset += (size_t)write_count;
        }
    }
    if (zip_fclose(input) != 0 || sceIoClose(output_fd) < 0) {
        zip_close(archive);
        (void)sceIoRemove(partial_path);
        vp_vita_set_error("No se pudo cerrar la imagen PSP extraida");
        return false;
    }
    zip_close(archive);
    (void)sceIoRemove(final_path);
    if (sceIoRename(partial_path, final_path) < 0) {
        (void)sceIoRemove(partial_path);
        vp_vita_set_error("No se pudo instalar la imagen PSP extraida");
        return false;
    }
    (void)sceIoRemove(archive_path);
    (void)snprintf(resolved, resolved_capacity, "%s", final_path);
    return true;
}

static bool path_exists(const char *path) {
    SceIoStat stat;
    return path != NULL && sceIoGetstat(path, &stat) >= 0;
}

static bool path_file_size(const char *path, uint64_t *size) {
    SceIoStat stat;
    if (path == NULL || size == NULL) return false;
    if (sceIoGetstat(path, &stat) < 0) return false;
    *size = (uint64_t)stat.st_size;
    return true;
}

bool vp_vita_path_exists(const char *path) {
    return path_exists(path);
}

bool vp_vita_app_installed(const char *titleid) {
    char eboot[VP_PATH];
    if (titleid == NULL || titleid[0] == '\0') return false;
    (void)snprintf(eboot, sizeof(eboot), "ux0:/app/%s/eboot.bin", titleid);
    return path_exists(eboot);
}

#define VP_NDS_ARCHIVE_MAX_SIZE (2ULL * 1024ULL * 1024ULL * 1024ULL)

static bool nds_archive_entry(const char *entry) {
    const char *name;
    const char *cursor;
    const char *dot = NULL;
    const char *end;
    size_t length;
    size_t index;

    if (entry == NULL) return false;
    name = entry;
    for (cursor = entry; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
        if (*cursor < 0x20 || *cursor == ':') return false;
    }
    end = name + strlen(name);
    for (cursor = name; cursor < end; cursor++) if (*cursor == '.') dot = cursor;
    if (dot == NULL || dot + 1 >= end) return false;
    length = (size_t)(end - (dot + 1));
    if (length != 3) return false;
    for (index = 0; index < length; index++)
        if (tolower((unsigned char)dot[1 + index]) != "nds"[index]) return false;
    return true;
}

bool vp_vita_extract_nds_archive(const VpItem *item, const char *archive_path,
                                 const char *nds_root, char *resolved,
                                 size_t resolved_capacity) {
    int error = 0;
    zip_t *archive;
    zip_int64_t count;
    zip_int64_t selected = -1;
    zip_file_t *input = NULL;
    SceUID output_fd = -1;
    char final_path[VP_PATH];
    char partial_path[VP_PATH + 8];
    zip_stat_t info;

    vp_vita_set_error(NULL);
    if (item == NULL || archive_path == NULL || nds_root == NULL ||
        resolved == NULL || resolved_capacity == 0) {
        vp_vita_set_error("No se pudo preparar la extraccion NDS");
        return false;
    }
    archive = zip_open(archive_path, ZIP_RDONLY, &error);
    if (archive == NULL) {
        vp_vita_set_error("No se pudo abrir el ZIP de la ROM NDS");
        return false;
    }
    count = zip_get_num_entries(archive, 0);
    if (count <= 0 || count > 128) {
        zip_close(archive);
        vp_vita_set_error("El ZIP de la ROM NDS tiene una cantidad de archivos invalida");
        return false;
    }
    for (zip_int64_t index = 0; index < count; index++) {
        const char *entry = zip_get_name(archive, (zip_uint64_t)index, 0);
        if (entry == NULL || entry[0] == '\0' || has_unsafe_path(entry) ||
            strncmp(entry, "__MACOSX/", 9) == 0 || entry[0] == '.') continue;
        if (entry[strlen(entry) - 1] == '/') continue;
        if (!nds_archive_entry(entry)) continue;
        if (zip_stat_index(archive, (zip_uint64_t)index, 0, &info) == 0 &&
            info.size > VP_NDS_ARCHIVE_MAX_SIZE) {
            zip_close(archive);
            vp_vita_set_error("La ROM NDS del ZIP es demasiado grande");
            return false;
        }
        if (selected >= 0) {
            zip_close(archive);
            vp_vita_set_error("El ZIP contiene mas de una ROM NDS");
            return false;
        }
        selected = index;
    }
    if (selected < 0) {
        zip_close(archive);
        vp_vita_set_error("El ZIP no contiene una ROM NDS");
        return false;
    }
    if (!vp_item_nds_rom_path(item, nds_root, "nds", final_path, sizeof(final_path)) ||
        snprintf(partial_path, sizeof(partial_path), "%s.part", final_path) >=
            (int)sizeof(partial_path)) {
        zip_close(archive);
        vp_vita_set_error("No se pudo preparar la ruta de extraccion NDS");
        return false;
    }
    if (!make_parent_dirs(partial_path)) {
        zip_close(archive);
        vp_vita_set_error("No se pudo crear el directorio de ROMs NDS");
        return false;
    }
    input = zip_fopen_index(archive, (zip_uint64_t)selected, 0);
    output_fd = sceIoOpen(partial_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (input == NULL || output_fd < 0) {
        if (input != NULL) (void)zip_fclose(input);
        if (output_fd >= 0) (void)sceIoClose(output_fd);
        zip_close(archive);
        (void)sceIoRemove(partial_path);
        vp_vita_set_error("No se pudo iniciar la extraccion NDS");
        return false;
    }
    for (;;) {
        zip_int64_t read_count = zip_fread(input, extract_block, sizeof(extract_block));
        size_t offset = 0;
        if (read_count < 0) {
            (void)zip_fclose(input);
            (void)sceIoClose(output_fd);
            zip_close(archive);
            (void)sceIoRemove(partial_path);
            vp_vita_set_error("Falló la lectura del ZIP NDS");
            return false;
        }
        if (read_count == 0) break;
        while (offset < (size_t)read_count) {
            int write_count = sceIoWrite(output_fd, extract_block + offset,
                                         (size_t)read_count - offset);
            if (write_count <= 0) {
                (void)zip_fclose(input);
                (void)sceIoClose(output_fd);
                zip_close(archive);
                (void)sceIoRemove(partial_path);
                vp_vita_set_error("No se pudo escribir la ROM NDS extraida");
                return false;
            }
            offset += (size_t)write_count;
        }
    }
    if (zip_fclose(input) != 0 || sceIoClose(output_fd) < 0) {
        zip_close(archive);
        (void)sceIoRemove(partial_path);
        vp_vita_set_error("No se pudo cerrar la ROM NDS extraida");
        return false;
    }
    zip_close(archive);
    (void)sceIoRemove(final_path);
    if (sceIoRename(partial_path, final_path) < 0) {
        (void)sceIoRemove(partial_path);
        vp_vita_set_error("No se pudo instalar la ROM NDS extraida");
        return false;
    }
    (void)sceIoRemove(archive_path);
    (void)snprintf(resolved, resolved_capacity, "%s", final_path);
    return true;
}

static bool copy_whole_file_with_status(const char *source, const char *destination, int *failure_code) {
    SceUID input = -1;
    SceUID output = -1;
    int read_count;
    int code = 0;
    bool ok = false;

    if (failure_code != NULL) *failure_code = 0;
    input = sceIoOpen(source, SCE_O_RDONLY, 0);
    if (input < 0) {
        code = (int)input;
        goto cleanup;
    }
    if (!make_parent_dirs(destination)) {
        code = -1;
        goto cleanup;
    }
    output = sceIoOpen(destination, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (output < 0) {
        code = (int)output;
        goto cleanup;
    }
    ok = true;
    for (;;) {
        size_t offset = 0;
        read_count = sceIoRead(input, extract_block, sizeof(extract_block));
        if (read_count < 0) {
            code = read_count;
            ok = false;
            break;
        }
        if (read_count == 0) break;
        while (offset < (size_t)read_count) {
            int write_count = sceIoWrite(output, extract_block + offset,
                                         (size_t)read_count - offset);
            if (write_count <= 0) {
                code = write_count;
                ok = false;
                break;
            }
            offset += (size_t)write_count;
        }
        if (!ok) break;
    }
cleanup:
    if (input >= 0 && sceIoClose(input) < 0 && ok) {
        code = -2;
        ok = false;
    }
    if (output >= 0 && sceIoClose(output) < 0 && ok) {
        code = -3;
        ok = false;
    }
    if (!ok) {
        if (failure_code != NULL) *failure_code = code != 0 ? code : -1;
        (void)sceIoRemove(destination);
    }
    return ok;
}

static bool copy_whole_file(const char *source, const char *destination) {
    return copy_whole_file_with_status(source, destination, NULL);
}

static bool same_file_bytes(const char *left, const char *right) {
    SceUID left_fd;
    SceUID right_fd;
    uint64_t left_size;
    uint64_t right_size;
    bool same = true;
    if (!path_file_size(left, &left_size) || !path_file_size(right, &right_size) ||
        left_size != right_size) return false;
    left_fd = sceIoOpen(left, SCE_O_RDONLY, 0);
    right_fd = sceIoOpen(right, SCE_O_RDONLY, 0);
    if (left_fd < 0 || right_fd < 0) {
        if (left_fd >= 0) (void)sceIoClose(left_fd);
        if (right_fd >= 0) (void)sceIoClose(right_fd);
        return false;
    }
    for (;;) {
        int left_count = sceIoRead(left_fd, extract_block, sizeof(extract_block));
        int right_count;
        if (left_count <= 0) break;
        right_count = sceIoRead(right_fd, extract_block, sizeof(extract_block));
        if (left_count != right_count || memcmp(extract_block, extract_block + left_count,
                                                (size_t)left_count) != 0) {
            same = false;
            break;
        }
    }
    if (sceIoRead(right_fd, extract_block, 1) > 0) same = false;
    (void)sceIoClose(left_fd);
    (void)sceIoClose(right_fd);
    return same;
}

static bool read_text_file(const char *path, char *buffer, size_t capacity) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    int count;
    size_t used = 0;
    if (fd < 0) {
        buffer[0] = '\0';
        return false;
    }
    count = sceIoRead(fd, buffer, capacity - 1);
    (void)sceIoClose(fd);
    if (count < 0) {
        buffer[0] = '\0';
        return false;
    }
    used = (size_t)count;
    while (used > 0 && (buffer[used - 1] == '\n' || buffer[used - 1] == '\r')) used--;
    buffer[used] = '\0';
    return true;
}

typedef enum {
    VP_PSP_LIST_CLASSIC = 0,
    VP_PSP_LIST_EPI
} VpPspPluginListFormat;

static bool line_mentions_plugin(const char *line, size_t length) {
    char buffer[512];
    size_t copy = length < sizeof(buffer) - 1 ? length : sizeof(buffer) - 1;
    (void)memcpy(buffer, line, copy);
    buffer[copy] = '\0';
    return strstr(buffer, "vitashelf.prx") != NULL ||
           strstr(buffer, VP_PSP_PLUGIN_LEGACY_NAME) != NULL;
}

static bool line_enables_plugin(const char *line, size_t length) {
    char buffer[512];
    size_t copy = length < sizeof(buffer) - 1 ? length : sizeof(buffer) - 1;
    (void)memcpy(buffer, line, copy);
    buffer[copy] = '\0';
    if (strstr(buffer, "vitashelf.prx") == NULL) return false;
    return strstr(buffer, " 1") != NULL || strstr(buffer, ", on") != NULL ||
           strstr(buffer, ", enabled") != NULL || strstr(buffer, ", true") != NULL;
}

static bool write_plugin_enable_file(const char *path, VpPspPluginListFormat format) {
    enum { CURRENT_SIZE = 8192, NEXT_SIZE = 9216 };
    char *current = (char *)malloc(CURRENT_SIZE);
    char *next = (char *)malloc(NEXT_SIZE);
    char temp[VP_PATH + 8];
    const char *cursor;
    const char *enable_line;
    size_t line_length;
    size_t used = 0;
    bool found = false;
    bool ok = false;
    SceUID fd;

    if (current == NULL || next == NULL) goto cleanup;
    enable_line = format == VP_PSP_LIST_EPI ? VP_PSP_PLUGIN_LINE_EPI
                                            : VP_PSP_PLUGIN_LINE_CLASSIC;
    line_length = strlen(enable_line);
    (void)read_text_file(path, current, CURRENT_SIZE);
    cursor = current;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        if (line_mentions_plugin(cursor, length)) {
            if (!found) {
                if (used + line_length + 1 >= NEXT_SIZE) goto cleanup;
                used += (size_t)snprintf(next + used, NEXT_SIZE - used, "%s\n", enable_line);
                found = true;
            }
        } else {
            if (used + length + 2 > NEXT_SIZE) goto cleanup;
            (void)memcpy(next + used, cursor, length);
            used += length;
            next[used++] = '\n';
        }
        if (end == NULL) break;
        cursor = end + 1;
    }
    if (!found) {
        if (used + line_length + 1 >= NEXT_SIZE) goto cleanup;
        used += (size_t)snprintf(next + used, NEXT_SIZE - used, "%s\n", enable_line);
    }
    next[used] = '\0';
    if (snprintf(temp, sizeof(temp), "%s.part", path) >= (int)sizeof(temp) ||
        !make_parent_dirs(temp)) goto cleanup;
    fd = sceIoOpen(temp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) goto cleanup;
    ok = sceIoWrite(fd, next, used) == (int)used;
    if (sceIoClose(fd) < 0) ok = false;
    if (!ok) {
        (void)sceIoRemove(temp);
        goto cleanup;
    }
    (void)sceIoRemove(path);
    if (sceIoRename(temp, path) < 0) {
        (void)sceIoRemove(temp);
        goto cleanup;
    }
    ok = true;

cleanup:
    free(current);
    free(next);
    return ok;
}

static bool plugin_file_enabled(const char *path) {
    char contents[8192];
    const char *cursor;
    if (!read_text_file(path, contents, sizeof(contents))) return false;
    cursor = contents;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        if (line_enables_plugin(cursor, length)) return true;
        if (end == NULL) break;
        cursor = end + 1;
    }
    return false;
}

bool vp_vita_psp_runtime_status(bool *adrenaline_installed, bool *plugin_installed,
                                bool *plugin_enabled) {
    if (adrenaline_installed != NULL) *adrenaline_installed = path_exists(VP_ADRENALINE_EBOOT);
    if (plugin_installed != NULL) *plugin_installed = path_exists(VP_PSP_PLUGIN_TARGET);
    if (plugin_enabled != NULL) {
        *plugin_enabled = plugin_file_enabled(VP_PSP_VSH_PLUGINS) ||
                          plugin_file_enabled(VP_PSP_EPI_PLUGINS) ||
                          plugin_file_enabled(VP_PSP_SE_PLUGINS);
    }
    return true;
}

static void ensure_psp_plugin_dirs(void) {
    int code = 0;
    if (!path_exists(VP_PSP_SE_ROOT)) code = sceIoMkdir(VP_PSP_SE_ROOT, 0777);
    record_psp_plugin_event("mkdir-pspemu-seplugins", code);
}

bool vp_vita_psp_plugin_install(void) {
    uint64_t source_size;
    uint64_t target_size;
    int copy_code = 0;
    vp_vita_set_error(NULL);
    if (!path_file_size(VP_PSP_PLUGIN_SOURCE, &source_size)) {
        record_psp_plugin_event("source-stat", -1);
        vp_vita_set_error("No se encontro el complemento PSP incluido en VitaShelf");
        return false;
    }
    if (!path_file_size(VP_PSP_PLUGIN_TARGET, &target_size) || source_size != target_size ||
        !same_file_bytes(VP_PSP_PLUGIN_SOURCE, VP_PSP_PLUGIN_TARGET)) {
        ensure_psp_plugin_dirs();
        if (path_exists(VP_PSP_PLUGIN_TARGET)) {
            int remove_code = sceIoRemove(VP_PSP_PLUGIN_TARGET);
            record_psp_plugin_event("remove-target", remove_code);
        }
        if (!copy_whole_file_with_status(VP_PSP_PLUGIN_SOURCE, VP_PSP_PLUGIN_TARGET,
                                         &copy_code)) {
            return psp_plugin_copy_failed("copy", copy_code);
        }
    }
    if (path_exists(VP_PSP_PLUGIN_LEGACY_TARGET)) {
        int legacy_code = sceIoRemove(VP_PSP_PLUGIN_LEGACY_TARGET);
        record_psp_plugin_event("remove-legacy-target", legacy_code);
    }
    if (!write_plugin_enable_file(VP_PSP_VSH_PLUGINS, VP_PSP_LIST_CLASSIC)) {
        vp_vita_set_error("No se pudo habilitar el complemento PSP en vsh.txt");
        return false;
    }
    if (path_exists(VP_PSP_EPI_PLUGINS)) {
        if (!write_plugin_enable_file(VP_PSP_EPI_PLUGINS, VP_PSP_LIST_EPI)) {
            vp_vita_set_error("No se pudo habilitar el complemento PSP en EPIplugins.txt");
            return false;
        }
    } else if (path_exists(VP_PSP_SE_PLUGINS)) {
        if (!write_plugin_enable_file(VP_PSP_SE_PLUGINS, VP_PSP_LIST_EPI)) {
            vp_vita_set_error("No se pudo habilitar el complemento PSP en plugins.txt");
            return false;
        }
    } else if (!write_plugin_enable_file(VP_PSP_SE_PLUGINS, VP_PSP_LIST_EPI)) {
        vp_vita_set_error("No se pudo crear la lista de complementos PSP");
        return false;
    }
    (void)sceIoRemove(VP_PSP_BOOT_REQUEST);
    return true;
}

static bool psp_request_name(const char *image_path, char *name, size_t capacity) {
    const char *prefix = "ux0:pspemu/ISO/";
    const char *file;
    const char *cursor;
    size_t length;
    if (image_path == NULL || strncmp(image_path, prefix, strlen(prefix)) != 0) return false;
    file = image_path + strlen(prefix);
    length = strlen(file);
    if (length == 0 || length >= capacity || strchr(file, '/') != NULL ||
        strstr(file, "..") != NULL) return false;
    for (cursor = file; *cursor != '\0'; cursor++) {
        if (*cursor < 0x20 || *cursor == ':' || *cursor == '\\') return false;
    }
    if (length < 4 || (strcasecmp(file + length - 4, ".iso") != 0 &&
                       strcasecmp(file + length - 4, ".cso") != 0)) return false;
    (void)snprintf(name, capacity, "%s", file);
    return true;
}

static bool psp_boot_request_stale(void) {
    char contents[256];
    char *end = NULL;
    unsigned long long written;
    SceRtcTick now;
    if (!read_text_file(VP_PSP_BOOT_REQUEST, contents, sizeof(contents))) return true;
    written = strtoull(contents, &end, 10);
    if (end == contents || sceRtcGetCurrentTick(&now) < 0) return true;
    return now.tick > written && now.tick - written > 30000000ULL;
}

bool vp_vita_write_psp_boot_request(const char *image_path) {
    char name[240];
    char contents[768];
    char temp[VP_PATH + 8];
    SceRtcTick now;
    SceUID fd;
    int written;
    bool ok = true;
    vp_vita_set_error(NULL);
    if (!psp_request_name(image_path, name, sizeof(name))) {
        vp_vita_set_error("La ruta ISO/CSO de PSP no es valida para Adrenaline");
        return false;
    }
    if (sceRtcGetCurrentTick(&now) < 0) {
        vp_vita_set_error("No se pudo leer el reloj de PS Vita");
        return false;
    }
    if (path_exists(VP_PSP_BOOT_REQUEST) && !psp_boot_request_stale()) {
        (void)sceIoRemove(VP_PSP_BOOT_REQUEST);
    }
    if (!make_parent_dirs(VP_PSP_BOOT_REQUEST) ||
        snprintf(temp, sizeof(temp), "%s.part", VP_PSP_BOOT_REQUEST) >= (int)sizeof(temp)) {
        vp_vita_set_error("No se pudo preparar la solicitud de arranque PSP");
        return false;
    }
    written = snprintf(contents, sizeof(contents), "%llu\niso\nms0:/ISO/%s\nEBOOT.BIN\n-\n",
                       (unsigned long long)now.tick, name);
    if (written <= 0 || (size_t)written >= sizeof(contents)) {
        vp_vita_set_error("No se pudo formar la solicitud de arranque PSP");
        return false;
    }
    fd = sceIoOpen(temp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        vp_vita_set_error("No se pudo crear la solicitud de arranque PSP");
        return false;
    }
    if (sceIoWrite(fd, contents, (size_t)written) != written) ok = false;
    if (sceIoClose(fd) < 0) ok = false;
    if (!ok) {
        (void)sceIoRemove(temp);
        vp_vita_set_error("No se pudo escribir la solicitud de arranque PSP");
        return false;
    }
    (void)sceIoRemove(VP_PSP_BOOT_REQUEST);
    if (sceIoRename(temp, VP_PSP_BOOT_REQUEST) < 0) {
        (void)sceIoRemove(temp);
        vp_vita_set_error("No se pudo publicar la solicitud de arranque PSP");
        return false;
    }
    return true;
}

static bool promote_vpk(const char *vpk_path) {
    int result;
    (void)sceIoMkdir("ux0:data/vitashelflauncher", 0777);
    (void)sceIoMkdir("ux0:data/vitashelflauncher/staging", 0777);
    (void)sceIoMkdir(VP_STAGE_DIR, 0777);
    if (!extract_vpk(vpk_path, VP_STAGE_DIR)) return false;
    result = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (result < 0) return false;
    result = scePromoterUtilityInit();
    if (result == 0) {
        result = scePromoterUtilityPromotePkg(VP_STAGE_DIR, 1);
        (void)scePromoterUtilityExit();
    }
    (void)sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    return result == 0;
}

static long probe_http(const char *url, const char *bearer_token,
                        char *response, size_t response_capacity, bool diagnostics) {
    CURL *curl;
    struct curl_slist *headers = NULL;
    ResponseBuffer buffer;
    char authorization[VP_TEXT + 32];
    CURLcode result;
    long status = 0;
    if (url == NULL || response == NULL || response_capacity == 0 || !valid_bearer_token(bearer_token)) return false;
    response[0] = '\0';
    buffer.buffer = response;
    buffer.capacity = response_capacity;
    buffer.used = 0;
    buffer.truncated = false;
    curl = curl_easy_init();
    if (curl == NULL) return false;
    if (bearer_token != NULL && bearer_token[0] != '\0') {
        (void)snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", bearer_token);
        headers = curl_slist_append(headers, authorization);
        if (headers == NULL) {
            curl_easy_cleanup(curl);
            return false;
        }
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
                     bearer_token != NULL && bearer_token[0] != '\0' ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    result = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    set_last_error(result, status, diagnostics?response:NULL, buffer.truncated);
    if (diagnostics) record_http_failure(url, result, status, response, buffer.truncated);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && !buffer.truncated ? status : 0;
}

long vp_vita_probe_romm(const char *url, const char *bearer_token,
                        char *response, size_t capacity, void *context) {
    (void)context;
    return probe_http(url,bearer_token,response,capacity,true);
}

long vp_vita_probe_service(const char *url, const char *bearer_token,
                           char *response, size_t capacity, void *context) {
    (void)context;
    return probe_http(url,bearer_token,response,capacity,false);
}

bool vp_vita_fetch(const char *url, const char *bearer_token,
                   char *response, size_t response_capacity, void *context) {
    long status = vp_vita_probe_romm(url, bearer_token, response, response_capacity, context);
    return status >= 200 && status < 300 && response[0] != '\0';
}

bool vp_vita_exchange_pair_code(const char *url, const char *code,
                                char *response, size_t response_capacity, void *context) {
    CURL *curl;
    struct curl_slist *headers = NULL;
    ResponseBuffer buffer;
    char payload[VP_TEXT + 16];
    CURLcode result;
    long status = 0;
    (void)context;
    if (url == NULL || response == NULL || response_capacity == 0 || !valid_pair_code(code)) return false;
    if (snprintf(payload, sizeof(payload), "{\"code\":\"%s\"}", code) >= (int)sizeof(payload)) return false;
    response[0] = '\0';
    buffer.buffer = response;
    buffer.capacity = response_capacity;
    buffer.used = 0;
    buffer.truncated = false;
    curl = curl_easy_init();
    if (curl == NULL) return false;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (headers == NULL) {
        curl_easy_cleanup(curl);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    result = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && status >= 200 && status < 300 && buffer.used > 0;
}

bool vp_vita_transfer(const char *url, const char *bearer_token, const char *destination,
                      VpTransferAction action, void *context) {
    return vp_vita_transfer_with_progress(url, bearer_token, destination, action, NULL, NULL,
                                          context);
}

bool vp_vita_transfer_with_progress(const char *url, const char *bearer_token,
                                    const char *destination, VpTransferAction action,
                                    VpTransferProgressFn progress_callback, void *progress_context,
                                    void *context) {
    CURL *curl;
    struct curl_slist *headers = NULL;
    DownloadFile file;
    CURLcode result;
    long status = 0;
    bool installed = true;
    bool downloaded;
    char authorization[VP_TEXT + 32];
    TransferProgress progress = {progress_callback, progress_context, 0, false};
    (void)context;
    if (url == NULL || destination == NULL || !valid_bearer_token(bearer_token) ||
        !make_parent_dirs(destination) || !vp_url_escape(url, transfer_url, sizeof(transfer_url)))
        return false;
    file.fd = sceIoOpen(destination, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (file.fd < 0) return false;
    curl = curl_easy_init();
    if (curl == NULL) {
        sceIoClose(file.fd);
        (void)sceIoRemove(destination);
        return false;
    }
    if (bearer_token != NULL && bearer_token[0] != '\0') {
        (void)snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", bearer_token);
        headers = curl_slist_append(headers, authorization);
        if (headers == NULL) {
            curl_easy_cleanup(curl);
            sceIoClose(file.fd);
            (void)sceIoRemove(destination);
            return false;
        }
    }
    curl_easy_setopt(curl, CURLOPT_URL, transfer_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, (long)VP_TRANSFER_BUFFER_SIZE);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, transfer_socket_options);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
                     bearer_token != NULL && bearer_token[0] != '\0' ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (progress_callback != NULL) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    result = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    sceIoClose(file.fd);
    downloaded = result == CURLE_OK && status >= 200 && status < 300;
    if (!downloaded) {
        set_last_error(result, status, NULL, false);
        record_http_failure(transfer_url, result, status, NULL, false);
        (void)sceIoRemove(destination);
        return false;
    }
    if (action == VP_TRANSFER_INSTALL_VPK) installed = promote_vpk(destination);
    return installed;
}

typedef struct {
    long status;
    char retry_after[64];
} TransformUploadResult;

static void capture_transform_retry_after(const char *line, TransformUploadResult *result) {
    static const char prefix[] = "Retry-After:";
    const char *value;
    size_t length;
    if (result->retry_after[0] != '\0' ||
        strncasecmp(line, prefix, sizeof(prefix) - 1U) != 0) return;
    value = line + sizeof(prefix) - 1U;
    while (*value == ' ' || *value == '\t') value++;
    length = strcspn(value, "\r\n");
    if (length == 0 || length + 1U > sizeof(result->retry_after)) return;
    memcpy(result->retry_after, value, length);
    result->retry_after[length] = '\0';
}

static size_t transform_header_write(char *data, size_t size, size_t count, void *context) {
    TransformUploadResult *result = context;
    size_t bytes = size * count;
    char line[128];
    if (bytes == 0) return 0;
    if (bytes < sizeof(line)) {
        memcpy(line, data, bytes);
        line[bytes] = '\0';
        capture_transform_retry_after(line, result);
    }
    return bytes;
}

static bool post_transform(const char *url, const SceIoStat *source_stat,
                           const char *authorization, const char *destination,
                           bool no_redirect, TransformUploadResult *result) {
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    DownloadFile upload = {-1};
    DownloadFile output = {-1};
    CURLcode code = CURLE_OK;
    bool uploaded = false;
    memset(result, 0, sizeof(*result));
    upload.fd = sceIoOpen(VP_COVER_SOURCE_PATH, SCE_O_RDONLY, 0);
    output.fd = sceIoOpen(destination, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (upload.fd < 0 || output.fd < 0) goto finish;
    curl = curl_easy_init();
    if (curl == NULL) goto finish;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    if (headers == NULL) goto finish;
    if (authorization != NULL && authorization[0] != '\0') {
        struct curl_slist *next = curl_slist_append(headers, authorization);
        if (next == NULL) goto finish;
        headers = next;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, file_read);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)source_stat->st_size);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, transform_header_write);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, result);
    if (no_redirect) curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    code = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->status);
    uploaded = code == CURLE_OK && result->status >= 200 && result->status < 300;
    if (!uploaded) {
        set_last_error(code, result->status, NULL, false);
        record_http_failure(url, code, result->status, NULL, false);
    }

finish:
    if (headers != NULL) curl_slist_free_all(headers);
    if (curl != NULL) curl_easy_cleanup(curl);
    if (upload.fd >= 0) sceIoClose(upload.fd);
    if (output.fd >= 0) sceIoClose(output.fd);
    if (!uploaded) (void)sceIoRemove(destination);
    return uploaded;
}

static bool transform_image(const char *source_url, const char *bearer_token,
                              const char *service_url, const char *destination, bool background) {
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    LimitedDownloadFile source = {-1, 0, VP_COVER_SOURCE_LIMIT, false};
    SceIoStat source_stat;
    CURLcode result;
    long status = 0;
    bool downloaded = false;
    bool official_auth = false;
    char authorization[VP_TEXT + 32];
    char session_token[VP_IMAGE_SESSION_TOKEN_MAX];
    char endpoint[VP_URL + 64];
    const char *separator;
    int attempt;
    if (source_url == NULL || service_url == NULL || destination == NULL ||
        !valid_bearer_token(bearer_token) ||
        !vp_url_escape(source_url, transfer_url, sizeof(transfer_url))) return false;

    source.fd = sceIoOpen(VP_COVER_SOURCE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (source.fd < 0) return false;
    curl = curl_easy_init();
    if (curl == NULL) goto finish;
    if (bearer_token != NULL && bearer_token[0] != '\0') {
        (void)snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", bearer_token);
        headers = curl_slist_append(headers, authorization);
        if (headers == NULL) goto finish;
    }
    curl_easy_setopt(curl, CURLOPT_URL, transfer_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, limited_file_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &source);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
                     bearer_token != NULL && bearer_token[0] != '\0' ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    result = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    headers = NULL;
    curl_easy_cleanup(curl);
    curl = NULL;
    sceIoClose(source.fd);
    source.fd = -1;
    downloaded = result == CURLE_OK && status >= 200 && status < 300 && source.used > 0;
    if (!downloaded) {
        if (source.exceeded)
            vp_vita_set_error("Imagen original supera 8 MiB");
        else
            set_last_error(result, status, NULL, false);
        record_http_failure(transfer_url, result, status, NULL, false);
        goto finish;
    }

    separator = strchr(service_url, '?') != NULL ? "&" : "?";
    if (snprintf(endpoint, sizeof(endpoint), "%s%swidth=%u&height=%u&mode=%s", service_url, separator,
                  background?480U:256U,background?272U:320U,background?"cover":"contain") >=
             (int)sizeof(endpoint) ||
         !vp_url_escape(endpoint, transform_url, sizeof(transform_url)) ||
         sceIoGetstat(VP_COVER_SOURCE_PATH, &source_stat) < 0 || source_stat.st_size <= 0 ||
         source_stat.st_size > VP_COVER_SOURCE_LIMIT || !make_parent_dirs(destination)) goto finish;

    authorization[0] = '\0';
    official_auth = vp_image_session_active();
    if (official_auth) {
        /* The RomM bearer token is only for the source download; official requests
         * carry the session token instead and never fall back to anonymous access. */
        if (!vp_image_session_ensure(service_url) ||
            !vp_image_session_token(session_token, sizeof(session_token))) goto finish;
        (void)snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s",
                       session_token);
    } else if (bearer_token != NULL && bearer_token[0] != '\0') {
        (void)snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", bearer_token);
    }

    for (attempt = 0; attempt < 2; attempt++) {
        TransformUploadResult upload_result;
        downloaded = post_transform(transform_url, &source_stat, authorization,
                                    destination, official_auth, &upload_result);
        if (downloaded || !official_auth || attempt != 0) break;
        if (upload_result.status == 401) {
            vp_image_session_invalidate();
            if (!vp_image_session_ensure(service_url) ||
                !vp_image_session_token(session_token, sizeof(session_token))) break;
            (void)snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s",
                           session_token);
        } else if (upload_result.status == 429) {
            (void)vp_image_session_wait_retry_after(upload_result.retry_after);
        } else break;
    }

finish:
    if (headers != NULL) curl_slist_free_all(headers);
    if (curl != NULL) curl_easy_cleanup(curl);
    if (source.fd >= 0) sceIoClose(source.fd);
    (void)sceIoRemove(VP_COVER_SOURCE_PATH);
    if (!downloaded) (void)sceIoRemove(destination);
    return downloaded;
}


bool vp_vita_transform_cover(const char *source_url,const char *bearer_token,
                             const char *service_url,const char *destination,void *context) {
    (void)context;
    return transform_image(source_url,bearer_token,service_url,destination,false);
}

bool vp_vita_transform_background(const char *source_url,const char *bearer_token,
                                  const char *service_url,const char *destination,void *context) {
    (void)context;
    return transform_image(source_url,bearer_token,service_url,destination,true);
}

bool vp_vita_launch_uri(const char *uri) {
    if (uri == NULL || uri[0] == '\0') return false;
    return sceAppMgrLaunchAppByUri(0x20000, uri) >= 0;
}
