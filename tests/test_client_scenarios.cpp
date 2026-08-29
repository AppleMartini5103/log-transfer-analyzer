// 클라이언트 강제 단절·취소 시나리오 — 과제 평가 기준 "네트워크 강제 단절 시 양쪽 모두
// 안전한 자원 해제"의 클라이언트 쪽 증빙.
//
// [왜 GUI 없이 TransferService를 직접 구동하는가]
//   ① 사람이 GUI를 조작한 검증은 제출물에 남지 않는다. 자동 테스트는 채점자가 실행해
//      확인할 수 있는 산출물이 된다.
//   ② Linux 빌드에도 포함되므로 이 경로를 ASan/valgrind로 돌려 누수 0을 증빙할 수 있다.
//   ③ TransferService는 ImGui·Win32에 의존하지 않는다 (design 6번 "Application은 조립만").
//   ※ UI 프리즈 여부·진행률 표시의 육안 확인은 이 테스트가 대체하지 못한다 (design 17번).
//
// [서버를 직접 흉내내는 이유]
//   실제 SessionManager는 Linux 전용이라 양 플랫폼 테스트에 쓸 수 없고, 무엇보다
//   "언제 끊을지"를 테스트가 정해야 한다. 강제 단절은 정상 서버가 하지 않는 행동이다.

#include "service/TransferService.h"

#include "net/Listener.h"
#include "net/TcpSocket.h"
#include "protocol/Codec.h"
#include "util/Crc32.h"

#include "TestLoop.h"

#include <catch_amalgamated.hpp>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace proto = common::protocol;

using client::LinkState;
using client::SessionState;
using client::TransferService;
using common::net::IListenerCallback;
using common::net::ISocket;
using common::net::ISocketCallback;
using common::net::Listener;
using common::net::WritableBuffer;
using testsupport::runUntil;

namespace {

constexpr const char* kUploadName = "scenario.log";
// ── 업로드 크기 두 종류 ──────────────────────────────────────────────────────
//
// 끊기는 시나리오(dropOnFirstPayload)는 서버가 첫 페이로드 조각에서 끊으므로 작아도 된다.
constexpr std::size_t kSmallUpload = 2 * 1024 * 1024;
//
// 반면 "업로드를 정지시킨 채 Cancel/Disconnect/종료를 넣는" 시나리오는 크기가 결정적이다.
// 클라이언트가 흘려보낼 수 있는 총량 = 링버퍼 예산 4MB + 미완료 쓰기 상한 8x64KB
//                                    + 커널 소켓 버퍼(송신+수신)
// 이보다 작으면 서버가 읽지 않아도 업로드가 그냥 끝나버려 _uploading=false가 되고, Cancel은
// 정상적으로 무시된다 — 제품이 옳고 테스트가 틀린 상황이 된다.
// 실제로 2MB로 잡았다가 Linux에서 이 경쟁에 걸렸다. 2MB는 링 예산 4MB보다도 작아서
// backpressure가 아예 발생하지 않았고, Windows에서 통과한 것은 커널 버퍼가 작아 우연히
// 정지했던 것뿐이다. 링·커널 버퍼를 모두 넘도록 여유 있게 잡는다.
constexpr std::size_t kLargeUpload = 32 * 1024 * 1024;

// 서버 역할 최소 구현. 바이트를 세기만 하고, 응답·단절 시점은 테스트가 정한다.
// 헤더/트레일러를 파싱하지 않는 이유: 파일 크기와 파일명을 테스트가 알고 있어 총
// 기대 바이트 수를 계산할 수 있다. 파싱을 넣으면 검증 대상이 아닌 코드가 늘어난다.
class FakeServer : public IListenerCallback, public ISocketCallback {
public:
    // 업로드를 다 받았을 때 무엇을 할지
    enum class OnUploadComplete {
        DropSilently,     // 아무 응답 없이 끊는다 (WAIT_ACK 중 단절)
        AckThenDrop,      // Ack만 보내고 끊는다 (WAIT_RESULT 중 단절)
        FullResult,       // Ack + ResultHeader + CSV — 정상 완주
        OversizedResult,  // ResultHeader에 상한 초과 csvSize를 선언한다 (본문은 보내지 않는다)
        RejectUpload,     // 실패 사유를 담은 Ack를 보내고 닫는다 (design 8번: 사유를 알리고 종료)
    };

    explicit FakeServer(uv_loop_t* loop) : _listener(loop) {}

    int listen() { return _listener.listen("127.0.0.1", 0, 16, this); }
    std::uint16_t port() const { return _listener.boundPort(); }

