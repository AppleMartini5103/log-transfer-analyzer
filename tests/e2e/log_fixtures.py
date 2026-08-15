"""로그 데이터 생성 — 실측 스키마(design 4번)에 맞춘 정상 라인 + 관측된 독약 5종.

500MB 실물 로그 없이도 E2E가 돌아가야 한다(제출 저장소에는 로그가 없다). 그래서
정상 5모듈과 독약 5종을 직접 만들어 낸다. 독약 종류·개수를 아는 상태로 생성하므로
서버가 돌려준 result.csv의 skipped_lines를 정확히 대조할 수 있다.
"""

MODULES = [
    "RadarTrackNodeState",
    "AntennaProfileSpec",
    "BeamSteerCtrlUnitImpl",
    "DetectionTaskRunner",
    "SectorSchedulerRTS",
]

_HEADER = "[{ts}][7710][30482][1885246073] BYDA::"

_BODIES = {
    "RadarTrackNodeState": "node_state_synced: nodeUID[47], rfLane[3], lockState[1->0]",
    "AntennaProfileSpec": "applyElement: sectorID[20641107], element[2][0->1][4]",
    "BeamSteerCtrlUnitImpl": "unitAddr[4181], spd[137500.000000], advDelta[62750.000000]",
    "DetectionTaskRunner": "Sector Command: jobID[7710000000415], command[RUN], mode[AUTO]",
    "SectorSchedulerRTS": "scan started: RT_SWEEP jobID[7710000000415], pattern[SW3], gatedFlag[1]",
}

SPD_VALUE = "137500.000000"


def timestamp(hour=22, second=0, micro=0):
    return f"2026-06-19_{hour:02d}:00:{second:02d}.{micro:06d}"


def good_line(module, hour=22, second=0, micro=0):
    return _HEADER.format(ts=timestamp(hour, second, micro)) + module + ": " + _BODIES[module]


# 관측된 독약 5종 (design 4번 — 2026-08-16 전량 실측으로 OpenBraceLeak 추가 확인).
# 각 항목은 (이름, 라인, 서버가 붙일 사유 코드)
POISON_LINES = [
    (
        "GARBAGE",
        "[2026-06-19_22:15:00.000000] !@#$RAW_FRAME_DECODE_FAILURE_GARBAGE_OCTETS%^&*()",
        "BAD_FRAME",
    ),
    (
        "BeyondLimit",
        "[2026-06-19_22:25:00.999999][7710][30482][1885246073] BYDA::BeyondLimit: "
        "spd[888888888888888888888.88]",
        "UNKNOWN_MODULE",
    ),
    (
        "HeadBraceLoss",
        "2026-06-19_22:20:00.111111][7710][30482][1885246073] BYDA::HeadBraceLoss: raw[9]",
        "BAD_FRAME",
    ),
    (
        "CorruptPayload",
        "[2026-06-19_22:10:00.654321][7710][30482][1885246073] BYDA::CorruptPayload: "
        "nodeUID[NONE], rfLane[X]",
        "UNKNOWN_MODULE",
    ),
    (
        "OpenBraceLeak",
        "[2026-06-19_22:05:00.123456][7710][30482][1885246073 BYDA::OpenBraceLeak: rfLane[3]",
        "BAD_FRAME",
    ),
]

# 문서에 없는 훼손 유형 — 화이트리스트(default-deny)가 처음 보는 독약도 막는지 확인용.
# 블랙리스트 방식이었다면 그대로 통과했을 것들이다 (design 4-1의 기각 근거)
UNSEEN_POISON_LINES = [
    (
        "unknown module name",
        "[2026-06-19_22:00:00.000000][7710][30482][1885246073] BYDA::TotallyNewModule: x[1]",
        "UNKNOWN_MODULE",
    ),
    (
        "impossible calendar date",
        "[2026-02-30_22:00:00.000000][7710][30482][1885246073] BYDA::RadarTrackNodeState: "
        "node_state_synced: nodeUID[47], rfLane[3], lockState[1->0]",
        "BAD_TIMESTAMP",
    ),
    (
        "int64 overflow in jobID",
        "[2026-06-19_22:00:00.000000][7710][30482][1885246073] BYDA::SectorSchedulerRTS: "
        "scan started: jobID[99999999999999999999], gatedFlag[1]",
        "NUM_OUT_OF_RANGE",
    ),
    (
        "unbalanced bracket",
        "[2026-06-19_22:00:00.000000][7710][30482][1885246073] BYDA::RadarTrackNodeState: "
        "node_state_synced: nodeUID[47, rfLane[3]",
        "BAD_BRACKET",
    ),
    (
        "control character",
        "[2026-06-19_22:00:00.000000][7710][30482][1885246073] BYDA::RadarTrackNodeState: "
        "node_state_synced:\tnodeUID[47], rfLane[3], lockState[1->0]",
        "CTRL_CHAR",
    ),
    (
        "empty line",
        "",
        "EMPTY",
    ),
]


def build_log(good_per_module=100, poison=True, unseen_poison=False, trailing_newline=True):
    """→ (bytes, expected) — expected에 모듈별 정상 개수와 스킵 총계가 들어 있다."""
    lines = []
    for index in range(good_per_module):
        for module in MODULES:
            lines.append(good_line(module, micro=index))

    skipped = 0
    if poison:
        for _, line, _ in POISON_LINES:
            lines.append(line)
            skipped += 1
    if unseen_poison:
        for _, line, _ in UNSEEN_POISON_LINES:
            lines.append(line)
            skipped += 1

    text = "\n".join(lines)
    if trailing_newline:
        text += "\n"

    expected = {
        "counts": {module: good_per_module for module in MODULES},
        "skipped": skipped,
        "valid_spd_samples": good_per_module,  # BeamSteerCtrlUnitImpl 라인 수
        "avg_speed": SPD_VALUE,
    }
    return text.encode("utf-8"), expected


def parse_result_csv(csv_bytes):
    """서버가 돌려준 result.csv를 (buckets, metrics)로 파싱한다 (표준 CSV 파서로 읽힌다)."""
    import csv as csv_module
    import io

    text = csv_bytes.decode("utf-8")
    blocks = text.split("\n\n")
    if len(blocks) < 2:
        raise ValueError("result.csv must have two blocks separated by a blank line")

    buckets = {}
    for row in csv_module.DictReader(io.StringIO(blocks[0])):
        buckets[(row["module"], row["hour"])] = int(row["count"])

    metrics = {}
    for row in csv_module.DictReader(io.StringIO(blocks[1])):
        metrics[row["metric"]] = row["value"]
    return buckets, metrics
