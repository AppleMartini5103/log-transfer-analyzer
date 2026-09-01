#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace client {

// result.csv의 metric 블록에서 뽑아낸 요약 — 스킵 경고 문구를 만들 재료만 담는다.
//
// 왜 별도 파일인가: 소켓·UI·uv 어디에도 의존하지 않는 순수 함수라야 "서버가 계약을
// 어겼을 때 클라이언트가 죽지 않는다"를 네트워크 없이 단위 테스트로 고정할 수 있다.
// 리뷰 3의 요구가 "포맷이 조금 바뀌어도 조용히 무너지지 않는 것"이므로, 그 성질을
// 검사할 수 없는 자리에 파서를 두면 같은 결함을 클라이언트에 옮기는 셈이다.
struct ResultSummary {
    bool hasTotal = false;  // total_lines 행이 있었는가 — 없으면 분모를 말하지 않는다
    std::uint64_t totalLines = 0;
    std::uint64_t skippedLines = 0;
    // skip_reason_* 행에서 접두사를 뗀 사유 코드와 건수. 건수 내림차순, 동수면 코드 오름차순.
    // 사유 코드 목록을 클라이언트가 알지 않는다 — 서버가 사유를 추가해도 그대로 표시된다.
    std::vector<std::pair<std::string, std::uint64_t>> reasons;
};

// result.csv를 훑어 요약을 만든다. 어떤 형태로 깨져 있어도 예외를 던지지 않으며,
// 읽어낼 수 없는 항목은 조용히 비운다 (그 결과 경고가 생략될 뿐 저장은 영향받지 않는다).
ResultSummary summarizeResultCsv(std::string_view csv);

// 사용자에게 보일 경고 한 줄. 경고할 것이 없으면 빈 문자열.
//
//   "26 of 3483528 lines were skipped - BAD_FRAME 13, UNKNOWN_MODULE 13"
//   "26 lines were skipped - BAD_FRAME 13, UNKNOWN_MODULE 13"   (total_lines 부재)
//   "26 of 3483528 lines were skipped"                          (사유 행 부재)
std::string formatSkipWarning(const ResultSummary& summary);

}  // namespace client
