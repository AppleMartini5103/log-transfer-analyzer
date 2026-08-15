#include "protocol/Codec.h"

namespace common::protocol {

namespace {

// ── BE 기록 헬퍼 — 시프트 연산 명시 (memcpy 금지, 컨벤션 9번) ───────────────
void putU16(std::vector<char>& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v & 0xFF));
}

void putU32(std::vector<char>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}

void putU64(std::vector<char>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}

// ── BE 판독 헬퍼 — 호출부가 길이를 보장한 오프셋에서만 사용 ─────────────────
std::uint8_t byteAt(ByteView buf, std::size_t off) {
    return static_cast<std::uint8_t>(buf[off]);
}

std::uint16_t getU16(ByteView buf, std::size_t off) {
    return static_cast<std::uint16_t>((byteAt(buf, off) << 8) | byteAt(buf, off + 1));
}

std::uint32_t getU32(ByteView buf, std::size_t off) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v = (v << 8) | byteAt(buf, off + i);
    }
    return v;
}

std::uint64_t getU64(ByteView buf, std::size_t off) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v = (v << 8) | byteAt(buf, off + i);
    }
    return v;
}

void putPreamble(std::vector<char>& out, MessageType type) {
    for (const auto b : kMagic) {
        out.push_back(static_cast<char>(b));
    }
    out.push_back(static_cast<char>(kProtocolVersion));
    out.push_back(static_cast<char>(type));
    out.push_back(0);  // reserved 2B = 0 (송신 측은 0, 수신 측은 무시 — design 8번)
    out.push_back(0);
}

// 프리앰블 검증 — 순서 고정: magic → version → type (검증 실패 ① 부류 판별)
// expected와 다른 "알려진 type"도 BadType — 1:1 세션은 메시지 순서가 고정이라 순서 위반 = 스트림 꼬임
DecodeStatus checkPreamble(ByteView buf, MessageType expected) {
    if (buf.size() < kPreambleSize) {
        return DecodeStatus::NeedMoreData;
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (byteAt(buf, kOffsetMagic + i) != kMagic[i]) {
            return DecodeStatus::BadMagic;
        }
    }
    if (byteAt(buf, kOffsetVersion) != kProtocolVersion) {
        return DecodeStatus::BadVersion;
    }
    if (byteAt(buf, kOffsetType) != static_cast<std::uint8_t>(expected)) {
        return DecodeStatus::BadType;
    }
    return DecodeStatus::Ok;  // reserved 2B는 값 무시
}

bool isValidFilename(std::string_view name) {
    // design 8번: 길이 1~255, 제어문자(0x00~0x1F, 0x7F)·경로 구분자(/ \) 금지.
    // 한글 등 비ASCII(UTF-8) 허용 — 경로로 쓰지 않는 값에 ASCII 화이트리스트는 과잉
    if (name.empty() || name.size() > kMaxFilenameLen) {
        return false;
    }
    for (const char ch : name) {
        const auto b = static_cast<std::uint8_t>(ch);
        if (b <= 0x1F || b == 0x7F || ch == '/' || ch == '\\') {
            return false;
        }
    }
    return true;
}

}  // namespace

DecodeStatus expectedMessageSize(ByteView buf, std::size_t& outSize) {
    if (buf.size() < kPreambleSize) {
        return DecodeStatus::NeedMoreData;
    }
    // 크기 판정 단계에서는 type만 필요 — magic/version 불량은 여기서도 즉시 걸러낸다
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (byteAt(buf, kOffsetMagic + i) != kMagic[i]) {
            return DecodeStatus::BadMagic;
        }
    }
    if (byteAt(buf, kOffsetVersion) != kProtocolVersion) {
        return DecodeStatus::BadVersion;
    }
    switch (static_cast<MessageType>(byteAt(buf, kOffsetType))) {
        case MessageType::UploadHeader:
            if (buf.size() < kUploadHeaderFixedSize) {
                return DecodeStatus::NeedMoreData;  // filenameLen(u16)까지 있어야 크기 확정
            }
            outSize = kUploadHeaderFixedSize + getU16(buf, kPreambleSize + 8);
            return DecodeStatus::Ok;
        case MessageType::UploadTrailer:
            outSize = kUploadTrailerSize;
            return DecodeStatus::Ok;
        case MessageType::Ack:
            outSize = kAckSize;
            return DecodeStatus::Ok;
        case MessageType::ResultHeader:
            outSize = kResultHeaderSize;
            return DecodeStatus::Ok;
        case MessageType::DownloadDone:
            outSize = kDownloadDoneSize;
            return DecodeStatus::Ok;
    }
    return DecodeStatus::BadType;  // 미지의 type (Heartbeat=6 예약 포함 — 미구현이므로 거부)
}

