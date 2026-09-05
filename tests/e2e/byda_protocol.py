"""BYDA 프로토콜 인코딩/디코딩 (design 8번 헤더 바이너리 규격).

서버 구현(common/protocol)과 **독립적으로** 작성한다. 같은 코드를 재사용하면
"두 구현의 합의"가 아니라 자기 자신과 비교하는 셈이라 규격 위반을 못 잡는다.
CRC32도 zlib.crc32를 쓴다 — 서버의 테이블 방식 구현과 교차 검증하기 위함
(design 8번: "Python zlib.crc32와 교차 검증 가능").
"""

import socket
import struct
import zlib

MAGIC = b"BYDA"
VERSION = 1
PREAMBLE_SIZE = 8

# 메시지 타입 (design 8번)
TYPE_UPLOAD_HEADER = 1
TYPE_UPLOAD_TRAILER = 2
TYPE_ACK = 3
TYPE_RESULT_HEADER = 4
TYPE_DOWNLOAD_DONE = 5
TYPE_HEARTBEAT = 6  # 번호만 예약 — 서버는 거부해야 한다
TYPE_RESULT_REQUEST = 7

# Ack 상태 코드
ACK_OK = 0
ACK_CRC_MISMATCH = 1
ACK_SIZE_MISMATCH = 2
ACK_PROTOCOL_ERROR = 3
ACK_SERVER_ERROR = 4
ACK_NO_SUCH_RESULT = 5

ACK_NAMES = {
    ACK_OK: "OK",
    ACK_CRC_MISMATCH: "CRC_MISMATCH",
    ACK_SIZE_MISMATCH: "SIZE_MISMATCH",
    ACK_PROTOCOL_ERROR: "PROTOCOL_ERROR",
    ACK_SERVER_ERROR: "SERVER_ERROR",
    ACK_NO_SUCH_RESULT: "NO_SUCH_RESULT",
}

ACK_SIZE = PREAMBLE_SIZE + 1 + 8
RESULT_HEADER_SIZE = PREAMBLE_SIZE + 8 + 4
RESULT_REQUEST_FIXED_SIZE = PREAMBLE_SIZE + 8 + 4 + 8 + 2
MAX_FILE_SIZE = 8 * 1024 * 1024 * 1024


def preamble(msg_type, magic=MAGIC, version=VERSION):
    """공통 프리앰블 8바이트. 훼손 테스트를 위해 magic/version을 바꿀 수 있게 열어둔다."""
    return magic + bytes([version, msg_type, 0, 0])


def upload_header(file_size, filename):
    """type=1: fileSize u64 + filenameLen u16 + filename (전부 빅엔디안)."""
    name = filename.encode("utf-8")
    return preamble(TYPE_UPLOAD_HEADER) + struct.pack(">QH", file_size, len(name)) + name


def result_request(file_size, crc, start_offset, filename):
    """type=7: fileSize u64 + crc32 u32 + startOffset u64 + filenameLen u16 + filename.

    앞의 세 값이 청구표다 — 이 결과를 만든 업로드가 무엇이었는지 서버가 대조한다.
    인증이 아니라 "남의 결과를 잘못 건네지 않기" 위한 것이고, 같은 파일을 가진 사람은
    어차피 업로드로 같은 결과를 얻으므로 노출이 늘지 않는다.
    """
    name = filename.encode("utf-8")
    return (preamble(TYPE_RESULT_REQUEST)
            + struct.pack(">QIQH", file_size, crc, start_offset, len(name)) + name)


def upload_trailer(payload):
    """type=2: 페이로드 CRC32. zlib.crc32가 곧 교차 검증의 기준값이다."""
    return preamble(TYPE_UPLOAD_TRAILER) + struct.pack(">I", zlib.crc32(payload))


def download_done():
    return preamble(TYPE_DOWNLOAD_DONE)


