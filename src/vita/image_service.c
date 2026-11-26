#include "image_service.h"

#include "image_service_auth.h"
#include "vita_backend.h"
#include "vitashelflauncher.h"

#include <curl/curl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <limits.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define VP_IMAGE_AUTH_RESPONSE_SIZE 2048U
#define VP_IMAGE_AUTH_PAYLOAD_SIZE 1024U
#define VP_IMAGE_RETRY_MAX_SECONDS 5UL
#define VP_IMAGE_SESSION_LIFETIME_MAX 3600UL
#define VP_IMAGE_SESSION_EXPIRY_MARGIN 30UL

typedef struct {
    char body[VP_IMAGE_AUTH_RESPONSE_SIZE];
    size_t used;
    bool truncated;
    char retry_after[64];
    long status;
} ImageAuthResponse;

static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
static char installation_id[VP_IMAGE_INSTALL_ID_CHARS + 1U];
static char configured_endpoint[VP_URL];
static char session_token[VP_IMAGE_SESSION_TOKEN_MAX];
static unsigned long long token_expiry_us;
static bool auth_enabled;

static void clear_token_locked(void) {
    session_token[0] = '\0';
    token_expiry_us = 0;
}

static bool token_valid_locked(void) {
    return session_token[0] != '\0' && sceKernelGetProcessTimeWide() < token_expiry_us;
}

static bool ensure_installation_id_locked(void) {
    unsigned char bytes[VP_IMAGE_INSTALL_ID_BYTES];
    if (vp_image_auth_valid_install_id(installation_id)) return true;
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return false;
    return vp_image_auth_format_install_id(bytes, installation_id, sizeof(installation_id));
}

bool vp_image_session_configure(const char *configured_url) {
    char endpoint[VP_URL];
    bool changed;
    bool active;
    if (configured_url != NULL && *configured_url != '\0')
        (void)snprintf(endpoint, sizeof(endpoint), "%s", configured_url);
    else endpoint[0] = '\0';
    active = vp_image_auth_active(configured_url);
    pthread_mutex_lock(&session_mutex);
    changed = strcmp(endpoint, configured_endpoint) != 0;
    auth_enabled = active;
    if (changed) {
        clear_token_locked();
        (void)snprintf(configured_endpoint, sizeof(configured_endpoint), "%s", endpoint);
    }
    pthread_mutex_unlock(&session_mutex);
    return active;
}

bool vp_image_session_active(void) {
    bool active;
    pthread_mutex_lock(&session_mutex);
    active = auth_enabled;
    pthread_mutex_unlock(&session_mutex);
    return active;
}

bool vp_image_session_installation_id(char *out, size_t capacity) {
    bool generated;
    if (out == NULL || capacity <= VP_IMAGE_INSTALL_ID_CHARS) return false;
    pthread_mutex_lock(&session_mutex);
    generated = ensure_installation_id_locked();
    if (generated) (void)snprintf(out, capacity, "%s", installation_id);
    pthread_mutex_unlock(&session_mutex);
    return generated;
}

void vp_image_session_invalidate(void) {
    pthread_mutex_lock(&session_mutex);
    clear_token_locked();
    pthread_mutex_unlock(&session_mutex);
}

void vp_image_session_reset(void) {
    pthread_mutex_lock(&session_mutex);
    clear_token_locked();
    configured_endpoint[0] = '\0';
    auth_enabled = false;
    pthread_mutex_unlock(&session_mutex);
}

bool vp_image_session_token(char *out, size_t capacity) {
    bool copied = false;
    if (out == NULL || capacity == 0) return false;
    pthread_mutex_lock(&session_mutex);
    if (token_valid_locked() && strlen(session_token) < capacity) {
        (void)snprintf(out, capacity, "%s", session_token);
        copied = true;
    }
    pthread_mutex_unlock(&session_mutex);
    return copied;
}

static size_t auth_body_write(char *data, size_t size, size_t count, void *context) {
    ImageAuthResponse *response = context;
    size_t bytes = size * count;
    if (bytes == 0) return 0;
    if (bytes + response->used + 1U > sizeof(response->body)) {
        response->truncated = true;
        return 0;
    }
    memcpy(response->body + response->used, data, bytes);
    response->used += bytes;
    response->body[response->used] = '\0';
    return bytes;
}

static void capture_retry_after(const char *line, ImageAuthResponse *response) {
    static const char prefix[] = "Retry-After:";
    const char *value;
    size_t length;
    if (strncasecmp(line, prefix, sizeof(prefix) - 1U) != 0) return;
    value = line + sizeof(prefix) - 1U;
    while (*value == ' ' || *value == '\t') value++;
    length = strcspn(value, "\r\n");
    if (length == 0 || length + 1U > sizeof(response->retry_after)) return;
    memcpy(response->retry_after, value, length);
    response->retry_after[length] = '\0';
}

