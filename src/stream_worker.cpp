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

ChannelSnapshot StreamWorker::Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    ChannelSnapshot s = snap_;  // 화면 픽셀은 shared_ptr 참조만 복사된다
    // FPS는 프레임이 올 때만 갱신되므로, 1초 넘게 프레임이 없으면 이전 값 대신 0을 보여 준다.
    if (s.lastFrameMs != 0 && NowMs() - s.lastFrameMs > 1000) s.decodeFps = 0.0;
    return s;
}

bool StreamWorker::ShouldAbortIo() const {
    if (stop_) return true;
    const int64_t deadline = deadlineMs_.load();
    return deadline != 0 && NowMs() > deadline;
}

void StreamWorker::SetState(ChannelState s) {
    std::lock_guard<std::mutex> lock(mu_);
    snap_.state = s;
}

void StreamWorker::SetError(const std::string& e) {
    std::lock_guard<std::mutex> lock(mu_);
    snap_.lastError = e;
}

std::string StreamWorker::LastError() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_.lastError;
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
            snap_.decodeFps = 0.0;
        }
        Log(name_, "retry #" + std::to_string(backoff.Attempt()) + " in " + std::to_string(wait.count()) +
                       " ms (" + LastError() + ")");

        // 대기 중에도 종료 요청에는 바로 반응하도록 잘게 나눠서 잔다.
        const int64_t until = NowMs() + wait.count();
        while (!stop_ && NowMs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    SetState(ChannelState::Stopped);
    Log(name_, "stopped");
}

