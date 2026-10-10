#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class ChannelState { Connecting, Live, Reconnecting, Stopped };

const char* ToString(ChannelState s);

// 디코딩이 끝난 BGRA 화면 한 장. 만든 뒤에는 바꾸지 않으므로 여러 스레드가 그대로 공유한다.
struct Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;
};

// UI 스레드가 읽어 가는 채널 상태와 최신 화면.
struct ChannelSnapshot {
    ChannelState state = ChannelState::Connecting;
    std::shared_ptr<const Frame> frame;  // 마지막으로 디코딩된 화면 (끊겨도 마지막 화면은 남겨 둔다)
    double decodeFps = 0.0;              // 디코더가 내놓은 프레임 수/초 (화면 표시 FPS와는 다름)
    std::string codec;
    int reconnects = 0;                  // 끊겼다가 다시 붙은 횟수
    int attempt = 0;                     // 현재 재연결 시도 번호
    int64_t retryAtMs = 0;               // 다음 재연결 시도 시각 (steady clock ms)
    int64_t lastFrameMs = 0;             // 마지막 영상 프레임 디코딩 시각
    std::string lastError;
};

// 채널 하나 = 스레드 하나. 연결 → 수신 → 디코딩 → 변환을 반복하고,
// 끊기면 스스로 다시 연결한다. UI는 Snapshot()으로 최신 상태만 가져간다.
class StreamWorker {
public:
    static constexpr int kConnectTimeoutMs = 5000;  // 접속 + 스트림 정보 확인까지 합친 제한
    static constexpr int kVideoStallMs = 3000;      // 이 시간 동안 영상 프레임이 안 나오면 끊긴 것으로 본다

    StreamWorker(std::string name, std::string url);
    ~StreamWorker();

    StreamWorker(const StreamWorker&) = delete;
    StreamWorker& operator=(const StreamWorker&) = delete;

    void Start();
    void Stop();

    const std::string& Name() const { return name_; }
    const std::string& Url() const { return url_; }

    // 화면은 공유 포인터만 복사하므로 잠금 구간이 짧다.
    ChannelSnapshot Snapshot() const;

    // FFmpeg 블로킹 호출(접속, 패킷 읽기)을 끊어 내기 위한 마감 시각 검사.
    bool ShouldAbortIo() const;

private:
    void Run();
    // 한 번 연결해서 끊길 때까지 받는다. 영상 프레임을 하나라도 받았으면 true.
    bool SessionOnce();
    void SetState(ChannelState s);
    void SetError(const std::string& e);
    std::string LastError() const;

    std::string name_;
    std::string url_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> deadlineMs_{0};

    mutable std::mutex mu_;
    ChannelSnapshot snap_;
};

int64_t NowMs();
