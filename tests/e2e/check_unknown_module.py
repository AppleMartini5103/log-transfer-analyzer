#!/usr/bin/env python3
"""샘플에 없던 모듈이 나타나면 실제로 무슨 일이 벌어지는지 보여주는 스크립트.

리뷰 2차 지적 2번("샘플에 없던 모듈이 나타나는 순간, 그 프로그램들은 정상 데이터를
아무 경고 없이 버리기 시작합니다")에 대한 답을 말이 아니라 실행으로 만든다.

우리 답은 절반만 반박이다.
  - 버린다        → 사실이다. 화이트리스트는 미지 모듈 라인을 전량 스킵한다
  - 경고 없이     → 사실이 아니다. 그 사실이 세 곳에 남는다
                     result.csv     skip_reason_UNKNOWN_MODULE (몇 줄인지)
                     skip_report.txt [Unknown module names]   (무엇이었는지)
                     클라이언트      로그 창 경고 (사용자가 본다)

화이트리스트를 걷어내지 않는 이유도 같은 자리에 있다. 참조 로그의 스킵 26건 중 13건이
BeyondLimit·CorruptPayload이고, 그것들은 UNKNOWN_MODULE에 도달했다는 사실 자체가
프레임·타임스탬프·헤더 숫자·괄호 짝을 전부 통과했다는 증명이다. 이름 검사를 없애면
그 13건이 정상 통계로 들어온다. 같은 규칙이 미지 정상 모듈을 버리고 독약을 잡는다.

구성:
  정상 5종 x 200줄 = 1000
  미지 SectorHealthMonitor x 250줄
  ------------------------------
  총 1250줄, 그중 250줄(20%)이 스킵된다

20%로 잡은 이유: 눈에 띄되 억지스럽지 않고, 비율이 한눈에 읽혀 total_lines를 CSV에 넣은
이유가 같이 드러난다. "250 skipped"만으로는 심각한지 알 수 없다.

사용법:
  python3 check_unknown_module.py                     # 업로드하고 검증한다
  python3 check_unknown_module.py --write-only x.log  # 로그 파일만 만든다 (GUI 캡처용)
  python3 check_unknown_module.py --self-test         # 검증 로직 자체를 시험한다
  (서버를 먼저 띄워둘 것: ./build/server/server -p 23507)
"""
import argparse
import sys

import byda_protocol as proto
import log_fixtures as fixtures

MODULE_UNKNOWN = "SectorHealthMonitor"
GOOD_PER_MODULE = 200
UNKNOWN_LINES = 250
HOUR_KEY = "2026-06-19 22"


def unknown_line(index):
    """미지 모듈 라인 — 이름 말고는 어디도 훼손되지 않았다.

    이게 핵심이다. 프레임 구조·타임스탬프·헤더 숫자·괄호 짝이 전부 정상이라
    화이트리스트가 유일한 탈락 사유가 된다. 훼손된 라인을 쓰면 "훼손이라 버렸다"와
    "모르는 이름이라 버렸다"가 섞여 무엇을 보인 것인지 알 수 없게 된다.
    """
    return (
        f"[{fixtures.timestamp(second=index % 60, micro=index)}]"
        "[7710][30482][1885246073] BYDA::" + MODULE_UNKNOWN + ": "
        "health_report: sectorID[20641107], status[2->1]"
    )


def build_log():
    """→ (bytes, expected). 무엇이 몇 줄인지 아는 상태로 만들어야 결과를 대조할 수 있다."""
    lines = []
    for index in range(GOOD_PER_MODULE):
        for module in fixtures.MODULES:
            lines.append(fixtures.good_line(module, micro=index))
    for index in range(UNKNOWN_LINES):
        lines.append(unknown_line(index))

    expected = {
        "known_per_module": GOOD_PER_MODULE,
        "unknown": UNKNOWN_LINES,
        "total": GOOD_PER_MODULE * len(fixtures.MODULES) + UNKNOWN_LINES,
    }
    return ("\n".join(lines) + "\n").encode("utf-8"), expected


