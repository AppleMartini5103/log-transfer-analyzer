#include "parser/ParserThread.h"

#include "csv/CsvBuilder.h"
#include "util/Crc32.h"
#include "util/Logger.h"

#include <utility>

namespace server::parser {

ParserThread::ParserThread(uv_loop_t* loop, std::size_t slotCount, std::size_t slotSize)
    : _loop(loop),
      _completionAsync(std::make_unique<uv_async_t>()),
      _resumeAsync(std::make_unique<uv_async_t>()),
      _abortDoneAsync(std::make_unique<uv_async_t>()),
      _ring(slotCount, slotSize) {}

ParserThread::~ParserThread() {
    stop();  // 이미 닫혔으면 아무 일도 하지 않는다
}

void ParserThread::closeHandles() {
    // async 핸들은 close 콜백까지 살아 있어야 하므로 소유권을 콜백으로 넘긴다
    const auto closeAsync = [](std::unique_ptr<uv_async_t>& handle) {
        if (!handle) {
            return;
        }
        handle->data = nullptr;
        uv_handle_t* raw = reinterpret_cast<uv_handle_t*>(handle.release());
        if (!uv_is_closing(raw)) {
            uv_close(raw, [](uv_handle_t* h) {
                std::unique_ptr<uv_async_t> owned{reinterpret_cast<uv_async_t*>(h)};
            });
        }
    };
    closeAsync(_completionAsync);
    closeAsync(_resumeAsync);
    closeAsync(_abortDoneAsync);
}

bool ParserThread::start(std::string& error) {
    if (_started) {
        return true;
    }
    int rc = uv_async_init(_loop, _completionAsync.get(), onCompletionAsync);
    if (rc != 0) {
        error = std::string{"uv_async_init(completion): "} + uv_strerror(rc);
        return false;
    }
    _completionAsync->data = this;
    rc = uv_async_init(_loop, _resumeAsync.get(), onResumeAsync);
    if (rc != 0) {
        error = std::string{"uv_async_init(resume): "} + uv_strerror(rc);
        return false;
    }
    _resumeAsync->data = this;
    rc = uv_async_init(_loop, _abortDoneAsync.get(), onAbortDoneAsync);
    if (rc != 0) {
        error = std::string{"uv_async_init(abortDone): "} + uv_strerror(rc);
        return false;
    }
    _abortDoneAsync->data = this;

    // 스레드 생성은 데몬화(fork) 이후여야 한다 — main의 초기화 순서가 이를 보장한다
    _thread = std::thread([this] { run(); });
    _started = true;
    return true;
}

void ParserThread::stop() {
    if (!_started) {
        closeHandles();  // start 전에 닫는 경우도 있다 (init 실패 등)
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _stopRequested.store(true, std::memory_order_release);
    }
    _cv.notify_one();  // cv.wait에서 자고 있으므로 신호 없이는 join이 영원히 안 끝난다
    if (_thread.joinable()) {
        _thread.join();
    }
    _started = false;
    // 스레드가 멈춘 뒤에 핸들을 닫는다 — 파서가 살아 있는 동안 닫으면
    // uv_async_send가 해제된 핸들을 건드릴 수 있다
    closeHandles();
}

void ParserThread::setHandlers(CompletionHandler onComplete, ResumeHandler onResume,
                               AbortDoneHandler onAbortDone) {
    const std::lock_guard<std::mutex> lock(_resultMutex);
    _onComplete = std::move(onComplete);
    _onResume = std::move(onResume);
    _onAbortDone = std::move(onAbortDone);
}

void ParserThread::beginSession(const std::string& skipReportPath, const std::string& csvPath) {
    // 파서는 대기 상태여야 한다 — SessionManager가 abortPending()이 false가 된 뒤에만
    // 새 세션을 시작하므로 이 시점에 미처리 중단은 없다.
    //
    // ★ _abortRequested를 여기서 초기화하지 않는다. 종전에는 초기화했는데, 그것이
    //   경쟁 조건이었다: abortSession()은 플래그만 세우고 폐기는 파서 스레드가 하는데,
    //   파서가 스케줄되기 전에 루프가 여기까지 오면 요청이 취소되어 폐기가 영영
    //   실행되지 않았다. 그러면 이전 세션의 통계가 다음 결과에 그대로 남는다.
    //   플래그의 수명은 파서가 단독으로 소유한다 — 세우는 것은 루프, 지우는 것은 파서다.
    _skipReportPath = skipReportPath;
    _csvPath = csvPath;
    _uploadComplete.store(false, std::memory_order_release);

    _readStopped.store(false, std::memory_order_release);
}

bool ParserThread::tryAcquireSlot(char*& data, std::size_t& capacity) {
    common::SpscRingBuffer::WriteSlot slot;
    if (!_ring.tryAcquire(slot)) {
        return false;
    }
    data = slot.data;
    capacity = slot.capacity;
    return true;
}

void ParserThread::commitSlot(std::size_t size) {
    _ring.commit(size);  // 소유권이 producer → consumer로 이동 (뮤텍스 없음)
    {
        // 뮤텍스를 짧게 잡았다 놓는다: 파서가 "링 빔"을 확인하고 잠들기 직전에
        // 커밋이 끼어들어 알림을 놓치는 창을 없앤다
        const std::lock_guard<std::mutex> lock(_mutex);
    }
    _cv.notify_one();
}

bool ParserThread::ringFull() const {
    return _ring.full();
}

void ParserThread::markUploadComplete() {
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _uploadComplete.store(true, std::memory_order_release);
    }
    _cv.notify_one();
}

