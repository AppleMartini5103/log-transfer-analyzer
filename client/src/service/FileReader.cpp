#include "service/FileReader.h"

namespace client {

FileReader::FileReader()
    : _ring(common::ringSlotCountFor(common::kDefaultSlotSize), common::kDefaultSlotSize) {}

FileReader::~FileReader() {
    stop();
}

void FileReader::start(WakeFn wake) {
    if (_thread.joinable()) {
        return;
    }
    _wake = std::move(wake);
    _thread = std::thread([this] { run(); });
}

void FileReader::stop() {
    if (!_thread.joinable()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _quit = true;
        _uploading = false;
    }
    // cv.wait에서 자고 있으므로 신호 없이는 join이 끝나지 않는다 (design 7번 총괄표).
    _abort.store(true, std::memory_order_release);
    _cv.notify_all();
    _thread.join();
}

bool FileReader::beginUpload(const std::string& path, std::uint64_t expectedSize,
                             std::string& error) {
    const std::lock_guard<std::mutex> lock(_mutex);
    if (_uploading) {
        error = "An upload is already in progress.";
        return false;
    }

    _file.open(path, std::ios::binary);
    if (!_file.is_open()) {
        error = "Cannot open the selected file.";
        return false;
    }

    _remaining = expectedSize;
    _uploading = true;
    _error.clear();
    _abort.store(false, std::memory_order_release);
    _readComplete.store(false, std::memory_order_release);
    _failed.store(false, std::memory_order_release);
    // 링 비우기는 호출자(루프 스레드 = consumer) 책임이다. SPSC 계약상 release는
    // consumer만 부를 수 있어, producer 쪽인 여기서 정리하면 계약 위반이 된다.

    _cv.notify_all();
    return true;
}

void FileReader::abortUpload() {
    // 중단 플래그를 먼저 세우고 깨운다 — 읽기 루프가 다음 확인 지점에서 빠져나온다.
    _abort.store(true, std::memory_order_release);
    _cv.notify_all();
}

void FileReader::notifySpaceAvailable() {
    _cv.notify_all();
}

std::string FileReader::takeError() {
    const std::lock_guard<std::mutex> lock(_mutex);
    std::string error;
    error.swap(_error);
    return error;
}

void FileReader::run() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] { return _quit || _uploading; });
            if (_quit) {
                return;
            }
        }

        readLoop();

        {
            const std::lock_guard<std::mutex> lock(_mutex);
            _file.close();
            _uploading = false;
        }
    }
}

void FileReader::readLoop() {
    for (;;) {
        if (_abort.load(std::memory_order_acquire)) {
            return;
        }

        common::SpscRingBuffer::WriteSlot slot;
        if (!_ring.tryAcquire(slot)) {
            // 링이 꽉 참 = backpressure. 소비 측이 슬롯을 비우고 깨워줄 때까지 잔다.
            // 이 대기가 곧 메모리 상한 장치다 — 파일이 아무리 커도 링 예산을 넘지 않는다.
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] {
                return _quit || _abort.load(std::memory_order_acquire) || !_ring.full();
            });
            if (_quit || _abort.load(std::memory_order_acquire)) {
                return;
            }
            continue;
        }

        std::uint64_t toRead = slot.capacity;
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            if (_remaining < toRead) {
                toRead = _remaining;
            }
            if (toRead > 0) {
                _file.read(slot.data, static_cast<std::streamsize>(toRead));
                const std::streamsize actual = _file.gcount();
                if (actual <= 0) {
                    // 헤더에 적어 보낸 크기만큼 못 읽었다 = 전송 중 파일이 잘렸거나 잠겼다.
                    // 스트림을 헤더와 불일치하게 만들 수 없으므로 실패로 끝낸다.
                    _error = "File ended earlier than its reported size.";
                    _failed.store(true, std::memory_order_release);
                    if (_wake) {
                        _wake();
                    }
                    return;
                }
                toRead = static_cast<std::uint64_t>(actual);
                _remaining -= toRead;
            }
        }

        if (toRead == 0) {
            // 빈 파일이거나 마지막 청크까지 다 읽었다 (design 8번: fileSize = 0도 정상 세션)
            _readComplete.store(true, std::memory_order_release);
            if (_wake) {
                _wake();
            }
            return;
        }

        _ring.commit(static_cast<std::size_t>(toRead));
        if (_wake) {
            _wake();  // 루프 스레드를 깨워 전송하게 한다 (uv_async_send)
        }
    }
}

}  // namespace client
