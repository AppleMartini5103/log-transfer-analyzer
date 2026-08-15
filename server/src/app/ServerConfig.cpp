#include "app/ServerConfig.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace server::app {

namespace {

ParseResult fail(std::string message) {
    return ParseResult{false, std::move(message)};
}

std::string_view trim(std::string_view text) {
    const auto isSpace = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// 숫자 파싱은 from_chars만 — 전체 소비 확인 포함 (컨벤션 2번, 레거시 변환 함수 금지)
bool parseUnsigned(std::string_view token, std::uint64_t& out) {
    if (token.empty() || token.front() < '0' || token.front() > '9') {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), out);
    return ec == std::errc{} && ptr == token.data() + token.size();
}

ParseResult applyPort(std::string_view value, std::uint16_t& out) {
    std::uint64_t number = 0;
    if (!parseUnsigned(value, number)) {
        return fail("invalid port: '" + std::string{value} + "' (expected a number)");
    }
    if (number < kMinPort || number > 65535) {
        return fail("port out of range: " + std::string{value} + " (allowed " +
                    std::to_string(kMinPort) + "-65535; below " + std::to_string(kMinPort) +
                    " requires root to bind)");
    }
    out = static_cast<std::uint16_t>(number);
    return ParseResult{};
}

ParseResult applySize(std::string_view value, std::size_t& out, std::size_t min, std::size_t max,
                      std::string_view name, bool allowZero) {
    std::uint64_t number = 0;
    if (!parseUnsigned(value, number)) {
        return fail("invalid " + std::string{name} + ": '" + std::string{value} +
                    "' (expected a number)");
    }
    if (allowZero && number == 0) {
        out = 0;  // snd_buf_size=0 → 커널 autotuning에 맡김 (design 네트워크 버퍼)
        return ParseResult{};
    }
    if (number < min || number > max) {
        return fail(std::string{name} + " out of range: " + std::string{value} + " (allowed " +
                    std::to_string(min) + "-" + std::to_string(max) +
                    (allowZero ? ", or 0 for kernel autotuning)" : ")"));
    }
    out = static_cast<std::size_t>(number);
    return ParseResult{};
}

}  // namespace

std::string usageText() {
    return "Usage: server [-d] [-p PORT] [-c CONFIG]\n"
           "  -d           run as a daemon (double-fork, detach from terminal)\n"
           "  -p PORT      listen port (default: " +
           std::to_string(common::protocol::kDefaultPort) +
           ")\n"
           "  -c CONFIG    config file path (default: ./server.conf)\n"
           "  -h, --help   show this message\n";
}

ParseResult parseCommandLine(int argc, const char* const* argv, CommandLine& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        // 값이 필요한 옵션은 다음 인자를 소비 — 누락 시 즉시 에러 (조용한 기본값 대체 금지)
        const auto nextValue = [&](std::string_view& value) {
            if (i + 1 >= argc) {
                return false;
            }
            value = std::string_view{argv[++i]};
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            out.helpRequested = true;
            return ParseResult{};  // 이후 인자는 무시 — 도움말만 출력하고 종료
        }
        if (arg == "-d") {
            out.daemonize = true;
        } else if (arg == "-p") {
            std::string_view value;
            if (!nextValue(value)) {
                return fail("-p requires a port number");
            }
            const ParseResult result = applyPort(value, out.port);
            if (!result.ok) {
                return result;
            }
            out.hasPort = true;
        } else if (arg == "-c") {
            std::string_view value;
            if (!nextValue(value)) {
                return fail("-c requires a config file path");
            }
            if (value.empty()) {
                return fail("-c requires a non-empty path");
            }
            out.configPath.assign(value);
        } else {
            return fail("unknown argument: '" + std::string{arg} + "'");
        }
    }
    return ParseResult{};
}

ParseResult parseConfigText(std::string_view text, ServerConfig& out) {
    std::istringstream stream{std::string{text}};
    std::string rawLine;
    int lineNumber = 0;
    while (std::getline(stream, rawLine)) {
        ++lineNumber;
        const std::string_view line = trim(rawLine);
        if (line.empty() || line.front() == '#') {
            continue;  // 빈 줄·주석 줄 (주석은 줄 단위만 — 값에 든 '#'을 잘라내지 않는다)
        }

        const std::size_t separator = line.find('=');
        // string_view가 아니라 string으로 보관할 것: string_view로 받으면 우변의 임시 string이
        // 그 줄 끝에서 파괴되어 해제된 스택 메모리를 가리키게 된다 (ASan stack-use-after-scope).
        // 단위 테스트는 통과했었다 — 해제된 메모리에 옛 내용이 남아 있었을 뿐이다
        const std::string where = " (line " + std::to_string(lineNumber) + ")";
        if (separator == std::string_view::npos) {
            return fail("missing '=' in config" + where + ": '" + std::string{line} + "'");
        }
        const std::string_view key = trim(line.substr(0, separator));
        const std::string_view value = trim(line.substr(separator + 1));
        if (key.empty()) {
            return fail("empty key in config" + where);
        }
        if (value.empty()) {
            return fail("empty value for '" + std::string{key} + "'" + where);
        }

        ParseResult result;
        if (key == "port") {
            result = applyPort(value, out.port);
        } else if (key == "chunk_size") {
            result = applySize(value, out.chunkSize, kMinChunkSize, kMaxChunkSize, "chunk_size",
                               /*allowZero=*/false);
        } else if (key == "snd_buf_size") {
            result = applySize(value, out.sendBufferSize, kMinSendBufferSize, kMaxSendBufferSize,
                               "snd_buf_size", /*allowZero=*/true);
        } else if (key == "log_path") {
            out.logPath.assign(value);
        } else {
            // default-deny: 오타가 조용히 무시되면 벤치 결과를 잘못 읽는다
            return fail("unknown config key: '" + std::string{key} + "'" + where);
        }
        if (!result.ok) {
            return fail(result.error + where);
        }
    }
    return ParseResult{};
}

ParseResult buildConfig(const CommandLine& cli, ServerConfig& out) {
    out = ServerConfig{};  // 내장 기본값에서 시작
    out.configPath = cli.configPath;

    std::ifstream file{cli.configPath};
    if (file.is_open()) {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const ParseResult result = parseConfigText(buffer.str(), out);
        if (!result.ok) {
            return fail("config file '" + cli.configPath + "': " + result.error);
        }
    }
    // 파일이 없으면 기본값 유지 — 에러 아님 (design 5번: "없으면 내장 기본값으로 동작")

    // CLI가 config를 덮어쓴다
    out.daemonize = cli.daemonize;
    if (cli.hasPort) {
        out.port = cli.port;
    }
    return ParseResult{};
}

}  // namespace server::app
