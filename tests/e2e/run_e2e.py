#!/usr/bin/env python3
"""서버 E2E 테스트 드라이버 (표준 라이브러리만 사용 — pip 설치 불필요).

    ./run_e2e.py --server ../../build/server/server
    ./run_e2e.py --server <path> --log <BYDA_Test_Log_500MB.log>   # 500MB 실물 포함
    ./run_e2e.py --server <path> --slow                            # 타임아웃 시나리오 포함

design README 구현 순서 4단계 "Python 테스트 드라이버로 서버 E2E (독약·강제 단절·
타임아웃 주입, zlib.crc32 교차 검증)"의 구현이며, 평가 기준 4개를 바깥에서 검증한다:
메모리·RAII / 독약 데이터 / 네트워크 견고성 / 결과 정확성.
"""

import argparse
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import byda_protocol as proto  # noqa: E402
import log_fixtures as fixtures  # noqa: E402
from server_harness import ServerProcess  # noqa: E402

MEMORY_LIMIT_KB = 50 * 1024  # 과제 제약: 프로세스 최대 메모리 50MB 이하


class Failure(AssertionError):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def check_equal(actual, expected, label):
    if actual != expected:
        raise Failure(f"{label}: expected {expected!r}, got {actual!r}")


# ── 공통 흐름 ────────────────────────────────────────────────────────────────


def upload(connection, payload, filename="test.log", chunk_size=64 * 1024):
    connection.send(proto.upload_header(len(payload), filename))
    connection.send_payload(payload, chunk_size)
    connection.send(proto.upload_trailer(payload))


def full_session(server, payload, filename="test.log", chunk_size=64 * 1024):
    """정상 왕복 한 바퀴 → (ack_status, received, csv_bytes, crc_ok)."""
    with proto.Connection(server.port) as connection:
        upload(connection, payload, filename, chunk_size)
        status, received = connection.read_ack()
        if status != proto.ACK_OK:
            return status, received, None, False
        csv, crc_ok = connection.read_result()
        connection.send(proto.download_done())
        return status, received, csv, crc_ok


# ── 시나리오 ────────────────────────────────────────────────────────────────


def test_happy_path(server, _options):
    """정상 왕복: 통계 정확성 + CSV CRC 교차 검증 (zlib.crc32 기준)."""
    payload, expected = fixtures.build_log(good_per_module=100, poison=False)
    status, received, csv, crc_ok = full_session(server, payload)

    check_equal(proto.ACK_NAMES[status], "OK", "ack status")
    check_equal(received, len(payload), "receivedBytes")
    check(crc_ok, "result.csv CRC in ResultHeader did not match zlib.crc32 of the body")

    buckets, metrics = fixtures.parse_result_csv(csv)
    for module, count in expected["counts"].items():
        check_equal(buckets.get((module, "2026-06-19 22")), count, f"count for {module}")
    check_equal(metrics["avg_speed"], expected["avg_speed"], "avg_speed")
    check_equal(int(metrics["valid_spd_samples"]), expected["valid_spd_samples"], "valid samples")
    check_equal(int(metrics["skipped_lines"]), 0, "skipped_lines")


def test_poison_data(server, _options):
    """독약 5종 + 미문서화 5종: 크래시 없이 스킵되고 정상 라인은 끝까지 파싱된다."""
    payload, expected = fixtures.build_log(
        good_per_module=50, poison=True, unseen_poison=True
    )
    status, _, csv, _ = full_session(server, payload)
    check_equal(proto.ACK_NAMES[status], "OK", "ack status")

    buckets, metrics = fixtures.parse_result_csv(csv)
    # 평가 기준: "나머지 정상 라인의 파싱을 끝까지 완료"
    for module, count in expected["counts"].items():
        check_equal(buckets.get((module, "2026-06-19 22")), count, f"count for {module}")
    check_equal(int(metrics["skipped_lines"]), expected["skipped"], "skipped_lines")

    # 스킵 로그가 사유 코드별로 남았는지 (평가 기준: "스킵 및 로그 기록")
    report = server.read_artifact("skip_report.txt")
    check(report is not None, "skip_report.txt was not written")
    text = report.decode("utf-8", "replace")
    for name, _, reason_code in fixtures.POISON_LINES + fixtures.UNSEEN_POISON_LINES:
        check(reason_code in text, f"skip report missing reason {reason_code} (for {name})")


def test_unknown_poison_is_rejected_by_whitelist(server, _options):
    """블랙리스트였다면 통과했을 '처음 보는' 훼손이 전부 걸러지는지 개별 확인."""
    for name, line, _ in fixtures.UNSEEN_POISON_LINES:
        good = fixtures.good_line("RadarTrackNodeState")
        payload = (good + "\n" + line + "\n" + good + "\n").encode("utf-8")
        status, _, csv, _ = full_session(server, payload)
        check_equal(proto.ACK_NAMES[status], "OK", f"ack status ({name})")
        buckets, metrics = fixtures.parse_result_csv(csv)
        check_equal(buckets.get(("RadarTrackNodeState", "2026-06-19 22")), 2, f"counts ({name})")
        check_equal(int(metrics["skipped_lines"]), 1, f"skipped_lines ({name})")