static size_t auth_header_write(char *data, size_t size, size_t count, void *context) {
    ImageAuthResponse *response = context;
    size_t bytes = size * count;
    char line[128];
    if (bytes == 0) return 0;
    if (bytes < sizeof(line)) {
        memcpy(line, data, bytes);
        line[bytes] = '\0';
        capture_retry_after(line, response);
    }
    return bytes;
}

static bool post_json(const char *url, const char *payload, ImageAuthResponse *response) {
    CURL *curl;
    struct curl_slist *headers = NULL;
    CURLcode result;
    if (url == NULL || payload == NULL || response == NULL) return false;
    memset(response, 0, sizeof(*response));
    curl = curl_easy_init();
    if (curl == NULL) return false;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (headers == NULL) {
        curl_easy_cleanup(curl);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, auth_body_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, auth_header_write);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
    /* Credentials travel in the body: never follow a redirect with them. */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    result = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && !response->truncated && response->status > 0;
}

static const char *find_json_field(const char *body, const char *name) {
    char key[64];
    const char *cursor;
    if (body == NULL || name == NULL ||
        snprintf(key, sizeof(key), "\"%s\"", name) >= (int)sizeof(key)) return NULL;
    cursor = strstr(body, key);
    if (cursor == NULL) return NULL;
    cursor += strlen(key);
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') cursor++;
    return *cursor == ':' ? cursor + 1 : NULL;
}

static bool json_string_field(const char *body, const char *name,
                              char *out, size_t capacity) {
    const char *cursor = find_json_field(body, name);
    size_t length = 0;
    if (cursor == NULL || *cursor != '"' || out == NULL || capacity == 0) return false;
    cursor++;
    while (*cursor != '\0' && *cursor != '"') {
        unsigned char value = (unsigned char)*cursor;
        if (value == '\\' || value < 0x20U || length + 1U >= capacity) return false;
        out[length++] = (char)value;
        cursor++;
    }
    if (*cursor != '"' || length == 0) return false;
    out[length] = '\0';
    return true;
}

static unsigned long json_number_field(const char *body, const char *name,
                                       unsigned long fallback) {
    const char *cursor = find_json_field(body, name);
    unsigned long value = 0;
    if (cursor == NULL) return fallback;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor < '0' || *cursor > '9') return fallback;
    while (*cursor >= '0' && *cursor <= '9') {
        value = value * 10UL + (unsigned long)(*cursor - '0');
        if (value > VP_IMAGE_SESSION_LIFETIME_MAX) value = VP_IMAGE_SESSION_LIFETIME_MAX;
        cursor++;
    }
    return value > 0 ? value : fallback;
}

static bool base64url_encode(const unsigned char *data, size_t length,
                             char *out, size_t capacity) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t index = 0;
    size_t position = 0;
    size_t remainder;
    size_t padding;
    size_t needed;
    if (length == 0 || out == NULL) return false;
    remainder = length % 3U;
    padding = remainder == 0U ? 0U : 3U - remainder;
    needed = ((length + 2U) / 3U) * 4U - padding;
    if (capacity <= needed) return false;
    while (length - index >= 3U) {
        unsigned int triple = ((unsigned int)data[index] << 16) |
                              ((unsigned int)data[index + 1U] << 8) |
                              (unsigned int)data[index + 2U];
        out[position++] = alphabet[(triple >> 18) & 0x3fU];
        out[position++] = alphabet[(triple >> 12) & 0x3fU];
        out[position++] = alphabet[(triple >> 6) & 0x3fU];
        out[position++] = alphabet[triple & 0x3fU];
        index += 3U;
    }
    if (length - index == 1U) {
        unsigned int triple = (unsigned int)data[index] << 16;
        out[position++] = alphabet[(triple >> 18) & 0x3fU];
        out[position++] = alphabet[(triple >> 12) & 0x3fU];
    } else if (length - index == 2U) {
        unsigned int triple = ((unsigned int)data[index] << 16) |
                              ((unsigned int)data[index + 1U] << 8);
        out[position++] = alphabet[(triple >> 18) & 0x3fU];
        out[position++] = alphabet[(triple >> 12) & 0x3fU];
        out[position++] = alphabet[(triple >> 6) & 0x3fU];
    }
    out[position] = '\0';
    return position == needed;
}

