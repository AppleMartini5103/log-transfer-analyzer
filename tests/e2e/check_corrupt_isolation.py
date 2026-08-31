#!/usr/bin/env python3
"""손상 세션의 통계가 다음 세션 결과에 새어나오는지 손으로 확인하는 스크립트.

리뷰 지적 1번("손상 판정 전에 분석이 시작되므로, 이미 계산된 통계가 확실히 폐기되는지")을
코드 읽기가 아니라 실행으로 답하기 위한 것이다.

절차:
  세션 A - 모듈 RadarTrackNodeState 로그를 보내되 트레일러 CRC를 일부러 틀린다
           → 서버는 Ack(CRC_MISMATCH)를 보내고 닫아야 한다
  세션 B - 모듈 SectorSchedulerRTS 로그만 보내고 정상 완주한다
           → 돌아온 result.csv에 A의 모듈이 하나도 없어야 하고,
             B의 카운트가 정확히 보낸 줄 수와 같아야 한다

두 번째 조건이 핵심이다. "A의 모듈이 없다"만 보면 A의 줄 수가 B에 합산됐을 가능성을 놓친다.

사용법:
  python3 check_corrupt_isolation.py --port 23507
  (서버를 먼저 띄워둘 것: ./build/server/server -p 23507)
"""
import argparse
import struct
import sys
import zlib

import byda_protocol as proto
import log_fixtures as fixtures

MODULE_A = "RadarTrackNodeState"
MODULE_B = "SectorSchedulerRTS"
LINES_A = 40
LINES_B = 7


def make_log(module, count):
    return "".join(fixtures.good_line(module, second=i % 60) + "\n" for i in range(count)).encode()


def corrupt_trailer(payload):
    """올바른 CRC에 1을 더해 어긋나게 만든다 — 페이로드는 손대지 않는다."""
    bad = (zlib.crc32(payload) + 1) & 0xFFFFFFFF
    return proto.preamble(proto.TYPE_UPLOAD_TRAILER) + struct.pack(">I", bad)


def session_a(port, host):
    payload = make_log(MODULE_A, LINES_A)
    conn = proto.Connection(port, host=host)
    try:
        conn.send(proto.upload_header(len(payload), "corrupt.log"))
        conn.send_payload(payload)
        conn.send(corrupt_trailer(payload))
        status, received = conn.read_ack()
        return proto.ACK_NAMES[status], received
    finally:
        conn.close()


def session_b(port, host):
    payload = make_log(MODULE_B, LINES_B)
    conn = proto.Connection(port, host=host)
    try:
        conn.send(proto.upload_header(len(payload), "clean.log"))
        conn.send_payload(payload)
        conn.send(proto.upload_trailer(payload))
        status, _ = conn.read_ack()
        if proto.ACK_NAMES[status] != "OK":
            raise SystemExit(f"session B was rejected: {proto.ACK_NAMES[status]}")
        csv, crc_ok = conn.read_result()
        conn.send(proto.download_done())
        return csv, crc_ok
    finally:
        conn.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=23507)
    parser.add_argument("--host", default="127.0.0.1")
    options = parser.parse_args()

    print(f"[A] uploading {LINES_A} {MODULE_A} lines with a deliberately wrong CRC...")
    name, received = session_a(options.port, options.host)
    print(f"    server answered Ack({name}), received {received} bytes")
    if name != "CRC_MISMATCH":
        print(f"    UNEXPECTED: wanted CRC_MISMATCH")
        return 1

    print(f"[B] uploading {LINES_B} {MODULE_B} lines with a correct CRC...")
    csv, crc_ok = session_b(options.port, options.host)
    print(f"    got result.csv, {len(csv)} bytes, header CRC {'matches' if crc_ok else 'MISMATCH'}")

    buckets, metrics = fixtures.parse_result_csv(csv)
    leaked = {k: v for k, v in buckets.items() if k[0] == MODULE_A}
    counted_b = sum(v for k, v in buckets.items() if k[0] == MODULE_B)

    print()
    print("=== result of session B ===")
    for (module, hour), count in sorted(buckets.items()):
        print(f"    {module:24} {hour}  {count}")
    print(f"    skipped_lines = {metrics.get('skipped_lines')}")
    print()

    ok = True
    if leaked:
        print(f"FAIL: session A's module leaked into B's result: {leaked}")
        ok = False
    else:
        print(f"OK  : no {MODULE_A} bucket - session A's statistics were discarded")

    if counted_b != LINES_B:
        print(f"FAIL: {MODULE_B} counted {counted_b}, expected {LINES_B}"
              f" (session A's lines may have been added)")
        ok = False
    else:
        print(f"OK  : {MODULE_B} counted exactly {LINES_B} - nothing was carried over")

    print()
    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
