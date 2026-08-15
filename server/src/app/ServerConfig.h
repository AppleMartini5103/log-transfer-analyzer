#pragma once

#include "protocol/protocol.h"

#include <cstdint>
#include <string>
#include <string_view>

// 서버 실행 설정 — CLI 인자 + config 파일 (design 5번 실행 인터페이스 확정).
//
//   server [-d] [-p PORT] [-c CONFIG]
//
// config 파일은 key=value 평문, '#'로 시작하는 줄은 주석. 없으면 내장 기본값으로 동작한다
// (파일 부재는 에러가 아님 — 채점자가 바이너리만 받아도 실행되어야 함).
//
// 우선순위: 내장 기본값 < config 파일 < CLI 인자
//   → CLI가 최우선인 이유: 벤치 스윕 중 한 값만 바꿔 반복 실행하기 위함 (design 측정 방법)
//
// config의 용도는 벤치 스윕(chunk_size, snd_buf_size)과 로그 경로 한정이다.
// 프로토콜 상수(매직·타입·타임아웃 30/120초)는 protocol.h에만 둔다 — 두 벌 정의 금지
// (컨벤션 9번). config로 프로토콜을 바꿀 수 있게 하면 클라/서버 불일치 통로가 된다.
//
// 미지의 키는 에러로 거부한다 (default-deny): 벤치 중 'chunck_size' 같은 오타가 조용히
// 무시되면 "설정을 바꿨는데 결과가 안 변한다"는 잘못된 결론으로 이어진다.

namespace server::app {

// 값 검증 범위 — 매직 넘버 금지 규칙에 따라 이름을 붙인다 (컨벤션 6번)
inline constexpr std::uint16_t kMinPort = 1024;  // 미만은 리눅스 bind에 root 필요 (design 5번)
inline constexpr std::size_t kMinChunkSize = 4 * 1024;
inline constexpr std::size_t kMaxChunkSize = 1024 * 1024;  // 스윕 상한 (32KB~1MB 계획)
inline constexpr std::size_t kMinSendBufferSize = 8 * 1024;
inline constexpr std::size_t kMaxSendBufferSize = 16 * 1024 * 1024;

struct ServerConfig {
    std::uint16_t port = common::protocol::kDefaultPort;
    bool daemonize = false;
    std::size_t chunkSize = 64 * 1024;  // 링 슬롯 크기 = 수신 단위 (벤치 스윕 대상)
    std::size_t sendBufferSize = 0;     // 0 = SO_SNDBUF 미설정(커널 autotuning 유지)
    std::string logPath = "./server.log";
    std::string configPath = "./server.conf";
};

// CLI에서 읽은 값 — config 파일보다 우선 적용되므로 "지정 여부"를 함께 들고 다닌다
struct CommandLine {
    std::string configPath = "./server.conf";
    bool daemonize = false;
    bool hasPort = false;
    std::uint16_t port = 0;
    bool helpRequested = false;
};

// 실패는 예외가 아니라 반환값 + 사람이 읽을 메시지 (컨벤션 3번)
struct ParseResult {
    bool ok = true;
    std::string error;
};

ParseResult parseCommandLine(int argc, const char* const* argv, CommandLine& out);

// config 파일 본문 파싱 (파일 I/O 없음 — 단위 테스트가 파일을 만들지 않아도 되도록 분리)
ParseResult parseConfigText(std::string_view text, ServerConfig& out);

// 최종 설정 조립: 기본값 → config 파일(있으면) → CLI 순으로 덮어쓴다
ParseResult buildConfig(const CommandLine& cli, ServerConfig& out);

std::string usageText();

}  // namespace server::app
