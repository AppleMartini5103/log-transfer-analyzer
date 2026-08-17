#include "util/Logger.h"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using common::Logger;
using common::LogLevel;

namespace {

namespace fs = std::filesystem;

constexpr const char* kBaseDir = "test_logger_base";
constexpr const char* kBaseName = "test_logger_out.log";

std::vector<std::string> readLines(const std::string& path) {
    std::ifstream in{path};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

// Logger.cpp와 같은 이유로 감싼다: std::localtime은 MSVC에서 C4996(경고 0 규칙 위반)
std::tm localTimeOf(std::time_t when) {
    std::tm local{};
#ifdef _WIN32
    REQUIRE(::localtime_s(&local, &when) == 0);
#else
    REQUIRE(::localtime_r(&when, &local) != nullptr);
#endif
    return local;
}

std::string todayString(std::time_t when) {
    const std::tm local = localTimeOf(when);
    char buffer[16];
    REQUIRE(std::strftime(buffer, sizeof(buffer), "%Y%m%d", &local) != 0);
    return std::string{buffer};
}

// 기준일에서 days일 전의 YYYYMMDD (정오 기준 — DST에서 하루가 밀리지 않게)
std::string dateStringDaysAgo(std::time_t reference, int days) {
    std::tm midday = localTimeOf(reference);
    midday.tm_hour = 12;
    midday.tm_min = 0;
    midday.tm_sec = 0;
    std::time_t shifted = std::mktime(&midday);
    REQUIRE(shifted != static_cast<std::time_t>(-1));
    shifted -= static_cast<std::time_t>(days) * 24 * 60 * 60;
    return todayString(shifted);
}

fs::path logsRoot() {
    return fs::path{kBaseDir} / "logs";
}

// 각 테스트가 깨끗한 트리에서 시작하고, 끝나면 stdout으로 복귀하도록 정리
struct FileSinkGuard {
    FileSinkGuard() {
        std::error_code ec;
        fs::remove_all(kBaseDir, ec);
    }
    ~FileSinkGuard() {
        Logger::instance().close();
        std::error_code ec;
        fs::remove_all(kBaseDir, ec);
    }
};

}  // namespace

TEST_CASE("logger: line format matches the convention") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    Logger::instance().info("Server listening on port 23507");

    const auto lines = readLines(Logger::instance().activeFilePath());
    REQUIRE(lines.size() == 1);
    // 컨벤션 8번: [YYYY-MM-DD HH:MM:SS] [레벨] 메시지
    const std::regex pattern{
        R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\] \[Info\] Server listening on port 23507$)"};
    INFO("line: " << lines[0]);
    REQUIRE(std::regex_match(lines[0], pattern));
}

TEST_CASE("logger: three levels carry the mockup tags") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    Logger::instance().info("info message");
    Logger::instance().warn("warn message");
    Logger::instance().error("error message");

    const auto lines = readLines(Logger::instance().activeFilePath());
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0].find("[Info] info message") != std::string::npos);
    REQUIRE(lines[1].find("[Warn] warn message") != std::string::npos);
    REQUIRE(lines[2].find("[Error] error message") != std::string::npos);
}

TEST_CASE("logger: openFile failure is a return value, not an exception") {
    const FileSinkGuard guard;
    // 디렉토리를 만들 수 없는 자리를 만든다: baseDir 위치에 "파일"을 놓으면
    // create_directories가 실패한다 (양 플랫폼 공통으로 재현되는 유일하게 깔끔한 방법)
    {
        std::ofstream blocker{kBaseDir};
        REQUIRE(blocker.is_open());
    }
    REQUIRE_FALSE(Logger::instance().openFile(kBaseDir, kBaseName));
    REQUIRE(Logger::instance().activeFilePath().empty());
    // 실패 후에도 로깅은 stdout으로 계속 동작해야 함 (크래시·예외 없음)
    Logger::instance().info("still alive on stdout");
    std::remove(kBaseDir);
}

TEST_CASE("logger: empty base name is rejected") {
    const FileSinkGuard guard;
    REQUIRE_FALSE(Logger::instance().openFile(kBaseDir, ""));
}

