#include "log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace {
std::mutex g_mu;
std::ofstream g_file;

std::string Timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return buf;
}
}  // namespace

void LogOpen(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_file.open(path, std::ios::app);
}

void Log(const std::string& channel, const std::string& message) {
    const std::string line = Timestamp() + "  [" + channel + "]  " + message;
    std::lock_guard<std::mutex> lock(g_mu);
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    if (g_file.is_open()) {
        g_file << line << '\n';
        g_file.flush();
    }
}
