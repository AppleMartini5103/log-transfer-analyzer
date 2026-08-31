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

모드가 셋이고, 각각 다른 것을 검사한다:

  기본(connect-after) : A가 끝난 뒤 B가 연결한다 → onConnection 경로로 수락
  --backlog           : B가 미리 대기하다 CLEANUP 직후 수락된다 → onReapCb 경로로 수락
  --preload           : B가 대기하면서 **업로드 전량을 미리 send까지** 해둔다

앞의 두 모드는 "폐기가 일어나는가"만 검사한다. 게이트(SessionManager가 파서의 폐기 완료를
기다렸다 accept하는 것)를 없애도 통과한다 — B가 A의 Ack를 읽은 뒤에야 헤더를 보내므로,
B의 바이트는 파서가 이미 깨어나 폐기를 끝낸 한참 뒤에 도착하기 때문이다. 실제로 게이트를
무력화한 바이너리로 두 모드 모두 PASS하는 것을 확인했다.

--preload가 그 구멍을 메운다. B의 바이트가 커널 수신 큐에 이미 들어있는 상태로 만들면,
게이트가 없을 때 루프 스레드는 accept → startRead → onRead → beginSession → 링 push 를
한 번도 양보하지 않고 연달아 실행할 수 있다. 그 뒤에 파서가 깨어나 resetSessionState()를
돌리면 **B의 데이터**가 폐기된다 → B의 카운트가 모자라 FAILED. 게이트가 있으면 폐기 완료
신호 전에는 accept 자체를 하지 않으므로 이 순서가 성립할 수 없다.

다만 이것은 스레드 스케줄링에 달린 확률적 경쟁이라 1회 통과는 근거가 약하다.
--rounds로 반복해서 "한 번도 지지 않았다"를 확인해야 한다.

사용법:
  python3 check_corrupt_isolation.py
  python3 check_corrupt_isolation.py --backlog              # reap 경로
  python3 check_corrupt_isolation.py --preload --rounds 20  # 게이트 검증 (변이 테스트용)
  python3 check_corrupt_isolation.py --settle 3             # 파서가 느릴 때 여유를 더 준다
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


def send_session_b(conn):
    """B의 업로드 전량을 밀어 넣기만 하고 응답은 읽지 않는다.

    --preload에서 A가 아직 세션을 쥐고 있는 동안 호출한다. 서버는 아직 이 연결을
    accept조차 하지 않았으므로 바이트는 커널 수신 큐에 그대로 쌓인다 (7줄 ~1KB라
    소켓 버퍼에 충분히 들어간다). 수락되는 순간 첫 read 콜백에 전량이 실린다.
    """
    payload = make_log(MODULE_B, LINES_B)
    conn.send(proto.upload_header(len(payload), "clean.log"))
    conn.send_payload(payload)
    conn.send(proto.upload_trailer(payload))


def recv_session_b(conn):
    """B의 Ack와 result.csv를 받는다."""
    status, _ = conn.read_ack()
    if proto.ACK_NAMES[status] != "OK":
        raise SystemExit(f"session B was rejected: {proto.ACK_NAMES[status]}")
    csv, crc_ok = conn.read_result()
    conn.send(proto.download_done())
    return csv, crc_ok


def run_session_b(conn):
    """정상 세션. 돌아온 result.csv와 CRC 일치 여부를 준다."""
    send_session_b(conn)
    return recv_session_b(conn)


def run_round(options, verbose):
    """A(손상) → B(정상) 한 판을 돌리고 (성공여부, 실패사유들)을 준다."""
    early = options.backlog or options.preload

    def say(text):
        if verbose:
            print(text)

    conn_a = proto.Connection(options.port, host=options.host)
    conn_b = None
    try:
        if early:
            # A가 세션을 쥔 상태에서 B를 연결해 백로그에 세워둔다
            time.sleep(0.2)
            conn_b = proto.Connection(options.port, host=options.host)
            say("    B connected first and is waiting in the backlog")
            if options.preload:
                send_session_b(conn_b)
                say("    B already pushed its whole upload into the kernel queue")

        say(f"[A] uploading {LINES_A} {MODULE_A} lines with a deliberately wrong CRC...")
        name, received = run_session_a(conn_a, options.settle)
        say(f"    server answered Ack({name}), received {received} bytes")
        if name != "CRC_MISMATCH":
            return False, [f"session A got Ack({name}), wanted CRC_MISMATCH"]
    finally:
        conn_a.close()

    if conn_b is None:
        conn_b = proto.Connection(options.port, host=options.host)

    try:
        say(f"[B] uploading {LINES_B} {MODULE_B} lines with a correct CRC...")
        if options.preload:
            csv, crc_ok = recv_session_b(conn_b)  # 이미 보냈으므로 받기만 한다
        else:
            csv, crc_ok = run_session_b(conn_b)
    finally:
        conn_b.close()
    say(f"    got result.csv, {len(csv)} bytes, header CRC {'matches' if crc_ok else 'MISMATCH'}")

    buckets, metrics = fixtures.parse_result_csv(csv)
    leaked = {k: v for k, v in buckets.items() if k[0] == MODULE_A}
    counted_b = sum(v for k, v in buckets.items() if k[0] == MODULE_B)

    if verbose:
        print()
        print("=== result of session B ===")
        for (module, hour), count in sorted(buckets.items()):
            print(f"    {module:24} {hour}  {count}")
        print(f"    skipped_lines = {metrics.get('skipped_lines')}")
        print()

    problems = []
    if leaked:
        problems.append(f"session A's module leaked into B's result: {leaked}")
    elif verbose:
        print(f"OK  : no {MODULE_A} bucket - session A's statistics were discarded")

    if counted_b != LINES_B:
        problems.append(f"{MODULE_B} counted {counted_b}, expected {LINES_B}"
                        f" (A's lines added, or B's own lines were discarded)")
    elif verbose:
        print(f"OK  : {MODULE_B} counted exactly {LINES_B} - nothing was carried over")

    return not problems, problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=23507)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--settle", type=float, default=1.0,
                        help="seconds to wait after the payload so the parser consumes it")
    parser.add_argument("--backlog", action="store_true",
                        help="connect B before A finishes so the server accepts it straight "
                             "out of the backlog (exercises the reap path instead of onConnection)")
    parser.add_argument("--preload", action="store_true",
                        help="like --backlog, but B also sends its whole upload up front so the "
                             "bytes are already queued when the server accepts it (this is what "
                             "makes the accept-after-discard gate observable)")
    parser.add_argument("--rounds", type=int, default=1,
                        help="repeat the scenario N times; the race is probabilistic, so a single "
                             "pass proves little (use 20+ with --preload)")
    options = parser.parse_args()

    if options.preload:
        mode = "preload (bytes already queued when B is accepted)"
    elif options.backlog:
        mode = "backlog (reap path)"
    else:
        mode = "connect-after (onConnection path)"
    print(f"mode: {mode}, rounds: {options.rounds}")

    failures = 0
    for index in range(options.rounds):
        verbose = options.rounds == 1
        if not verbose:
            print(f"round {index + 1}/{options.rounds}...", end=" ", flush=True)
        ok, problems = run_round(options, verbose)
        if ok:
            if not verbose:
                print("ok")
        else:
            failures += 1
            if not verbose:
                print("FAIL")
            for text in problems:
                print(f"    FAIL: {text}")

    print()
    if failures:
        print(f"FAILED  ({failures}/{options.rounds} rounds lost the race)")
        return 1
    print(f"PASS  ({options.rounds}/{options.rounds} rounds clean)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