def test_crc_mismatch(server, _options):
    """CRC 불일치 → 그냥 끊지 않고 사유를 알리고 닫는다 (design 8번)."""
    payload = b"payload-that-will-not-match\n"
    with proto.Connection(server.port) as connection:
        connection.send(proto.upload_header(len(payload), "bad-crc.log"))
        connection.send_payload(payload)
        connection.send(proto.preamble(proto.TYPE_UPLOAD_TRAILER) + struct.pack(">I", 0xDEADBEEF))
        status, _ = connection.read_ack()
        check_equal(proto.ACK_NAMES[status], "CRC_MISMATCH", "ack status")
        check(connection.expect_closed(), "server should close after CRC mismatch")


def test_protocol_violations(server, _options):
    """검증 실패 2부류가 규격대로 갈리는지 (design 11번)."""
    # ① 스트림 신뢰 불가 → 응답 없이 즉시 종료
    for label, header in [
        ("bad magic", proto.preamble(proto.TYPE_UPLOAD_HEADER, magic=b"XXXX")),
        ("unknown version", proto.preamble(proto.TYPE_UPLOAD_HEADER, version=2)),
        ("reserved heartbeat type", proto.preamble(proto.TYPE_HEARTBEAT)),
    ]:
        with proto.Connection(server.port) as connection:
            connection.send(header + struct.pack(">QH", 10, 5) + b"x.log")
            check(connection.expect_closed(), f"{label}: expected silent close")

    # ② 파싱은 됐으나 값 무효 → Ack(PROTOCOL_ERROR) 후 종료
    for label, header in [
        ("fileSize over 8GiB",
         proto.upload_header(proto.MAX_FILE_SIZE + 1, "huge.log")),
        ("path separator in filename",
         proto.upload_header(10, "../../etc/passwd")),
        ("empty filename",
         proto.preamble(proto.TYPE_UPLOAD_HEADER) + struct.pack(">QH", 10, 0)),
    ]:
        with proto.Connection(server.port) as connection:
            connection.send(header)
            status, _ = connection.read_ack()
            check_equal(proto.ACK_NAMES[status], "PROTOCOL_ERROR", f"{label}: ack status")


def test_split_header_bytes(server, _options):
    """헤더가 TCP 경계에 쪼개져 도착해도 누적 후 파싱된다 (컨벤션 9번)."""
    payload = fixtures.good_line("RadarTrackNodeState").encode("utf-8") + b"\n"
    header = proto.upload_header(len(payload), "split.log")
    with proto.Connection(server.port) as connection:
        for index in range(len(header)):  # 1바이트씩
            connection.send(header[index : index + 1])
            time.sleep(0.001)
        connection.send_payload(payload)
        connection.send(proto.upload_trailer(payload))
        status, received = connection.read_ack()
        check_equal(proto.ACK_NAMES[status], "OK", "ack status")
        check_equal(received, len(payload), "receivedBytes")


