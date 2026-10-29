#include <ns-harness.h>
#include <ns-util.h>
#include <stdlib.h>

static void test_parse_int(void) {
    long v = 0;
    CHECK(ns_parse_int("0", &v) && v == 0L);
    CHECK(ns_parse_int("42", &v) && v == 42L);
    CHECK(ns_parse_int("-23", &v) && v == -23L);
    CHECK(ns_parse_int("21436138964", &v) && v == 21436138964L);
    CHECK(!ns_parse_int("", &v));
    CHECK(!ns_parse_int(NULL, &v));
    CHECK(!ns_parse_int("abc", &v));
    CHECK(!ns_parse_int("13x", &v));
}

static void test_parse_float(void) {
    float v = 0.0f;
    CHECK(ns_parse_float("0", &v) && feq(v, 0.0f));
    CHECK(ns_parse_float("1.5", &v) && feq(v, 1.5f));
    CHECK(ns_parse_float("-0.25", &v) && feq(v, -0.25f));
    CHECK(!ns_parse_float("", &v));
    CHECK(!ns_parse_float(NULL, &v));
    CHECK(!ns_parse_float("abc", &v));
    CHECK(!ns_parse_float("1.5x", &v));
}

static void test_trim(void) {
    char a[] = "  zoot  ";
    char b[] = "foobar";
    char c[] = " foo@*(&#ba r  ";
    char d[] = "";
    char e[] = "   ";

    CHECK_EQ_STR(ns_trim(a), "zoot");
    CHECK_EQ_STR(ns_trim(b), "foobar");
    CHECK_EQ_STR(ns_trim(c), "foo@*(&#ba r");
    CHECK(*ns_trim(d) == '\0');
    CHECK(*ns_trim(e) == '\0');
}

int main(void) {
    test_parse_int();
    test_parse_float();
    test_trim();

    return test_summary();
}
