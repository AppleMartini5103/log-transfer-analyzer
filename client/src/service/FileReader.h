#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "util/SpscRingBuffer.h"

namespace client {

// 파일 리더: 업로드할 파일을 청크 단위로 읽어 링버퍼에 넣는 producer.
//
// [스레드 수명·소유권] design 7번 총괄표의 "파일 리더(클라)" 행:
//   생성    : start() 시 1개, 상주 (업로드마다 만들지 않는다 — 스레드 구성 고정 규칙)
//   소유    : 파일 핸들, 읽기 오프셋, 링버퍼의 producer 측
//   잠/깨움 : cv.wait (할 일 없음 / 링 꽉 참) <- notify
//   종료    : 종료 플래그 + notify -> join
//   중단    : 중단 플래그 -> 파일 닫고 대기 복귀 (취소·에러 공통)
//
// 링버퍼가 곧 메모리 상한 장치다: 링이 꽉 차면 읽기를 멈추므로, 500MB 파일이라도
// 프로세스가 들고 있는 양은 링 예산(4MB)을 넘지 않는다.
class FileReader {
public:
    FileReader();
    ~FileReader();

    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    // 링에 데이터가 들어갔음을 워커에 알리는 콜백 (uv_async_send로 루프를 깨우는 자리).
    // 리더 스레드에서 호출되므로, 콜백은 스레드 안전해야 한다.
    using WakeFn = std::function<void()>;

    void start(WakeFn wake);
    void stop();

    // 업로드 시작 — 실패 시 false + error (예외 대신 반환값, 컨벤션 3번)
    bool beginUpload(const std::string& path, std::uint64_t expectedSize, std::string& error);
    // 취소·에러 시: 읽기를 멈추고 파일을 닫은 뒤 대기로 복귀한다
    void abortUpload();

    common::SpscRingBuffer& ring() { return _ring; }

    // 소비 측(루프 스레드)이 슬롯을 비운 뒤 호출 — 리더가 다시 읽도록 깨운다
    void notifySpaceAvailable();

    // 파일 끝까지 읽어 링에 넣었는가 (남은 슬롯 소비는 별개)
    bool readComplete() const { return _readComplete.load(std::memory_order_acquire); }
    // 읽기 중 오류 — 루프 스레드가 확인해 CLEANUP으로 보낸다
    bool failed() const { return _failed.load(std::memory_order_acquire); }
    std::string takeError();

private:
    void run();
    void readLoop();

    common::SpscRingBuffer _ring;
    WakeFn _wake;

    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cv;

    // _mutex 보호
    bool _uploading = false;
    bool _quit = false;
    std::ifstream _file;
    std::uint64_t _remaining = 0;
    std::string _error;

    std::atomic<bool> _abort{false};
    std::atomic<bool> _readComplete{false};
    std::atomic<bool> _failed{false};
};

}  // namespace client
