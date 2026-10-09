#include "stream_worker.h"

#include <chrono>

#include "backoff.h"
#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {
constexpr int kOpenTimeoutMs = 5000;   // 접속과 스트림 정보 확인에 허용하는 시간
constexpr int kStallTimeoutMs = 3000;  // 이 시간 동안 패킷이 하나도 안 오면 끊긴 것으로 본다

std::string AvError(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

int InterruptCallback(void* opaque) {
    return static_cast<StreamWorker*>(opaque)->ShouldAbortIo() ? 1 : 0;
}

// 세션 하나에서 쓰는 FFmpeg 자원. 스코프를 벗어나면 전부 해제된다.
struct Session {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    ~Session() {
        sws_freeContext(sws);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }
};
}  // namespace

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* ToString(ChannelState s) {
    switch (s) {
        case ChannelState::Connecting: return "CONNECTING";
        case ChannelState::Live: return "LIVE";
        case ChannelState::Reconnecting: return "RECONNECTING";
        case ChannelState::Stopped: return "STOPPED";
    }
    return "?";
}

StreamWorker::StreamWorker(std::string name, std::string url)
    : name_(std::move(name)), url_(std::move(url)) {}

StreamWorker::~StreamWorker() { Stop(); }

void StreamWorker::Start() {
    stop_ = false;
    thread_ = std::thread(&StreamWorker::Run, this);
}

void StreamWorker::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

ChannelSnapshot StreamWorker::Snapshot(bool withPixels) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (withPixels) return snap_;
    ChannelSnapshot s = snap_;
    s.bgra.clear();
    return s;
}

bool StreamWorker::ShouldAbortIo() const {
    if (stop_) return true;
    const int64_t deadline = deadlineMs_.load();
    return deadline != 0 && NowMs() > deadline;
}

void StreamWorker::ArmDeadline(int timeoutMs) { deadlineMs_ = NowMs() + timeoutMs; }

void StreamWorker::SetState(ChannelState s) {
    std::lock_guard<std::mutex> lock(mu_);
    snap_.state = s;
}

void StreamWorker::SetError(const std::string& e) {
    std::lock_guard<std::mutex> lock(mu_);
    snap_.lastError = e;
}

void StreamWorker::Run() {
    Backoff backoff;

    while (!stop_) {
        SetState(ChannelState::Connecting);
        Log(name_, "connecting " + url_);

        const bool gotFrames = SessionOnce();
        if (stop_) break;

        // 영상을 받다가 끊긴 경우: 대기 시간을 처음부터 다시 센다.
        if (gotFrames) backoff.Reset();

        const auto wait = backoff.Next();
        {
            std::lock_guard<std::mutex> lock(mu_);
            snap_.state = ChannelState::Reconnecting;
            snap_.attempt = backoff.Attempt();
            snap_.retryAtMs = NowMs() + wait.count();
            snap_.fps = 0.0;
        }
        Log(name_, "retry #" + std::to_string(backoff.Attempt()) + " in " + std::to_string(wait.count()) +
                       " ms (" + Snapshot(false).lastError + ")");

        // 대기 중에도 종료 요청에는 바로 반응하도록 잘게 나눠서 잔다.
        const int64_t until = NowMs() + wait.count();
        while (!stop_ && NowMs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    SetState(ChannelState::Stopped);
    Log(name_, "stopped");
}

bool StreamWorker::SessionOnce() {
    Session s;
    bool gotFrames = false;

    s.fmt = avformat_alloc_context();
    s.fmt->interrupt_callback.callback = InterruptCallback;
    s.fmt->interrupt_callback.opaque = this;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);  // 패킷 손실로 화면이 깨지지 않게 TCP 사용
    av_dict_set(&opts, "fflags", "nobuffer", 0);     // 지연을 줄이기 위해 버퍼링 최소화
    av_dict_set(&opts, "flags", "low_delay", 0);

    ArmDeadline(kOpenTimeoutMs);
    int err = avformat_open_input(&s.fmt, url_.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        SetError("open: " + AvError(err));
        return false;
    }

    ArmDeadline(kOpenTimeoutMs);
    if ((err = avformat_find_stream_info(s.fmt, nullptr)) < 0) {
        SetError("stream info: " + AvError(err));
        return false;
    }

    const AVCodec* codec = nullptr;
    const int videoIndex = av_find_best_stream(s.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (videoIndex < 0 || !codec) {
        SetError("no video stream");
        return false;
    }

    s.dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(s.dec, s.fmt->streams[videoIndex]->codecpar);
    s.dec->thread_count = 2;
    if ((err = avcodec_open2(s.dec, codec, nullptr)) < 0) {
        SetError("decoder: " + AvError(err));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.codec = codec->name;
    }

    std::vector<uint8_t> buffer;
    int frameCount = 0;
    int64_t fpsWindowStart = NowMs();

    while (!stop_) {
        // 패킷 하나를 기다리는 시간에 상한을 둔다. 카메라가 응답 없이 멈추면 여기서 빠져나온다.
        ArmDeadline(kStallTimeoutMs);
        err = av_read_frame(s.fmt, s.pkt);
        if (err < 0) {
            SetError(err == AVERROR_EXIT ? "stalled: no packet for " + std::to_string(kStallTimeoutMs) + " ms"
                                         : "read: " + AvError(err));
            break;
        }
        if (s.pkt->stream_index != videoIndex) {
            av_packet_unref(s.pkt);
            continue;
        }

        err = avcodec_send_packet(s.dec, s.pkt);
        av_packet_unref(s.pkt);
        if (err < 0 && err != AVERROR(EAGAIN)) continue;  // 손상된 패킷 하나는 건너뛰고 계속 받는다

        while (avcodec_receive_frame(s.dec, s.frame) == 0) {
            const int w = s.frame->width;
            const int h = s.frame->height;
            s.sws = sws_getCachedContext(s.sws, w, h, static_cast<AVPixelFormat>(s.frame->format), w, h,
                                         AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            buffer.resize(static_cast<size_t>(w) * h * 4);
            uint8_t* dst[4] = {buffer.data(), nullptr, nullptr, nullptr};
            int dstStride[4] = {w * 4, 0, 0, 0};
            sws_scale(s.sws, s.frame->data, s.frame->linesize, 0, h, dst, dstStride);

            ++frameCount;
            const int64_t now = NowMs();
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (!gotFrames) {
                    // 이 세션의 첫 프레임: 끊겼다가 돌아온 것이면 재연결 횟수를 센다.
                    if (snap_.state == ChannelState::Connecting && snap_.lastFrameMs != 0) ++snap_.reconnects;
                    snap_.state = ChannelState::Live;
                    snap_.attempt = 0;
                }
                snap_.width = w;
                snap_.height = h;
                snap_.bgra.swap(buffer);
                snap_.lastFrameMs = now;
                if (now - fpsWindowStart >= 1000) {
                    snap_.fps = frameCount * 1000.0 / (now - fpsWindowStart);
                    frameCount = 0;
                    fpsWindowStart = now;
                }
            }
            if (!gotFrames) {
                gotFrames = true;
                Log(name_, std::string("LIVE ") + std::to_string(w) + "x" + std::to_string(h) + " " + codec->name);
            }
        }
    }

    if (gotFrames) Log(name_, "lost: " + Snapshot(false).lastError);
    return gotFrames;
}
