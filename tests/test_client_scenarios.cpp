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

#include <algorithm>
#include <cstdio>
#include <deque>
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
// 업로드 크기. "전송 중"을 확정하는 수단은 크기가 아니라 서버 동작이다 — dropOnFirstPayload는
// 첫 페이로드 조각에서 끊고, stallAfterHeader는 읽기를 멈춰 아예 정지시킨다. 그래서 크기는
// 소켓 버퍼를 채워 정지가 성립할 만큼만 크면 된다(미완료 쓰기 상한 8x64KB=512KB보다 넉넉히).
// (처음엔 16MB로 시간을 벌려 했는데, 테스트 6개 x 40회 반복에서 3.8GB를 써 10분 타임아웃이
//  났고 그렇게 벌어도 경쟁은 남았다 — Linux에서 실제로 걸렸다. 크기로 푸는 문제가 아니다.)
constexpr std::size_t kUploadBytes = 2 * 1024 * 1024;

// 서버 역할 최소 구현. 바이트를 세기만 하고, 응답·단절 시점은 테스트가 정한다.
// 헤더/트레일러를 파싱하지 않는 이유: 파일 크기와 파일명을 테스트가 알고 있어 총
// 기대 바이트 수를 계산할 수 있다. 파싱을 넣으면 검증 대상이 아닌 코드가 늘어난다.
class FakeServer : public IListenerCallback, public ISocketCallback {
public:
    // 업로드를 다 받았을 때 무엇을 할지
    enum class OnUploadComplete {
        DropSilently,   // 아무 응답 없이 끊는다 (WAIT_ACK 중 단절)
        AckThenDrop,    // Ack만 보내고 끊는다 (WAIT_RESULT 중 단절)
        FullResult,     // Ack + ResultHeader + CSV — 정상 완주
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

        // 헤더를 받은 뒤 읽기를 멈춰 업로드를 정지시킨다.
        //
        // 왜 필요한가: "업로드 중"에 Cancel·Disconnect·종료를 넣는 시나리오는 그 순간 실제로
        // 업로드가 진행 중이어야 성립한다. 그런데 2MB는 루프백에서 너무 빨리 끝나, 명령이
        // 드레인되기 전에 업로드가 완료돼 버린다 (_uploading=false가 되면 Cancel은 정상적으로
        // 무시된다 — 제품이 옳고 테스트가 틀린 상황이다). Linux에서 실제로 이 경쟁에 걸렸고
        // Windows에서 통과한 것도 우연이었다.
        // 읽기를 멈추면 양쪽 소켓 버퍼가 차고 미완료 쓰기가 상한(8청크)에 묶여 업로드가
        // 확실히 멈춘 채로 남는다 — 파일을 키워 시간을 버는 것보다 결정적이다.
        if (stallAfterHeader && received.size() >= headerSize() && _accepted &&
            _accepted->isReading()) {
            _accepted->stopRead();
            return;
        }

        if (_responded || received.size() < totalUploadBytes()) {
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
    std::uint64_t uploadSize = kUploadBytes;
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

        proto::Ack ack;
        ack.status = proto::AckStatus::Ok;
        ack.receivedBytes = uploadSize;
        sendBytes(proto::encode(ack));

        if (policy == OnUploadComplete::AckThenDrop) {
            closeAccepted();
            return;
        }

        proto::ResultHeader header;
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

// 테스트 하네스: 가짜 서버 루프(이 스레드) + TransferService(자기 스레드)를 실제 TCP로 잇는다.
struct Harness {
    Harness() {
        REQUIRE(uv_loop_init(&loop) == 0);
        server = std::make_unique<FakeServer>(&loop);
        REQUIRE(server->listen() == 0);
        REQUIRE(server->port() != 0);
        std::string error;
        REQUIRE(service.start(error));
        writeTempFile(filePath, kUploadBytes);
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
        std::remove(filePath.c_str());
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
        service.post(client::StartUploadCommand{filePath, kUploadName, kUploadBytes});
    }

    uv_loop_t loop{};
    std::unique_ptr<FakeServer> server;
    TransferService service;

    std::string filePath = "client_scenario_upload.bin";

    LinkState link = LinkState::Disconnected;
    SessionState session = SessionState::Idle;
    float uploadProgress = 0.0f;
    int errorLogs = 0;
    std::deque<std::string> logs;
    std::deque<LinkState> linkHistory;
    std::deque<SessionState> sessionHistory;
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
    Harness harness;
    // 서버는 받기만 하고 응답하지 않는다 — 취소가 먼저 일어나야 한다
    harness.server->stallAfterHeader = true;  // 업로드를 정지시켜 "전송 중"을 확정한다

    harness.connect();
    harness.startUpload();
    REQUIRE(harness.waitFor([&] { return harness.session == SessionState::Streaming; }));

    harness.service.post(client::CancelUploadCommand{});

    // 취소는 사용자 의도이므로 Warn이다 (에러가 아니다 — design 7번: 경로는 같고 표기는 다르다)
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
    Harness harness;
    harness.server->stallAfterHeader = true;  // 업로드를 정지시켜 "전송 중"을 확정한다

    harness.connect();
    harness.startUpload();
    REQUIRE(harness.waitFor([&] { return harness.session == SessionState::Streaming; }));

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

TEST_CASE("client scenario: quitting mid-upload joins every thread cleanly") {
    // 소멸자(=stop())가 업로드 중에도 파일 리더와 루프 스레드를 정리해야 한다.
    // design 7번 스레드 총괄표의 "종료" 칸이 비어 있으면 여기서 걸린다 (H-1 유형).
    // 정리가 실패하면 join이 걸려 이 테스트가 타임아웃으로 죽는다 — 통과 자체가 검증이다.
    Harness harness;
    harness.server->stallAfterHeader = true;  // 업로드를 정지시켜 "전송 중"을 확정한다

    harness.connect();
    harness.startUpload();
    REQUIRE(harness.waitFor([&] { return harness.session == SessionState::Streaming; }));

    harness.service.stop();
    harness.service.stop();  // 여러 번 불러도 안전해야 한다 (stop 계약)
    SUCCEED("stop() returned without hanging");
}