void ParserThread::abortSession() {
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _abortRequested.store(true, std::memory_order_release);
    }
    _cv.notify_one();
}

void ParserThread::run() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] {
                return _stopRequested.load(std::memory_order_acquire) ||
                       _abortRequested.load(std::memory_order_acquire) ||
                       _uploadComplete.load(std::memory_order_acquire) || !_ring.empty();
            });
            if (_stopRequested.load(std::memory_order_acquire)) {
                return;
            }
        }

        if (_abortRequested.load(std::memory_order_acquire)) {
            // 세션 중단: 루프가 직접 못 지우는 소유 데이터를 파서 자신이 폐기한다
            // (소유권 규칙 유지 — 총괄표의 "세션 중단 시" 칸)
            resetSessionState();
            _abortRequested.store(false, std::memory_order_release);
            // 폐기가 끝났음을 루프에 알린다 — SessionManager는 이 신호를 받고서야
            // 다음 연결을 accept한다 (그 전에 시작하면 통계가 이어진다)
            if (_abortDoneAsync) {
                uv_async_send(_abortDoneAsync.get());
            }
            continue;
        }

        consumeReadySlots();

        // 종료 판정 = 업로드 완료 플래그(루프가 set) && 링 빔
        if (_uploadComplete.load(std::memory_order_acquire) && _ring.empty()) {
            finishSession();
            _uploadComplete.store(false, std::memory_order_release);
        }
    }
}

void ParserThread::consumeReadySlots() {
    common::SpscRingBuffer::ReadView view;
    while (_ring.tryPeek(view)) {
        if (_abortRequested.load(std::memory_order_acquire) ||
            _stopRequested.load(std::memory_order_acquire)) {
            return;  // 중단·종료 요청이 오면 남은 슬롯은 상위 루프가 폐기한다
        }
        _reassembler.feed(std::string_view{view.data, view.size},
                          [this](const LineReassembler::Line& line) {
                              ++_totalLines;
                              const auto result = _parser.parse(line.text, line.tooLong);
                              if (!result.ok) {
                                  _reporter.record(result.reason, line.offset, line.text);
                              } else if (!_stats.record(result.line)) {
                                  _reporter.record(SkipReason::MapLimit, line.offset, line.text);
                              }
                          });
        _ring.release();

        // 링에 여유가 생겼다 — 루프가 수신을 멈춘 상태였으면 깨워서 재개시킨다
        if (_readStopped.load(std::memory_order_acquire) && _resumeAsync) {
            uv_async_send(_resumeAsync.get());
        }
    }
}

