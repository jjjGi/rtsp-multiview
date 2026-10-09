#pragma once
#include <algorithm>
#include <chrono>

// 재연결 대기 시간: 0.5s → 1s → 2s → 4s → 8s(상한).
// 카메라가 꺼져 있을 때 서버를 두드리지 않으면서, 복구되면 빠르게 다시 붙기 위한 값.
class Backoff {
public:
    using ms = std::chrono::milliseconds;

    Backoff(ms base = ms(500), ms cap = ms(8000)) : base_(base), cap_(cap) {}

    // 다음 시도까지 기다릴 시간. 호출할 때마다 시도 횟수가 1 늘어난다.
    ms Next() {
        const int shift = std::min(attempt_, 16);
        const long long delay = base_.count() * (1LL << shift);
        ++attempt_;
        return ms(std::min<long long>(delay, cap_.count()));
    }

    // 프레임을 정상적으로 받기 시작하면 처음부터 다시 센다.
    void Reset() { attempt_ = 0; }

    int Attempt() const { return attempt_; }

private:
    ms base_;
    ms cap_;
    int attempt_ = 0;
};
