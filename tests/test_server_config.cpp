#include "app/ServerConfig.h"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using server::app::CommandLine;
using server::app::ServerConfig;

namespace {

// argv 흉내 — argv[0]은 프로그램 이름
server::app::ParseResult parseArgs(const std::vector<const char*>& args, CommandLine& out) {
    std::vector<const char*> argv{"server"};
    argv.insert(argv.end(), args.begin(), args.end());
    return server::app::parseCommandLine(static_cast<int>(argv.size()), argv.data(), out);
}

struct TempConfigFile {
    explicit TempConfigFile(const std::string& contents) {
        std::ofstream out{path};
        out << contents;
    }
    ~TempConfigFile() { std::remove(path.c_str()); }
    std::string path = "test_server.conf";
};

}  // namespace

// ── CLI ──

TEST_CASE("config: defaults match the confirmed execution interface") {
    CommandLine cli;
    ServerConfig config;
    REQUIRE(parseArgs({}, cli).ok);
    REQUIRE(server::app::buildConfig(cli, config).ok);

    REQUIRE(config.port == common::protocol::kDefaultPort);  // 23507 (design 5번)
    REQUIRE(config.port == 23507);
    REQUIRE_FALSE(config.daemonize);  // 기본은 포그라운드
    REQUIRE(config.chunkSize == 64 * 1024);
    REQUIRE(config.sendBufferSize == 0);  // 커널 autotuning 유지
    REQUIRE(config.logPath == "./server.log");
    REQUIRE(config.configPath == "./server.conf");
}

TEST_CASE("config: CLI flags are parsed") {
    CommandLine cli;
    REQUIRE(parseArgs({"-d", "-p", "30000", "-c", "/etc/other.conf"}, cli).ok);
    REQUIRE(cli.daemonize);
    REQUIRE(cli.hasPort);
    REQUIRE(cli.port == 30000);
    REQUIRE(cli.configPath == "/etc/other.conf");
}

TEST_CASE("config: help is requested and short-circuits") {
    for (const char* flag : {"-h", "--help"}) {
        CommandLine cli;
        INFO("flag: " << flag);
        REQUIRE(parseArgs({flag, "-p", "bogus"}, cli).ok);  // 이후 인자는 검사하지 않음
        REQUIRE(cli.helpRequested);
    }
    REQUIRE(server::app::usageText().find("23507") != std::string::npos);
}

TEST_CASE("config: port validation rejects bad values") {
    struct Case {
        const char* value;
        const char* expectedFragment;
    };
    for (const auto& c : {Case{"abc", "expected a number"}, Case{"-1", "expected a number"},
                          Case{"80", "out of range"},        // 1024 미만은 root 필요
                          Case{"1023", "out of range"}, Case{"65536", "out of range"},
                          Case{"230507", "out of range"},    // 사용자 희망값 — 16비트 초과
                          Case{"8080abc", "expected a number"}}) {
        CommandLine cli;
        const auto result = parseArgs({"-p", c.value}, cli);
        INFO("port: " << c.value << " → " << result.error);
        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error.find(c.expectedFragment) != std::string::npos);
    }
    CommandLine ok;
    REQUIRE(parseArgs({"-p", "1024"}, ok).ok);  // 경계는 허용
    REQUIRE(parseArgs({"-p", "65535"}, ok).ok);
}

TEST_CASE("config: missing option values and unknown flags are errors") {
    CommandLine cli;
    REQUIRE_FALSE(parseArgs({"-p"}, cli).ok);
    REQUIRE_FALSE(parseArgs({"-c"}, cli).ok);
    REQUIRE_FALSE(parseArgs({"-x"}, cli).ok);
    REQUIRE_FALSE(parseArgs({"--verbose"}, cli).ok);
    REQUIRE_FALSE(parseArgs({"stray"}, cli).ok);  // 위치 인자 없음
}

// ── config 파일 ──

TEST_CASE("config: key=value text with comments and blank lines") {
    ServerConfig config;
    const auto result = server::app::parseConfigText(
        "# benchmark sweep settings\n"
        "\n"
        "chunk_size=131072\n"
        "   snd_buf_size = 262144   \n"
        "log_path=/var/log/byda-server.log\n"
        "# port stays at the default\n",
        config);
    REQUIRE(result.ok);
    REQUIRE(config.chunkSize == 131072);
    REQUIRE(config.sendBufferSize == 262144);
    REQUIRE(config.logPath == "/var/log/byda-server.log");
    REQUIRE(config.port == common::protocol::kDefaultPort);
}

