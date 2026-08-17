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

using client::humanSize;

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
