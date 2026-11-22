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
const char *vp_vita_last_error(void);
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
