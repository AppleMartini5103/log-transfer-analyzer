// UiState의 공용 헬퍼 두 개 — humanSize(크기 표기)와 bufferText(고정 버퍼 읽기).
//
// humanSize는 화면(File 줄의 크기 표시)과 전송 확인 대화상자가 함께 쓴다.
//
// 왜 테스트로 묶는가: 같은 파일을 두고 두 곳이 다른 숫자를 말하면 사용자는 어느 쪽을 믿어야
// 하는지 알 수 없다. 원래 UiRenderer.cpp 안에 갇혀 있던 함수를 공유 위치로 옮기면서
// (커밋 [43]) 표기가 바뀌지 않았음을 여기서 고정한다.
//
// MB = 1024x1024 기준인 것도 의도다 — 로그의 처리량 표기(MB/s)와 같은 기준이어야 같은 전송을
// 두 곳이 같은 숫자로 말한다 (design 실측 절).

#include "ui/UiState.h"

#include <catch_amalgamated.hpp>

#include <array>

using client::bufferText;
using client::humanSize;

// ── bufferText: 무경계 스캔 제거 (커밋 [43]) ────────────────────────────────
//
// 종전에는 parsePort가 std::strlen을, Application이 std::string(ptr)을 썼다. 둘 다 널을
// 만날 때까지 읽으므로 종료되지 않은 버퍼에서 배열 경계를 넘는다 — 컨벤션 1번이 strlen을
// 금지하는 이유가 그것이다. 여기서 고정하는 것은 "종료가 없어도 넘치지 않는다"다.

TEST_CASE("bufferText: stops at the null terminator") {
    std::array<char, 16> buffer{};
    buffer[0] = '2'; buffer[1] = '3'; buffer[2] = '5'; buffer[3] = '0'; buffer[4] = '7';
    REQUIRE(bufferText(buffer) == "23507");
}

TEST_CASE("bufferText: an empty buffer yields an empty string") {
    const std::array<char, 16> buffer{};  // 전부 0
    REQUIRE(bufferText(buffer).empty());
}

TEST_CASE("bufferText: a buffer with no terminator stops at the array end") {
    // ★ 이 케이스가 이 함수의 존재 이유다. std::strlen이나 std::string(ptr)은 여기서
    //   배열을 넘어 계속 읽는다. bufferText는 end()에서 멈춘다.
    std::array<char, 8> buffer{};
    buffer.fill('9');  // 널이 하나도 없다
    const std::string text = bufferText(buffer);
    REQUIRE(text.size() == buffer.size());
    REQUIRE(text == "99999999");
}

TEST_CASE("bufferText: trailing bytes after the terminator are ignored") {
    // ImGui가 짧은 문자열로 덮어썼을 때 뒤에 남는 쓰레기를 읽지 않는다
    std::array<char, 8> buffer{};
    buffer[0] = '8'; buffer[1] = '0'; buffer[2] = '\0';
    buffer[3] = 'X'; buffer[4] = 'Y';
    REQUIRE(bufferText(buffer) == "80");
}

TEST_CASE("humanSize: bytes below 1 MB are shown as an exact byte count") {
    REQUIRE(humanSize(0) == "0 bytes");
    REQUIRE(humanSize(1) == "1 bytes");
    REQUIRE(humanSize(512) == "512 bytes");
    REQUIRE(humanSize(1024) == "1024 bytes");
    REQUIRE(humanSize(1024 * 1024 - 1) == "1048575 bytes");  // 1MB 경계 바로 아래
}

TEST_CASE("humanSize: 1 MB and above switch to MB with one decimal") {
    REQUIRE(humanSize(1024 * 1024) == "1.0 MB");
    REQUIRE(humanSize(10 * 1024 * 1024) == "10.0 MB");

    // 실제 과제 파일 — 화면에 "482.8 MB"로 나오는 값 (스크린샷으로 확인된 표기)
    REQUIRE(humanSize(506286814) == "482.8 MB");
}

TEST_CASE("humanSize: MB uses 1024x1024, not 1000x1000") {
    // 십진 MB(1000x1000)로 계산하면 506286814는 "506.3 MB"가 된다. 그 기준으로 바뀌면
    // 로그의 MB/s 표기와 어긋나므로 여기서 못 박는다.
    REQUIRE(humanSize(506286814) != "506.3 MB");
    REQUIRE(humanSize(8ULL * 1024 * 1024 * 1024) == "8192.0 MB");  // 프로토콜 상한 8GiB
}