def evaluate(csv_bytes, expected):
    """돌아온 result.csv를 판정해 문제 목록을 준다. 빈 목록이면 통과.

    소켓과 분리된 순수 함수인 이유: 서버를 건드리지 않고도 이 판정이 실제로 결함을
    잡는지 시험할 수 있어야 한다 (--self-test). "통과했다"를 근거로 쓰려면 그 검사가
    무엇을 잡는지 먼저 보여야 한다.
    """
    buckets, metrics = fixtures.parse_result_csv(csv_bytes)
    problems = []

    def number(name):
        try:
            return int(metrics[name])
        except (KeyError, ValueError):
            return None

    # 1) 미지 모듈은 통계에 없다 — 버려진다는 사실 자체
    leaked = {k: v for k, v in buckets.items() if k[0] == MODULE_UNKNOWN}
    if leaked:
        problems.append(f"{MODULE_UNKNOWN} reached the statistics: {leaked}")

    # 2) 버려진 줄 수가 정확히 미지 모듈 줄 수다 — 더도 덜도 아니다
    if number("skipped_lines") != expected["unknown"]:
        problems.append(
            f"skipped_lines is {metrics.get('skipped_lines')}, expected {expected['unknown']}")
    if number("skip_reason_UNKNOWN_MODULE") != expected["unknown"]:
        problems.append(
            f"skip_reason_UNKNOWN_MODULE is {metrics.get('skip_reason_UNKNOWN_MODULE')},"
            f" expected {expected['unknown']}")

    # 3) 다른 사유로는 한 줄도 버려지지 않았다 — 미지 모듈만이 유일한 탈락 사유였음
    others = sorted(k for k in metrics if k.startswith("skip_reason_")
                    and k != "skip_reason_UNKNOWN_MODULE")
    if others:
        problems.append(f"unexpected skip reasons: {', '.join(others)}")

    # 4) 나머지가 온전하다 — 미지 모듈 때문에 정상 데이터가 망가지지 않았다.
    #    이걸 빼면 "버려진다"만 보이고 "나머지는 멀쩡하다"를 놓친다
    for module in fixtures.MODULES:
        counted = sum(v for k, v in buckets.items() if k[0] == module)
        if counted != expected["known_per_module"]:
            problems.append(
                f"{module} counted {counted}, expected {expected['known_per_module']}")

    # 5) 분모가 실려 있다 — 250이 심각한 수인지 판단하려면 필요하다
    if number("total_lines") != expected["total"]:
        problems.append(
            f"total_lines is {metrics.get('total_lines')}, expected {expected['total']}")

    # 6) 작업2는 영향받지 않았다
    if number("valid_spd_samples") != expected["known_per_module"]:
        problems.append(
            f"valid_spd_samples is {metrics.get('valid_spd_samples')},"
            f" expected {expected['known_per_module']}")
    if metrics.get("avg_speed") != fixtures.SPD_VALUE:
        problems.append(f"avg_speed is {metrics.get('avg_speed')}, expected {fixtures.SPD_VALUE}")

    return problems


def make_csv(buckets, metrics):
    """--self-test용 result.csv 조립기."""
    lines = ["module,hour,count"]
    lines += [f"{module},{HOUR_KEY},{count}" for module, count in buckets]
    lines.append("")
    lines.append("metric,value")
    lines += [f"{name},{value}" for name, value in metrics]
    return ("\n".join(lines) + "\n").encode("utf-8")


