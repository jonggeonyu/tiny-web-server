/* net 단위 테스트 (F1, F12 일부) */
#include <unistd.h>

#include "net.h"
#include "test.h"

static void test_f12_valid_port_range(void)
{
    CHECK(net_is_valid_port("1"));
    CHECK(net_is_valid_port("8080"));
    CHECK(net_is_valid_port("65535"));
    CHECK(!net_is_valid_port("0"));
    CHECK(!net_is_valid_port("65536"));
    CHECK(!net_is_valid_port("-1"));
    CHECK(!net_is_valid_port("80a"));
    CHECK(!net_is_valid_port(""));
    CHECK(!net_is_valid_port(NULL));
}

static void test_f1_same_port_twice_fails(void)
{
    int first = net_open_listenfd("18101");
    CHECK(first >= 0);
    fprintf(stderr, "  (아래 bind 에러 메시지는 의도한 것)\n");
    int second = net_open_listenfd("18101");
    CHECK(second < 0);
    close(first);
}

static void test_f1_reopen_after_close_succeeds(void)
{
    int first = net_open_listenfd("18102");
    CHECK(first >= 0);
    close(first);
    int second = net_open_listenfd("18102");
    CHECK(second >= 0);
    close(second);
}

int main(void)
{
    RUN(test_f12_valid_port_range);
    RUN(test_f1_same_port_twice_fails);
    RUN(test_f1_reopen_after_close_succeeds);
    printf("net: %d checks, %d failed\n", test_checks, test_failures);
    return TEST_EXIT_CODE();
}