    // 다음 세션을 위해 수신 상태만 초기화한다 (리스너는 계속 살아 있다)
    void resetSession() {
        received.clear();
        _responded = false;
    }

    void closeAccepted() {
        if (_accepted && !_accepted->isClosing()) {
            _accepted->close();
        }
    }

    // ── IListenerCallback ──
    void onConnection() override {
        _accepted = _listener.acceptPending();
        if (_accepted) {
            _accepted->setCallback(this);
            _accepted->startRead();
            ++connections;
        }
    }
    void onListenError(int status) override { lastListenError = status; }

    // ── ISocketCallback (수락된 소켓) ──
    WritableBuffer onAllocate(std::size_t suggested) override {
        _scratch.resize(suggested > 0 ? suggested : 1);
        return WritableBuffer{_scratch.data(), _scratch.size()};
    }

    void onRead(std::string_view data) override {
        received.append(data);

        // 페이로드가 시작되자마자 끊는 정책 (업로드 중 강제 단절)
        if (dropOnFirstPayload && received.size() > headerSize()) {
            dropOnFirstPayload = false;
            closeAccepted();
            return;
        }

        // 헤더를 받은 뒤 읽기를 멈춰 업로드를 정지시킨다 (kLargeUpload와 짝을 이룬다).
        //
        // 왜 필요한가: "업로드 중"에 Cancel·Disconnect·종료를 넣는 시나리오는 그 순간 실제로
        // 업로드가 진행 중이어야 성립한다. 읽기를 멈추면 소켓 버퍼가 차고, 링버퍼가 차고,
        // 미완료 쓰기가 상한(8청크)에 묶여 업로드가 멈춘 채로 남는다.
        // 단 이것만으로는 부족하다 — 파일이 링 예산(4MB)과 커널 버퍼보다 작으면 서버가 읽지
        // 않아도 전부 흘러들어가 업로드가 끝난다. 그래서 크기(kLargeUpload)와 함께 써야 한다.
        if (stallAfterHeader && received.size() >= headerSize() && _accepted &&
            _accepted->isReading()) {
            _accepted->stopRead();
            return;
        }

        if (_responded) {
            // 응답을 이미 보냈다 — 이제 남은 것은 클라이언트의 DownloadDone뿐이다.
            // 그걸 받으면 닫는다: 실제 서버가 1:1 정책상 하는 일과 같다
            // (design 11번 WAIT_DONE → CLEANUP — CLEANUP이 accept를 재개하는 유일한 지점이라
            //  닫지 않을 수 없다). 이 동작을 흉내내지 않으면 "완료 직후 서버가 끊는다"를
            //  전제로 한 클라이언트 경로(EOF Info 분류, Save 게이팅)를 검증할 수 없다.
            if (received.size() >= totalUploadBytes() + proto::kDownloadDoneSize) {
                closeAccepted();
            }
            return;
        }
        if (received.size() < totalUploadBytes()) {
            return;  // 아직 업로드가 다 도착하지 않았다
        }
        _responded = true;
        respond();
    }

    void onSendComplete(int) override {}
    void onError(int status, std::string_view) override { lastError = status; }
    void onClosed() override { _accepted.reset(); }

    // 테스트가 설정하는 정책
    OnUploadComplete policy = OnUploadComplete::FullResult;
    bool dropOnFirstPayload = false;
    bool stallAfterHeader = false;
    std::uint64_t uploadSize = kSmallUpload;
    proto::AckStatus rejectStatus = proto::AckStatus::CrcMismatch;  // RejectUpload일 때 실을 사유
    std::string csv = "module,hour,count\nRadarTrackNodeState,2026-06-19 22,7\n";

    std::string received;
    int connections = 0;
    int lastError = 0;
    int lastListenError = 0;

private:
    std::size_t headerSize() const {
        return proto::kUploadHeaderFixedSize + std::string(kUploadName).size();
    }
    std::size_t totalUploadBytes() const {
        return headerSize() + static_cast<std::size_t>(uploadSize) + proto::kUploadTrailerSize;
    }

    void sendBytes(const std::vector<char>& bytes) {
        if (_accepted) {
            _accepted->send(std::string_view(bytes.data(), bytes.size()));
        }
    }

