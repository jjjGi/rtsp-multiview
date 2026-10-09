// 재연결 대기 시간 규칙 확인: 0.5 → 1 → 2 → 4 → 8 → 8(상한), 영상 수신 후 초기화.
#include <cstdio>

#include "../src/backoff.h"

static int g_failed = 0;

static void Expect(long long actual, long long expected, const char* what) {
    if (actual != expected) {
        std::printf("FAIL %s: expected %lld, got %lld\n", what, expected, actual);
        ++g_failed;
    }
}

int main() {
    Backoff b;
    const long long expected[] = {500, 1000, 2000, 4000, 8000, 8000, 8000};
    for (long long e : expected) Expect(b.Next().count(), e, "growing delay");
    Expect(b.Attempt(), 7, "attempt count");

    b.Reset();
    Expect(b.Attempt(), 0, "reset attempt");
    Expect(b.Next().count(), 500, "delay after reset");

    // 아주 많이 실패해도 넘치지 않고 상한을 지킨다
    Backoff many;
    for (int i = 0; i < 100; ++i) many.Next();
    Expect(many.Next().count(), 8000, "cap after many failures");

    if (g_failed == 0) std::printf("backoff_test: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