bool StreamWorker::SessionOnce() {
    Session s;
    if (!s.pkt || !s.frame) {
        SetError("out of memory");
        return false;
    }
    bool gotFrames = false;

    s.fmt = avformat_alloc_context();
    if (!s.fmt) {
        SetError("out of memory");
        return false;
    }
    s.fmt->interrupt_callback.callback = InterruptCallback;
    s.fmt->interrupt_callback.opaque = this;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);  // 패킷 손실로 화면이 깨지지 않게 TCP 사용
    av_dict_set(&opts, "fflags", "nobuffer", 0);     // 입력 단계 버퍼링 최소화

    // 접속부터 스트림 정보 확인까지 한 번의 제한 시간 안에 끝나야 한다.
    const int64_t tStart = NowMs();
    deadlineMs_ = tStart + kConnectTimeoutMs;
    int err = avformat_open_input(&s.fmt, url_.c_str(), nullptr, &opts);
    if (err < 0) {
        av_dict_free(&opts);
        SetError("open: " + AvError(err));
        return false;
    }
    // 접속에 성공했는데 dict에 남은 옵션은 FFmpeg가 적용하지 않은 것이다. 조용히 무시되지 않도록 드러낸다.
    for (const AVDictionaryEntry* e = nullptr; (e = av_dict_iterate(opts, e));)
        Log(name_, std::string("unused option ") + e->key + "=" + e->value);
    av_dict_free(&opts);
    const int64_t tOpened = NowMs();

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
    if (!s.dec) {
        SetError("out of memory");
        return false;
    }
    if ((err = avcodec_parameters_to_context(s.dec, s.fmt->streams[videoIndex]->codecpar)) < 0) {
        SetError("decoder params: " + AvError(err));
        return false;
    }
    s.dec->thread_count = 2;
    s.dec->flags |= AV_CODEC_FLAG_LOW_DELAY;  // 디코더 출력 지연 최소화 (디코더 컨텍스트에 직접 설정)
    if ((err = avcodec_open2(s.dec, codec, nullptr)) < 0) {
        SetError("decoder: " + AvError(err));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.codec = codec->name;
    }

    const int64_t tInfo = NowMs();
    int frameCount = 0;
    int64_t fpsWindowStart = NowMs();
    // 영상 프레임 기준 마감: 오디오 등 다른 패킷이 계속 와도 영상이 멈추면 끊긴 것으로 본다.
    int64_t lastVideoMs = NowMs();
    int otherPackets = 0;  // 마지막 영상 프레임 이후 받은 영상 외 패킷 수 (오디오 등)
    bool failed = false;

    // 디코더에서 나온 프레임을 전부 꺼내 BGRA로 바꿔 최신 화면으로 올린다.
    auto drain = [&] {
        while (!failed && avcodec_receive_frame(s.dec, s.frame) == 0) {
            const int w = s.frame->width;
            const int h = s.frame->height;
            s.sws = sws_getCachedContext(s.sws, w, h, static_cast<AVPixelFormat>(s.frame->format), w, h,
                                         AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!s.sws) {
                SetError("scaler: unsupported format");
                failed = true;
                break;
            }
            auto out = std::make_shared<Frame>();
            out->width = w;
            out->height = h;
            out->bgra.resize(static_cast<size_t>(w) * h * 4);
            uint8_t* dst[4] = {out->bgra.data(), nullptr, nullptr, nullptr};
            int dstStride[4] = {w * 4, 0, 0, 0};
            sws_scale(s.sws, s.frame->data, s.frame->linesize, 0, h, dst, dstStride);

            ++frameCount;
            const int64_t now = NowMs();
            lastVideoMs = now;
            otherPackets = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (!gotFrames) {
                    // 이 세션의 첫 프레임: 끊겼다가 돌아온 것이면 재연결 횟수를 센다.
                    if (snap_.lastFrameMs != 0) ++snap_.reconnects;
                    snap_.state = ChannelState::Live;
                    snap_.attempt = 0;
                }
                snap_.frame = std::move(out);  // 포인터 교체만 잠금 안에서 한다
                snap_.lastFrameMs = now;
                if (now - fpsWindowStart >= 1000) {
                    snap_.decodeFps = frameCount * 1000.0 / (now - fpsWindowStart);
                    frameCount = 0;
                    fpsWindowStart = now;
                }
            }
            if (!gotFrames) {
                gotFrames = true;
                // 접속부터 첫 화면까지 구간별 소요 시간 (복구 시간 분해용)
                Log(name_, std::string("LIVE ") + std::to_string(w) + "x" + std::to_string(h) + " " + codec->name +
                               " (open " + std::to_string(tOpened - tStart) + " ms, stream info " +
                               std::to_string(tInfo - tOpened) + " ms, first frame " + std::to_string(now - tInfo) +
                               " ms)");
            }
        }
    };

    while (!stop_ && !failed) {
        deadlineMs_ = lastVideoMs + kVideoStallMs;
        err = av_read_frame(s.fmt, s.pkt);
        const bool videoStalled = NowMs() > lastVideoMs + kVideoStallMs;
        if (err < 0 || videoStalled) {
            av_packet_unref(s.pkt);
            if (videoStalled)
                SetError("stalled: no video frame for " + std::to_string(NowMs() - lastVideoMs) + " ms (" +
                         std::to_string(otherPackets) + " other packets meanwhile)");
            else
                SetError("read: " + AvError(err));
            break;
        }
        if (s.pkt->stream_index != videoIndex) {
            ++otherPackets;
            av_packet_unref(s.pkt);
            continue;
        }

        // 디코더 입력이 가득 차면(EAGAIN) 출력을 먼저 꺼낸 뒤 같은 패킷을 다시 넣는다.
        err = avcodec_send_packet(s.dec, s.pkt);
        while (err == AVERROR(EAGAIN) && !failed) {
            drain();
            err = avcodec_send_packet(s.dec, s.pkt);
        }
        av_packet_unref(s.pkt);
        if (err < 0) continue;  // 손상된 패킷 하나는 건너뛰고 계속 받는다 (영상 정지 마감은 그대로 흐른다)
        drain();
    }

    if (stop_) SetError("stopped");
    if (gotFrames) Log(name_, "lost: " + LastError());
    return gotFrames;
}