bool vp_image_session_wait_retry_after(const char *header_value) {
    unsigned long seconds = 1UL;
    unsigned long long jitter;
    unsigned char random_byte = 0;
    if (header_value != NULL && *header_value != '\0') {
        const char *cursor = header_value;
        bool digits = false;
        seconds = 0UL;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        while (*cursor >= '0' && *cursor <= '9') {
            digits = true;
            if (seconds <= VP_IMAGE_RETRY_MAX_SECONDS)
                seconds = seconds * 10UL + (unsigned long)(*cursor - '0');
            if (seconds > VP_IMAGE_RETRY_MAX_SECONDS) {
                seconds = VP_IMAGE_RETRY_MAX_SECONDS;
                break;
            }
            cursor++;
        }
        if (!digits || seconds == 0UL) seconds = 1UL;
    }
    if (RAND_bytes(&random_byte, 1) == 1)
        jitter = (unsigned long long)random_byte * 2000ULL;
    else
        jitter = sceKernelGetProcessTimeWide() % 500000ULL;
    sceKernelDelayThread((unsigned int)(seconds * 1000000ULL + jitter));
    return true;
}

bool vp_image_session_ensure(const char *service_url) {
    char endpoint_base[VP_URL];
    char challenge_url[VP_URL + 32U];
    char session_url[VP_URL + 32U];
    char install[VP_IMAGE_INSTALL_ID_CHARS + 1U];
    char challenge[VP_IMAGE_AUTH_FIELD_MAX + 1U];
    char message[VP_IMAGE_AUTH_MESSAGE_MAX];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0U;
    char proof[128];
    char payload[VP_IMAGE_AUTH_PAYLOAD_SIZE];
    char token[VP_IMAGE_SESSION_TOKEN_MAX];
    const char *key;
    size_t key_length;
    unsigned long lifetime;
    bool success = false;
    int attempt;

    if (service_url == NULL) return false;
    pthread_mutex_lock(&session_mutex);
    if (!auth_enabled) goto finish;
    if (!vp_image_auth_origins_match(service_url, configured_endpoint)) goto finish;
    if (token_valid_locked()) {
        success = true;
        goto finish;
    }
    if (!ensure_installation_id_locked() ||
        !vp_image_auth_origin_base(service_url, endpoint_base, sizeof(endpoint_base)) ||
        snprintf(challenge_url, sizeof(challenge_url), "%s/v1/challenge", endpoint_base) >=
            (int)sizeof(challenge_url) ||
        snprintf(session_url, sizeof(session_url), "%s/v1/session", endpoint_base) >=
            (int)sizeof(session_url)) goto finish;
    (void)snprintf(install, sizeof(install), "%s", installation_id);
    key = vp_image_auth_key();
    key_length = strlen(key);
    if (key_length == 0U || key_length > INT_MAX) goto finish;

    for (attempt = 0; attempt < 2; attempt++) {
        ImageAuthResponse challenge_response;
        ImageAuthResponse session_response;
        if (!post_json(challenge_url, "{}", &challenge_response)) break;
        if (challenge_response.status == 429) {
            (void)vp_image_session_wait_retry_after(challenge_response.retry_after);
            continue;
        }
        if (challenge_response.status < 200 || challenge_response.status >= 300 ||
            !json_string_field(challenge_response.body, "challenge",
                               challenge, sizeof(challenge)) ||
            !vp_image_auth_valid_field(challenge)) break;
        if (!vp_image_auth_build_message(vp_image_auth_key_id(), install, challenge,
                                         message, sizeof(message))) break;
        if (HMAC(EVP_sha256(), key, (int)key_length,
                 (const unsigned char *)message, strlen(message),
                 digest, &digest_length) == NULL ||
            !base64url_encode(digest, digest_length, proof, sizeof(proof))) break;
        if (snprintf(payload, sizeof(payload),
                     "{\"key_id\":\"%s\",\"installation_id\":\"%s\","
                     "\"challenge\":\"%s\",\"proof\":\"%s\"}",
                     vp_image_auth_key_id(), install, challenge, proof) >= (int)sizeof(payload))
            break;
        if (!post_json(session_url, payload, &session_response)) break;
        if (session_response.status == 429) {
            (void)vp_image_session_wait_retry_after(session_response.retry_after);
            continue;
        }
        if (session_response.status < 200 || session_response.status >= 300 ||
            !json_string_field(session_response.body, "token", token, sizeof(token)) ||
            !vp_image_auth_valid_field(token)) break;
        lifetime = json_number_field(session_response.body, "expires_in",
                                     VP_IMAGE_SESSION_LIFETIME_MAX);
        if (lifetime > VP_IMAGE_SESSION_LIFETIME_MAX) lifetime = VP_IMAGE_SESSION_LIFETIME_MAX;
        if (lifetime <= VP_IMAGE_SESSION_EXPIRY_MARGIN) lifetime = 1UL;
        (void)snprintf(session_token, sizeof(session_token), "%s", token);
        token_expiry_us = sceKernelGetProcessTimeWide() +
                          (unsigned long long)(lifetime - VP_IMAGE_SESSION_EXPIRY_MARGIN) *
                              1000000ULL;
        success = true;
        break;
    }

finish:
    pthread_mutex_unlock(&session_mutex);
    if (!success) vp_vita_set_error("No se pudo iniciar sesión con el servicio de imágenes");
    return success;
}
