// ICMP 진단은 OS의 ping 도구에 위임한다 (design 12번, 커밋 [42]).
//
// 그 방식의 유일한 위험은 셸 주입이다 — 편집 가능한 IP 입력이 cmd.exe 인자로 들어간다.
// 그래서 여기서 검증하는 것은 "ping이 응답하는가"가 아니라 **어떤 문자열이 셸에 도달할 수
// 있는가**다. 프로세스를 실제로 띄우는 부분은 단위 테스트의 대상이 아니고(콘솔 창이 열린다),
// 방어선인 isValidIpv4는 헤더에 있어 링크 없이 테스트할 수 있다.

#include "ui/PingConsole.h"

#include <catch_amalgamated.hpp>

using client::isValidIpv4;

TEST_CASE("ping console: accepts well-formed IPv4 addresses") {
    REQUIRE(isValidIpv4("10.0.0.2"));
    REQUIRE(isValidIpv4("127.0.0.1"));
    REQUIRE(isValidIpv4("192.168.0.109"));
    REQUIRE(isValidIpv4("0.0.0.0"));
    REQUIRE(isValidIpv4("255.255.255.255"));
    REQUIRE(isValidIpv4("010.001.000.002"));  // 선행 0은 무해하다 (값만 본다)
}

TEST_CASE("ping console: rejects shell metacharacters") {
    // 이 목록이 이 파일의 존재 이유다. 하나라도 통과하면 cmd.exe에 임의 명령이 들어간다.
    const char* attacks[] = {
        "10.0.0.2 && calc",
        "10.0.0.2&calc",
        "10.0.0.2 | more",
        "10.0.0.2;calc",
        "10.0.0.2\"",
        "10.0.0.2 > out.txt",
        "10.0.0.2 %PATH%",
        "10.0.0.2^",
        "10.0.0.2 (echo hi)",
        "$(calc)",
        "`calc`",
        "10.0.0.2\ncalc",
        "10.0.0.2\tcalc",
    };
    for (const char* attack : attacks) {
        INFO("input: " << attack);
        REQUIRE_FALSE(isValidIpv4(attack));
    }
}

TEST_CASE("ping console: rejects malformed addresses") {
    REQUIRE_FALSE(isValidIpv4(""));
    REQUIRE_FALSE(isValidIpv4("10.0.0"));          // 옥텟 부족
    REQUIRE_FALSE(isValidIpv4("10.0.0.2.5"));      // 옥텟 초과
    REQUIRE_FALSE(isValidIpv4("10.0.0."));         // 마지막 옥텟 없음
    REQUIRE_FALSE(isValidIpv4(".10.0.0.2"));       // 선행 점
    REQUIRE_FALSE(isValidIpv4("10..0.2"));         // 빈 옥텟
    REQUIRE_FALSE(isValidIpv4("256.0.0.1"));       // 범위 초과
    REQUIRE_FALSE(isValidIpv4("10.0.0.1000"));     // 자릿수 초과
    REQUIRE_FALSE(isValidIpv4("-1.0.0.1"));        // 부호
    REQUIRE_FALSE(isValidIpv4(" 10.0.0.2"));       // 선행 공백
    REQUIRE_FALSE(isValidIpv4("10.0.0.2 "));       // 후행 공백
    REQUIRE_FALSE(isValidIpv4("localhost"));       // 호스트명은 받지 않는다 (IPv4 전제)
    REQUIRE_FALSE(isValidIpv4("::1"));             // IPv6도 받지 않는다
    REQUIRE_FALSE(isValidIpv4("255.255.255.2555"));  // 15자 초과
}
