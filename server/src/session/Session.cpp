#include "session/Session.h"

#include "csv/CsvBuilder.h"
#include "parser/SkipReporter.h"
#include "protocol/Codec.h"
#include "stats/StatsCollector.h"
#include "util/Crc32.h"
#include "util/Logger.h"

#include <algorithm>
#include <utility>

namespace server::session {

namespace proto = common::protocol;

namespace {

std::string describe(int status) {
    return std::string{uv_err_name(status)} + " (" + uv_strerror(status) + ")";
}

}  // namespace

std::string_view stateName(SessionState state) {
    switch (state) {
        case SessionState::WaitHeader:
            return "WAIT_HEADER";
        case SessionState::Receiving:
            return "RECEIVING";
        case SessionState::Verifying:
            return "VERIFYING";
        case SessionState::Analyzing:
            return "ANALYZING";
        case SessionState::SendingResult:
            return "SENDING_RESULT";
        case SessionState::WaitDone:
            return "WAIT_DONE";
        case SessionState::Cleanup:
            return "CLEANUP";
    }
    return "UNKNOWN";
}

Session::Session(uv_loop_t* loop, std::unique_ptr<common::net::ISocket> socket,
                 ISessionObserver* observer, std::size_t chunkSize, SessionTimeouts timeouts)
    : _loop(loop),
      _socket(std::move(socket)),
      _observer(observer),
      _timer(loop),
      _timeouts(timeouts),
      _framer(proto::MessageType::UploadHeader),
      _readBuffer(chunkSize) {}

Session::~Session() = default;

bool Session::start() {
    if (!_socket) {
        return false;
    }
    _socket->setCallback(this);
    const int rc = _socket->startRead();
    if (rc != 0) {
        common::Logger::instance().error("Session: startRead failed: " + describe(rc));
        cleanup("startRead failed");
        return false;
    }
    common::Logger::instance().info("Session started from " + _socket->peerAddress());
    armTimer();  // WAIT_HEADER — 상대가 헤더를 보낼 때까지 120초
    return true;
}

std::uint64_t Session::timeoutForState(SessionState state, const SessionTimeouts& timeouts) {
    // ★ 상태→타이머 매핑은 이 함수 하나뿐이다 (design 11번 / 컨벤션 4번).
    //   상태 전이 코드 여기저기에 타임아웃 값을 흩뿌리면 상태 추가 시 누락이 생긴다
    switch (state) {
        case SessionState::WaitHeader:
        case SessionState::WaitDone:
            return timeouts.responseMs;  // ②류: 상대의 유한한 작업을 기다림
        case SessionState::Receiving:
        case SessionState::SendingResult:
            return timeouts.idleMs;  // ①류: 무활동 감시 (활동마다 리셋)
        case SessionState::Verifying:
        case SessionState::Analyzing:
            return 0;  // 자기 CPU 작업 — 타이머 없음 (느린 머신에서 자살 방지)
        case SessionState::Cleanup:
            return 0;
    }
    return 0;
}

void Session::armTimer() {
    const std::uint64_t timeout = timeoutForState(_state, _timeouts);
    if (timeout == 0) {
        _timer.stop();
        return;
    }
    _timer.start(timeout, [this] { onTimeout(); });
}

void Session::transition(SessionState next) {
    if (_state == next) {
        return;
    }
    common::Logger::instance().info(std::string{"Session: "} + std::string{stateName(_state)} +
                                    " -> " + std::string{stateName(next)});
    _state = next;
    armTimer();
}

void Session::onTimeout() {
    // 만료는 소켓 에러와 같은 경로로 수렴한다 (design 9번) — 실패 경로가 하나면 테스트도 하나
    cleanup(std::string{"timeout in "} + std::string{stateName(_state)});
}

common::net::WritableBuffer Session::onAllocate(std::size_t suggestedSize) {
    // 다음 이슈에서 링버퍼 슬롯을 직접 내주도록 교체된다 (복사 0회).
    // 지금은 세션 소유 고정 버퍼 — malloc은 어느 쪽이든 쓰지 않는다
    const std::size_t size = std::min(suggestedSize, _readBuffer.size());
    return common::net::WritableBuffer{_readBuffer.data(), size};
}

void Session::onRead(std::string_view data) {
    _timer.restart();  // 활동 감지 — 무활동 타이머 리셋 (②류 상태에서는 동작 중이 아니라 무시됨)

    // 한 청크에 여러 단계의 바이트가 섞여 올 수 있다 (헤더+페이로드, 페이로드+트레일러).
    // 각 단계가 자기 몫만 소비하고 나머지를 다음 단계로 넘긴다
    while (!data.empty() && !_cleanupStarted) {
        std::size_t consumed = 0;
        switch (_state) {
            case SessionState::WaitHeader:
                consumed = consumeHeader(data);
                break;
            case SessionState::Receiving:
                consumed = (_receivedBytes < _fileSize) ? consumePayload(data)
                                                        : consumeTrailer(data);
                break;
            case SessionState::WaitDone:
                consumed = consumeDone(data);
                break;
            default:
                // VERIFYING/ANALYZING/SENDING_RESULT 중 도착한 데이터는 프로토콜 위반이다
                // (클라이언트는 Ack를 기다려야 함) — 스트림이 꼬였으므로 조용히 닫는다
                closeSilently("unexpected data in " + std::string{stateName(_state)});
                return;
        }
        if (consumed == 0) {
            break;  // 더 필요하거나 처리 중단 — 다음 read를 기다린다
        }
        data.remove_prefix(consumed);
    }
}

std::size_t Session::consumeHeader(std::string_view data) {
    const auto result = _framer.feed(data);
    switch (result.status) {
        case proto::DecodeStatus::NeedMoreData:
            return result.consumed;
        case proto::DecodeStatus::Ok:
            break;
        case proto::DecodeStatus::BadMagic:
        case proto::DecodeStatus::BadVersion:
        case proto::DecodeStatus::BadType:
            // ①부류: 스트림 신뢰 불가 — 이 위로 보내는 응답도 의미 보장이 없다
            closeSilently("bad preamble in header");
            return 0;
        case proto::DecodeStatus::BadValue:
            failWithAck(proto::AckStatus::ProtocolError, "header value out of range");
            return 0;
    }

    proto::UploadHeader header;
    const proto::DecodeStatus decoded = proto::decode(_framer.message(), header);
    if (decoded == proto::DecodeStatus::BadValue) {
        // ②부류: 스트림은 멀쩡하므로 사유를 알리고 닫는다
        failWithAck(proto::AckStatus::ProtocolError, "invalid header value");
        return 0;
    }
    if (decoded != proto::DecodeStatus::Ok) {
        closeSilently("header decode failed");
        return 0;
    }

    _fileSize = header.fileSize;
    _filename = header.filename;
    _framer.reset(proto::MessageType::UploadTrailer);
    common::Logger::instance().info("Session: upload header accepted (" + _filename + ", " +
                                    std::to_string(_fileSize) + " bytes)");
    transition(SessionState::Receiving);
    return result.consumed;
}

std::size_t Session::consumePayload(std::string_view data) {
    const std::uint64_t remaining = _fileSize - _receivedBytes;
    const std::size_t take =
        static_cast<std::size_t>(std::min<std::uint64_t>(remaining, data.size()));
    const std::string_view payload = data.substr(0, take);

    // CRC는 루프 스레드가 수신 시점에 증분 계산한다 — 트레일러 비교 주체와 같은 스레드라
    // 파서 드레인 완료를 기다리는 추가 동기화가 생기지 않는다 (design 체크섬 설계)
    _payloadCrc = common::crc32(_payloadCrc, payload);
    _receivedBytes += take;

    // 다음 이슈: 여기서 페이로드를 링버퍼 슬롯으로 커밋해 파서 스레드로 넘긴다.
    // 현재는 CRC만 계산하고 흘려보내므로 통계는 비어 있다
    return take;
}

std::size_t Session::consumeTrailer(std::string_view data) {
    const auto result = _framer.feed(data);
    switch (result.status) {
        case proto::DecodeStatus::NeedMoreData:
            return result.consumed;
        case proto::DecodeStatus::Ok:
            break;
        case proto::DecodeStatus::BadValue:
            failWithAck(proto::AckStatus::ProtocolError, "invalid trailer value");
            return 0;
        default:
            closeSilently("bad preamble in trailer");
            return 0;
    }
    verifyAndAck();
    return result.consumed;
}

void Session::verifyAndAck() {
    transition(SessionState::Verifying);

    proto::UploadTrailer trailer;
    if (proto::decode(_framer.message(), trailer) != proto::DecodeStatus::Ok) {
        closeSilently("trailer decode failed");
        return;
    }
    if (trailer.crc32 != _payloadCrc) {
        // CRC 불일치는 정상 상황이 아니라 버그 신호다 (TCP가 전송 중 손상을 걸러주므로).
        // 재전송 최적화 없이 사유를 알리고 닫는다 — 클라 로그에 원인이 남아야 대응 가능
        failWithAck(proto::AckStatus::CrcMismatch, "payload CRC mismatch");
        return;
    }

    proto::Ack ack;
    ack.status = proto::AckStatus::Ok;
    ack.receivedBytes = _receivedBytes;
    const auto bytes = proto::encode(ack);
    if (_socket->send(std::string_view{bytes.data(), bytes.size()}) != 0) {
        cleanup("failed to send Ack");
        return;
    }
    common::Logger::instance().info("Session: CRC verified, Ack(OK) sent (" +
                                    std::to_string(_receivedBytes) + " bytes)");
    analyzeAndSendResult();
}

void Session::analyzeAndSendResult() {
    transition(SessionState::Analyzing);

    // 다음 이슈에서 파서 스레드의 통계로 교체된다. 지금은 빈 통계 — 프로토콜 왕복이
    // 끝까지 도는지 먼저 검증하기 위한 중간 단계다 (CSV 스키마·전송 경로는 실제와 동일)
    const server::stats::StatsCollector stats;
    const server::parser::SkipReporter reporter;
    _csv = server::csv::buildResultCsv(stats, reporter);
    server::csv::writeCsvFile("./result.csv", _csv);  // 실패해도 전송은 계속한다

    proto::ResultHeader header;
    header.csvSize = _csv.size();
    header.crc32 = common::crc32(0, _csv);  // 수 KB라 일괄 계산 — 블로킹 무시 가능
    const auto headerBytes = proto::encode(header);

    transition(SessionState::SendingResult);
    if (_socket->send(std::string_view{headerBytes.data(), headerBytes.size()}) != 0) {
        cleanup("failed to send ResultHeader");
        return;
    }
    if (_socket->send(_csv) != 0) {
        cleanup("failed to send result.csv");
        return;
    }
}

std::size_t Session::consumeDone(std::string_view data) {
    const auto result = _framer.feed(data);
    if (result.status == proto::DecodeStatus::NeedMoreData) {
        return result.consumed;
    }
    if (result.status != proto::DecodeStatus::Ok) {
        closeSilently("bad DownloadDone");
        return 0;
    }
    common::Logger::instance().info("Session: DownloadDone received");
    cleanup("completed");
    return result.consumed;
}

void Session::onSendComplete(int status) {
    if (status != 0) {
        return;  // onError가 이어서 정리한다
    }
    _timer.restart();  // 전송도 활동 — SENDING_RESULT의 무활동 타이머 리셋
    if (_state == SessionState::SendingResult) {
        // ResultHeader와 CSV 두 번의 완료 중 마지막에 WAIT_DONE으로 넘어간다
        _framer.reset(proto::MessageType::DownloadDone);
        transition(SessionState::WaitDone);
    }
}

void Session::onError(int status, std::string_view where) {
    // EOF·RST·타임아웃 전부 같은 경로 — 실패 종류마다 정리 코드가 갈라지면
    // 그 조합 수만큼 테스트해야 한다 (총괄 원칙 ③)
    cleanup(std::string{where} + ": " + describe(status));
}

void Session::onClosed() {
    if (_finished) {
        return;
    }
    _finished = true;
    common::Logger::instance().info("Session closed (" + _finishReason + ")");
    if (_observer != nullptr) {
        _observer->onSessionFinished();  // 관찰자는 즉시 파괴하지 않고 지연 파괴한다
    }
}

void Session::failWithAck(proto::AckStatus status, const std::string& reason) {
    proto::Ack ack;
    ack.status = status;
    ack.receivedBytes = _receivedBytes;
    const auto bytes = proto::encode(ack);
    _socket->send(std::string_view{bytes.data(), bytes.size()});  // 실패해도 어차피 닫는다
    common::Logger::instance().warn("Session: " + reason + " — Ack sent, closing");
    cleanup(reason);
}

void Session::closeSilently(const std::string& reason) {
    common::Logger::instance().warn("Session: " + reason + " — closing without response");
    cleanup(reason);
}

void Session::cleanup(const std::string& reason) {
    if (_cleanupStarted) {
        return;  // 재진입 방지 — 어떤 실패든 정리는 한 번만
    }
    _cleanupStarted = true;
    _finishReason = reason;
    _state = SessionState::Cleanup;
    _timer.stop();
    if (_socket) {
        _socket->close();  // 완료는 onClosed()에서 관찰자에게 알린다
    }
}

}  // namespace server::session
