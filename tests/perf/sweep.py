#!/usr/bin/env python3
"""벤치마크 스윕 — design "검증(실측 필수)" 절의 세 실험을 실행한다.

    실험        변수                                측정 지표
    청크 스윕    32/64/128/256KB/1MB (버퍼 고정)      CPU 사용량 + peak RSS
    버퍼 스윕    SO_SNDBUF 64KB~4MB (청크 고정)       전송 시간(처리량)
    상한 검증    최종 설정                            전송 중 peak RSS ≤ 50MB

사용:
    ./sweep.py --server ../../build/server/server                 # 합성 페이로드
    ./sweep.py --server <path> --log <BYDA_Test_Log_500MB.log>    # 실물 500MB

원칙 (design 측정 방법): **한 번에 하나씩만 바꾼다**(변수 분리). 그래서 청크 스윕에서는
snd_buf_size를 0(커널 autotuning)으로 고정하고, 버퍼 스윕에서는 chunk_size를 64KB로 고정한다.

주의: 이 스윕은 loopback에서 돈다. BDP(대역폭×RTT)가 사실상 0이라 SO_SNDBUF 튜닝 효과가
거의 없을 것으로 예상되며, 그래프가 평평해도 실패가 아니라 "이 환경에선 병목이 아니다"라는
유효한 결론이다 (design). 또한 클라이언트가 Python이라 측정된 처리량에는 클라이언트 측
한계가 섞여 있다 — 서버 간 상대 비교에는 유효하지만 절대 성능치로 읽으면 안 된다.
"""

import argparse
import os
import statistics
import sys
import time

E2E_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "e2e")
sys.path.insert(0, os.path.abspath(E2E_DIR))

import byda_protocol as proto  # noqa: E402
import log_fixtures as fixtures  # noqa: E402
from server_harness import ServerProcess  # noqa: E402

KB = 1024
MB = 1024 * 1024
MEMORY_LIMIT_KB = 50 * KB

CHUNK_SIZES = [32 * KB, 64 * KB, 128 * KB, 256 * KB, 1 * MB]
BUFFER_SIZES = [0, 64 * KB, 128 * KB, 256 * KB, 1 * MB, 4 * MB]


def human(size):
    if size == 0:
        return "auto"
    if size >= MB:
        return f"{size // MB}MB"
    return f"{size // KB}KB"


def write_config(directory, chunk_size, buffer_size):
    path = os.path.join(directory, "sweep.conf")
    with open(path, "w") as handle:
        handle.write(f"chunk_size={chunk_size}\n")
        handle.write(f"snd_buf_size={buffer_size}\n")
    return path


def run_transfer(binary, payload, chunk_size, buffer_size, send_chunk, repeats):
    """한 설정으로 repeats회 업로드 → 측정치 딕셔너리."""
    samples = []
    # 설정 파일은 그 서버 자신의 작업 디렉토리에 쓰고, 기동 전에 -c로 물린다
    # (다른 서버의 디렉토리에 쓰면 cleanup 때 지워져 설정이 조용히 무시된다)
    server = ServerProcess(binary)
    config = write_config(server.workdir, chunk_size, buffer_size)
    server.extra_args = ["-c", config]
    server.start()
    if True:
        try:
            for _ in range(repeats):
                cpu_before = server.cpu_seconds()
                started = time.time()
                with proto.Connection(server.port, timeout=180.0) as connection:
                    connection.send(proto.upload_header(len(payload), "sweep.log"))
                    connection.send_payload(payload, send_chunk)
                    connection.send(proto.upload_trailer(payload))
                    status, received = connection.read_ack()
                    if status != proto.ACK_OK:
                        raise RuntimeError(f"ack status {proto.ACK_NAMES[status]}")
                    if received != len(payload):
                        raise RuntimeError(f"receivedBytes {received} != {len(payload)}")
                    connection.read_result()
                    connection.send(proto.download_done())
                elapsed = time.time() - started
                samples.append({
                    "elapsed": elapsed,
                    "cpu": server.cpu_seconds() - cpu_before,
                    "throughput": len(payload) / elapsed / MB,
                })
            peak_rss = server.sample_rss_kb()
        finally:
            exit_code = server.stop()
            log = server.log_text()  # 종료 후 읽어야 버퍼가 플러시된 상태다
            server.cleanup()

    median = lambda key: statistics.median(sample[key] for sample in samples)  # noqa: E731
    actual_buffer = 0
    for line in log.splitlines():
        if "Socket buffers requested" in line and "actual snd=" in line:
            actual_buffer = int(line.split("actual snd=")[1].split()[0])
    return {
        "elapsed": median("elapsed"),
        "cpu": median("cpu"),
        "throughput": median("throughput"),
        "peak_rss_kb": peak_rss,
        "actual_buffer": actual_buffer,
        "exit_code": exit_code,
    }


