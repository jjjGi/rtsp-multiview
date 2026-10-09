#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class ChannelState { Connecting, Live, Reconnecting, Stopped };

const char* ToString(ChannelState s);

// UI 스레드가 읽어 가는 화면 한 장과 채널 상태.
struct ChannelSnapshot {
    ChannelState state = ChannelState::Connecting;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;     // 마지막으로 디코딩된 프레임 (끊겨도 마지막 화면은 남겨 둔다)
    double fps = 0.0;
    std::string codec;
    int reconnects = 0;            // 끊겼다가 다시 붙은 횟수
    int attempt = 0;               // 현재 재연결 시도 번호
    int64_t retryAtMs = 0;         // 다음 재연결 시도 시각 (steady clock ms)
    int64_t lastFrameMs = 0;       // 마지막 프레임 수신 시각
    std::string lastError;
};

// 채널 하나 = 스레드 하나. 연결 → 수신 → 디코딩 → 변환을 반복하고,
// 끊기면 스스로 다시 연결한다. UI는 Snapshot()으로 최신 상태만 가져간다.
class StreamWorker {
public:
    StreamWorker(std::string name, std::string url);
    ~StreamWorker();

    StreamWorker(const StreamWorker&) = delete;
    StreamWorker& operator=(const StreamWorker&) = delete;

    void Start();
    void Stop();

    const std::string& Name() const { return name_; }
    const std::string& Url() const { return url_; }

    // 프레임 픽셀 복사 없이 상태만 필요할 때는 withPixels=false.
    ChannelSnapshot Snapshot(bool withPixels) const;

    // FFmpeg 블로킹 호출(접속, 패킷 읽기)을 끊어 내기 위한 마감 시각 검사.
    bool ShouldAbortIo() const;

private:
    void Run();
    // 한 번 연결해서 끊길 때까지 받는다. 정상 종료면 true.
    bool SessionOnce();
    void SetState(ChannelState s);
    void SetError(const std::string& e);
    void ArmDeadline(int timeoutMs);

    std::string name_;
    std::string url_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> deadlineMs_{0};

    mutable std::mutex mu_;
    ChannelSnapshot snap_;
};

int64_t NowMs();
