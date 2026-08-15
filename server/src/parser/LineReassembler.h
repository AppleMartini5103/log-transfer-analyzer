#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// 라인 재조립기 — TCP 스트림은 라인이 수신 경계에 걸쳐 쪼개져 도착한다 (design 소비자 파이프라인).
// 링 슬롯 청크를 받아 \n 단위 완성 라인을 콜백으로 내보내고, 미완성 꼬리는 보관한다.
//
// 책임 (4-1의 0단계 중 재조립 관련만 — 내용 검증은 검증 파이프라인 몫):
//   - \n 기준 분리 + 청크 경계 재조립 (같은 청크 안의 완성 라인은 복사 없이 뷰로 전달)
//   - 최대 라인 길이 상한: 초과 시 tooLong 1회 보고 후 다음 \n까지 폐기
//     → 개행 없는 독약 데이터로 인한 메모리 폭증 방지. 앞부분(상한만큼)은 보존해
//       스킵 로그의 "원문 앞부분 기록" 재료로 제공 (design 4-1 5단계)
//   - 라인 끝 \r 제거 (CRLF 혼입 대비 — 내부 \r은 데이터로 남겨 검증 단계가 거부)
//   - 라인 시작의 스트림 바이트 오프셋 제공 (스킵 로그 포맷: [사유코드] 바이트 오프셋 + 원문)
//   - EOF/세션 종료 시 잔여분 배출(finish) — 개행 없이 잘린 마지막 라인도 동일 파이프라인행
//
// 파서 스레드 전용 (스레드 소유권 총괄표 — 재조립 버퍼는 파서만 접근). 동기화 없음.

namespace server::parser {

// 재조립 버퍼 최대 라인 길이 (design 5번 확정 값). 링 슬롯(64KB)과 같은 값이지만
// 별개의 확정값이므로 protocol.h가 아닌 파서 도메인에 둔다
inline constexpr std::size_t kMaxLineLength = 64 * 1024;

class LineReassembler {
public:
    struct Line {
        std::string_view text;      // \r 제거된 라인. 콜백 동안만 유효 — 보관하려면 복사할 것
        std::uint64_t offset = 0;   // 스트림 내 라인 시작 바이트 오프셋
        bool tooLong = false;       // 상한 초과 — text는 상한까지의 앞부분 (스킵 로그 재료)
    };

    explicit LineReassembler(std::size_t maxLineLength = kMaxLineLength)
        : _maxLen(maxLineLength) {
        _fragment.reserve(_maxLen);  // 생성 시 1회 확보 — 이후 재할당 없음 (bounded)
    }

    // 청크를 소비하며 완성 라인마다 onLine(const Line&) 호출
    template <typename Callback>
    void feed(std::string_view chunk, Callback&& onLine) {
        const std::uint64_t base = _streamOffset;
        std::size_t pos = 0;
        while (pos < chunk.size()) {
            const std::size_t nl = chunk.find('\n', pos);

            if (_discarding) {
                // 상한 초과 보고 후 — 이 라인의 나머지는 다음 \n까지 버린다
                if (nl == std::string_view::npos) {
                    pos = chunk.size();
                    break;
                }
                _discarding = false;
                pos = nl + 1;
                _lineStart = base + pos;
                continue;
            }

            if (nl == std::string_view::npos) {
                appendTail(chunk.substr(pos), onLine);
                pos = chunk.size();
                break;
            }

            const std::string_view piece = chunk.substr(pos, nl - pos);
            if (_fragment.empty()) {
                if (piece.size() > _maxLen) {
                    // 한 청크 안에 통째로 들어온 초과 라인 — 빠른 경로에도 상한은 동일 적용
                    emitLine(piece.substr(0, _maxLen), _lineStart, true, onLine);
                } else {
                    emitLine(piece, _lineStart, false, onLine);  // 빠른 경로 — 복사 없음
                }
            } else if (_fragment.size() + piece.size() > _maxLen) {
                // 조각 + 이번 구간이 상한 초과. \n은 이미 있으므로 폐기 모드는 불필요
                const std::size_t keep = _maxLen - _fragment.size();
                _fragment.append(piece.data(), keep);
                emitLine(_fragment, _lineStart, true, onLine);
                _fragment.clear();
            } else {
                _fragment.append(piece);
                emitLine(_fragment, _lineStart, false, onLine);
                _fragment.clear();
            }
            pos = nl + 1;
            _lineStart = base + pos;
        }
        _streamOffset = base + chunk.size();
    }

    // EOF/업로드 완료: 개행 없는 마지막 라인 잔여분을 배출 (있으면 1회)
    template <typename Callback>
    void finish(Callback&& onLine) {
        if (!_discarding && !_fragment.empty()) {
            emitLine(_fragment, _lineStart, false, onLine);
        }
        _fragment.clear();
        _discarding = false;
    }

    // 세션 중단 → 상태 리셋 후 다음 세션 재사용 (총괄표 "세션 중단 시" 칸)
    void reset() {
        _fragment.clear();
        _streamOffset = 0;
        _lineStart = 0;
        _discarding = false;
    }

private:
    template <typename Callback>
    void emitLine(std::string_view text, std::uint64_t offset, bool tooLong, Callback&& onLine) {
        if (!tooLong && !text.empty() && text.back() == '\r') {
            text.remove_suffix(1);  // 라인 끝 \r만 제거 — 내부 \r은 검증 단계가 거부
        }
        onLine(Line{text, offset, tooLong});
    }

    // 개행 없는 청크 꼬리 처리 — 상한 도달 시 too-long 보고 후 폐기 모드 진입
    template <typename Callback>
    void appendTail(std::string_view tail, Callback&& onLine) {
        if (_fragment.size() + tail.size() > _maxLen) {
            const std::size_t keep = _maxLen - _fragment.size();
            _fragment.append(tail.data(), keep);
            emitLine(_fragment, _lineStart, true, onLine);
            _fragment.clear();
            _discarding = true;
        } else {
            _fragment.append(tail);
        }
    }

    std::string _fragment;            // 미완성 조각 — reserve로 상한 고정 (bounded)
    std::uint64_t _streamOffset = 0;  // 지금까지 feed된 총 바이트
    std::uint64_t _lineStart = 0;     // 현재 조립 중인 라인의 시작 오프셋
    bool _discarding = false;         // 상한 초과 후 다음 \n까지 폐기 중
    const std::size_t _maxLen;
};

}  // namespace server::parser