TEST_CASE("config: unknown keys are rejected, not silently ignored") {
    // 벤치 중 오타가 무시되면 "설정을 바꿨는데 결과가 그대로"인 잘못된 결론이 나온다
    ServerConfig config;
    const auto result = server::app::parseConfigText("chunck_size=65536\n", config);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("unknown config key") != std::string::npos);
    REQUIRE(result.error.find("chunck_size") != std::string::npos);
    REQUIRE(result.error.find("line 1") != std::string::npos);
}

TEST_CASE("config: protocol constants are not configurable") {
    // 타임아웃·매직·타입은 protocol.h 단일 정의 (컨벤션 9번) — config로 뚫리면 안 됨
    ServerConfig config;
    for (const char* line : {"idle_timeout_ms=5000\n", "magic=WXYZ\n", "version=2\n",
                             "response_timeout_ms=1000\n"}) {
        INFO("line: " << line);
        REQUIRE_FALSE(server::app::parseConfigText(line, config).ok);
    }
}

TEST_CASE("config: malformed lines report the line number") {
    ServerConfig config;
    const auto missingEquals = server::app::parseConfigText("chunk_size=65536\nlog_path\n", config);
    REQUIRE_FALSE(missingEquals.ok);
    REQUIRE(missingEquals.error.find("missing '='") != std::string::npos);
    REQUIRE(missingEquals.error.find("line 2") != std::string::npos);

    REQUIRE_FALSE(server::app::parseConfigText("=65536\n", config).ok);       // 키 없음
    REQUIRE_FALSE(server::app::parseConfigText("chunk_size=\n", config).ok);  // 값 없음
}

TEST_CASE("config: numeric ranges are validated") {
    ServerConfig config;
    REQUIRE_FALSE(server::app::parseConfigText("chunk_size=1024\n", config).ok);     // 하한 미만
    REQUIRE_FALSE(server::app::parseConfigText("chunk_size=2097152\n", config).ok);  // 상한 초과
    REQUIRE_FALSE(server::app::parseConfigText("chunk_size=0\n", config).ok);
    REQUIRE_FALSE(server::app::parseConfigText("snd_buf_size=1024\n", config).ok);

    // 스윕 계획값 32KB~1MB는 전부 허용되어야 한다 (design 측정 방법)
    for (const char* line : {"chunk_size=32768\n", "chunk_size=65536\n", "chunk_size=131072\n",
                             "chunk_size=262144\n", "chunk_size=1048576\n"}) {
        INFO("line: " << line);
        REQUIRE(server::app::parseConfigText(line, config).ok);
    }
    // snd_buf_size=0은 "설정하지 않음"의 의미로 허용
    REQUIRE(server::app::parseConfigText("snd_buf_size=0\n", config).ok);
    REQUIRE(config.sendBufferSize == 0);
}

// ── 병합 우선순위 ──

TEST_CASE("config: missing config file is not an error") {
    CommandLine cli;
    REQUIRE(parseArgs({"-c", "./definitely_absent.conf"}, cli).ok);
    ServerConfig config;
    REQUIRE(server::app::buildConfig(cli, config).ok);  // 내장 기본값으로 동작
    REQUIRE(config.port == common::protocol::kDefaultPort);
    REQUIRE(config.chunkSize == 64 * 1024);
}

TEST_CASE("config: file values apply, then CLI overrides them") {
    const TempConfigFile file{"port=40000\nchunk_size=32768\nlog_path=./from-file.log\n"};

    SECTION("file only") {
        CommandLine cli;
        REQUIRE(parseArgs({"-c", file.path.c_str()}, cli).ok);
        ServerConfig config;
        REQUIRE(server::app::buildConfig(cli, config).ok);
        REQUIRE(config.port == 40000);
        REQUIRE(config.chunkSize == 32768);
        REQUIRE(config.logPath == "./from-file.log");
    }
    SECTION("CLI port wins over the file") {
        CommandLine cli;
        REQUIRE(parseArgs({"-c", file.path.c_str(), "-p", "50000", "-d"}, cli).ok);
        ServerConfig config;
        REQUIRE(server::app::buildConfig(cli, config).ok);
        REQUIRE(config.port == 50000);       // CLI 우선
        REQUIRE(config.chunkSize == 32768);  // 파일 값은 그대로 유지
        REQUIRE(config.daemonize);
    }
}

TEST_CASE("config: a broken config file reports the file path") {
    const TempConfigFile file{"chunk_size=nope\n"};
    CommandLine cli;
    REQUIRE(parseArgs({"-c", file.path.c_str()}, cli).ok);
    ServerConfig config;
    const auto result = server::app::buildConfig(cli, config);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find(file.path) != std::string::npos);
    REQUIRE(result.error.find("chunk_size") != std::string::npos);
}
