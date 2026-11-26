#ifndef VP_VITA_IMAGE_SERVICE_H
#define VP_VITA_IMAGE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#define VP_IMAGE_SESSION_TOKEN_MAX 128U

/* Called whenever the configured image endpoint changes. Returns true when the
 * compiled official auth mode is active for that endpoint. */
bool vp_image_session_configure(const char *configured_url);
bool vp_image_session_active(void);
bool vp_image_session_installation_id(char *out, size_t capacity);
bool vp_image_session_ensure(const char *service_url);
bool vp_image_session_token(char *out, size_t capacity);
void vp_image_session_invalidate(void);
void vp_image_session_reset(void);
bool vp_image_session_wait_retry_after(const char *header_value);

#endif