    void respond() {
        if (policy == OnUploadComplete::DropSilently) {
            closeAccepted();
            return;
        }

        if (policy == OnUploadComplete::RejectUpload) {
            // 실서버는 조용히 끊지 않고 사유를 담은 Ack를 보낸 뒤 닫는다
            // (design 8번 근거 3: 클라이언트 로그에 원인이 찍혀야 "왜 죽었지?"가 되지 않는다).
            proto::Ack rejected;
            rejected.status = rejectStatus;
            rejected.receivedBytes = uploadSize;
            sendBytes(proto::encode(rejected));
            closeAccepted();
            return;
        }

        proto::Ack ack;
        ack.status = proto::AckStatus::Ok;
        ack.receivedBytes = uploadSize;
        sendBytes(proto::encode(ack));

        if (policy == OnUploadComplete::AckThenDrop) {
            closeAccepted();
            return;
        }

        proto::ResultHeader header;

        if (policy == OnUploadComplete::OversizedResult) {
            // 상한을 딱 1바이트 넘긴다 — 경계에서 통과/거부가 뒤집히는지까지 본다.
            // 본문은 보내지 않는다: 클라이언트는 헤더만 보고 거부해야 하며, 실제 공격자도
            // 선언한 만큼 보낼 의무가 없다(오히려 큰 값을 선언해 할당만 유도하는 것이 목적이다).
            header.csvSize = proto::kMaxCsvSize + 1;
            header.crc32 = 0;
            sendBytes(proto::encode(header));
            return;
        }

        header.csvSize = csv.size();
        header.crc32 = common::crc32(0, csv);
        sendBytes(proto::encode(header));
        if (_accepted) {
            _accepted->send(csv);
        }
    }

    Listener _listener;
    std::unique_ptr<ISocket> _accepted;
    std::vector<char> _scratch;
    bool _responded = false;
};

// 픽스처 디렉토리 — OS 임시 디렉토리 아래에 프로세스 전용으로 만든다.
// → 왜 실행 디렉토리가 아닌가: 이 파일들은 최대 32MB다. 저장소 루트에서 테스트를 돌리면
//   루트에 32MB 바이너리가 생기고, 프로세스가 비정상 종료하면(크래시·Ctrl-C·새니타이저
//   중단) 소멸자가 돌지 않아 그대로 남는다. .gitignore에 없으면 커밋 후보로 올라오고,
//   실제로 이슈 46의 SIGPIPE 즉사가 34MB를 루트에 남겼다. 산출물의 위치는 테스트가
//   책임질 몫이며, 임시 파일은 OS 임시 디렉토리에 두는 것이 그 답이다.
// → 이름에 PID를 붙이는 이유: 같은 머신에서 두 실행이 겹쳐도 서로의 픽스처를 지우지 않는다.
struct FixtureDir {
    FixtureDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path(ec) /
               ("byda-client-tests-" + std::to_string(currentProcessId()));
        std::filesystem::create_directories(path, ec);
        REQUIRE_FALSE(ec);
    }

    ~FixtureDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);  // 실패는 삼킨다 — 임시 디렉토리는 OS가 정리한다
    }

    static long currentProcessId() {
#ifdef _WIN32
        return static_cast<long>(::_getpid());
#else
        return static_cast<long>(::getpid());
#endif
    }

    std::filesystem::path path;
};

// 프로세스 수명 동안 하나 — 종료 시 디렉토리째 삭제된다.
const std::filesystem::path& fixtureDir() {
    static FixtureDir dir;
    return dir.path;
}

std::string fixturePath(const std::string& name) {
    return (fixtureDir() / name).string();
}

// 업로드용 임시 파일. 내용은 검증 대상이 아니므로(서버가 바이트만 센다) 단순 패턴으로 채운다.
std::string writeTempFile(const std::string& path, std::size_t bytes) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    REQUIRE(out.is_open());
    const std::string block(64 * 1024, 'A');
    std::size_t written = 0;
    while (written < bytes) {
        const std::size_t take = std::min(block.size(), bytes - written);
        out.write(block.data(), static_cast<std::streamsize>(take));
        written += take;
    }
    out.close();
    return path;
}

// 정지 시나리오용 대용량 파일은 프로세스에서 한 번만 만든다.
// → 왜: 테스트마다 32MB를 쓰면 반복 실행에서 디스크가 병목이 된다. 16MB를 6개 테스트 x 40회
//   반복하다 3.8GB를 쓰고 10분 타임아웃을 실제로 겪었다. 내용은 검증 대상이 아니라 공유해도 된다.
struct SharedLargeFile {
    SharedLargeFile() { writeTempFile(path, kLargeUpload); }
    ~SharedLargeFile() { std::remove(path.c_str()); }
    std::string path = fixturePath("client_scenario_large.bin");
};

