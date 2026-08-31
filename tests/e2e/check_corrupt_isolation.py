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

서버가 다음 연결을 받는 경로는 둘이고 둘 다 확인해야 한다 —
기본 모드는 A가 끝난 뒤 B가 연결하는 onConnection 경로, --backlog는 B가 미리 대기하다
CLEANUP 직후 수락되는 onReapCb 경로다. 한쪽만 막으면 경쟁은 다른 문으로 들어온다.

사용법:
  python3 check_corrupt_isolation.py
  python3 check_corrupt_isolation.py --backlog     # B를 먼저 대기시켜 reap 경로를 탄다
  python3 check_corrupt_isolation.py --settle 3    # 파서가 느릴 때 여유를 더 준다
  (서버를 먼저 띄워둘 것: ./build/server/server -p 23507)
"""
import argparse
import struct
import sys
import time
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


def run_session_a(conn, settle):
    """CRC만 어긋나게 해서 서버가 세션을 중단하게 만든다."""
    payload = make_log(MODULE_A, LINES_A)
    conn.send(proto.upload_header(len(payload), "corrupt.log"))
    conn.send_payload(payload)
    # 트레일러를 바로 보내지 않고 기다린다.
    #
    # 왜 필요한가: 이 지연이 없으면 페이로드-트레일러-CRC실패-abort가 파서 스레드가
    # 링 슬롯을 소비하기도 전에 끝난다. resetSessionState()는 남은 슬롯을 파싱하지 않고
    # 버리므로, 통계가 애초에 쌓이지 않아 "폐기됐다"를 검증하지 못한다.
    # 실제 500MB 전송은 수십 초가 걸려 파서가 대부분을 이미 처리한 뒤 CRC가 실패한다 —
    # 리뷰가 지적한 상황이 그것이므로, 그 조건을 지연으로 재현한다.
    time.sleep(settle)
    conn.send(corrupt_trailer(payload))
    status, received = conn.read_ack()
    return proto.ACK_NAMES[status], received


def run_session_b(conn):
    """정상 세션. 돌아온 result.csv와 CRC 일치 여부를 준다."""
    payload = make_log(MODULE_B, LINES_B)
    conn.send(proto.upload_header(len(payload), "clean.log"))
    conn.send_payload(payload)
    conn.send(proto.upload_trailer(payload))
    status, _ = conn.read_ack()
    if proto.ACK_NAMES[status] != "OK":
        raise SystemExit(f"session B was rejected: {proto.ACK_NAMES[status]}")
    csv, crc_ok = conn.read_result()
    conn.send(proto.download_done())
    return csv, crc_ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=23507)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--settle", type=float, default=1.0,
                        help="seconds to wait after the payload so the parser consumes it")
    parser.add_argument("--backlog", action="store_true",
                        help="connect B before A finishes so the server accepts it straight "
                             "out of the backlog (exercises the reap path instead of onConnection)")
    options = parser.parse_args()

    # 서버가 다음 연결을 받는 경로는 둘이고, 둘 다 폐기 완료를 기다려야 한다.
    #   기본    : A가 끝난 뒤 B가 연결한다  → onConnection 경로
    #   backlog : B가 먼저 대기하다 CLEANUP 직후 수락된다 → onReapCb 경로
    # 한쪽만 막으면 경쟁은 다른 문으로 들어온다.
    mode = "backlog (reap path)" if options.backlog else "connect-after (onConnection path)"
    print(f"mode: {mode}")

    conn_a = proto.Connection(options.port, host=options.host)
    conn_b = None
    try:
        if options.backlog:
            # A가 세션을 쥔 상태에서 B를 연결해 백로그에 세워둔다
            time.sleep(0.2)
            conn_b = proto.Connection(options.port, host=options.host)
            print("    B connected first and is waiting in the backlog")

        print(f"[A] uploading {LINES_A} {MODULE_A} lines with a deliberately wrong CRC...")
        name, received = run_session_a(conn_a, options.settle)
        print(f"    server answered Ack({name}), received {received} bytes")
        if name != "CRC_MISMATCH":
            print("    UNEXPECTED: wanted CRC_MISMATCH")
            return 1
    finally:
        conn_a.close()

    if conn_b is None:
        conn_b = proto.Connection(options.port, host=options.host)

    try:
        print(f"[B] uploading {LINES_B} {MODULE_B} lines with a correct CRC...")
        csv, crc_ok = run_session_b(conn_b)
    finally:
        conn_b.close()
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