def test_abrupt_disconnect(server, _options):
    """전송 도중 RST 단절 → 서버 무크래시 + 자원 반환 + 다음 세션 정상 (평가 기준)."""
    payload, _ = fixtures.build_log(good_per_module=200, poison=False)
    connection = proto.Connection(server.port)
    connection.send(proto.upload_header(len(payload) * 10, "cut.log"))  # 크게 선언하고
    connection.send_payload(payload[: len(payload) // 2])  # 절반만 보낸 뒤
    connection.abort()  # RST로 끊는다
    time.sleep(0.3)

    check(server.is_alive(), "server died after an abrupt disconnect")
    # 중단된 세션의 부분 데이터가 다음 세션에 섞이면 안 된다
    fresh, expected = fixtures.build_log(good_per_module=10, poison=False)
    status, _, csv, _ = full_session(server, fresh)
    check_equal(proto.ACK_NAMES[status], "OK", "ack status after recovery")
    buckets, _ = fixtures.parse_result_csv(csv)
    for module, count in expected["counts"].items():
        check_equal(buckets.get((module, "2026-06-19 22")), count, f"count for {module}")


def test_disconnect_while_waiting_result(server, _options):
    """Ack 직후 결과를 안 받고 끊어도 서버가 살아남는다 (SIGPIPE/EPIPE 경로)."""
    payload, _ = fixtures.build_log(good_per_module=20, poison=False)
    connection = proto.Connection(server.port)
    upload(connection, payload, "leave.log")
    connection.read_ack()
    connection.abort()  # 결과 전송 중 끊김
    time.sleep(0.3)
    check(server.is_alive(), "server died when the peer vanished during result send")

    status, _, _, _ = full_session(server, payload)
    check_equal(proto.ACK_NAMES[status], "OK", "server should serve the next session")


def test_one_to_one_policy(server, _options):
    """세션 진행 중 새 연결은 백로그 대기 → CLEANUP 후 서비스된다 (design 11번)."""
    payload, _ = fixtures.build_log(good_per_module=20, poison=False)
    first = proto.Connection(server.port)
    first.send(proto.upload_header(len(payload), "first.log"))

    second = proto.Connection(server.port)  # TCP 연결은 성공한다 (거절이 아니라 대기)
    second.send(proto.upload_header(len(payload), "second.log"))
    second.send_payload(payload)
    second.send(proto.upload_trailer(payload))
    second.socket.settimeout(0.5)
    try:
        second.recv_exactly(1)
        raise Failure("second client should not be served while a session is active")
    except (TimeoutError, OSError):
        pass

    first.send_payload(payload)
    first.send(proto.upload_trailer(payload))
    check_equal(proto.ACK_NAMES[first.read_ack()[0]], "OK", "first ack")
    first.read_result()
    first.send(proto.download_done())
    first.close()

    # 대기 중 보낸 바이트가 보존된 채 이어서 처리되어야 한다
    second.socket.settimeout(10.0)
    status, received = second.read_ack()
    check_equal(proto.ACK_NAMES[status], "OK", "second ack after backlog wait")
    check_equal(received, len(payload), "second receivedBytes")
    second.close()


def test_empty_file(server, _options):
    """fileSize = 0도 정상 세션 (design 8번) — 빈 통계 CSV를 돌려준다."""
    status, received, csv, crc_ok = full_session(server, b"", "empty.log")
    check_equal(proto.ACK_NAMES[status], "OK", "ack status")
    check_equal(received, 0, "receivedBytes")
    check(crc_ok, "empty-session CSV CRC mismatch")
    _, metrics = fixtures.parse_result_csv(csv)
    check_equal(metrics["avg_speed"], "0.000000", "avg_speed")  # 0으로 나누지 않는다
    check_equal(int(metrics["skipped_lines"]), 0, "skipped_lines")


def test_server_side_artifacts(server, _options):
    """서버 쪽에도 result.csv가 남는지 (design 4번: 채점자가 서버에서 확인 가능)."""
    payload, _ = fixtures.build_log(good_per_module=10, poison=True)
    _, _, csv, _ = full_session(server, payload)
    on_disk = server.read_artifact("result.csv")
    check(on_disk is not None, "server did not write ./result.csv")
    # 전송본과 디스크본이 바이트 단위로 같아야 한다 (개행 변환·크기 불일치 방지)
    check_equal(on_disk, csv, "disk result.csv differs from the transmitted CSV")


def test_memory_limit(server, options):
    """전송 중 peak RSS ≤ 50MB (과제 절대 제약)."""
    payload, _ = fixtures.build_log(good_per_module=2000, poison=True)
    for _ in range(3):  # 여러 세션을 돌려 누수·누적이 없는지 함께 본다
        status, _, _, _ = full_session(server, payload)
        check_equal(proto.ACK_NAMES[status], "OK", "ack status")
        server.sample_rss_kb()
    peak = server.sample_rss_kb()
    check(peak > 0, "could not read VmHWM")
    check(peak <= MEMORY_LIMIT_KB, f"peak RSS {peak} kB exceeds the {MEMORY_LIMIT_KB} kB limit")
    print(f"      peak RSS = {peak} kB ({peak / 1024:.1f} MB) for {len(payload) // 1024} KB uploads")


def test_real_log(server, options):
    """실물 500MB 로그 — --log 로 경로를 준 경우에만 실행."""
    with open(options.log, "rb") as handle:
        payload = handle.read()
    started = time.time()
    status, received, csv, crc_ok = full_session(server, payload, os.path.basename(options.log))
    elapsed = time.time() - started

    check_equal(proto.ACK_NAMES[status], "OK", "ack status")
    check_equal(received, len(payload), "receivedBytes")
    check(crc_ok, "result.csv CRC mismatch")
    buckets, metrics = fixtures.parse_result_csv(csv)

    total_lines = sum(buckets.values()) + int(metrics["skipped_lines"])
    peak = server.sample_rss_kb()
    print(f"      {len(payload) / 1024 / 1024:.0f} MB in {elapsed:.2f}s "
          f"({len(payload) / elapsed / 1024 / 1024:.0f} MB/s), peak RSS {peak / 1024:.1f} MB")
    print(f"      lines={total_lines}, buckets={len(buckets)}, "
          f"skipped={metrics['skipped_lines']}, avg_speed={metrics['avg_speed']}")
    check(peak <= MEMORY_LIMIT_KB, f"peak RSS {peak} kB exceeds the limit")
    check(total_lines > 3_000_000, "expected millions of lines from the real log")


def test_wait_header_timeout(server, _options):
    """연결만 하고 헤더를 안 보내면 ②류 타임아웃(120초)이 유령 세션을 정리한다."""
    connection = proto.Connection(server.port, timeout=200.0)
    started = time.time()
    check(connection.expect_closed(timeout=180.0), "server did not time out an idle connection")
    elapsed = time.time() - started
    check(100 <= elapsed <= 170, f"timeout fired at {elapsed:.0f}s, expected ~120s")
    connection.close()
    check(server.is_alive(), "server should survive a session timeout")


def test_daemon_mode(_server, options):
    """-d 데몬 모드에서도 같은 프로토콜이 돈다 + PID 파일로 종료된다 (design 10번)."""
    with ServerProcess(options.server, daemon=True) as daemon:
        payload, expected = fixtures.build_log(good_per_module=10, poison=True)
        status, _, csv, _ = full_session(daemon, payload)
        check_equal(proto.ACK_NAMES[status], "OK", "ack status in daemon mode")
        _, metrics = fixtures.parse_result_csv(csv)
        check_equal(int(metrics["skipped_lines"]), expected["skipped"], "skipped_lines")

        check(daemon.artifact("server.pid") is not None, "daemon did not write server.pid")
        log = daemon.log_text()
        check("Server started" in log, "daemon log missing startup line")
        check(daemon.stop() == 0, "daemon did not shut down gracefully")
        check(daemon.artifact("server.pid") is None, "daemon left a stale pid file")


# 각 시나리오는 (이름, 함수, 전용 서버 필요 여부, 느린 테스트 여부)
SCENARIOS = [
    ("happy path", test_happy_path, False, False),
    ("empty file", test_empty_file, False, False),
    ("poison data (5 known + 5 unseen)", test_poison_data, False, False),
    ("unseen poison via whitelist", test_unknown_poison_is_rejected_by_whitelist, False, False),
    ("CRC mismatch", test_crc_mismatch, False, False),
    ("protocol violations", test_protocol_violations, False, False),
    ("header split byte by byte", test_split_header_bytes, False, False),
    ("abrupt disconnect mid-upload", test_abrupt_disconnect, False, False),
    ("disconnect while waiting result", test_disconnect_while_waiting_result, False, False),
    ("1:1 policy with backlog", test_one_to_one_policy, False, False),
    ("server-side artifacts", test_server_side_artifacts, False, False),
    ("peak RSS under 50MB", test_memory_limit, False, False),
    ("daemon mode", test_daemon_mode, True, False),
    ("real 500MB log", test_real_log, False, False),
    ("WAIT_HEADER timeout (~120s)", test_wait_header_timeout, False, True),
]


def main():
    parser = argparse.ArgumentParser(description="BYDA server end-to-end tests")
    parser.add_argument("--server", default="build/server/server", help="server binary path")
    parser.add_argument("--log", help="real 500MB log file (enables the real-log scenario)")
    parser.add_argument("--slow", action="store_true", help="include timeout scenarios (~2 min)")
    parser.add_argument("--filter", help="run only scenarios whose name contains this text")
    options = parser.parse_args()

    if not os.path.exists(options.server):
        print(f"error: server binary not found: {options.server}", file=sys.stderr)
        return 2

    selected = []
    for name, function, needs_own_server, is_slow in SCENARIOS:
        if is_slow and not options.slow:
            continue
        if function is test_real_log and not options.log:
            continue
        if options.filter and options.filter not in name:
            continue
        selected.append((name, function, needs_own_server))

    print(f"Running {len(selected)} E2E scenario(s) against {options.server}\n")
    failures = []
    for index, (name, function, needs_own_server) in enumerate(selected, start=1):
        print(f"[{index}/{len(selected)}] {name} ... ", end="", flush=True)
        started = time.time()
        try:
            if needs_own_server:
                function(None, options)
            else:
                with ServerProcess(options.server) as server:
                    function(server, options)
                    check(server.is_alive(), "server died during the scenario")
                    exit_code = server.stop()
                    check_equal(exit_code, 0, "server exit code")
            print(f"ok ({time.time() - started:.1f}s)")
        except Exception as error:  # noqa: BLE001 — 시나리오 실패를 모두 모아 보고한다
            print(f"FAILED ({time.time() - started:.1f}s)")
            print(f"      {type(error).__name__}: {error}")
            failures.append((name, error))

    print()
    if failures:
        print(f"{len(failures)}/{len(selected)} scenario(s) FAILED:")
        for name, error in failures:
            print(f"  - {name}: {error}")
        return 1
    print(f"All {len(selected)} scenario(s) passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