const std::string& largeUploadPath() {
    static SharedLargeFile file;  // 첫 사용 시 생성, 프로세스 종료 시 삭제
    return file.path;
}

// 테스트 하네스: 가짜 서버 루프(이 스레드) + TransferService(자기 스레드)를 실제 TCP로 잇는다.
struct Harness {
    explicit Harness(std::size_t bytes = kSmallUpload) : uploadBytes(bytes) {
        REQUIRE(uv_loop_init(&loop) == 0);
        server = std::make_unique<FakeServer>(&loop);
        server->uploadSize = bytes;
        REQUIRE(server->listen() == 0);
        REQUIRE(server->port() != 0);
        std::string error;
        REQUIRE(service.start(error));

        if (bytes == kLargeUpload) {
            filePath = largeUploadPath();  // 공유 파일 — 지우지 않는다
            _ownsFile = false;
        } else {
            writeTempFile(filePath, bytes);
        }
    }

    ~Harness() {
        service.stop();  // QuitCommand -> 핸들 close -> uv_run 반환 -> join

        // ★ 순서가 중요하다: 서버를 먼저 놓고 나서 루프를 닫는다.
        //   FakeServer가 Listener와 수락된 소켓의 uv 핸들을 들고 있어, 루프를 먼저 닫으면
        //   그 소멸자들이 이미 닫힌 루프에 uv_close를 걸어 SIGSEGV가 난다. 소멸자 본문은
        //   멤버 소멸보다 먼저 실행되므로 여기서 명시적으로 놓아줘야 순서가 보장된다
        //   (test_net.cpp는 LoopFixture를 먼저 선언해 같은 순서를 얻는다).
        server->closeAccepted();
        server.reset();

        // 남은 핸들을 모두 닫고 루프를 정리한다 (테스트 간 자원 누수 방지)
        uv_walk(
            &loop,
            [](uv_handle_t* handle, void*) {
                if (!uv_is_closing(handle)) {
                    uv_close(handle, nullptr);
                }
            },
            nullptr);
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
        if (_ownsFile) {
            std::remove(filePath.c_str());
        }
    }

    // 워커가 쌓아둔 이벤트를 흡수해 누적 상태로 반영한다 (UI 스레드가 매 프레임 하는 일).
    void absorb() {
        for (const auto& event : service.drainEvents()) {
            if (event.hasLink) {
                link = event.link;
                linkHistory.push_back(event.link);
            }
            if (event.hasSession) {
                session = event.session;
                sessionHistory.push_back(event.session);
            }
            if (event.hasUploadProgress) {
                uploadProgress = event.uploadProgress;
            }
            if (!event.message.empty()) {
                logs.push_back(event.message);
                if (event.level == common::LogLevel::Error) {
                    ++errorLogs;
                }
            }
        }
    }

    // 조건이 만족될 때까지 서버 루프를 돌리며 이벤트를 흡수한다.
    // runUntil이 predicate를 uv_run 전후로 호출하므로 흡수를 predicate 안에서 한다.
    template <typename Predicate>
    bool waitFor(Predicate predicate, std::uint64_t timeoutMs = 10000) {
        return runUntil(
            &loop,
            [&] {
                absorb();
                return predicate();
            },
            timeoutMs);
    }

