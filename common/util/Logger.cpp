#include "util/Logger.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <system_error>

namespace common {

namespace {

namespace fs = std::filesystem;

// 날짜 디렉토리를 모아두는 고정 폴더 이름 (design 14번: <baseDir>/logs/<YYYYMMDD>/)
constexpr const char* kLogsFolder = "logs";
// 회전 인덱스 상한 — 한 날짜에 이만큼 쌓이면(10MB x 999) 더 밀지 않는다
constexpr int kMaxRotationIndex = 999;
constexpr std::size_t kDateLength = 8;  // YYYYMMDD

std::string_view levelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return "Info";
        case LogLevel::Warn:
            return "Warn";
        case LogLevel::Error:
            return "Error";
    }
    return "Info";  // 도달 불가 — 컴파일러 경고 억제용
}

// "_007" 같은 접미사. printf 계열을 쓰지 않는 이유: 컨벤션 1번의 금지 키워드(sscanf 등)와
// 같은 계열을 로거에서까지 끌어들일 이유가 없다 — 문자열 조립으로 충분하다
std::string rotationSuffix(int index) {
    std::string number = std::to_string(index);
    while (number.size() < 3) {
        number.insert(number.begin(), '0');
    }
    return "_" + number;
}

bool isDateName(const std::string& name) {
    if (name.size() != kDateLength) {
        return false;
    }
    for (const char c : name) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// 지역시 변환. 표준 std::localtime은 정적 버퍼를 돌려주는 재진입 불가 함수라 MSVC가 C4996으로
// 거부한다 — 컨벤션 7번이 요구하는 "경고 0"을 지키려면 감싸야 한다.
// → 이 형태를 택한 이유: <windows.h>/<unistd.h>를 끌어오지 않아 common/ 순수성 규칙(컨벤션
//   10번)에 어긋나지 않고, 값 반환이라 "두 번째 호출이 첫 번째 결과를 무효화한다"는 함정도
//   사라진다 (아래 pruneOldLogs가 기준일과 만료일을 연달아 구한다). client의 UiState.cpp가
//   이미 같은 패턴을 쓴다.
bool localTimeOf(std::time_t when, std::tm& out) {
#ifdef _WIN32
    return ::localtime_s(&out, &when) == 0;
#else
    return ::localtime_r(&when, &out) != nullptr;
#endif
}

// tm에서 YYYYMMDD 문자열을 만든다 (실패 시 빈 문자열)
std::string dateStringOf(const std::tm& local) {
    std::array<char, 16> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y%m%d", &local) == 0) {
        return std::string{};
    }
    return std::string{buffer.data()};
}

}  // namespace

Logger& Logger::instance() {
    static Logger logger;  // C++11 매직 스태틱 — 초기화 자체가 스레드 안전
    return logger;
}

bool Logger::openFile(const std::string& baseDir, const std::string& baseName) {
    if (baseName.empty()) {
        return false;
    }

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    const std::lock_guard<std::mutex> lock(_mutex);
    std::tm local{};
    if (!localTimeOf(now, local)) {
        return false;
    }
    const std::string today = dateStringOf(local);
    if (today.empty()) {
        return false;
    }

    // 실패 시 되돌리기 위해 보관한다 → 근거: 이미 파일 싱크가 열려 있는데 두 번째 openFile이
    // 실패하면, 기존 파일은 계속 열려 있는 채로 baseDir만 사라져 회전·롤오버가 불가능해진다
    const std::string previousDir = _baseDir;
    const std::string previousName = _baseName;

    // baseDir이 비면 실행 디렉토리 기준 (path 조립에서 빈 경로는 의도와 다르게 동작한다)
    _baseDir = baseDir.empty() ? std::string{"."} : baseDir;
    _baseName = baseName;
    if (!openForDateLocked(today)) {
        _baseDir = previousDir;
        _baseName = previousName;
        return false;
    }
    _fileRequested = true;
    return true;
}

