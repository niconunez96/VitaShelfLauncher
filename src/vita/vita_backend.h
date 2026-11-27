#ifndef VITASHELF_LAUNCHER_VITA_BACKEND_H
#define VITASHELF_LAUNCHER_VITA_BACKEND_H

#include "vitashelflauncher.h"

typedef bool (*VpTransferProgressFn)(unsigned long long downloaded, unsigned long long total,
                                     void *context);

bool vp_vita_fetch(const char *url, const char *bearer_token,
                   char *response, size_t response_capacity, void *context);
long vp_vita_probe_romm(const char *url, const char *bearer_token,
                        char *response, size_t response_capacity, void *context);
long vp_vita_probe_service(const char *url, const char *bearer_token,
                           char *response, size_t response_capacity, void *context);
void vp_vita_last_error(char *destination, size_t capacity);
void vp_vita_set_error(const char *message);
typedef enum {
    VP_IMAGE_LOG_SERVICE_HTTP,
    VP_IMAGE_LOG_HEALTH_START,
    VP_IMAGE_LOG_HEALTH_OK,
    VP_IMAGE_LOG_HEALTH_FAILED,
    VP_IMAGE_LOG_CHALLENGE,
    VP_IMAGE_LOG_SESSION,
    VP_IMAGE_LOG_AUTH_OK,
    VP_IMAGE_LOG_AUTH_FAILED,
    VP_IMAGE_LOG_AUTH_CACHED
} VpImageLogStage;

/* Fixed stage labels and numeric metadata only; never pass URLs or credentials. */
void vp_vita_log_image_event(VpImageLogStage stage, int curl_code, long status, bool truncated);
/* Extracts only fixed TLS labels and an OpenSSL numeric error; never logs raw detail. */
void vp_vita_log_tls_details(const char *detail, int rng_ready, int errorbuffer_result);
void vp_vita_log_tls_transport(long socket_error, int info_result, int trace_result);
bool vp_vita_path_exists(const char *path);
bool vp_vita_app_installed(const char *titleid);
bool vp_vita_extract_psp_archive(const VpItem *item, const char *archive_path,
                                 const char *psp_root, char *resolved,
                                 size_t resolved_capacity);
bool vp_vita_extract_nds_archive(const VpItem *item, const char *archive_path,
                                 const char *nds_root, char *resolved,
                                 size_t resolved_capacity);
bool vp_vita_psp_runtime_status(bool *adrenaline_installed, bool *plugin_installed,
                                bool *plugin_enabled);
bool vp_vita_psp_plugin_install(void);
bool vp_vita_write_psp_boot_request(const char *image_path);
bool vp_vita_exchange_pair_code(const char *url, const char *code,
                                char *response, size_t response_capacity, void *context);
bool vp_vita_transfer(const char *url, const char *bearer_token, const char *destination,
                      VpTransferAction action, void *context);
bool vp_vita_transfer_with_progress(const char *url, const char *bearer_token,
                                    const char *destination, VpTransferAction action,
                                    VpTransferProgressFn progress, void *progress_context,
                                    void *context);
bool vp_vita_transform_cover(const char *source_url, const char *bearer_token,
                             const char *service_url, const char *destination, void *context);
bool vp_vita_transform_background(const char *source_url, const char *bearer_token,
                                  const char *service_url, const char *destination, void *context);
bool vp_vita_launch_uri(const char *uri);

#endif
