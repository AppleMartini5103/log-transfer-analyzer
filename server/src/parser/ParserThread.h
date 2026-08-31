#pragma once

#include "parser/LineReassembler.h"
#include "parser/LogLineParser.h"
#include "parser/SkipReporter.h"
#include "stats/StatsCollector.h"
#include "util/SpscRingBuffer.h"

#include <uv.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// 파서 스레드 — SPSC consumer (design 7번 스레드 수명·소유권 총괄표의 마지막 행).
//
//   생성        : 서버 시작 시 상주 1개 (세션마다 만들지 않는다)
//   소유 데이터 : 링버퍼, 재조립 버퍼, 통계 맵, 스킵 카운터 — 파서만 접근하므로 뮤텍스 없음
//   잠/깨우기   : cv.wait(링 빔) / commit 후 cv notify
//   종료 신호   : 종료 플래그 + notify → join (cv에서 자고 있어 신호 없이는 join 불가)
//   세션 중단 시: 중단 플래그 + notify → 파서 자신이 링을 비우며 폐기, 상태 리셋 후 대기 복귀
//
// [스레드 경계를 넘는 신호는 두 방향뿐]
//   루프 → 파서 : cv notify (커밋·업로드 완료·중단·종료)
//   파서 → 루프 : uv_async_send (수신 재개 / 분석 완료) — libuv에서 유일한 스레드 안전 API
//
// 링 슬롯은 uv_alloc_cb에 직결되어 수신이 복사 0회가 된다. 링이 꽉 차면 루프가
// uv_read_stop을 걸고, 파서가 슬롯을 반환하면 uv_async_send로 깨워 재개시킨다
// — 이것이 곧 50MB 메모리 상한 장치다.

namespace server::parser {

struct AnalysisResult {
    std::string csv;
    std::uint32_t crc32 = 0;
    std::uint64_t totalLines = 0;
    std::uint64_t skippedLines = 0;
};

class ParserThread {
public:
    // 두 핸들러 모두 루프 스레드에서 호출된다 (uv_async 콜백 문맥)
    using CompletionHandler = std::function<void(AnalysisResult)>;
    using ResumeHandler = std::function<void()>;
    // 중단된 세션의 폐기가 끝났음을 루프에 알린다 (경쟁 조건 해소)
    using AbortDoneHandler = std::function<void()>;

    ParserThread(uv_loop_t* loop, std::size_t slotCount, std::size_t slotSize);
    ~ParserThread();

    ParserThread(const ParserThread&) = delete;
    ParserThread& operator=(const ParserThread&) = delete;
    ParserThread(ParserThread&&) = delete;
    ParserThread& operator=(ParserThread&&) = delete;

    bool start(std::string& error);
    // 종료 플래그 + notify + join, 그리고 uv 핸들 close까지.
    // ★ 반드시 uv_run이 반환하기 전에(=루프 스레드에서) 불러야 한다 — async 핸들이
    //   열려 있으면 활성 핸들로 남아 uv_run이 영원히 반환하지 않는다. 여러 번 불러도 안전
    void stop();

    void setHandlers(CompletionHandler onComplete, ResumeHandler onResume,
                     AbortDoneHandler onAbortDone = nullptr);

    // ── producer 측 (루프 스레드 전용) ──
    void beginSession(const std::string& skipReportPath, const std::string& csvPath);
    bool tryAcquireSlot(char*& data, std::size_t& capacity);
    void commitSlot(std::size_t size);  // 소유권 이동 + notify
    bool ringFull() const;
    void markUploadComplete();  // 트레일러 검증 후 — 파서의 종료 판정 재료
    void abortSession();        // 강제 단절·타임아웃 — 링 폐기 + 상태 리셋 요청

    // 중단 요청이 아직 처리되지 않았는가 (루프 스레드에서 읽는다).
    //
    // 루프는 이것이 false가 된 뒤에야 다음 세션을 시작해야 한다. abortSession()은 플래그만
    // 세우고 실제 폐기는 파서 스레드가 하므로, 그 사이에 새 세션이 시작되면 이전 세션의
    // 통계가 그대로 이어진다. 그 창을 SessionManager가 accept 재개를 미뤄서 닫는다.
    bool abortPending() const { return _abortRequested.load(std::memory_order_acquire); }

    // 루프가 수신을 멈췄음을 알린다 — 파서가 슬롯을 반환할 때 깨울지 판단하는 기준
    void setReadStopped(bool stopped) { _readStopped.store(stopped, std::memory_order_release); }

private:
    void run();
    void closeHandles();
    void consumeReadySlots();
    void finishSession();
    void resetSessionState();
    static void onCompletionAsync(uv_async_t* handle);
    static void onResumeAsync(uv_async_t* handle);
    static void onAbortDoneAsync(uv_async_t* handle);

    uv_loop_t* _loop = nullptr;
    std::unique_ptr<uv_async_t> _completionAsync;
    std::unique_ptr<uv_async_t> _resumeAsync;
    std::unique_ptr<uv_async_t> _abortDoneAsync;

    common::SpscRingBuffer _ring;
    LineReassembler _reassembler;
    LogLineParser _parser;
    server::stats::StatsCollector _stats;
    SkipReporter _reporter;

    std::thread _thread;
    mutable std::mutex _mutex;  // cv 대기 조건 보호 전용 — 링 데이터는 SPSC라 락 없음
    std::condition_variable _cv;

    std::atomic<bool> _stopRequested{false};
    std::atomic<bool> _uploadComplete{false};
    std::atomic<bool> _abortRequested{false};
    std::atomic<bool> _readStopped{false};

    std::mutex _resultMutex;  // 완료 결과를 루프 스레드로 넘기는 지점만 보호
    AnalysisResult _result;
    CompletionHandler _onComplete;
    ResumeHandler _onResume;
    AbortDoneHandler _onAbortDone;

    std::string _skipReportPath;
    std::string _csvPath;
    std::uint64_t _totalLines = 0;
    bool _started = false;
};

}  // namespace server::parser