bool Logger::openForDateLocked(const std::string& date) {
    std::error_code ec;
    const fs::path directory = fs::path{_baseDir} / kLogsFolder / date;
    // 방어 생성 — 이미 있으면 create_directories는 false를 돌려주지만 ec는 비어 있다.
    // 그래서 성공 판정은 반환값이 아니라 ec로만 한다 (놓치기 쉬운 지점)
    fs::create_directories(directory, ec);
    if (ec) {
        return false;
    }

    const fs::path path = directory / _baseName;
    std::ofstream opened{path.string(), std::ios::app};  // 이어쓰기 — 재시작 시 데몬 로그 관례
    if (!opened.is_open()) {
        return false;
    }

    // 새 파일이 열린 뒤에야 기존 싱크를 놓는다 → 근거: 열기 실패 시 이미 쓰던 파일을
    // 잃어버리면 실패 순간의 로그까지 사라진다. 실패해도 기존 싱크가 살아 있어야 한다
    if (_file.is_open()) {
        _file.flush();
        _file.close();
    }
    _file = std::move(opened);
    _toFile = true;
    _activeDate = date;
    _activePath = path.string();

    // 이어쓰기이므로 기존 크기에서 시작한다 (0에서 시작하면 회전이 밀린다 — design 14번)
    const std::uintmax_t existing = fs::file_size(path, ec);
    _bytesWritten = ec ? 0 : existing;
    return true;
}

void Logger::rotateLocked() {
    // 이름을 바꾸려면 먼저 닫아야 한다 (Windows는 열린 파일의 rename을 거부한다)
    _file.flush();
    _file.close();
    _toFile = false;

    const fs::path active{_activePath};
    const fs::path directory = active.parent_path();
    const std::string stem = active.stem().string();
    const std::string extension = active.extension().string();

    bool renamed = false;
    std::error_code ec;
    for (int index = 1; index <= kMaxRotationIndex; ++index) {
        const fs::path candidate = directory / (stem + rotationSuffix(index) + extension);
        if (fs::exists(candidate, ec)) {
            continue;
        }
        fs::rename(active, candidate, ec);
        renamed = !ec;
        break;
    }

    const std::string date = _activeDate;
    openForDateLocked(date);
    if (!renamed) {
        // 회전 실패(번호 소진·권한 등) — 임계를 넘긴 채 계속 쓴다. 카운터를 되돌리지 않으면
        // 줄마다 회전을 재시도해 매번 exists() 스캔이 돌게 된다. 파일이 커지는 편이
        // 로그를 잃거나 느려지는 것보다 낫다 (design 14번: 실패는 삼키고 로깅은 계속)
        _bytesWritten = 0;
    }
}

void Logger::close() {
    const std::lock_guard<std::mutex> lock(_mutex);
    if (_toFile) {
        _file.flush();
        _file.close();
    }
    _toFile = false;
    _fileRequested = false;
    _activeDate.clear();
    _activePath.clear();
    _baseDir.clear();
    _baseName.clear();
    _bytesWritten = 0;
}

std::string Logger::activeFilePath() const {
    const std::lock_guard<std::mutex> lock(_mutex);
    return _toFile ? _activePath : std::string{};
}