def self_test():
    """판정이 실제로 결함을 잡는지 시험한다. 서버가 필요 없다."""
    good_buckets = [(module, GOOD_PER_MODULE) for module in fixtures.MODULES]
    good_metrics = [
        ("total_lines", 1250),
        ("avg_speed", fixtures.SPD_VALUE),
        ("valid_spd_samples", GOOD_PER_MODULE),
        ("excluded_spd_samples", 0),
        ("missing_spd_samples", 0),
        ("skipped_lines", UNKNOWN_LINES),
        ("skip_reason_UNKNOWN_MODULE", UNKNOWN_LINES),
    ]
    expected = build_log()[1]

    cases = [
        ("a correct result is accepted", good_buckets, good_metrics, True),
        # 화이트리스트가 사라진 경우 — 미지 모듈이 통계로 들어오고 스킵이 0이 된다
        ("the unknown module counted instead of skipped",
         good_buckets + [(MODULE_UNKNOWN, UNKNOWN_LINES)],
         [("total_lines", 1250), ("avg_speed", fixtures.SPD_VALUE),
          ("valid_spd_samples", GOOD_PER_MODULE), ("skipped_lines", 0)], False),
        # 미지 모듈이 다른 라인까지 데리고 나간 경우
        ("a known module came up short",
         [(m, GOOD_PER_MODULE - 3 if m == "RadarTrackNodeState" else GOOD_PER_MODULE)
          for m in fixtures.MODULES], good_metrics, False),
        # 사유가 UNKNOWN_MODULE이 아닌 경우 — 이름이 아니라 구조 때문에 걸린 것
        ("skipped for the wrong reason", good_buckets,
         good_metrics + [("skip_reason_BAD_FRAME", 4)], False),
        # 분모가 빠진 경우
        ("total_lines missing", good_buckets,
         [m for m in good_metrics if m[0] != "total_lines"], False),
    ]

    failures = 0
    for name, buckets, metrics, should_pass in cases:
        problems = evaluate(make_csv(buckets, metrics), expected)
        passed = not problems
        ok = passed == should_pass
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}"
              f"{'' if ok else '  <-- ' + ('accepted a bad result' if passed else str(problems))}")
        if not ok:
            failures += 1

    print()
    print("PASS  the checks reject what they should" if not failures
          else f"FAILED  {failures} of {len(cases)} checks are vacuous")
    return 0 if not failures else 1


def run_session(options, payload):
    conn = proto.Connection(options.port, host=options.host)
    try:
        conn.send(proto.upload_header(len(payload), "unknown_module.log"))
        conn.send_payload(payload)
        conn.send(proto.upload_trailer(payload))
        status, _ = conn.read_ack()
        if proto.ACK_NAMES[status] != "OK":
            raise SystemExit(f"server rejected the upload: {proto.ACK_NAMES[status]}")
        csv, crc_ok = conn.read_result()
        conn.send(proto.download_done())
        return csv, crc_ok
    finally:
        conn.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=23507)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--write-only", metavar="PATH",
                        help="write the log file and stop (input for the GUI capture)")
    parser.add_argument("--self-test", action="store_true",
                        help="check that the assertions below actually reject a wrong result")
    options = parser.parse_args()

    if options.self_test:
        print("self-test: do the checks catch a wrong result?")
        return self_test()

    payload, expected = build_log()
    print(f"log: {expected['total']} lines"
          f" = {len(fixtures.MODULES)} known modules x {expected['known_per_module']}"
          f" + {MODULE_UNKNOWN} x {expected['unknown']}")

    if options.write_only:
        with open(options.write_only, "wb") as out:
            out.write(payload)
        print(f"wrote {len(payload)} bytes to {options.write_only}")
        print("upload it with the client to capture the warning in the log pane")
        return 0

    csv, crc_ok = run_session(options, payload)
    print(f"got result.csv, {len(csv)} bytes, header CRC {'matches' if crc_ok else 'MISMATCH'}")

    buckets, metrics = fixtures.parse_result_csv(csv)
    print()
    print("=== what came back ===")
    for (module, hour), count in sorted(buckets.items()):
        print(f"    {module:24} {hour}  {count}")
    for name in ("total_lines", "skipped_lines", "skip_reason_UNKNOWN_MODULE"):
        print(f"    {name:24} {metrics.get(name, '<missing>')}")
    print()

    problems = evaluate(csv, expected)
    for problem in problems:
        print(f"FAIL: {problem}")
    if problems:
        print()
        print("FAILED")
        return 1

    skipped = int(metrics["skipped_lines"])
    total = int(metrics["total_lines"])
    print(f"OK  : {MODULE_UNKNOWN} never reached the statistics - its lines were dropped")
    print(f"OK  : exactly {skipped} of {total} lines were skipped, all as UNKNOWN_MODULE")
    print(f"OK  : the five known modules are intact at {expected['known_per_module']} each")
    print(f"OK  : avg_speed and valid_spd_samples are unaffected")
    print()
    print("The loss is real and the record of it is complete: result.csv carries the count,")
    print("skip_report.txt on the server names the module, and the client warns from the CSV.")
    print()
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
