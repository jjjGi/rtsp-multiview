#pragma once
#include <string>

// 상태 변화(연결, 끊김, 재연결)를 시각과 함께 콘솔과 logs/multiview.log에 남긴다.
void LogOpen(const std::string& path);
void Log(const std::string& channel, const std::string& message);
