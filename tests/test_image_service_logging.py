"""Exercise the Vita diagnostic sink and HTTP paths with host doubles."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import re

from test_vita_t2_concurrency_probe import body

ROOT = Path(__file__).resolve().parents[1]


class ImageServiceLoggingTest(unittest.TestCase):
    def test_transport_records_are_persistent_and_secret_free(self):
        backend = (ROOT / "src/vita/vita_backend.c").read_text()
        auth = (ROOT / "src/vita/image_service.c").read_text()
        header = (ROOT / "src/vita/vita_backend.h").read_text()
        start = header.index("typedef enum {")
        end = header.index("} VpImageLogStage;", start) + len("} VpImageLogStage;")
        functions = "\n".join((
            body(backend, "void vp_vita_log_image_event("),
            body(backend, "static const char *tls_error_reason("),
            body(backend, "void vp_vita_log_tls_details("),
            body(backend, "static void record_raw_tls_text("),
            body(backend, "static bool record_tls_clienthello("),
            body(backend, "static int tls_debug("),
            body(backend, "CURLcode vp_vita_configure_debug_tls_version("),
            body(backend, "CURLcode vp_vita_enable_tls_trace("),
            body(backend, "void vp_vita_log_tls_transport("),
            body(backend, "static long probe_http("),
            body(auth, "static bool post_json("),
        ))
        code = r'''
        #include <assert.h>
        #include <stdbool.h>
        #include <stdio.h>
        #include <string.h>
        #include <stdlib.h>
        #include <stdarg.h>
        #define VP_TEXT 192
        #define CURL_ERROR_SIZE 256
        #define CURLVERSION_NOW 0
        typedef int CURL;
        typedef int CURLcode;
        typedef struct { bool raw_text; bool clienthello_captured; } VpTlsTrace;
        typedef enum { CURLINFO_TEXT, CURLINFO_HEADER_IN, CURLINFO_HEADER_OUT,
                       CURLINFO_DATA_IN, CURLINFO_DATA_OUT, CURLINFO_SSL_DATA_IN,
                       CURLINFO_SSL_DATA_OUT } curl_infotype;
        typedef int (*DebugFn)(CURL *, curl_infotype, char *, size_t, void *);
        static DebugFn debug_callback;
        static void *debug_context;
        static long verbose;
        static int trace_setup_result;
        #define CURL_SSLVERSION_TLSv1_2 6
        #define CURL_SSLVERSION_MAX_TLSv1_2 (6 << 16)
        static long tls_version;
        static int tls12_enabled, version_result, performs;
        enum { CURLE_OK, CURLE_FAILED_INIT, CURLE_OUT_OF_MEMORY,
               CURLE_PEER_FAILED_VERIFICATION = 60 };
        struct curl_slist { int unused; };
        typedef struct { char *buffer; size_t capacity, used; bool truncated; } ResponseBuffer;
        typedef struct { bool tls_debug; bool tls12; } VpRommConfig;
        typedef struct { char body[2048]; size_t used; bool truncated;
                         char retry_after[64]; long status; } ImageAuthResponse;
        static CURL handle;
        static struct curl_slist list;
        static int result, init_failed, headers_failed, legacy_logs, deny_log;
        static int verify_peer, verify_host;
        static long http_status;
        static FILE *capture;
        static FILE *raw_capture;
        static FILE *hello_capture;
        static int raw_enabled;
        static char *error_buffer;
        static const char *error_detail = "";
        static int rng_ready = 1;
        static int RAND_status(void) { return rng_ready; }
        typedef struct { const char *version; const char *ssl_version; } curl_version_info_data;
        static curl_version_info_data versions = {"8.0-test", "OpenSSL/3.0-test"};
        static curl_version_info_data *curl_version_info(int age) { (void)age; return &versions; }
        static const char *ERR_reason_error_string(unsigned long code) {
            return code == 0x0A000102UL ? "unsupported protocol" : NULL;
        }
        /*OPTIONS*/
        static int sceIoMkdir(const char *p, int mode) { (void)p; (void)mode; return 0; }
        static FILE *log_open(const char *p, const char *mode) {
            if (strcmp(p, "ux0:data/vitashelflauncher/tls-clienthello.log") == 0) {
                assert(strcmp(mode, "a") == 0);
                return deny_log ? NULL : hello_capture;
            }
            if (strcmp(p, "ux0:data/vitashelflauncher/tls-debug.log") == 0) {
                assert(strcmp(mode, "ab") == 0);
                return deny_log ? NULL : raw_capture;
            }
            assert(strcmp(p, "ux0:data/vitashelflauncher/network.log") == 0);
            assert(strcmp(mode, "a") == 0);
            return deny_log ? NULL : capture;
        }
        static int log_close(FILE *f) { return fflush(f); }
        static const char *curl_easy_strerror(CURLcode c) {
            return c == 60 ? "SSL peer certificate cannot be authenticated" : "curl status";
        }
        static CURL *curl_easy_init(void) {
            debug_callback = NULL; debug_context = NULL; verbose = 0; tls_version = 0;
            return init_failed ? NULL : &handle;
        }
        static struct curl_slist *curl_slist_append(struct curl_slist *h, const char *v) {
            (void)h; (void)v; return headers_failed ? NULL : &list;
        }
        static void curl_easy_cleanup(CURL *c) { (void)c; }
        static void curl_slist_free_all(struct curl_slist *h) { (void)h; }
        static CURLcode curl_easy_perform(CURL *c) {
            (void)c;
            performs++;
            if (error_buffer) snprintf(error_buffer, CURL_ERROR_SIZE, "%s", error_detail);
            if (verbose && debug_callback && result == 35) {
                char line[] = "OpenSSL SSL_connect: SSL_ERROR_SYSCALL in connection to secret-host";
                assert(debug_callback(c, CURLINFO_TEXT, line, sizeof(line)-1, debug_context) == 0);
                assert(debug_callback(c, CURLINFO_HEADER_OUT, line, sizeof(line)-1, debug_context) == 0);
                assert(debug_callback(c, CURLINFO_DATA_IN, line, sizeof(line)-1, debug_context) == 0);
            }
            return result;
        }
        static int curl_easy_setopt(CURL *c, int option, ...) {
            va_list args; (void)c;
            va_start(args, option);
            if (option == CURLOPT_ERRORBUFFER) error_buffer = va_arg(args, char *);
            if (option == CURLOPT_SSLVERSION) {
                long version = va_arg(args, long);
                if (version_result == 0) tls_version = version;
                va_end(args); return version_result;
            }
            if (option == CURLOPT_DEBUGFUNCTION) {
                DebugFn callback = va_arg(args, DebugFn);
                if (trace_setup_result == 0) debug_callback = callback;
                va_end(args); return trace_setup_result;
            }
            if (option == CURLOPT_DEBUGDATA) debug_context = va_arg(args, void *);
            if (option == CURLOPT_VERBOSE) {
                verbose = va_arg(args, long);
                assert(debug_callback != NULL);
            }
            if (option == CURLOPT_SSL_VERIFYPEER) verify_peer = (int)va_arg(args, long);
            if (option == CURLOPT_SSL_VERIFYHOST) verify_host = (int)va_arg(args, long);
            va_end(args);
            return 0;
        }
        #define CURLINFO_RESPONSE_CODE 0
        #define CURLINFO_OS_ERRNO 1
        #define curl_easy_getinfo(c, option, dest) ((void)(c), *(dest) = (option == CURLINFO_OS_ERRNO ? 54L : http_status), 0)
        static int response_write, auth_body_write, auth_header_write;
        static bool valid_bearer_token(const char *t) { (void)t; return true; }
        static void set_last_error(CURLcode r, long s, const char *b, bool t) {
            (void)r; (void)s; (void)b; (void)t;
        }
        static void record_http_failure(const char *u, CURLcode r, long s,
                                        const char *b, bool t) {
            (void)u; (void)r; (void)s; (void)b; (void)t; legacy_logs++;
        }
        /*ENUM*/
        #define fopen log_open
        #define fclose log_close
        /*FUNCTIONS*/
        #undef fopen
        #undef fclose
        static long run_probe(const char *url, const char *token, char *response,
                              size_t capacity, bool diagnostics) {
            VpRommConfig options = { raw_enabled != 0, tls12_enabled != 0 };
            return /*PROBE_CALL*/;
        }
        static bool run_post(const char *url, const char *payload, ImageAuthResponse *response,
                             VpImageLogStage stage) {
            return /*POST_CALL*/;
        }
        int main(void) {
            char response[128], logs[16384];
            ImageAuthResponse auth;
            size_t n;
            capture = tmpfile(); assert(capture != NULL);
            raw_capture = tmpfile(); assert(raw_capture != NULL);
            hello_capture = tmpfile(); assert(hello_capture != NULL);
            http_status = 200;
            for (int enabled = 0; enabled <= 1; enabled++) {
                tls12_enabled = enabled;
                for (int failure = 0; failure <= 1; failure++) {
                    int expected = !(enabled && failure);
                    int before = performs;
                    version_result = failure ? 43 : 0;
                    assert(probe_http("https://local/health", NULL, response,
                                      sizeof(response), false) == (expected ? 200 : 0));
                    assert(performs == before + expected);
                    before = performs;
                    assert(post_json("https://local/session", "{}", &auth,
                                     VP_IMAGE_LOG_SESSION) == expected);
                    assert(performs == before + expected);
                    assert(verify_peer == 1 && verify_host == 2);
                }
            }
            tls12_enabled = 0; version_result = 0;
            assert(probe_http("https://local/health", NULL, response,
                              sizeof(response), false) == 200);
            assert(tls_version == 0);
            vp_vita_log_image_event(VP_IMAGE_LOG_HEALTH_START, 0, 0, false);
            vp_vita_log_image_event(VP_IMAGE_LOG_HEALTH_FAILED, 0, 0, false);
            http_status = 200;
            assert(probe_http("https://private-host/health?secret", "secret-token",
                              response, sizeof(response), false) == 200);
            assert(post_json("https://private-host/v1/challenge", "secret-proof", &auth,
                             VP_IMAGE_LOG_CHALLENGE));
            assert(verify_peer == 1 && verify_host == 2);
            result = 60; http_status = 0;
            error_detail = "secret-host secret-token error:0A000102:SSL routines::unsupported protocol";
            assert(probe_http("https://private-host/health", NULL,
                              response, sizeof(response), false) == 0);
            assert(!post_json("https://private-host/v1/session", "secret-proof", &auth,
                              VP_IMAGE_LOG_SESSION));
            error_detail = "OpenSSL SSL_connect: SSL_ERROR_SYSCALL in connection to secret-host";
            result = 35;
            assert(!post_json("secret-url", "secret-proof", &auth, VP_IMAGE_LOG_SESSION));
            assert(verbose == 1 && debug_callback != NULL);
            {
                char raw[] = "secret-token secret-host";
                char trace[] = "OpenSSL SSL_connect: SSL_ERROR_ZERO_RETURN secret-token";
                long before = ftell(capture);
                assert(tls_debug(NULL, CURLINFO_TEXT, raw, sizeof(raw)-1, NULL) == 0);
                for (int kind = CURLINFO_HEADER_IN; kind <= CURLINFO_SSL_DATA_OUT; kind++)
                    assert(tls_debug(NULL, (curl_infotype)kind, trace, sizeof(trace)-1, NULL) == 0);
                assert(ftell(capture) == before);
                assert(tls_debug(NULL, CURLINFO_TEXT, trace, sizeof(trace)-1, NULL) == 0);
                before = ftell(capture);
                assert(tls_debug(NULL, CURLINFO_TEXT, trace, 0, NULL) == 0);
                assert(tls_debug(NULL, CURLINFO_TEXT, NULL, 123, NULL) == 0);
                assert(ftell(capture) == before);
            }
            trace_setup_result = 48;
            assert(!post_json("secret-url", "secret-proof", &auth, VP_IMAGE_LOG_SESSION));
            assert(verbose == 0 && debug_callback == NULL);
            trace_setup_result = 0;
            tls12_enabled = 1;
            assert(!post_json("secret-url", "secret-proof", &auth, VP_IMAGE_LOG_SESSION));
            assert(tls_version == (CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
            assert(verify_peer == 1 && verify_host == 2);
            version_result = 43;
            {
                int before = performs;
                assert(!post_json("secret-url", "secret-proof", &auth, VP_IMAGE_LOG_SESSION));
                assert(performs == before && tls_version == 0);
            }
            version_result = 0;
            tls12_enabled = 0;
            assert(!post_json("secret-url", "secret-proof", &auth, VP_IMAGE_LOG_SESSION));
            assert(tls_version == 0);
            vp_vita_log_tls_details("secret-token\nerror:0123456789abcdef:secret-proof", 1, 0);
            vp_vita_log_tls_details(NULL, 1, 0);
            versions.version = NULL; versions.ssl_version = NULL;
            vp_vita_log_tls_details("secret-token\nerror:short:secret-proof", 1, 0);
            deny_log = 1;
            vp_vita_log_tls_details("secret-token", 1, 0);
            deny_log = 0;
            error_detail = "Insufficient randomness";
            rng_ready = 0;
            assert(!post_json("secret-url", "secret-proof", &auth, VP_IMAGE_LOG_SESSION));
            rng_ready = 1;
            vp_vita_log_tls_details("TLS connect error: error:00000000:lib(0)::reason(0)", 1, 0);
            result = 0; http_status = 401;
            assert(post_json("secret-url", "secret-proof", &auth, VP_IMAGE_LOG_SESSION));
            init_failed = 1;
            assert(!post_json("secret-url", "secret-proof", &auth, VP_IMAGE_LOG_SESSION));
            assert(probe_http("secret-url", NULL, response, sizeof(response), false) == 0);
            init_failed = 0; headers_failed = 1;
            assert(!post_json("secret-url", "secret-proof", &auth, VP_IMAGE_LOG_SESSION));
            headers_failed = 0; http_status = 200;
            assert(probe_http("secret-url", NULL, response, sizeof(response), true) == 200);
            assert(verbose == 0 && debug_callback == NULL);
            assert(legacy_logs == 1);
            assert(ftell(raw_capture) == 0);
            {
                char text[] = "TLS debug: private-host secret-raw-detail\n";
                char headers[] = "Authorization: Bearer secret-token";
                char raw[1024];
                long before;
                VpTlsTrace raw_option = { true, false };
                raw_enabled = 1;
                assert(tls_debug(NULL, CURLINFO_TEXT, text, sizeof(text)-1, &raw_option) == 0);
                before = ftell(raw_capture);
                assert(before > 0);
                for (int kind = CURLINFO_HEADER_IN; kind <= CURLINFO_SSL_DATA_OUT; kind++)
                    assert(tls_debug(NULL, (curl_infotype)kind, headers, sizeof(headers)-1, &raw_option) == 0);
                assert(ftell(raw_capture) == before);
                raw_enabled = 0;
                raw_option.raw_text = false;
                assert(tls_debug(NULL, CURLINFO_TEXT, headers, sizeof(headers)-1, &raw_option) == 0);
                assert(ftell(raw_capture) == before);
                rewind(raw_capture);
                n = fread(raw, 1, sizeof(raw)-1, raw_capture); raw[n] = 0;
                assert(strstr(raw, "private-host secret-raw-detail") != NULL);
                assert(strstr(raw, "Authorization") == NULL);
                assert(strstr(raw, "secret-token") == NULL);
            }
            {
                unsigned char hello[47] = {1, 0, 0, 43, 3, 3};
                char dump[512];
                VpTlsTrace trace = { false, false };
                long end;
                hello[40] = 2; hello[41] = 0xc0; hello[42] = 0x2b; hello[43] = 1;
                assert(tls_debug(NULL, CURLINFO_SSL_DATA_OUT, (char *)hello, sizeof(hello), &trace) == 0);
                assert(ftell(hello_capture) == 0 && !trace.clienthello_captured);
                trace.raw_text = true;
                for (int kind = CURLINFO_TEXT; kind < CURLINFO_SSL_DATA_OUT; kind++)
                    assert(tls_debug(NULL, (curl_infotype)kind, (char *)hello, sizeof(hello), &trace) == 0);
                assert(ftell(hello_capture) == 0);
                for (size_t size = 0; size < sizeof(hello); size++)
                    assert(!record_tls_clienthello(hello, size));
                assert(!record_tls_clienthello(NULL, sizeof(hello)));
                assert(!record_tls_clienthello(hello, 16385));
                deny_log = 1;
                assert(!record_tls_clienthello(hello, sizeof(hello)));
                deny_log = 0;
                assert(tls_debug(NULL, CURLINFO_SSL_DATA_OUT, (char *)hello, sizeof(hello), &trace) == 0);
                assert(trace.clienthello_captured);
                end = ftell(hello_capture); assert(end > 0);
                assert(tls_debug(NULL, CURLINFO_SSL_DATA_OUT, (char *)hello, sizeof(hello), &trace) == 0);
                assert(ftell(hello_capture) == end);
                rewind(hello_capture);
                n = fread(dump, 1, sizeof(dump)-1, hello_capture); dump[n] = 0;
                assert(strstr(dump, "[tls-clienthello bytes=47]\n01 00 00 2b 03 03") != NULL);
                assert(strstr(dump, "c0 2b 01") != NULL);
                {
                    const size_t offsets[] = {0, 1, 4, 38, 39, 40, 43, 45, 46};
                    const unsigned char values[] = {2, 1, 4, 33, 1, 1, 0, 1, 1};
                    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
                        unsigned char saved = hello[offsets[i]];
                        hello[offsets[i]] = values[i];
                        assert(!record_tls_clienthello(hello, sizeof(hello)));
                        assert(ftell(hello_capture) == end);
                        hello[offsets[i]] = saved;
                    }
                }
                trace = (VpTlsTrace){true, false};
                assert(tls_debug(NULL, CURLINFO_SSL_DATA_OUT, (char *)hello, sizeof(hello), &trace) == 0);
                assert(trace.clienthello_captured && ftell(hello_capture) == end * 2);
            }
            rewind(capture); n = fread(logs, 1, sizeof(logs)-1, capture); logs[n] = 0;
            assert(strstr(logs, "stage=service-http curl=0") != NULL);
            assert(strstr(logs, "stage=health-start\n") != NULL);
            assert(strstr(logs, "stage=health-failed\n") != NULL);
            assert(strstr(logs, "stage=health-failed curl=") == NULL);
            assert(strstr(logs, "stage=challenge curl=0") != NULL);
            assert(strstr(logs, "stage=session curl=60") != NULL);
            assert(strstr(logs, "http=401") != NULL);
            assert(strstr(logs, "SSL peer certificate") != NULL);
            assert(strstr(logs, "curl_version=8.0-test ssl_version=OpenSSL/3.0-test") != NULL);
            assert(strstr(logs, "openssl_error=0a000102 openssl_error_present=1 reason=unsupported protocol") != NULL);
            assert(strstr(logs, "tls_detail=SSL_ERROR_SYSCALL") != NULL);
            assert(strstr(logs, "tls_trace=SSL_ERROR_SYSCALL\n") != NULL);
            assert(strstr(logs, "tls_trace=SSL_ERROR_ZERO_RETURN\n") != NULL);
            assert(strstr(logs, "os_errno=54 os_errno_result=0 trace_result=0") != NULL);
            assert(strstr(logs, "os_errno=54 os_errno_result=0 trace_result=48") != NULL);
            assert(strstr(logs, "tls_policy=tls1.2-only setup_result=0") != NULL);
            assert(strstr(logs, "tls_policy=tls1.2-only setup_result=43") != NULL);
            assert(strstr(logs, "tls_detail=Insufficient randomness") != NULL);
            assert(strstr(logs, "errorbuffer_nonempty=1 errorbuffer_result=0 rng_ready=0") != NULL);
            assert(strstr(logs, "errorbuffer_nonempty=0 errorbuffer_result=0 rng_ready=1") != NULL);
            assert(strstr(logs, "tls_detail=TLS connect error openssl_error=00000000 openssl_error_present=1") != NULL);
            assert(strstr(logs, "openssl_error=00000000 openssl_error_present=0 reason=unavailable") != NULL);
            assert(strstr(logs, "curl_version=unavailable ssl_version=unavailable") != NULL);
            assert(strstr(logs, "secret") == NULL);
            assert(strstr(logs, "private-host") == NULL);
            fclose(capture);
            fclose(raw_capture);
            fclose(hello_capture);
            return 0;
        }
        '''.replace("probe_http(", "run_probe(").replace("post_json(", "run_post(")
        code = code.replace("/*ENUM*/", header[start:end]).replace("/*FUNCTIONS*/", functions)
        code = code.replace("/*PROBE_CALL*/", "probe_http(url, token, response, capacity, diagnostics, &options)")
        code = code.replace("/*POST_CALL*/", "post_json(url, payload, response, stage, raw_enabled != 0, tls12_enabled != 0)")
        self.assertNotIn(".enabled", backend)
        options = sorted(set(re.findall(r"CURLOPT_[A-Z0-9_]+", functions)))
        code = code.replace("/*OPTIONS*/", "enum { " + ", ".join(options) + " };")
        with tempfile.TemporaryDirectory(prefix="vitashelf-log-test-") as directory:
            source = Path(directory) / "probe.c"
            binary = Path(directory) / "probe"
            source.write_text(code)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-pedantic", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