std::size_t Logger::pruneOldLogs(std::time_t referenceTime, int keepDays) {
    const std::lock_guard<std::mutex> lock(_mutex);
    if (!_fileRequested || _baseDir.empty() || keepDays < 1) {
        return 0;  // 파일 싱크가 아니면 정리할 대상이 없다
    }

    // 정오로 정규화한 뒤 날짜를 빼는 이유: DST 전환일은 23시간/25시간이라 자정 근처 시각에서
    // 86400을 곱해 빼면 하루가 밀릴 수 있다. 정오 기준이면 그 오차가 날짜를 넘기지 못한다
    std::tm midday{};
    if (!localTimeOf(referenceTime, midday)) {
        return 0;
    }
    midday.tm_hour = 12;
    midday.tm_min = 0;
    midday.tm_sec = 0;
    std::time_t cutoffTime = std::mktime(&midday);
    if (cutoffTime == static_cast<std::time_t>(-1)) {
        return 0;
    }
    cutoffTime -= static_cast<std::time_t>(keepDays - 1) * 24 * 60 * 60;

    std::tm cutoffLocal{};
    if (!localTimeOf(cutoffTime, cutoffLocal)) {
        return 0;
    }
    const std::string cutoff = dateStringOf(cutoffLocal);
    if (cutoff.empty()) {
        return 0;
    }

    std::error_code ec;
    const fs::path root = fs::path{_baseDir} / kLogsFolder;
    std::size_t removed = 0;
    // 순회를 range-for로 쓰지 않는 이유: range-for가 쓰는 operator++는 예외를 던지는
    // 오버로드다. 컨벤션 3번(예외 대신 반환값)을 지키려면 increment(ec)를 직접 돌려야 한다.
    // directory_iterator의 error_code 생성자는 디렉토리가 없어도 end()와 같아져 빈 순회가 된다
    fs::directory_iterator it{root, ec};
    const fs::directory_iterator end;
    while (!ec && it != end) {
        const fs::path entry = it->path();
        const std::string name = entry.filename().string();
        std::error_code entryEc;
        const bool isDirectory = fs::is_directory(entry, entryEc);

        // logs/ 아래에서 우리가 만든 날짜 디렉토리만 건드린다. 활성 날짜는 절대 제외 —
        // 활성 파일이 그 안에 있다 (design 14번). YYYYMMDD는 고정 폭이라 문자열 비교가
        // 곧 날짜 비교다
        if (isDirectory && !entryEc && isDateName(name) && name != _activeDate && name < cutoff) {
            std::error_code removeEc;
            fs::remove_all(entry, removeEc);
            if (!removeEc) {
                ++removed;
            }
        }
        it.increment(ec);
    }
    return removed;
}

void Logger::log(LogLevel level, std::string_view message) {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    const std::lock_guard<std::mutex> lock(_mutex);
    std::tm local{};
    std::array<char, 32> prefix{};
    std::string today;
    if (localTimeOf(now, local)) {
        if (std::strftime(prefix.data(), prefix.size(), "[%Y-%m-%d %H:%M:%S]", &local) == 0) {
            prefix[0] = '\0';  // 시계 이상 시에도 메시지는 잃지 않는다
        }
        today = dateStringOf(local);
    }

    std::string line;
    line.reserve(prefix.size() + message.size() + 16);
    line += prefix.data();
    line += " [";
    line += levelTag(level);
    line += "] ";
    line += message;
    line += '\n';

    if (_fileRequested) {
        // 자정 롤오버 — 날짜가 바뀌면 새 날짜 디렉토리로 옮겨 쓴다 (데몬은 며칠 연속 돈다)
        if (_toFile && !today.empty() && today != _activeDate) {
            openForDateLocked(today);
        }
        // 끊긴 싱크 복구 — 외부에서 logs/를 지웠거나 직전 쓰기가 실패한 경우.
        // "로그를 남길 때 디렉토리가 없으면 생성한다"는 요구가 여기서 충족된다
        if (!_toFile) {
            openForDateLocked(today.empty() ? _activeDate : today);
        }
        if (_toFile && _bytesWritten + line.size() > kMaxLogFileBytes) {
            rotateLocked();
        }
    }

    if (!_toFile) {
        std::cout << line;
        std::cout.flush();
        return;
    }

    _file << line;
    // 매 줄 플러시: 데몬이 크래시해도 직전 로그가 남아야 원인 추적 가능.
    // 로그는 핫 패스 금지 규칙(컨벤션 8번) 위라 플러시 비용은 무시 가능
    _file.flush();
    if (!_file.fail()) {
        _bytesWritten += line.size();
        return;
    }

    // 쓰기 실패 — 싱크를 놓고 한 번만 다시 열어 같은 줄을 재시도한다.
    // 여기서도 실패하면 이 줄은 잃되 프로세스는 계속 간다 (로깅이 서버를 죽이면 안 된다)
    _file.clear();
    _file.close();
    _toFile = false;
    if (openForDateLocked(today.empty() ? _activeDate : today)) {
        _file << line;
        _file.flush();
        if (!_file.fail()) {
            _bytesWritten += line.size();
        }
    }
}

void Logger::flush() {
    const std::lock_guard<std::mutex> lock(_mutex);
    if (_toFile) {
        _file.flush();
    } else {
        std::cout.flush();
    }
}

}  // namespace common
