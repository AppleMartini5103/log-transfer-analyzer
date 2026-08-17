#include "app/Application.h"

#include <string>

// GUI 서브시스템(WIN32)이라 콘솔이 없다 — 초기화 실패는 MessageBox로 알린다.
// design 12번의 "감지·복구를 사용자에게 알린다" 원칙과 같은 맥락: 조용히 죽지 않는다.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    client::Application application;

    std::string error;
    if (!application.initialize(instance, error)) {
        ::MessageBoxA(nullptr, error.c_str(), "log-transfer-analyzer client",
                      MB_ICONERROR | MB_OK);
        return 1;
    }

    application.run();
    return 0;
}