    bool sawLog(const std::string& needle) {
        absorb();
        for (const auto& line : logs) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    // 실패 시 무슨 일이 있었는지 보이게 한다 — 상태값만 보면 원인 추적이 불가능하다
    std::string allLogs() {
        absorb();
        std::string joined;
        for (const auto& line : logs) {
            joined += "\n    " + line;
        }
        return joined;
    }

    // "세션이 끝나고 UI가 다시 조작 가능한 상태"를 기다린다.
    // 두 조건을 함께 기다려야 한다 — session(Idle)과 link(Disconnected)는 서로 다른
    // 콜백에서 각각 push되므로(abortUpload가 session, onClosed가 link) 한쪽만 기다리면
    // 다른 쪽이 아직 도착하지 않은 순간을 검사하게 된다.
    bool waitForReusable(std::uint64_t timeoutMs = 30000) {
        return waitFor(
            [&] { return link == LinkState::Disconnected && session == SessionState::Idle; },
            timeoutMs);
    }

    // 이벤트가 quietMs 동안 더 들어오지 않을 때까지 기다린다.
    //
    // 왜 필요한가: link==Disconnected와 session==Idle은 소켓이 실제로 닫히기 "전에" 이미
    // push된다(onError가 링크를, abortUpload가 세션을 올리고 그 뒤에 closeSocket이 시작된다).
    // 그 순간 곧바로 Connect를 보내면 워커는 아직 _socket을 들고 있어 "Already connected or
    // connecting"으로 거절한다. 실제 UI에서는 사람이 그 속도로(한 루프 반복 이내) 누를 수
    // 없지만 테스트는 즉시 보내므로, 소켓이 실제로 놓일 때까지 가라앉기를 기다려야 한다.
    void settle(std::uint64_t quietMs = 200) {
        const auto counted = [&] { return logs.size() + linkHistory.size() + sessionHistory.size(); };
        std::uint64_t quietSince = uv_hrtime();
        std::size_t last = counted();
        for (;;) {
            uv_run(&loop, UV_RUN_NOWAIT);
            absorb();
            const std::size_t now = counted();
            if (now != last) {
                last = now;
                quietSince = uv_hrtime();
            } else if ((uv_hrtime() - quietSince) / 1000000ULL >= quietMs) {
                return;
            }
            uv_sleep(1);
        }
    }

    // 업로드가 "정지한 채 진행 중"임을 확정한다.
    //
    // 왜 단정까지 하는가: 정지 수단(서버가 읽기를 멈춤 + 링·커널 버퍼를 넘는 크기)이 어긋나면
    // 업로드가 조용히 완료되고, 그러면 Cancel은 정상적으로 무시된다. 그 상태로 "cancelled by
    // user" 로그를 기다리면 10초 타임아웃만 나고 원인을 알 수 없다 — Linux에서 정확히 그렇게
    // 헤맸다. 여기서 걸리면 실패 메시지가 원인을 바로 가리킨다.
    void requireUploadStalled() {
        REQUIRE(waitFor([&] { return session == SessionState::Streaming; }));
        settle();  // 진행률 이벤트가 멈출 때까지 = 더 밀어낼 수 없는 상태
        INFO("session=" << static_cast<int>(session) << " progress=" << uploadProgress
                        << " logs:" << allLogs());
        REQUIRE(session == SessionState::Streaming);  // 완료됐다면 정지 수단이 깨진 것이다
    }

    void connect() {
        // 이력으로 판정한다 — link는 "마지막 값"이라, 직전 소켓의 늦은 Disconnected가 같은
        // drain 배치에 섞이면 Connected를 지나쳐 버린다 (absorb는 배치를 한 번에 처리한다).
        linkHistory.clear();
        service.post(client::ConnectCommand{"127.0.0.1", server->port()});
        const bool connected = waitFor([&] {
            return std::find(linkHistory.begin(), linkHistory.end(), LinkState::Connected) !=
                   linkHistory.end();
        });
        // 진단은 실패 직전에 만들어야 한다 — INFO는 매크로 실행 시점에 문자열을 확정하므로
        // REQUIRE 뒤에 두면 낡은 값이 찍힌다 (실제로 그렇게 헤맸다).
        INFO("connect() link=" << static_cast<int>(link) << " logs:" << allLogs());
        REQUIRE(connected);
    }

    void startUpload() {
        service.post(client::StartUploadCommand{filePath, kUploadName, uploadBytes});
    }

    uv_loop_t loop{};
    std::unique_ptr<FakeServer> server;
    TransferService service;

    std::size_t uploadBytes = kSmallUpload;
    std::string filePath = fixturePath("client_scenario_upload.bin");

    LinkState link = LinkState::Disconnected;
    SessionState session = SessionState::Idle;
    float uploadProgress = 0.0f;
    int errorLogs = 0;
    std::deque<std::string> logs;
    std::deque<LinkState> linkHistory;
    std::deque<SessionState> sessionHistory;
    bool _ownsFile = true;
};

// 세션을 흔드는 경로가 끝난 뒤 "UI가 다시 조작 가능한 상태로 돌아왔는가".
// design 12번: 끊김은 곧 연결 해제이므로 회색 인디케이터 + Connect만 열린 상태여야 한다.
// 이것이 무너지면 어떤 버튼도 눌리지 않아 사용자가 앱을 재시작해야 한다.
void requireReusableIdleState(Harness& harness) {
    client::UiState ui;
    ui.link = harness.link;
    ui.session = harness.session;
    ui.filePath = harness.filePath;

    INFO("link=" << static_cast<int>(harness.link)
                 << " session=" << static_cast<int>(harness.session));
    REQUIRE(harness.link == LinkState::Disconnected);
    REQUIRE(harness.session == SessionState::Idle);
    REQUIRE(ui.canConnect());       // 사용자가 다시 붙을 수 있어야 한다
    REQUIRE(ui.canEditAddress());
    REQUIRE_FALSE(ui.canSend());    // 끊긴 상태에서 Send는 잠겨 있어야 한다
    REQUIRE_FALSE(ui.canCancelUpload());
}

}  // namespace