std::vector<char> encode(const UploadHeader& msg) {
    std::vector<char> out;
    out.reserve(kUploadHeaderFixedSize + msg.filename.size());
    putPreamble(out, MessageType::UploadHeader);
    putU64(out, msg.fileSize);
    putU16(out, static_cast<std::uint16_t>(msg.filename.size()));
    out.insert(out.end(), msg.filename.begin(), msg.filename.end());
    return out;
}

std::vector<char> encode(const UploadTrailer& msg) {
    std::vector<char> out;
    out.reserve(kUploadTrailerSize);
    putPreamble(out, MessageType::UploadTrailer);
    putU32(out, msg.crc32);
    return out;
}

std::vector<char> encode(const Ack& msg) {
    std::vector<char> out;
    out.reserve(kAckSize);
    putPreamble(out, MessageType::Ack);
    out.push_back(static_cast<char>(msg.status));
    putU64(out, msg.receivedBytes);
    return out;
}

std::vector<char> encode(const ResultHeader& msg) {
    std::vector<char> out;
    out.reserve(kResultHeaderSize);
    putPreamble(out, MessageType::ResultHeader);
    putU64(out, msg.csvSize);
    putU32(out, msg.crc32);
    return out;
}

std::vector<char> encodeDownloadDone() {
    std::vector<char> out;
    out.reserve(kDownloadDoneSize);
    putPreamble(out, MessageType::DownloadDone);
    return out;
}

DecodeStatus decode(ByteView buf, UploadHeader& out) {
    const DecodeStatus pre = checkPreamble(buf, MessageType::UploadHeader);
    if (pre != DecodeStatus::Ok) {
        return pre;
    }
    if (buf.size() < kUploadHeaderFixedSize) {
        return DecodeStatus::NeedMoreData;
    }
    const std::uint64_t fileSize = getU64(buf, kPreambleSize);
    const std::uint16_t nameLen = getU16(buf, kPreambleSize + 8);
    if (buf.size() < kUploadHeaderFixedSize + nameLen) {
        return DecodeStatus::NeedMoreData;
    }
    // 값 검증 (검증 실패 ② 부류) — 길이 프리픽스 검증 후 가변부 접근 (컨벤션 9번 순서)
    if (fileSize > kMaxFileSize) {
        return DecodeStatus::BadValue;  // fileSize = 0은 허용 (빈 파일도 정상 세션)
    }
    const ByteView name = buf.substr(kUploadHeaderFixedSize, nameLen);
    if (!isValidFilename(name)) {
        return DecodeStatus::BadValue;
    }
    out.fileSize = fileSize;
    out.filename.assign(name);
    return DecodeStatus::Ok;
}

DecodeStatus decode(ByteView buf, UploadTrailer& out) {
    const DecodeStatus pre = checkPreamble(buf, MessageType::UploadTrailer);
    if (pre != DecodeStatus::Ok) {
        return pre;
    }
    if (buf.size() < kUploadTrailerSize) {
        return DecodeStatus::NeedMoreData;
    }
    out.crc32 = getU32(buf, kPreambleSize);
    return DecodeStatus::Ok;
}

DecodeStatus decode(ByteView buf, Ack& out) {
    const DecodeStatus pre = checkPreamble(buf, MessageType::Ack);
    if (pre != DecodeStatus::Ok) {
        return pre;
    }
    if (buf.size() < kAckSize) {
        return DecodeStatus::NeedMoreData;
    }
    const std::uint8_t status = byteAt(buf, kPreambleSize);
    if (status > static_cast<std::uint8_t>(AckStatus::ServerError)) {
        return DecodeStatus::BadValue;  // 미지의 상태 코드 — default-deny
    }
    out.status = static_cast<AckStatus>(status);
    out.receivedBytes = getU64(buf, kPreambleSize + 1);
    return DecodeStatus::Ok;
}

DecodeStatus decode(ByteView buf, ResultHeader& out) {
    const DecodeStatus pre = checkPreamble(buf, MessageType::ResultHeader);
    if (pre != DecodeStatus::Ok) {
        return pre;
    }
    if (buf.size() < kResultHeaderSize) {
        return DecodeStatus::NeedMoreData;
    }
    out.csvSize = getU64(buf, kPreambleSize);
    out.crc32 = getU32(buf, kPreambleSize + 8);
    return DecodeStatus::Ok;
}

DecodeStatus decodeDownloadDone(ByteView buf) {
    return checkPreamble(buf, MessageType::DownloadDone);
}

}  // namespace common::protocol
