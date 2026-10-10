// 화면 없이 채널 하나를 지정한 시간 동안 받아 보며 상태 변화를 로그로 남긴다.
// 장애 재현 테스트(tests/failure_tests.ps1)가 이 로그로 감지·복구 시간을 잰다.
// 사용: probe.exe <이름> <rtsp 주소> <초>
#include <cstdio>
#include <string>
#include <thread>

#include "../src/log.h"
#include "../src/stream_worker.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: probe <name> <rtsp-url> <seconds>\n");
        return 2;
    }
    const std::string name = argv[1];
    const int seconds = std::stoi(argv[3]);

    av_log_set_level(AV_LOG_QUIET);
    avformat_network_init();

    StreamWorker w(name, argv[2]);
    w.Start();

    ChannelState lastState = ChannelState::Stopped;
    double maxFps = 0.0;
    const int64_t end = NowMs() + seconds * 1000LL;
    while (NowMs() < end) {
        const ChannelSnapshot s = w.Snapshot();
        if (s.state != lastState) {
            Log("probe", std::string("state ") + ToString(s.state));
            lastState = s.state;
        }
        if (s.decodeFps > maxFps) maxFps = s.decodeFps;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const ChannelSnapshot s = w.Snapshot();
    w.Stop();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "SUMMARY state=%s reconnects=%d decode_fps_max=%.1f", ToString(lastState),
                  s.reconnects, maxFps);
    Log("probe", buf);
    avformat_network_deinit();
    return 0;
}
