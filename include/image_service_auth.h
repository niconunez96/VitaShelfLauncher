#ifndef VP_IMAGE_SERVICE_AUTH_H
#define VP_IMAGE_SERVICE_AUTH_H

#include <stdbool.h>
#include <stddef.h>

#define VP_IMAGE_AUTH_FIELD_MAX 128
#define VP_IMAGE_AUTH_PREFIX "vitashelf-image-auth-v1\n"
#define VP_IMAGE_AUTH_PREFIX_LEN (sizeof(VP_IMAGE_AUTH_PREFIX) - 1U)
/* Prefix, three fields and their three trailing newlines, plus the terminator. */
#define VP_IMAGE_AUTH_MESSAGE_MAX (VP_IMAGE_AUTH_PREFIX_LEN + \
                                   3U * (VP_IMAGE_AUTH_FIELD_MAX + 1U) + 1U)

#define VP_IMAGE_INSTALL_ID_BYTES 16U
#define VP_IMAGE_INSTALL_ID_CHARS (VP_IMAGE_INSTALL_ID_BYTES * 2U)
#define VP_IMAGE_AUTH_ORIGIN_MAX 256U
/* Defined only where the Vita session client is compiled alongside these helpers. */
#define VP_IMAGE_SERVICE_AUTH_CLIENT 1

typedef struct {
    char scheme[16];
    char host[VP_IMAGE_AUTH_ORIGIN_MAX];
    unsigned int port;
} VpImageOrigin;

const char *vp_image_auth_official_url(void);
const char *vp_image_auth_key_id(void);
const char *vp_image_auth_key(void);

bool vp_image_auth_origin_parse(const char *url, VpImageOrigin *origin);
bool vp_image_auth_origin_equal(const VpImageOrigin *left, const VpImageOrigin *right);
bool vp_image_auth_origins_match(const char *configured_url, const char *official_url);
bool vp_image_auth_origin_base(const char *url, char *out, size_t capacity);
bool vp_image_auth_active_for(const char *configured_url, const char *official_url,
                              const char *key_id, const char *key);
bool vp_image_auth_active(const char *configured_url);
bool vp_image_auth_valid_field(const char *value);
bool vp_image_auth_build_message(const char *key_id, const char *installation_id,
                                 const char *challenge, char *out, size_t capacity);
bool vp_image_auth_format_install_id(const unsigned char bytes[VP_IMAGE_INSTALL_ID_BYTES],
                                     char *out, size_t capacity);
bool vp_image_auth_valid_install_id(const char *id);

#endif