void ParserThread::finishSession() {
    // 개행 없이 끝난 마지막 라인도 같은 파이프라인으로 검증해야 유실이 없다
    _reassembler.finish([this](const LineReassembler::Line& line) {
        ++_totalLines;
        const auto result = _parser.parse(line.text, line.tooLong);
        if (!result.ok) {
            _reporter.record(result.reason, line.offset, line.text);
        } else if (!_stats.record(result.line)) {
            _reporter.record(SkipReason::MapLimit, line.offset, line.text);
        }
    });

    AnalysisResult result;
    result.csv = server::csv::buildResultCsv(_stats, _reporter, _totalLines);
    result.crc32 = common::crc32(0, result.csv);  // 수 KB 일괄 계산
    result.totalLines = _totalLines;
    result.skippedLines = _reporter.total();

    // CSV 생성 주체가 파서인 이유: 통계·스킵 카운터가 파서 소유라 여기서 만들어야
    // 소유권 공유·뮤텍스가 생기지 않는다 (design 11번 ANALYZING 실행 주체)
    if (!_csvPath.empty()) {
        server::csv::writeCsvFile(_csvPath, result.csv);
    }
    if (!_skipReportPath.empty()) {
        _reporter.writeReport(_skipReportPath);
    }
    common::Logger::instance().info("Parser: analysis done (" + std::to_string(result.totalLines) +
                                    " lines, " + std::to_string(result.skippedLines) +
                                    " skipped, csv " + std::to_string(result.csv.size()) +
                                    " bytes)");

    {
        const std::lock_guard<std::mutex> lock(_resultMutex);
        _result = std::move(result);
    }
    resetSessionState();
    if (_completionAsync) {
        uv_async_send(_completionAsync.get());  // 완성 버퍼·CRC 소유권을 루프로 이동
    }
}

void ParserThread::resetSessionState() {
    // 링을 비우며 폐기 (중단 시) — 이후 재조립·통계·스킵 카운터 초기화
    common::SpscRingBuffer::ReadView view;
    while (_ring.tryPeek(view)) {
        _ring.release();
    }
    _reassembler.reset();
    _parser.reset();
    _stats.reset();
    _reporter.reset();
    _totalLines = 0;
    if (_readStopped.load(std::memory_order_acquire) && _resumeAsync) {
        uv_async_send(_resumeAsync.get());
    }
}

void ParserThread::onCompletionAsync(uv_async_t* handle) {
    auto* self = static_cast<ParserThread*>(handle->data);
    if (self == nullptr) {
        return;
    }
    AnalysisResult result;
    CompletionHandler handler;
    {
        const std::lock_guard<std::mutex> lock(self->_resultMutex);
        result = std::move(self->_result);
        self->_result = AnalysisResult{};
        handler = self->_onComplete;
    }
    if (handler) {
        handler(std::move(result));
    }
}

void ParserThread::onResumeAsync(uv_async_t* handle) {
    auto* self = static_cast<ParserThread*>(handle->data);
    if (self == nullptr) {
        return;
    }
    ResumeHandler handler;
    {
        const std::lock_guard<std::mutex> lock(self->_resultMutex);
        handler = self->_onResume;
    }
    // uv_async_send N회가 콜백 1회로 합쳐질 수 있다(coalescing) — 재개는 멱등이라 문제없다
    if (handler) {
        handler();
    }
}

void ParserThread::onAbortDoneAsync(uv_async_t* handle) {
    auto* self = static_cast<ParserThread*>(handle->data);
    if (self == nullptr) {
        return;
    }
    AbortDoneHandler handler;
    {
        const std::lock_guard<std::mutex> lock(self->_resultMutex);
        handler = self->_onAbortDone;
    }
    // 합쳐져도 무해하다 — 핸들러는 "지금 accept해도 되는가"를 다시 판정할 뿐이다
    if (handler) {
        handler();
    }
}

}  // namespace server::parser