TEST_CASE("logger: file sink appends across reopen (daemon restart)") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    const std::string path = Logger::instance().activeFilePath();
    Logger::instance().info("first run");
    Logger::instance().close();

    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    REQUIRE(Logger::instance().activeFilePath() == path);  // 같은 날이면 같은 파일
    Logger::instance().info("second run");
    Logger::instance().close();

    const auto lines = readLines(path);
    REQUIRE(lines.size() == 2);  // 이어쓰기 — 재시작이 이전 로그를 지우지 않는다
}

TEST_CASE("logger: lines from pre-open never appear in the file") {
    const FileSinkGuard guard;
    Logger::instance().info("goes to stdout, not the file");
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    Logger::instance().info("goes to the file");

    const auto lines = readLines(Logger::instance().activeFilePath());
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].find("goes to the file") != std::string::npos);
}

// ── design 14번: 날짜 디렉토리 / 회전 / 보관 기간 ────────────────────────────

TEST_CASE("logger: the log lands in logs/YYYYMMDD under the base directory") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));

    const fs::path active{Logger::instance().activeFilePath()};
    REQUIRE(active.filename().string() == kBaseName);
    // 부모가 오늘 날짜 디렉토리, 그 부모가 logs/
    REQUIRE(active.parent_path().filename().string() == todayString(std::time(nullptr)));
    REQUIRE(active.parent_path().parent_path().filename().string() == "logs");
    REQUIRE(fs::exists(active));
}

TEST_CASE("logger: a missing log directory is recreated on the next write") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    const std::string path = Logger::instance().activeFilePath();
    Logger::instance().info("before the directory disappears");

    // 외부에서 logs/ 트리를 지운 상황 — 방어 생성이 없으면 이후 로그가 조용히 사라진다
    Logger::instance().close();
    std::error_code ec;
    fs::remove_all(logsRoot(), ec);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(fs::exists(logsRoot()));

    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    Logger::instance().info("after the directory was removed");
    REQUIRE(fs::exists(path));
    const auto lines = readLines(path);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].find("after the directory was removed") != std::string::npos);
}

TEST_CASE("logger: the file rotates to _001 once it passes the size limit") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    const fs::path active{Logger::instance().activeFilePath()};
    const fs::path rotated = active.parent_path() / "test_logger_out_001.log";

    // 10MB를 넘기도록 밀어넣는다. 매 줄 flush가 기본이라 줄을 크게 잡아 횟수를 줄인다
    // (8KB x 약 1,300줄이면 임계를 넘는다 — 1KB로 하면 flush가 10,000회를 넘어 느려진다)
    const std::string filler(8000, 'x');
    const int lineCount = static_cast<int>(common::kMaxLogFileBytes / filler.size()) + 16;
    for (int i = 0; i < lineCount; ++i) {
        Logger::instance().info(filler);
    }

    REQUIRE(fs::exists(rotated));       // 회전본이 생겼다
    REQUIRE(fs::exists(active));        // 활성 파일 이름은 고정 — tail 대상이 바뀌지 않는다
    std::error_code ec;
    REQUIRE(fs::file_size(rotated, ec) >= common::kMaxLogFileBytes - filler.size());
    REQUIRE(fs::file_size(active, ec) < common::kMaxLogFileBytes);  // 새로 시작했다
}

TEST_CASE("logger: the byte counter resumes from the existing file size") {
    // 이어쓰기 모드에서 카운터를 0부터 세면 9MB 파일을 다시 열었을 때 회전이 무한정 밀린다
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    const fs::path active{Logger::instance().activeFilePath()};
    const fs::path rotated = active.parent_path() / "test_logger_out_001.log";

    const std::string filler(8000, 'x');
    const int halfWay = static_cast<int>(common::kMaxLogFileBytes / filler.size()) - 100;
    for (int i = 0; i < halfWay; ++i) {
        Logger::instance().info(filler);
    }
    REQUIRE_FALSE(fs::exists(rotated));  // 아직 임계 미달

    // 닫고 다시 연다 — 카운터가 기존 크기에서 이어져야 한다
    Logger::instance().close();
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    for (int i = 0; i < 200; ++i) {
        Logger::instance().info(filler);
    }
    REQUIRE(fs::exists(rotated));  // 카운터가 0부터였다면 여기서 회전하지 않는다
}

