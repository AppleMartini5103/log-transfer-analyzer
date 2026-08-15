#include "util/Logger.h"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using common::Logger;
using common::LogLevel;

namespace {

constexpr const char* kTestLogPath = "test_logger_out.log";

std::vector<std::string> readLines(const char* path) {
    std::ifstream in{path};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

// 각 테스트가 깨끗한 파일에서 시작하고 끝나면 stdout으로 복귀하도록 정리
struct FileSinkGuard {
    FileSinkGuard() { std::remove(kTestLogPath); }
    ~FileSinkGuard() {
        Logger::instance().close();
        std::remove(kTestLogPath);
    }
};

}  // namespace

TEST_CASE("logger: line format matches the convention") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kTestLogPath));
    Logger::instance().info("Server listening on port 23507");

    const auto lines = readLines(kTestLogPath);
    REQUIRE(lines.size() == 1);
    // 컨벤션 8번: [YYYY-MM-DD HH:MM:SS] [레벨] 메시지
    const std::regex pattern{
        R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\] \[Info\] Server listening on port 23507$)"};
    INFO("line: " << lines[0]);
    REQUIRE(std::regex_match(lines[0], pattern));
}

TEST_CASE("logger: three levels carry the mockup tags") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kTestLogPath));
    Logger::instance().info("info message");
    Logger::instance().warn("warn message");
    Logger::instance().error("error message");

    const auto lines = readLines(kTestLogPath);
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0].find("[Info] info message") != std::string::npos);
    REQUIRE(lines[1].find("[Warn] warn message") != std::string::npos);
    REQUIRE(lines[2].find("[Error] error message") != std::string::npos);
}

TEST_CASE("logger: openFile failure is a return value, not an exception") {
    const FileSinkGuard guard;
    REQUIRE_FALSE(Logger::instance().openFile("no_such_dir/impossible.log"));
    // 실패 후에도 로깅은 stdout으로 계속 동작해야 함 (크래시·예외 없음)
    Logger::instance().info("still alive on stdout");
}

TEST_CASE("logger: file sink appends across reopen (daemon restart)") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kTestLogPath));
    Logger::instance().info("first run");
    Logger::instance().close();

    REQUIRE(Logger::instance().openFile(kTestLogPath));
    Logger::instance().info("second run");
    Logger::instance().close();

    const auto lines = readLines(kTestLogPath);
    REQUIRE(lines.size() == 2);  // 이어쓰기 — 재시작이 이전 로그를 지우지 않는다
}

TEST_CASE("logger: lines from pre-open never appear in the file") {
    const FileSinkGuard guard;
    Logger::instance().info("goes to stdout, not the file");
    REQUIRE(Logger::instance().openFile(kTestLogPath));
    Logger::instance().info("goes to the file");

    const auto lines = readLines(kTestLogPath);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].find("goes to the file") != std::string::npos);
}

TEST_CASE("logger: concurrent writers produce whole, untorn lines") {
    // 루프 스레드 + 파서 스레드가 동시에 쓰는 실전 배치 — 줄 단위 원자성 검증
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kTestLogPath));

    constexpr int kThreads = 4;
    constexpr int kLinesEach = 500;
    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([t] {
            for (int i = 0; i < kLinesEach; ++i) {
                std::ostringstream msg;
                msg << "thread" << t << " line" << i << " padding-payload-0123456789";
                Logger::instance().info(msg.str());
            }
        });
    }
    for (auto& w : writers) {
        w.join();
    }

    const auto lines = readLines(kTestLogPath);
    REQUIRE(lines.size() == kThreads * kLinesEach);
    const std::regex pattern{
        R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\] \[Info\] thread\d line\d+ padding-payload-0123456789$)"};
    for (const auto& line : lines) {
        INFO("line: " << line);
        REQUIRE(std::regex_match(line, pattern));  // 찢어진/섞인 줄이 없어야 함
    }
}