def parse_ack(data):
    """→ (status, received_bytes). 규격 위반이면 예외."""
    if len(data) < ACK_SIZE:
        raise ValueError(f"Ack too short: {len(data)} < {ACK_SIZE}")
    if data[:4] != MAGIC:
        raise ValueError(f"Ack magic mismatch: {data[:4]!r}")
    if data[4] != VERSION:
        raise ValueError(f"Ack version mismatch: {data[4]}")
    if data[5] != TYPE_ACK:
        raise ValueError(f"expected Ack(3), got type {data[5]}")
    status = data[PREAMBLE_SIZE]
    (received,) = struct.unpack(">Q", data[PREAMBLE_SIZE + 1 : ACK_SIZE])
    return status, received


def parse_result_header(data):
    """→ (csv_size, crc32)."""
    if len(data) < RESULT_HEADER_SIZE:
        raise ValueError(f"ResultHeader too short: {len(data)}")
    if data[:4] != MAGIC or data[4] != VERSION:
        raise ValueError("ResultHeader preamble mismatch")
    if data[5] != TYPE_RESULT_HEADER:
        raise ValueError(f"expected ResultHeader(4), got type {data[5]}")
    return struct.unpack(">QI", data[PREAMBLE_SIZE:RESULT_HEADER_SIZE])


class Connection:
    """세션 하나를 주도하는 클라이언트. 수신은 필요한 만큼만 정확히 읽는다."""

    def __init__(self, port, host="127.0.0.1", timeout=30.0):
        self.socket = socket.create_connection((host, port), timeout=timeout)

    def send(self, data):
        self.socket.sendall(data)

    def send_payload(self, payload, chunk_size=64 * 1024):
        view = memoryview(payload)
        for offset in range(0, len(payload), chunk_size):
            self.socket.sendall(view[offset : offset + chunk_size])

    def recv_exactly(self, count):
        """정확히 count 바이트. 상대가 먼저 닫으면 ConnectionError."""
        buffer = bytearray()
        while len(buffer) < count:
            chunk = self.socket.recv(count - len(buffer))
            if not chunk:
                raise ConnectionError(f"closed after {len(buffer)}/{count} bytes")
            buffer += chunk
        return bytes(buffer)

    def read_ack(self):
        return parse_ack(self.recv_exactly(ACK_SIZE))

    def read_result_header(self):
        """→ (csv_size, crc). 본문을 읽지 않고 헤더만 — 중간에 끊는 시나리오에 필요하다."""
        return parse_result_header(self.recv_exactly(RESULT_HEADER_SIZE))

    def read_some(self, count):
        """정확히 count 바이트만 읽고 나머지는 소켓에 남겨둔다."""
        return self.recv_exactly(count)

    def peek_reply_type(self):
        """응답 프리앰블만 읽어 타입을 본다 — 재요청의 답은 Ack 또는 ResultHeader다.

        → (msg_type, preamble_bytes). 프리앰블은 호출부가 본문과 이어 붙여 파싱한다.
        """
        head = self.recv_exactly(PREAMBLE_SIZE)
        return head[5], head

    def read_result(self):
        """→ (csv_bytes, crc_ok). 헤더의 CRC와 실제 CSV의 zlib.crc32를 비교한다."""
        csv_size, crc = parse_result_header(self.recv_exactly(RESULT_HEADER_SIZE))
        csv = self.recv_exactly(csv_size)
        return csv, zlib.crc32(csv) == crc

    def expect_closed(self, timeout=5.0):
        """서버가 응답 없이 닫았는지 확인 (검증 실패 ①부류)."""
        self.socket.settimeout(timeout)
        try:
            return self.socket.recv(1) == b""
        except (ConnectionResetError, TimeoutError, socket.timeout):
            return True

    def abort(self):
        """RST로 강제 단절 — '전송 도중 강제 단절' 시나리오용."""
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.socket.close()

    def close(self):
        try:
            self.socket.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()