TEST_CASE("client scenario: server killed mid-upload releases resources and reopens Connect") {
    Harness harness;
    harness.server->dropOnFirstPayload = true;  // 페이로드 첫 조각에서 끊는다

    harness.connect();
    harness.startUpload();

    const bool reusable = harness.waitForReusable();
    INFO("link=" << static_cast<int>(harness.link) << " session="
                 << static_cast<int>(harness.session) << " logs:" << harness.allLogs());
    REQUIRE(reusable);

    // 업로드 중 단절은 Error로 남아야 한다 (design 12번 EOF 3분류의 ③).
    // 구체적 문구는 단정하지 않는다 — 끊긴 소켓에 쓰는 중이면 EOF보다 쓰기 실패가 먼저
    // 도착할 수 있어(플랫폼별로 ECONNRESET/EPIPE) 사유 문구가 갈린다. 요구되는 성질은
    // "Error로 남고, 세션이 정리되고, 다시 조작 가능해진다"는 것이다.
    REQUIRE(harness.errorLogs > 0);

    // 진행률이 0으로 되돌아가야 한다 — 실패한 전송의 막대가 남아 있으면 화면이 거짓말을 한다
    REQUIRE(harness.uploadProgress == 0.0f);
    requireReusableIdleState(harness);
}

TEST_CASE("client scenario: cancelling an upload converges on the same cleanup path") {
    Harness harness{kLargeUpload};  // 링 예산·커널 버퍼를 넘겨야 정지가 성립한다
    harness.server->stallAfterHeader = true;  // 업로드를 정지시켜 "전송 중"을 확정한다

    harness.connect();
    harness.startUpload();
    harness.requireUploadStalled();

    harness.service.post(client::CancelUploadCommand{});

    // 취소는 사용자 의도이므로 Warn이다 (에러가 아니다 — design 7번: 경로는 같고 표기는 다르다)
    INFO("cancel logs:" << harness.allLogs());
    REQUIRE(harness.waitFor([&] { return harness.sawLog("cancelled by user"); }));
    const bool reusable = harness.waitForReusable();
    INFO("link=" << static_cast<int>(harness.link) << " session="
                 << static_cast<int>(harness.session) << " logs:" << harness.allLogs());
    REQUIRE(reusable);
    REQUIRE(harness.uploadProgress == 0.0f);
    requireReusableIdleState(harness);
}

TEST_CASE("client scenario: disconnect during an upload cancels it too") {
    // Cancel 버튼이 아니라 Disconnect로 끊는 경로. design 7번의 "취소·에러·타임아웃이 모두
    // 같은 CLEANUP으로 수렴한다"가 이 경로에서도 성립해야 한다.
    Harness harness{kLargeUpload};  // 링 예산·커널 버퍼를 넘겨야 정지가 성립한다
    harness.server->stallAfterHeader = true;  // 업로드를 정지시켜 "전송 중"을 확정한다

    harness.connect();
    harness.startUpload();
    harness.requireUploadStalled();

    harness.service.post(client::DisconnectCommand{});

    const bool reusable = harness.waitForReusable();
    INFO("link=" << static_cast<int>(harness.link) << " session="
                 << static_cast<int>(harness.session) << " logs:" << harness.allLogs());
    REQUIRE(reusable);
    requireReusableIdleState(harness);
}

TEST_CASE("client scenario: connection lost while waiting for the result unlocks the UI") {
    // ★ 여기가 가장 놓치기 쉬운 경로다. 업로드는 성공했고 Ack까지 받았는데 서버가 결과를
    //   주지 않고 끊는 경우다. 이때 세션 상태가 WaitResult에 남으면 링크는 Disconnected인데
    //   session이 "바쁨"이라 canConnect()·canDisconnect()·canSend()가 전부 false가 되어
    //   사용자가 앱을 재시작하는 수밖에 없다.
    Harness harness;
    harness.server->policy = FakeServer::OnUploadComplete::AckThenDrop;

    harness.connect();
    harness.startUpload();

    // Ack까지는 정상 진행
    REQUIRE(harness.waitFor([&] { return harness.sawLog("waiting for ack"); }, 30000));
    const bool reusable = harness.waitForReusable();
    INFO("link=" << static_cast<int>(harness.link) << " session="
                 << static_cast<int>(harness.session) << " logs:" << harness.allLogs());
    REQUIRE(reusable);
    requireReusableIdleState(harness);
}

