#include "image_service_auth.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *OFFICIAL_INVALID = "https://images.example.invalid/image";
static const char *OFFICIAL_PUBLIC = "https://images.example.com/image";

static void test_origins(void) {
    VpImageOrigin left;
    VpImageOrigin right;
    char base[96];
    char long_host[VP_IMAGE_AUTH_ORIGIN_MAX + 16];

    assert(vp_image_auth_origin_parse(OFFICIAL_INVALID, &left));
    assert(strcmp(left.scheme, "https") == 0);
    assert(strcmp(left.host, "images.example.invalid") == 0);
    assert(left.port == 0);

    assert(vp_image_auth_origin_parse("HTTPS://Images.Example.INVALID:443/other?x=1", &right));
    assert(vp_image_auth_origin_equal(&left, &right));
    assert(vp_image_auth_origins_match("HTTPS://Images.Example.INVALID:443/image?x=1",
                                       OFFICIAL_INVALID));
    assert(vp_image_auth_origins_match("https://images.example.invalid/image/", OFFICIAL_INVALID));
    assert(!vp_image_auth_origins_match("http://images.example.invalid/image", OFFICIAL_INVALID));
    assert(!vp_image_auth_origins_match("https://images.example.invalid:8443/image",
                                        OFFICIAL_INVALID));
    assert(!vp_image_auth_origins_match("https://192.168.1.10:8090/image", OFFICIAL_INVALID));
    assert(!vp_image_auth_origin_parse("file:///tmp/image", &left));
    assert(!vp_image_auth_origin_parse("https://user:secret@images.example.invalid/image", &left));
    assert(!vp_image_auth_origin_parse("https://images.example.invalid:notaport/image", &left));
    assert(!vp_image_auth_origin_parse("https://images.example.invalid:70000/image", &left));
    memset(long_host, 'a', sizeof(long_host) - 1);
    long_host[sizeof(long_host) - 1] = '\0';
    assert(!vp_image_auth_origin_parse(long_host, &left));

    assert(vp_image_auth_origin_base("HTTPS://Images.Example.INVALID:443/image?x=1",
                                     base, sizeof(base)));
    assert(strcmp(base, "https://images.example.invalid") == 0);
    assert(vp_image_auth_origin_base("http://192.168.1.10:8090/image", base, sizeof(base)));
    assert(strcmp(base, "http://192.168.1.10:8090") == 0);
    assert(!vp_image_auth_origin_base("file:///tmp/image", base, sizeof(base)));
}

static void test_mode_selection(void) {
    assert(vp_image_auth_active_for("https://IMAGES.EXAMPLE.COM:443/image",
                                    OFFICIAL_PUBLIC, "key-id", "secret"));
    assert(vp_image_auth_active_for(OFFICIAL_PUBLIC, OFFICIAL_PUBLIC, "key-id", "secret"));
    assert(!vp_image_auth_active_for("https://images.example.com:8443/image",
                                     OFFICIAL_PUBLIC, "key-id", "secret"));
    assert(!vp_image_auth_active_for("http://192.168.1.10:8090/image",
                                     OFFICIAL_PUBLIC, "key-id", "secret"));
    assert(!vp_image_auth_active_for("http://images.example.test/image",
                                     "http://images.example.test/image", "key-id", "secret"));
    assert(!vp_image_auth_active_for(OFFICIAL_PUBLIC, OFFICIAL_PUBLIC, "key-id", ""));
    assert(!vp_image_auth_active_for(OFFICIAL_PUBLIC, OFFICIAL_PUBLIC, "", "secret"));
    assert(!vp_image_auth_active_for(OFFICIAL_INVALID, OFFICIAL_INVALID, "key-id", "secret"));
    assert(!vp_image_auth_active_for(OFFICIAL_PUBLIC, "https://images.example.invalid/image",
                                     "key-id", "secret"));

    assert(strcmp(vp_image_auth_official_url(), OFFICIAL_INVALID) == 0);
    assert(vp_image_auth_key()[0] == '\0');
    assert(!vp_image_auth_active(OFFICIAL_INVALID));
}

static void test_message(void) {
    char message[VP_IMAGE_AUTH_MESSAGE_MAX];
    char maximum[VP_IMAGE_AUTH_FIELD_MAX + 1];
    char too_long[VP_IMAGE_AUTH_FIELD_MAX + 2];
    size_t index;

    assert(vp_image_auth_build_message("kid", "install", "challenge", message, sizeof(message)));
    assert(strcmp(message, "vitashelf-image-auth-v1\nkid\ninstall\nchallenge\n") == 0);

    memset(maximum, 'a', VP_IMAGE_AUTH_FIELD_MAX);
    maximum[VP_IMAGE_AUTH_FIELD_MAX] = '\0';
    memset(too_long, 'a', VP_IMAGE_AUTH_FIELD_MAX + 1);
    too_long[VP_IMAGE_AUTH_FIELD_MAX + 1] = '\0';
    assert(vp_image_auth_build_message(maximum, maximum, maximum, message, sizeof(message)));
    assert(strlen(message) == 411U);
    assert(!vp_image_auth_build_message(maximum, maximum, maximum, message, 411U));

    {
        const char *invalid[] = {"has space", "has\nnewline", "has/slash", "has+plus",
                                 "has=pad", "has\x01control", "", NULL};
        for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
            assert(!vp_image_auth_build_message(invalid[index], "install", "challenge",
                                                message, sizeof(message)));
            assert(!vp_image_auth_build_message("kid", invalid[index], "challenge",
                                                message, sizeof(message)));
            assert(!vp_image_auth_build_message("kid", "install", invalid[index],
                                                message, sizeof(message)));
        }
    }
    assert(vp_image_auth_valid_field("A-Z.a-z_0-9"));
    assert(!vp_image_auth_valid_field(too_long));
}

static void test_install_id_format(void) {
    unsigned char bytes[VP_IMAGE_INSTALL_ID_BYTES];
    char id[VP_IMAGE_INSTALL_ID_CHARS + 1];
    size_t index;

    for (index = 0; index < sizeof(bytes); index++) bytes[index] = (unsigned char)index;
    assert(vp_image_auth_format_install_id(bytes, id, sizeof(id)));
    assert(strcmp(id, "000102030405060708090a0b0c0d0e0f") == 0);
    assert(vp_image_auth_valid_install_id(id));
    assert(!vp_image_auth_valid_install_id("000102030405060708090a0b0c0d0e0F"));
    assert(!vp_image_auth_valid_install_id("000102030405060708090a0b0c0d0e0f0"));
    assert(!vp_image_auth_valid_install_id("g000102030405060708090a0b0c0d0e0e"));
    assert(!vp_image_auth_format_install_id(bytes, id, VP_IMAGE_INSTALL_ID_CHARS));
}

int main(void) {
    test_origins();
    test_mode_selection();
    test_message();
    test_install_id_format();
    puts("Image-service auth helpers: PASS");
    return 0;
}