TEST_CASE("logger: pruning removes date folders outside the retention window") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    const std::time_t reference = std::time(nullptr);

    // 시스템 시계를 건드리지 않고 과거 날짜 디렉토리를 만들어 검증한다 (design 14번 이음새)
    const std::string keptEdge = dateStringDaysAgo(reference, 6);     // 오늘 포함 7일째 → 유지
    const std::string expiredEdge = dateStringDaysAgo(reference, 7);  // 8일째 → 삭제
    const std::string longExpired = dateStringDaysAgo(reference, 30);
    for (const auto& date : {keptEdge, expiredEdge, longExpired}) {
        std::error_code ec;
        fs::create_directories(logsRoot() / date, ec);
        REQUIRE_FALSE(ec);
        std::ofstream marker{(logsRoot() / date / "old.log").string()};
        REQUIRE(marker.is_open());
    }
    // 우리가 만들지 않은 이름은 건드리지 않아야 한다
    std::error_code ec;
    fs::create_directories(logsRoot() / "not-a-date", ec);
    REQUIRE_FALSE(ec);

    const std::size_t removed = Logger::instance().pruneOldLogs(reference);

    REQUIRE(removed == 2);
    REQUIRE(fs::exists(logsRoot() / keptEdge));
    REQUIRE_FALSE(fs::exists(logsRoot() / expiredEdge));
    REQUIRE_FALSE(fs::exists(logsRoot() / longExpired));
    REQUIRE(fs::exists(logsRoot() / "not-a-date"));
    // 활성 파일이 든 오늘 디렉토리는 살아 있어야 한다
    REQUIRE(fs::exists(Logger::instance().activeFilePath()));
}

TEST_CASE("logger: pruning never deletes the active date folder") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    Logger::instance().info("active session line");
    const std::string active = Logger::instance().activeFilePath();

    // keepDays=1이면 오늘 말고는 전부 만료 — 그래도 오늘 디렉토리는 대상이 아니다.
    // 게다가 기준일을 한참 미래로 줘서 오늘이 만료 범위에 들어가게 만들어도 지워지면 안 된다
    const std::time_t farFuture = std::time(nullptr) + 60 * 24 * 60 * 60;
    Logger::instance().pruneOldLogs(farFuture, 1);

    REQUIRE(fs::exists(active));
    Logger::instance().info("still logging after prune");
    const auto lines = readLines(active);
    REQUIRE(lines.size() == 2);
}

TEST_CASE("logger: pruning is a no-op without a file sink") {
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().pruneOldLogs(std::time(nullptr)) == 0);
}

TEST_CASE("logger: concurrent writers produce whole, untorn lines") {
    // 루프 스레드 + 파서 스레드가 동시에 쓰는 실전 배치 — 줄 단위 원자성 검증
    const FileSinkGuard guard;
    REQUIRE(Logger::instance().openFile(kBaseDir, kBaseName));
    const std::string path = Logger::instance().activeFilePath();

    constexpr int kThreads = 4;
    // static: 아래 람다가 기본 캡처 모드 없이([t]) 이 상수를 쓴다 — MSVC는 캡처를
    // 요구하고(C3493) gcc는 허용한다. 정적 저장 기간이면 캡처 대상이 아니라 양쪽 통과.
    static constexpr int kLinesEach = 500;
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

    const auto lines = readLines(path);
    REQUIRE(lines.size() == kThreads * kLinesEach);
    const std::regex pattern{
        R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\] \[Info\] thread\d line\d+ padding-payload-0123456789$)"};
    for (const auto& line : lines) {
        INFO("line: " << line);
        REQUIRE(std::regex_match(line, pattern));  // 찢어진/섞인 줄이 없어야 함
    }
}
