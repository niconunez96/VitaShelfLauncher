#ifndef VP_VITA_TLS_DIAGNOSTICS_H
#define VP_VITA_TLS_DIAGNOSTICS_H

#include <curl/curl.h>
#include <stdbool.h>

typedef struct {
    bool raw_text;
    bool clienthello_captured;
} VpTlsTrace;

/* trace must remain alive until curl cleanup; headers and bodies are discarded. */
CURLcode vp_vita_enable_tls_trace(CURL *curl, VpTlsTrace *trace);
/* Optional TLS 1.2 experiment; callers must not perform after setup failure. */
CURLcode vp_vita_configure_debug_tls_version(CURL *curl, bool tls12_only);

#endif
