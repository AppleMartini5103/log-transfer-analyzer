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
                 ISessionObserver* observer, server::parser::ParserThread* parser,
                 std::size_t chunkSize, SessionTimeouts timeouts)
    : _loop(loop),
      _socket(std::move(socket)),
      _observer(observer),
      _timer(loop),
      _timeouts(timeouts),
      _framer(proto::MessageType::UploadHeader),
      _parser(parser),
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
    if (_parser != nullptr) {
        _parser->beginSession(kSkipReportPath, kResultCsvPath);
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
    // 페이로드 구간을 벗어나면 링 상태와 무관하게 수신을 되살린다.
    // (backpressure로 멈춘 채 VERIFYING 이후로 넘어가면 상대의 다음 메시지를 못 받는다)
    if (next != SessionState::Receiving && next != SessionState::Cleanup) {
        resumeReading();
    }
}

void Session::onTimeout() {
    // 만료는 소켓 에러와 같은 경로로 수렴한다 (design 9번) — 실패 경로가 하나면 테스트도 하나
    cleanup(std::string{"timeout in "} + std::string{stateName(_state)});
}

common::net::WritableBuffer Session::onAllocate(std::size_t suggestedSize) {
    _slotAcquired = false;
    // 페이로드 구간에서는 링버퍼의 빈 슬롯을 그대로 수신 버퍼로 내준다 → 복사 0회.
    // 수동 할당 금지 규칙과 uv_alloc_cb 요구의 화해 지점 (design 추가 설계 2번)
    if (_state == SessionState::Receiving && _receivedBytes < _fileSize && _parser != nullptr) {
        char* data = nullptr;
        std::size_t capacity = 0;
        if (_parser->tryAcquireSlot(data, capacity)) {
            _slotAcquired = true;
            return common::net::WritableBuffer{data, capacity};
        }
    }
    // 헤더·트레일러·DownloadDone은 링에 넣지 않는다 (파싱 대상이 아님)
    const std::size_t size = std::min(suggestedSize, _readBuffer.size());
    return common::net::WritableBuffer{_readBuffer.data(), size};
}

void Session::onRead(std::string_view data) {
    _timer.restart();  // 활동 감지 — 무활동 타이머 리셋 (②류 상태에서는 동작 중이 아니라 무시됨)

    if (_slotAcquired) {
        _slotAcquired = false;
        // data는 링 슬롯 안을 가리킨다. 페이로드 몫만 커밋하고, 경계를 넘은 부분(트레일러)은
        // ★ 커밋 전에 복사해 둔다 — 커밋 순간 슬롯 소유권이 파서로 넘어가기 때문
        const std::uint64_t remaining = _fileSize - _receivedBytes;
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, data.size()));
        const std::string tail{data.substr(take)};  // 최대 12B (트레일러) — 복사 비용 무시

        _payloadCrc = common::crc32(_payloadCrc, data.substr(0, take));
        _receivedBytes += take;
        _parser->commitSlot(take);  // 소유권 이동 + 파서 깨우기
        applyBackpressure();

        if (tail.empty()) {
            return;
        }
        data = std::string_view{tail};  // 남은 트레일러 바이트는 아래 일반 경로로
        std::size_t consumed = consumeTrailer(data);
        (void)consumed;
        return;
    }

    // 한 청크에 여러 단계의 바이트가 섞여 올 수 있다 (헤더+페이로드, 페이로드+트레일러).
    // 각 단계가 자기 몫만 소비하고 나머지를 다음 단계로 넘긴다
    while (!data.empty() && !_cleanupStarted && !_closeAfterSend) {
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

    // 여기 오는 경우는 헤더와 페이로드가 한 청크에 섞여 온 전이 청크뿐이다
    // (그 외에는 onAllocate가 슬롯을 직접 내주므로 복사가 없다).
    // 세션 버퍼에 담긴 바이트를 슬롯으로 옮긴다 — 세션당 최대 1회
    std::size_t copied = 0;
    while (copied < take) {
        char* slot = nullptr;
        std::size_t capacity = 0;
        if (_parser == nullptr || !_parser->tryAcquireSlot(slot, capacity)) {
            // 세션 시작 직후라 링은 비어 있어야 한다. 그래도 못 얻으면 데이터를 잃는 대신 끊는다
            cleanup("ring slot unavailable for transition chunk");
            return 0;
        }
        const std::size_t chunk = std::min(capacity, take - copied);
        std::copy_n(payload.data() + copied, chunk, slot);
        _parser->commitSlot(chunk);
        copied += chunk;
    }
    applyBackpressure();
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
    // 잔여 파싱·CSV 생성은 파서 스레드가 한다 — 통계가 파서 소유이므로 여기서 만들어야
    // 소유권 공유·뮤텍스가 생기지 않는다 (design 11번 ANALYZING 실행 주체).
    // 완료는 uv_async를 통해 onAnalysisComplete()로 돌아온다. 내부 유계 작업이라 타이머 없음
    if (_parser == nullptr) {
        cleanup("no parser thread");
        return;
    }
    _parser->markUploadComplete();
}

