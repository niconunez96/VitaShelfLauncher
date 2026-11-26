#include "image_service_auth.h"

#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include("image_service_build.h")
#    include "image_service_build.h"
#  endif
#endif

#ifndef VP_BUILD_IMAGE_SERVICE_URL
#define VP_BUILD_IMAGE_SERVICE_URL "https://images.example.invalid/image"
#endif
#ifndef VP_BUILD_IMAGE_SERVICE_KEY_ID
#define VP_BUILD_IMAGE_SERVICE_KEY_ID ""
#endif
#ifndef VP_BUILD_IMAGE_SERVICE_KEY
#define VP_BUILD_IMAGE_SERVICE_KEY ""
#endif

const char *vp_image_auth_official_url(void) {
    return VP_BUILD_IMAGE_SERVICE_URL;
}

const char *vp_image_auth_key_id(void) {
    return VP_BUILD_IMAGE_SERVICE_KEY_ID;
}

const char *vp_image_auth_key(void) {
    return VP_BUILD_IMAGE_SERVICE_KEY;
}

static char lower_ascii(char value) {
    if (value >= 'A' && value <= 'Z') return (char)(value - 'A' + 'a');
    return value;
}

static bool parse_scheme(const char *text, size_t length, char *out, size_t capacity) {
    size_t index;
    if (length == 0 || length + 1U > capacity) return false;
    for (index = 0; index < length; index++) out[index] = lower_ascii(text[index]);
    out[length] = '\0';
    return (length == 4U && strcmp(out, "http") == 0) ||
           (length == 5U && strcmp(out, "https") == 0);
}

static bool valid_host_char(unsigned char value, bool bracketed) {
    if (value <= 0x20U || value >= 0x7fU) return false;
    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || value == '.' || value == '-' || value == '_')
        return true;
    return bracketed && (value == '[' || value == ']' || value == ':');
}

static bool parse_port(const char *text, size_t length, unsigned int *port) {
    unsigned int value = 0;
    size_t index;
    if (length == 0) return false;
    for (index = 0; index < length; index++) {
        if (text[index] < '0' || text[index] > '9') return false;
        value = value * 10U + (unsigned int)(text[index] - '0');
        if (value > 65535U) return false;
    }
    if (value == 0U) return false;
    *port = value;
    return true;
}

bool vp_image_auth_origin_parse(const char *url, VpImageOrigin *origin) {
    const char *authority;
    const char *authority_end;
    const char *host_begin;
    size_t scheme_length;
    size_t authority_length;
    size_t host_length;
    size_t index;
    unsigned int port = 0U;
    bool bracketed;
    VpImageOrigin parsed;

    if (origin == NULL) return false;
    memset(origin, 0, sizeof(*origin));
    if (url == NULL) return false;
    authority = strstr(url, "://");
    if (authority == NULL) return false;
    scheme_length = (size_t)(authority - url);
    authority += 3;
    if (!parse_scheme(url, scheme_length, parsed.scheme, sizeof(parsed.scheme))) return false;

    authority_end = authority;
    while (*authority_end != '\0' && *authority_end != '/' &&
           *authority_end != '?' && *authority_end != '#') authority_end++;
    authority_length = (size_t)(authority_end - authority);
    if (authority_length == 0) return false;
    host_begin = authority;
    bracketed = authority[0] == '[';
    if (bracketed) {
        const char *close = memchr(authority, ']', authority_length);
        if (close == NULL) return false;
        host_length = (size_t)(close - authority) + 1U;
        if (authority_length > host_length && authority[host_length] != ':') return false;
    } else {
        const char *colon = memchr(authority, ':', authority_length);
        host_length = colon != NULL ? (size_t)(colon - authority) : authority_length;
    }
    if (host_length == 0 || host_length + 1U > sizeof(parsed.host)) return false;
    for (index = 0; index < host_length; index++)
        if (!valid_host_char((unsigned char)host_begin[index], bracketed)) return false;
    for (index = 0; index < host_length; index++)
        parsed.host[index] = lower_ascii(host_begin[index]);
    parsed.host[host_length] = '\0';

    if (authority_length > host_length) {
        const char *colon = authority + host_length;
        if (*colon != ':' || !parse_port(colon + 1, authority_length - host_length - 1U, &port))
            return false;
    }
    if (strcmp(parsed.scheme, "https") == 0 && port == 443U) port = 0U;
    else if (strcmp(parsed.scheme, "http") == 0 && port == 80U) port = 0U;
    parsed.port = port;
    *origin = parsed;
    return true;
}