def print_table(title, header, rows):
    print(f"\n=== {title} ===")
    widths = [max(len(str(row[i])) for row in [header] + rows) for i in range(len(header))]
    line = "  ".join(str(header[i]).ljust(widths[i]) for i in range(len(header)))
    print(line)
    print("-" * len(line))
    for row in rows:
        print("  ".join(str(row[i]).ljust(widths[i]) for i in range(len(header))))


def main():
    parser = argparse.ArgumentParser(description="BYDA server benchmark sweep")
    parser.add_argument("--server", default="build/server/server")
    parser.add_argument("--log", help="real log file (default: synthetic payload)")
    parser.add_argument("--size-mb", type=int, default=200,
                        help="synthetic payload size when --log is absent")
    parser.add_argument("--repeats", type=int, default=3, help="runs per setting (median wins)")
    options = parser.parse_args()

    if not os.path.exists(options.server):
        print(f"error: server binary not found: {options.server}", file=sys.stderr)
        return 2

    if options.log:
        with open(options.log, "rb") as handle:
            payload = handle.read()
        source = os.path.basename(options.log)
    else:
        block, _ = fixtures.build_log(good_per_module=2000, poison=False)
        target = options.size_mb * MB
        payload = (block * (target // len(block) + 1))[:target]
        source = f"synthetic {options.size_mb}MB"

    print(f"payload: {source} ({len(payload) / MB:.0f} MB), "
          f"{options.repeats} run(s) per setting, median reported")
    print(f"note: loopback + Python client — compare settings relatively, not as absolute limits")

    # 실험 1: 청크 스윕 (SO_SNDBUF 고정 = 커널 autotuning)
    chunk_rows = []
    for chunk in CHUNK_SIZES:
        result = run_transfer(options.server, payload, chunk, 0, 64 * KB, options.repeats)
        chunk_rows.append([
            human(chunk), f"{result['elapsed']:.2f}", f"{result['throughput']:.0f}",
            f"{result['cpu']:.2f}", f"{result['peak_rss_kb'] / KB:.1f}",
        ])
    print_table("Experiment 1: chunk size sweep (snd_buf=auto)",
                ["chunk", "time(s)", "MB/s", "CPU(s)", "peak RSS(MB)"], chunk_rows)

    # 실험 2: SO_SNDBUF 스윕 (청크 64KB 고정)
    buffer_rows = []
    for buffer_size in BUFFER_SIZES:
        result = run_transfer(options.server, payload, 64 * KB, buffer_size, 64 * KB,
                              options.repeats)
        buffer_rows.append([
            human(buffer_size),
            human(result["actual_buffer"]) if result["actual_buffer"] else "auto",
            f"{result['elapsed']:.2f}", f"{result['throughput']:.0f}",
            f"{result['peak_rss_kb'] / KB:.1f}",
        ])
    print_table("Experiment 2: SO_SNDBUF sweep (chunk=64KB)",
                ["requested", "actual", "time(s)", "MB/s", "peak RSS(MB)"], buffer_rows)

    # 실험 3: 최종 설정으로 메모리 상한 검증
    final = run_transfer(options.server, payload, 64 * KB, 0, 64 * KB, options.repeats)
    peak_mb = final["peak_rss_kb"] / KB
    print(f"\n=== Experiment 3: memory ceiling (chunk=64KB, snd_buf=auto) ===")
    print(f"peak RSS = {final['peak_rss_kb']} kB ({peak_mb:.1f} MB), limit 50.0 MB "
          f"→ {'PASS' if final['peak_rss_kb'] <= MEMORY_LIMIT_KB else 'FAIL'}")
    print(f"graceful shutdown exit code: {final['exit_code']}")

    if final["peak_rss_kb"] > MEMORY_LIMIT_KB:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
