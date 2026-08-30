#include <fstapi.h>
#include <glib.h>
#include <string.h>

// Round trip every possible byte value through the escaping helpers.

static void test_round_trip(void)
{
    unsigned char raw[256];
    for (int i = 0; i < 256; i++) {
        raw[i] = (unsigned char)i;
    }

    int escaped_len = fstUtilityBinToEscConvertedLen(raw, sizeof(raw));
    g_assert_cmpint(escaped_len, >, 0);

    unsigned char *escaped = g_malloc(escaped_len + 1);
    int written = fstUtilityBinToEsc(escaped, raw, sizeof(raw));
    g_assert_cmpint(written, ==, escaped_len);

    // The escaped form must not contain white space, that is what makes it safe
    // to store in the space separated enum table attribute.

    for (int i = 0; i < written; i++) {
        g_assert_true(escaped[i] > ' ');
        g_assert_true(escaped[i] <= '~');
    }

    unsigned char decoded[256];
    int decoded_len = fstUtilityEscToBin(decoded, escaped, written);
    g_assert_cmpint(decoded_len, ==, sizeof(raw));
    g_assert_cmpint(memcmp(decoded, raw, sizeof(raw)), ==, 0);

    g_free(escaped);
}

// Passing a NULL destination decodes in place.

static void test_decode_in_place(void)
{
    unsigned char buffer[] = "a\\tb\\\\c\\101";

    int len = fstUtilityEscToBin(NULL, buffer, strlen((char *)buffer));

    g_assert_cmpint(len, ==, 6);
    g_assert_cmpint(memcmp(buffer, "a\tb\\cA", 6), ==, 0);
}

// Escapes that fstUtilityBinToEsc() never emits but the decoder still accepts.

static void test_decode_extra_escapes(void)
{
    unsigned char hex[] = "\\x41\\x6a\\xFF";
    int len = fstUtilityEscToBin(NULL, hex, strlen((char *)hex));
    g_assert_cmpint(len, ==, 3);
    g_assert_cmpint(memcmp(hex, "Aj\xFF", 3), ==, 0);

    // An unknown escape decodes to the escaped character itself.

    unsigned char unknown[] = "\\z\\9";
    len = fstUtilityEscToBin(NULL, unknown, strlen((char *)unknown));
    g_assert_cmpint(len, ==, 2);
    g_assert_cmpint(memcmp(unknown, "z9", 2), ==, 0);
}

// Each C escape has a dedicated branch in both directions.

static void test_named_escapes(void)
{
    const char *raw = "\a\b\f\n\r\t\v'\"\\?";
    int raw_len = strlen(raw);

    g_assert_cmpint(fstUtilityBinToEscConvertedLen((const unsigned char *)raw, raw_len),
                    ==,
                    2 * raw_len);

    unsigned char escaped[64];
    int written = fstUtilityBinToEsc(escaped, (const unsigned char *)raw, raw_len);
    g_assert_cmpint(written, ==, 2 * raw_len);
    g_assert_cmpint(memcmp(escaped, "\\a\\b\\f\\n\\r\\t\\v\\'\\\"\\\\\\?", written), ==, 0);

    unsigned char decoded[64];
    int decoded_len = fstUtilityEscToBin(decoded, escaped, written);
    g_assert_cmpint(decoded_len, ==, raw_len);
    g_assert_cmpint(memcmp(decoded, raw, raw_len), ==, 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_round_trip();
    test_decode_in_place();
    test_decode_extra_escapes();
    test_named_escapes();

    return 0;
}