bool vp_image_auth_origin_equal(const VpImageOrigin *left, const VpImageOrigin *right) {
    return left != NULL && right != NULL && left->port == right->port &&
           strcmp(left->scheme, right->scheme) == 0 && strcmp(left->host, right->host) == 0;
}

bool vp_image_auth_origins_match(const char *configured_url, const char *official_url) {
    VpImageOrigin configured;
    VpImageOrigin official;
    return vp_image_auth_origin_parse(configured_url, &configured) &&
           vp_image_auth_origin_parse(official_url, &official) &&
           vp_image_auth_origin_equal(&configured, &official);
}

bool vp_image_auth_origin_base(const char *url, char *out, size_t capacity) {
    VpImageOrigin origin;
    int written;
    if (out == NULL || capacity == 0 || !vp_image_auth_origin_parse(url, &origin)) return false;
    if (origin.port == 0U)
        written = snprintf(out, capacity, "%s://%s", origin.scheme, origin.host);
    else
        written = snprintf(out, capacity, "%s://%s:%u", origin.scheme, origin.host, origin.port);
    return written > 0 && (size_t)written < capacity;
}

bool vp_image_auth_valid_field(const char *value) {
    const char *cursor;
    size_t length;
    if (value == NULL) return false;
    for (cursor = value, length = 0; *cursor != '\0'; cursor++, length++) {
        unsigned char character = (unsigned char)*cursor;
        if (length >= VP_IMAGE_AUTH_FIELD_MAX) return false;
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '.' || character == '_' ||
            character == '-') continue;
        return false;
    }
    return length > 0U;
}

bool vp_image_auth_build_message(const char *key_id, const char *installation_id,
                                 const char *challenge, char *out, size_t capacity) {
    size_t position;
    size_t needed;
    if (!vp_image_auth_valid_field(key_id) || !vp_image_auth_valid_field(installation_id) ||
        !vp_image_auth_valid_field(challenge) || out == NULL) return false;
    needed = VP_IMAGE_AUTH_PREFIX_LEN + strlen(key_id) + strlen(installation_id) +
             strlen(challenge) + 3U;
    if (capacity <= needed) return false;
    memcpy(out, VP_IMAGE_AUTH_PREFIX, VP_IMAGE_AUTH_PREFIX_LEN);
    position = VP_IMAGE_AUTH_PREFIX_LEN;
    (void)snprintf(out + position, capacity - position, "%s\n%s\n%s\n",
                   key_id, installation_id, challenge);
    return strlen(out) == needed;
}

bool vp_image_auth_format_install_id(const unsigned char bytes[VP_IMAGE_INSTALL_ID_BYTES],
                                     char *out, size_t capacity) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    if (bytes == NULL || out == NULL || capacity <= VP_IMAGE_INSTALL_ID_CHARS) return false;
    for (index = 0; index < VP_IMAGE_INSTALL_ID_BYTES; index++) {
        out[index * 2U] = digits[bytes[index] >> 4];
        out[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    out[VP_IMAGE_INSTALL_ID_CHARS] = '\0';
    return true;
}

bool vp_image_auth_valid_install_id(const char *id) {
    size_t index;
    if (id == NULL || strlen(id) != VP_IMAGE_INSTALL_ID_CHARS) return false;
    for (index = 0; index < VP_IMAGE_INSTALL_ID_CHARS; index++) {
        unsigned char value = (unsigned char)id[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
    }
    return true;
}

static bool official_url_is_invalid(const char *official_url) {
    static const char suffix[] = ".invalid";
    VpImageOrigin origin;
    size_t length;
    size_t index;
    if (!vp_image_auth_origin_parse(official_url, &origin)) return true;
    length = strlen(origin.host);
    if (length < sizeof(suffix) - 1U) return false;
    for (index = 0; index < sizeof(suffix) - 1U; index++)
        if (lower_ascii(origin.host[length - (sizeof(suffix) - 1U) + index]) != suffix[index])
            return false;
    return true;
}

static bool official_url_is_https(const char *official_url) {
    VpImageOrigin origin;
    return vp_image_auth_origin_parse(official_url, &origin) &&
           strcmp(origin.scheme, "https") == 0;
}

bool vp_image_auth_active_for(const char *configured_url, const char *official_url,
                              const char *key_id, const char *key) {
    if (key == NULL || key[0] == '\0' || !vp_image_auth_valid_field(key_id)) return false;
    if (official_url_is_invalid(official_url) || !official_url_is_https(official_url)) return false;
    return vp_image_auth_origins_match(configured_url, official_url);
}

bool vp_image_auth_active(const char *configured_url) {
    return vp_image_auth_active_for(configured_url, vp_image_auth_official_url(),
                                    vp_image_auth_key_id(), vp_image_auth_key());
}
