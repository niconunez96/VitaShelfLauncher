#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

/* GNU ld resolves this to the unwrapped SDK implementation. */
extern int __real_inet_pton(int family, const char *source, void *destination);

int __wrap_inet_pton(int family, const char *source, void *destination) {
    unsigned char address[4];
    const char *cursor = source;
    unsigned part;
    if (family != AF_INET) return __real_inet_pton(family, source, destination);
    if (source == NULL || destination == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* curl 8.17 classifies any nonzero result as an IP, omitting SNI. The
     * Vita SDK can return -1 for DNS names: parse IPv4 with POSIX semantics
     * here instead, retaining native behavior for other address families. */
    for (part = 0; part < 4; part++) {
        unsigned value = 0;
        const char *start = cursor;
        if (*cursor < '0' || *cursor > '9') return 0;
        do {
            value = value * 10U + (unsigned)(*cursor - '0');
            if (value > 255U) return 0;
            cursor++;
        } while (*cursor >= '0' && *cursor <= '9');
        if (cursor - start > 1 && *start == '0') return 0;
        address[part] = (unsigned char)value;
        if (part < 3) {
            if (*cursor != '.') return 0;
            cursor++;
        } else if (*cursor != '\0') return 0;
    }
    memcpy(destination, address, sizeof(address));
    return 1;
}
