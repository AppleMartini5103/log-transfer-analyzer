#include "parser/LineReassembler.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <string_view>
#include <vector>

using server::parser::LineReassembler;

namespace {

struct Collected {
    std::string text;
    std::uint64_t offset;
    bool tooLong;
};

// 콜백의 string_view는 콜백 동안만 유효 — 테스트는 복사해서 수집
std::vector<Collected> collect(LineReassembler& reassembler, std::string_view chunk) {
    std::vector<Collected> out;
    reassembler.feed(chunk, [&out](const LineReassembler::Line& line) {
        out.push_back({std::string{line.text}, line.offset, line.tooLong});
    });
    return out;
}

std::vector<Collected> finishCollect(LineReassembler& reassembler) {
    std::vector<Collected> out;
    reassembler.finish([&out](const LineReassembler::Line& line) {
        out.push_back({std::string{line.text}, line.offset, line.tooLong});
    });
    return out;
}

}  // namespace

TEST_CASE("reassembler: multiple lines in one chunk with offsets") {
    LineReassembler reassembler;
    const auto lines = collect(reassembler, "first\nsecond\nthird\n");
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0].text == "first");
    REQUIRE(lines[0].offset == 0);
    REQUIRE(lines[1].text == "second");
    REQUIRE(lines[1].offset == 6);
    REQUIRE(lines[2].text == "third");
    REQUIRE(lines[2].offset == 13);
}

TEST_CASE("reassembler: line split across chunks keeps start offset") {
    LineReassembler reassembler;
    REQUIRE(collect(reassembler, "AAAA\nBB").size() == 1);
    const auto lines = collect(reassembler, "CC\n");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].text == "BBCC");
    REQUIRE(lines[0].offset == 5);  // 'B'가 시작된 지점 — 조각이 넘어와도 시작 오프셋 유지
}

TEST_CASE("reassembler: every split point yields identical line sequence") {
    const std::string stream = "alpha\nbeta\r\ngamma\n\ndelta";  // CRLF·빈 줄·무개행 꼬리 포함
    // 기준: 한 번에 전부 공급
    LineReassembler whole;
    auto expected = collect(whole, stream);
    auto expectedTail = finishCollect(whole);
    expected.insert(expected.end(), expectedTail.begin(), expectedTail.end());

    for (std::size_t split = 0; split <= stream.size(); ++split) {
        LineReassembler split2;
        auto got = collect(split2, std::string_view{stream}.substr(0, split));
        const auto second = collect(split2, std::string_view{stream}.substr(split));
        got.insert(got.end(), second.begin(), second.end());
        const auto tail = finishCollect(split2);
        got.insert(got.end(), tail.begin(), tail.end());

        INFO("split at " << split);
        REQUIRE(got.size() == expected.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            REQUIRE(got[i].text == expected[i].text);
            REQUIRE(got[i].offset == expected[i].offset);
            REQUIRE(got[i].tooLong == expected[i].tooLong);
        }
    }
}

TEST_CASE("reassembler: CRLF trailing \\r removed, inner \\r preserved") {
    LineReassembler reassembler;
    const auto lines = collect(reassembler, "win\r\nlockState[1\r0]\n");
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].text == "win");            // 끝 \r 제거
    REQUIRE(lines[1].text == "lockState[1\r0]");  // 내부 \r은 데이터 — 검증 단계가 거부할 것
}

TEST_CASE("reassembler: empty lines are emitted for the pipeline to skip") {
    LineReassembler reassembler;
    const auto lines = collect(reassembler, "\n\r\nx\n");
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0].text.empty());
    REQUIRE(lines[1].text.empty());  // "\r\n" → \r 제거 후 빈 줄
    REQUIRE(lines[2].text == "x");
}

TEST_CASE("reassembler: over-limit line reports once and discards to next newline") {
    LineReassembler reassembler{8};  // 테스트용 소형 상한
    const auto first = collect(reassembler, "0123456789ABCDEF");  // 개행 없는 16바이트
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].tooLong);
    REQUIRE(first[0].text == "01234567");  // 상한(8)까지의 앞부분 보존 — 스킵 로그 재료
    REQUIRE(first[0].offset == 0);

    // 같은 라인의 연속 — 추가 보고 없이 계속 폐기
    REQUIRE(collect(reassembler, "GHIJKL").empty());
    // \n 도달 → 폐기 종료, 다음 라인은 정상 (파싱 계속 — 전체 중단 금지)
    const auto after = collect(reassembler, "MN\nnormal\n");
    REQUIRE(after.size() == 1);
    REQUIRE(after[0].text == "normal");
    REQUIRE_FALSE(after[0].tooLong);
    REQUIRE(after[0].offset == 25);  // 16 + 6 + 3 ("MN\n")
}

TEST_CASE("reassembler: over-limit via fragment + newline in same chunk") {
    LineReassembler reassembler{8};
    REQUIRE(collect(reassembler, "01234").empty());       // 조각 5바이트
    const auto lines = collect(reassembler, "56789AB\nok\n");  // 조각+7 > 8, \n 보유
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].tooLong);
    REQUIRE(lines[0].text == "01234567");
    REQUIRE(lines[1].text == "ok");  // 폐기 모드 없이 즉시 복구
}

TEST_CASE("reassembler: exact-limit line is valid, one over is not") {
    LineReassembler reassembler{8};
    const auto ok = collect(reassembler, "01234567\n");  // 정확히 상한 = 정상
    REQUIRE(ok.size() == 1);
    REQUIRE_FALSE(ok[0].tooLong);
    REQUIRE(ok[0].text == "01234567");

    const auto bad = collect(reassembler, "012345678\n");  // 상한+1 = 손상
    REQUIRE(bad.size() == 1);
    REQUIRE(bad[0].tooLong);
}

TEST_CASE("reassembler: finish flushes the unterminated last line") {
    LineReassembler reassembler;
    REQUIRE(collect(reassembler, "complete\ntail-without-newline").size() == 1);
    const auto tail = finishCollect(reassembler);
    REQUIRE(tail.size() == 1);
    REQUIRE(tail[0].text == "tail-without-newline");
    REQUIRE(tail[0].offset == 9);

    // 배출 후 재호출은 아무것도 내지 않음
    REQUIRE(finishCollect(reassembler).empty());
}

TEST_CASE("reassembler: finish after discarding emits nothing") {
    LineReassembler reassembler{8};
    REQUIRE(collect(reassembler, "way-too-long-line-without-newline").size() == 1);  // 보고됨
    REQUIRE(finishCollect(reassembler).empty());  // 폐기 중 EOF — 이미 보고했으므로 침묵
}

TEST_CASE("reassembler: reset clears state for the next session") {
    LineReassembler reassembler;
    REQUIRE(collect(reassembler, "orphan-fragment").empty());
    reassembler.reset();

    const auto lines = collect(reassembler, "fresh\n");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].text == "fresh");   // 이전 세션 조각이 섞이지 않음
    REQUIRE(lines[0].offset == 0);       // 오프셋도 0부터
}
