"""Exercise the Vita IPv4 compatibility boundary and its actual linker wrapping."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VitaInetPtonTest(unittest.TestCase):
    def test_dns_is_not_an_ip_and_ipv4_matches_posix(self):
        caller = r'''
        #include <arpa/inet.h>
        #include <assert.h>
        #include <errno.h>
        #include <stdio.h>
        #include <string.h>
        extern int native_calls;
        extern int __real_inet_pton(int, const char *, void *);
        int main(void) {
            const char *invalid[] = {"example.com", "images.example.test", "",
                "1", "1.2.3", "1.2.3.4.5", "256.1.2.3", "01.2.3.4",
                "1.2.3.04", " 1.2.3.4", "1.2.3.4 ", "+1.2.3.4",
                "1..2.3", "1.2.3.", "0x7f.0.0.1", "9999999999999999.1.2.3"};
            unsigned char out[16], expected[4];
            char address[32];
            memset(out, 0xa5, sizeof(out));
            assert(__real_inet_pton(AF_INET, "example.com", out) == -1);
            int before = native_calls;
            for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
                errno = 123;
                assert(inet_pton(AF_INET, invalid[i], out) == 0);
                assert(errno == 123);
                for (unsigned j=0; j<sizeof(out); j++) assert(out[j] == 0xa5);
            }
            for (unsigned i=0; i<256; i++) {
                snprintf(address, sizeof(address), "%u.%u.%u.%u", i, 255-i, i/2, 0U);
                expected[0]=(unsigned char)i; expected[1]=(unsigned char)(255-i);
                expected[2]=(unsigned char)(i/2); expected[3]=0;
                assert(inet_pton(AF_INET, address, out) == 1);
                assert(!memcmp(out, expected, 4));
            }
            assert(native_calls == before);
            assert(inet_pton(AF_INET6, "::1", out) == 1);
            assert(native_calls == before+1 && out[15] == 1);
            errno=0;
            assert(inet_pton(-123, "ignored", out) == -1 && errno == EAFNOSUPPORT);
            assert(native_calls == before+2);
            return 0;
        }
        '''
        native = r'''
        #include <arpa/inet.h>
        #include <errno.h>
        #include <string.h>
        int native_calls;
        int inet_pton(int af, const char *src, void *dst) {
            native_calls++;
            if (af == AF_INET) { errno=EINVAL; return -1; }
            if (af == AF_INET6 && !strcmp(src, "::1")) {
                memset(dst, 0, 16); ((unsigned char *)dst)[15]=1; return 1;
            }
            errno=EAFNOSUPPORT; return -1;
        }
        '''
        with tempfile.TemporaryDirectory(prefix="vita-inet-pton-") as directory:
            path = Path(directory)
            (path / "caller.c").write_text(caller)
            (path / "native.c").write_text(native)
            executable = path / "probe"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                str(path / "caller.c"), str(path / "native.c"),
                str(ROOT / "src/vita/inet_pton_compat.c"),
                "-Wl,--wrap=inet_pton", "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
