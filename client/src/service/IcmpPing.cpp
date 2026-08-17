#include "service/IcmpPing.h"

#ifdef _WIN32

#include <winsock2.h>  // in_addr — iphlpapi보다 먼저 와야 한다
#include <ws2tcpip.h>  // inet_pton
#include <iphlpapi.h>
#include <icmpapi.h>

#include <array>
#include <cstdio>
#include <cstring>

namespace client {
namespace {

// echo 요청 데이터. 관례상 32바이트를 보낸다 (Windows ping.exe과 같은 크기)
constexpr const char kPayload[] = "log-transfer-analyzer ping probe";
constexpr DWORD kTimeoutMs = 1000;  // 회당 대기. LAN 진단이므로 길게 잡을 이유가 없다

// 응답 버퍼는 "ICMP_ECHO_REPLY + 보낸 데이터 + 여유"가 필요하다 (API 요구사항).
// 여유 8바이트는 오류 응답에 붙는 IP 헤더 조각용 — 문서가 요구하는 최소 여유다.
constexpr DWORD kReplyBufferSize =
    static_cast<DWORD>(sizeof(ICMP_ECHO_REPLY) + sizeof(kPayload) + 8);

std::string statusText(ULONG status) {
    switch (status) {
        case IP_SUCCESS:            return "success";
        case IP_DEST_HOST_UNREACHABLE: return "destination host unreachable";
        case IP_DEST_NET_UNREACHABLE:  return "destination net unreachable";
        case IP_REQ_TIMED_OUT:      return "request timed out";
        case IP_TTL_EXPIRED_TRANSIT: return "TTL expired in transit";
        case IP_BUF_TOO_SMALL:      return "reply buffer too small";
        default:                    break;
    }
    std::array<char, 48> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "ICMP status %lu", status);
    return std::string{buffer.data()};
}

std::string addressText(IPAddr address) {
    in_addr shown{};
    shown.S_un.S_addr = address;
    std::array<char, INET_ADDRSTRLEN> text{};
    if (::inet_ntop(AF_INET, &shown, text.data(), text.size()) == nullptr) {
        return std::string{};
    }
    return std::string{text.data()};
}

}  // namespace

PingOutcome icmpPing(const std::string& ip, int attempts, const PingReplySink& onReply) {
    PingOutcome outcome;

    // IPv4만 지원한다 → 근거: 서버 주소 입력이 IPv4 전제이고(design 5번 기본 포트/주소 규격),
    // IPv6까지 다루면 Icmp6 계열 분기가 더 붙는데 과제 범위에서 얻는 것이 없다.
    in_addr parsed{};
    if (::inet_pton(AF_INET, ip.c_str(), &parsed) != 1) {
        outcome.error = "not a valid IPv4 address: " + ip;
        return outcome;
    }

    // 핸들은 RAII로 닫는다 (컨벤션 1번). IcmpCloseHandle을 빠뜨리면 진단을 누를 때마다 샌다
    const HANDLE handle = ::IcmpCreateFile();
    if (handle == INVALID_HANDLE_VALUE) {
        outcome.error = "IcmpCreateFile failed";
        return outcome;
    }
    struct HandleGuard {
        HANDLE value;
        ~HandleGuard() { ::IcmpCloseHandle(value); }
    } guard{handle};

    std::array<char, kReplyBufferSize> replyBuffer{};
    outcome.replies.reserve(static_cast<std::size_t>(attempts > 0 ? attempts : 0));

    for (int attempt = 0; attempt < attempts; ++attempt) {
        PingReply reply;
        const DWORD count =
            ::IcmpSendEcho(handle, parsed.S_un.S_addr, const_cast<char*>(kPayload),
                           static_cast<WORD>(sizeof(kPayload)), nullptr, replyBuffer.data(),
                           kReplyBufferSize, kTimeoutMs);
        if (count == 0) {
            // 응답이 없다 = 타임아웃이 가장 흔하다. GetLastError로 사유를 구분한다
            const DWORD error = ::GetLastError();
            reply.success = false;
            reply.detail = (error == IP_REQ_TIMED_OUT) ? "request timed out"
                                                       : statusText(error);
        } else {
            const auto* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(replyBuffer.data());
            reply.success = (echo->Status == IP_SUCCESS);
            reply.roundTripMs = echo->RoundTripTime;
            reply.detail = reply.success ? addressText(echo->Address) : statusText(echo->Status);
        }

        // 누적보다 통지를 먼저 한다 — 한 회가 끝나는 즉시 화면에 뜨는 것이 이 콜백의 목적이다
        if (onReply) {
            onReply(reply);
        }
        outcome.replies.push_back(std::move(reply));
    }
    return outcome;
}

}  // namespace client

#else  // !_WIN32

namespace client {

// 클라이언트는 Windows 전용이지만 이 파일은 tests 타깃을 통해 Linux에서도 컴파일된다.
// raw 소켓 ICMP는 보통 root를 요구하므로 흉내내지 않고, "제공하지 않음"을 명시한다 —
// 조용히 성공을 반환하면 테스트가 진단 기능을 검증했다고 착각한다.
PingOutcome icmpPing(const std::string& /*ip*/, int /*attempts*/,
                     const PingReplySink& /*onReply*/) {
    PingOutcome outcome;
    outcome.supported = false;
    outcome.error = "ICMP diagnostics are available on the Windows client only";
    return outcome;  // 시도 자체를 하지 않으므로 onReply는 부르지 않는다
}

}  // namespace client

#endif  // _WIN32