TEST_CASE("client scenario: the service is reusable after a forced disconnect") {
    // 강제 단절 후 다시 붙어 세션을 완주할 수 있어야 한다 — 자원이 실제로 반환됐다는 증거.
    Harness harness;
    harness.server->dropOnFirstPayload = true;

    harness.connect();
    harness.startUpload();
    const bool reusable = harness.waitForReusable();
    INFO("link=" << static_cast<int>(harness.link) << " session="
                 << static_cast<int>(harness.session) << " logs:" << harness.allLogs());
    REQUIRE(reusable);
    requireReusableIdleState(harness);

    // 2회차: 정상 완주. 직전 소켓이 실제로 놓일 때까지 기다린 뒤 다시 붙는다 (settle 주석 참조)
    harness.settle();
    harness.server->resetSession();
    harness.server->dropOnFirstPayload = false;
    harness.server->policy = FakeServer::OnUploadComplete::FullResult;

    harness.connect();
    harness.startUpload();

    REQUIRE(harness.waitFor([&] { return harness.session == SessionState::Done; }, 60000));
    REQUIRE(harness.service.takeResultCsv() == harness.server->csv);
    REQUIRE(harness.server->connections == 2);  // 두 번째 연결이 실제로 수락됐다
}

TEST_CASE("client scenario: the result stays saveable after the server closes") {
    // 정상 완주 직후 서버는 1:1 정책상 곧바로 연결을 닫는다(design 11번: CLEANUP이 accept를
    // 재개하는 유일한 지점). 그래서 화면에는 "링크는 Disconnected인데 Save만 활성"인 조합이
    // 남는다. 이 조합이 의도된 것임을 여기서 고정한다.
    //
    // 왜 테스트로 묶는가: Save를 링크 상태에 묶으면 서버가 같은 순간에 닫으므로 사람이 그보다
    // 빨리 누를 수 없어 Save가 사실상 못 쓰는 버튼이 된다. 즉 이건 취향이 아니라 프로토콜이
    // 강제하는 요구사항이고, 무심코 canSaveResult()에 링크 조건을 더하면 조용히 깨진다.
    Harness harness;
    harness.server->policy = FakeServer::OnUploadComplete::FullResult;

    harness.connect();
    harness.startUpload();
    REQUIRE(harness.waitFor([&] { return harness.session == SessionState::Done; }, 60000));

    // 완료 로그에 소요 시간·처리량이 실려야 한다 (커밋 [41]).
    // 포맷을 테스트로 고정하는 이유: 이 값은 사람이 로그를 읽어 성능을 판단하는 유일한 근거라,
    // 문구를 손대다 조용히 사라지면 다음 측정에서 다시 타임스탬프로 추정하게 된다.
    INFO("logs:" << harness.allLogs());
    REQUIRE(harness.sawLog("Upload finished"));
    REQUIRE(harness.sawLog(" in "));     // "... bytes in 4.21s, ..."
    REQUIRE(harness.sawLog("MB/s"));
    REQUIRE(harness.sawLog("crc32="));   // 기존 정보가 밀려나지 않았다

    // 서버가 닫는 것을 기다린다 — 완료 직후이므로 EOF는 Info로 분류돼야 한다 (design 12번)
    REQUIRE(harness.waitFor([&] { return harness.link == LinkState::Disconnected; }));
    INFO("logs:" << harness.allLogs());
    REQUIRE(harness.sawLog("Server closed the connection."));

    client::UiState ui;
    ui.link = harness.link;
    ui.session = harness.session;

    REQUIRE(ui.canSaveResult());                        // ★ 끊긴 뒤에도 저장할 수 있어야 한다
    REQUIRE(harness.service.takeResultCsv() == harness.server->csv);  // 내용도 온전하다
    REQUIRE(ui.canConnect());                           // 다시 붙는 것도 열려 있다
    REQUIRE_FALSE(ui.canSend());                        // 연결이 없으니 Send는 잠긴다
    REQUIRE_FALSE(ui.canDisconnect());
}

