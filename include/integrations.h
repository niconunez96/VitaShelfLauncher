#ifndef VP_INTEGRATIONS_H
#define VP_INTEGRATIONS_H
#include "vitashelflauncher.h"

typedef enum {
    VP_CONNECTION_UNCONFIGURED,
    VP_CONNECTION_CHECKING,
    VP_CONNECTION_CONNECTED,
    VP_CONNECTION_INVALID_KEY,
    VP_CONNECTION_UNREACHABLE,
    VP_CONNECTION_INVALID_RESPONSE,
    VP_CONNECTION_RATE_LIMITED,
    VP_CONNECTION_NOT_CHECKED
} VpConnectionState;

typedef enum { VP_ARTWORK_FOUND, VP_ARTWORK_NOT_FOUND, VP_ARTWORK_FAILED } VpArtworkResult;
bool vp_image_service_endpoint(const char *host, const char *endpoint, char *url, size_t capacity);
VpConnectionState vp_image_service_check(const char *host, VpRommProbeBackend backend);
VpConnectionState vp_steamgrid_check(const char *key, VpRommProbeBackend backend);
VpArtworkResult vp_steamgrid_cover(VpItem *item, const char *key, VpRommProbeBackend backend,
                                   VpConnectionState *state);
#endif