void Session::onAnalysisComplete(server::parser::AnalysisResult result) {
    if (_cleanupStarted || _state != SessionState::Analyzing) {
        return;  // 이미 정리됐거나 이전 세션의 결과 — 무시
    }
    _csv = std::move(result.csv);  // 완성 버퍼 소유권이 루프로 이동했다

    proto::ResultHeader header;
    header.csvSize = _csv.size();
    header.crc32 = result.crc32;  // 파서가 계산해 함께 넘긴 값
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

void Session::applyBackpressure() {
    if (_parser == nullptr) {
        return;
    }
    if (_parser->ringFull() && _socket->isReading()) {
        // 링이 꽉 찼다 → 수신 정지. 커널 버퍼가 차면 TCP 윈도우가 닫히고 송신 측이
        // 자연 감속한다 — 이것이 50MB 메모리 상한을 지키는 장치다
        _socket->stopRead();
        _parser->setReadStopped(true);
        // ★ 미스 웨이크업 방어: 파서가 ringFull() 확인과 setReadStopped(true) 사이에
        //   링을 다 비웠다면, 그때는 _readStopped가 아직 false라 깨우기 신호를 보내지 않는다.
        //   그대로 두면 아무도 재개시켜 주지 않아 전송이 멈춘다 — 여기서 직접 재확인한다
        if (!_parser->ringFull()) {
            resumeReading();
        }
    }
}

void Session::resumeReading() {
    if (_cleanupStarted || _parser == nullptr || _socket == nullptr || _socket->isReading()) {
        return;  // 재개는 멱등 — async 합쳐짐(coalescing)에 안전
    }
    // 링 여유를 따지는 것은 페이로드를 링에 넣는 RECEIVING 구간뿐이다.
    // 그 밖의 상태(WAIT_DONE 등)는 링과 무관하므로 무조건 다시 읽어야 한다 —
    // 안 그러면 backpressure로 멈춘 채 상태가 넘어갔을 때 DownloadDone을 영영 못 받는다
    if (_state == SessionState::Receiving && _receivedBytes < _fileSize && _parser->ringFull()) {
        return;
    }
    _parser->setReadStopped(false);
    _socket->startRead();
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
    if (_closeAfterSend) {
        cleanup(_pendingFailReason);  // 실패 Ack가 커널에 넘어갔다 — 이제 닫는다
        return;
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
    if (_socket->send(std::string_view{bytes.data(), bytes.size()}) != 0) {
        cleanup(reason);  // 송신 시작조차 실패 — 알릴 방법이 없으니 그냥 닫는다
        return;
    }
    // ★ 여기서 바로 close하지 않는다 (design 8: "Ack 송신 완료 후 세션 종료").
    //   uv_close는 미완료 write를 취소할 수 있어, 즉시 닫으면 느린 링크에서 클라이언트가
    //   사유(CRC_MISMATCH/PROTOCOL_ERROR)를 영영 모른다. Ack는 17B라 커널 버퍼에 항상
    //   들어가므로 완료 콜백은 상대와 무관하게 곧 온다 — 기다리는 비용이 없다
    _socket->stopRead();  // 정리 예정 세션의 추가 수신은 무의미
    _closeAfterSend = true;
    _pendingFailReason = reason;
    common::Logger::instance().warn("Session: " + reason + " — Ack sent, closing after flush");
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
    if (_parser != nullptr) {
        // 재조립 버퍼·통계는 파서 소유라 루프가 직접 못 지운다 — 중단 플래그로 파서가
        // 스스로 링을 비우고 상태를 리셋하게 한다 (총괄표 "세션 중단 시" 칸)
        _parser->abortSession();
    }
    if (_socket) {
        _socket->close();  // 완료는 onClosed()에서 관찰자에게 알린다
    }
}

}  // namespace server::session