TEST_CASE("client scenario: an oversized declared csvSize is rejected, not allocated") {
    // 서버가 ResultHeader에 상한을 넘는 csvSize를 선언하는 경우.
    //
    // 왜 이 테스트가 필요한가: 종전 코드는 그 값을 그대로 _csv.reserve()에 넘겼다. 상대가 보낸
    // u64 하나로 std::length_error/bad_alloc이 나고 잡는 곳이 없어 프로세스가 죽는다 — 즉
    // "서버가 이상해지면 클라이언트가 죽는" 결합이었다. 상한 검증은 그 결합을 끊는다.
    //
    // 통과 조건이 "죽지 않는다"에서 멈추지 않는 이유: 거부한 뒤 UI가 다시 조작 가능해야
    // 비로소 방어가 완결된다. WaitResult에 멈춰 있으면 사용자는 앱을 재시작해야 한다.
    Harness harness;
    harness.server->policy = FakeServer::OnUploadComplete::OversizedResult;

    harness.connect();
    harness.startUpload();

    const bool reusable = harness.waitForReusable();
    INFO("link=" << static_cast<int>(harness.link) << " session="
                 << static_cast<int>(harness.session) << " logs:" << harness.allLogs());
    REQUIRE(reusable);
    requireReusableIdleState(harness);

    // 실패 사유가 로그에 남아야 한다 — 사용자가 "왜 결과를 못 받았는지" 알 유일한 경로다
    REQUIRE(harness.sawLog("oversized result.csv"));
    REQUIRE(harness.errorLogs > 0);

    // 거부했으므로 저장할 결과는 없다 (Done으로 가지 않았다)
    REQUIRE(harness.service.takeResultCsv().empty());
}

TEST_CASE("client scenario: a failure Ack is reported with its reason and unlocks the UI") {
    // 서버가 업로드를 거부하는 경로. 구현은 TransferService에 있었지만 한 번도 실행된 적이
    // 없었다 — FakeServer가 언제나 AckStatus::Ok만 보냈기 때문이다. 서버 쪽은 test_session이
    // CrcMismatch·ProtocolError 응답을 이미 검증하는데 클라이언트 쪽만 비어 있었다.
    //
    // 네 상태를 모두 도는 이유: 사유 문자열이 상태마다 다르고, 그것이 design 8번 근거 3의
    // 요구다 — 사용자가 "왜 실패했는지"를 로그에서 읽을 수 있어야 한다. SizeMismatch인데
    // "CRC mismatch"라고 찍히면 통과하지만 요구는 깨진다. 매핑까지 봐야 하는 이유다.
    struct Case {
        proto::AckStatus status;
        const char* reason;
    };
    const Case cases[] = {
        {proto::AckStatus::CrcMismatch, "CRC mismatch"},
        {proto::AckStatus::SizeMismatch, "size mismatch"},
        {proto::AckStatus::ProtocolError, "protocol error"},
        {proto::AckStatus::ServerError, "server error"},
    };

    for (const Case& c : cases) {
        INFO("status=" << static_cast<int>(c.status) << " expecting reason '" << c.reason << "'");
        Harness harness;
        harness.server->policy = FakeServer::OnUploadComplete::RejectUpload;
        harness.server->rejectStatus = c.status;

        harness.connect();
        harness.startUpload();

        const bool reusable = harness.waitForReusable();
        INFO("link=" << static_cast<int>(harness.link) << " session="
                     << static_cast<int>(harness.session) << " logs:" << harness.allLogs());
        REQUIRE(reusable);
        requireReusableIdleState(harness);

        // 사유가 그대로 노출되어야 한다 — 조용히 실패하면 사용자는 원인을 알 수 없다
        REQUIRE(harness.sawLog(std::string("Server rejected the upload: ") + c.reason));
        REQUIRE(harness.errorLogs > 0);

        // 거부됐으므로 저장할 결과가 없다
        REQUIRE(harness.service.takeResultCsv().empty());
    }
}

TEST_CASE("client scenario: quitting mid-upload joins every thread cleanly") {
    // 소멸자(=stop())가 업로드 중에도 파일 리더와 루프 스레드를 정리해야 한다.
    // design 7번 스레드 총괄표의 "종료" 칸이 비어 있으면 여기서 걸린다 (H-1 유형).
    // 정리가 실패하면 join이 걸려 이 테스트가 타임아웃으로 죽는다 — 통과 자체가 검증이다.
    Harness harness{kLargeUpload};  // 링 예산·커널 버퍼를 넘겨야 정지가 성립한다
    harness.server->stallAfterHeader = true;  // 업로드를 정지시켜 "전송 중"을 확정한다

    harness.connect();
    harness.startUpload();
    harness.requireUploadStalled();

    harness.service.stop();
    harness.service.stop();  // 여러 번 불러도 안전해야 한다 (stop 계약)
    SUCCEED("stop() returned without hanging");
}
