#!/usr/bin/env python3
"""결과 수신이 끊겼을 때 재업로드 없이 이어 받는지 확인하는 스크립트.

리뷰 2차 지적 3번("업로드는 방어해 놓고, 같은 비용을 다시 치르게 되는 다운로드 방향은
방어하지 않았다")에 대한 답을 말이 아니라 실행으로 만든다.

핵심은 "결과를 다시 받았다"가 아니라 **"파일을 다시 보내지 않고 받았다"** 이다.
끊긴 다운로드가 잃는 것은 CSV 몇 KB가 아니라 업로드 전체다. 그래서 아래 검사는
매번 재요청 연결이 실제로 몇 바이트를 보냈는지 함께 센다 — 그 수가 페이로드 크기와
비교조차 되지 않아야 이 기능이 값을 한 것이다.

절차 (오프셋마다 반복):
  1) 업로드를 완주하고 Ack(OK)를 받는다
  2) ResultHeader까지 받은 뒤 CSV를 cut 바이트만 읽고 RST로 끊는다
  3) 새로 연결해 ResultRequest{fileSize, crc32, startOffset=cut, filename}을 보낸다
  4) 돌아온 조각을 이어 붙여 완성 CSV가 한 번에 받은 것과 바이트 단위로 같은지 본다

거절 경로도 함께 본다 — 청구표가 어긋나면 NO_SUCH_RESULT, 오프셋이 결과보다 크면
PROTOCOL_ERROR여야 한다. 전자가 없으면 남의 결과가 건네질 수 있고, 후자가 없으면
서버가 잘못된 오프셋으로 계산한다.

사용법:
  python3 check_result_resume.py
  python3 check_result_resume.py --port 23507 --host 192.168.0.105
  (서버를 먼저 띄워둘 것: ./build/server/server -p 23507)
"""
import argparse
import sys
import zlib

import byda_protocol as proto
import log_fixtures as fixtures

UPLOAD_NAME = "resume.log"


def build_payload():
    payload, _ = fixtures.build_log(good_per_module=100, poison=True)
    return payload


def upload_and_break(options, payload, cut):
    """업로드를 완주하고 결과를 cut 바이트만 받은 뒤 끊는다.

    RST로 끊는 이유: 정상 close는 서버가 WAIT_DONE에서 기다리다 정리하는 경로와
    구별되지 않는다. 링크가 끊긴 모양을 만들어야 재개가 필요한 상황이 재현된다.
    """
    conn = proto.Connection(options.port, host=options.host)
    try:
        conn.send(proto.upload_header(len(payload), UPLOAD_NAME))
        conn.send_payload(payload)
        conn.send(proto.upload_trailer(payload))
        status, _ = conn.read_ack()
        if proto.ACK_NAMES[status] != "OK":
            raise SystemExit(f"upload was rejected: {proto.ACK_NAMES[status]}")
        csv_size, crc = conn.read_result_header()
        partial = conn.read_some(cut) if cut else b""
    finally:
        conn.abort()
    return csv_size, crc, partial


def request_result(options, payload, start_offset, claim_crc=None):
    """재요청 한 번. → (kind, value, sent_bytes)

    kind == "ack"    : value = (status, received)      — 서버가 거절했다
    kind == "result" : value = (csv_size, crc, body)   — 헤더와 오프셋 이후 본문
    """
    request = proto.result_request(
        len(payload),
        zlib.crc32(payload) if claim_crc is None else claim_crc,
        start_offset,
        UPLOAD_NAME,
    )
    conn = proto.Connection(options.port, host=options.host)
    try:
        conn.send(request)
        msg_type, head = conn.peek_reply_type()
        if msg_type == proto.TYPE_ACK:
            rest = conn.recv_exactly(proto.ACK_SIZE - proto.PREAMBLE_SIZE)
            return "ack", proto.parse_ack(head + rest), len(request)
        if msg_type != proto.TYPE_RESULT_HEADER:
            raise SystemExit(f"unexpected reply type {msg_type}")
        rest = conn.recv_exactly(proto.RESULT_HEADER_SIZE - proto.PREAMBLE_SIZE)
        csv_size, crc = proto.parse_result_header(head + rest)
        body = conn.recv_exactly(csv_size - start_offset)
        conn.send(proto.download_done())
        return "result", (csv_size, crc, body), len(request)
    finally:
        conn.close()


def check_resume(options, payload, reference, cut):
    """cut 바이트까지 받고 끊은 뒤 이어 받아, 완성본이 기준과 같은지 본다."""
    problems = []
    csv_size, crc, partial = upload_and_break(options, payload, cut)

    if csv_size != len(reference):
        problems.append(f"header said {csv_size} bytes, reference is {len(reference)}")
    if len(partial) != cut:
        problems.append(f"read {len(partial)} of the requested {cut} bytes before the break")

    kind, value, sent = request_result(options, payload, len(partial))
    if kind != "result":
        return problems + [f"re-request was refused: {proto.ACK_NAMES[value[0]]}"], 0, 0

    total, resumed_crc, body = value
    assembled = partial + body

    # ResultHeader는 재개에서도 "완성 CSV 전체"의 크기·CRC여야 한다. 조각의 값이면
    # 이어 붙인 전체를 검증할 방법이 사라진다.
    if total != len(reference):
        problems.append(f"resumed header said {total}, reference is {len(reference)}")
    if resumed_crc != zlib.crc32(reference):
        problems.append("resumed header carries the CRC of a fragment, not of the whole result")
    if len(body) != len(reference) - cut:
        problems.append(f"server sent {len(body)} bytes from offset {cut},"
                        f" expected {len(reference) - cut}")
    if assembled != reference:
        problems.append("the assembled result differs from a result received in one piece")
    if zlib.crc32(assembled) != crc:
        problems.append("the assembled result fails the CRC the first header declared")
    return problems, sent, len(payload)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=23507)
    parser.add_argument("--host", default="127.0.0.1")
    options = parser.parse_args()

    payload = build_payload()
    print(f"log: {len(payload)} bytes")

    # 기준본 — 한 번에 받은 결과. 이어 붙인 것과 바이트 단위로 대조할 대상이다.
    conn = proto.Connection(options.port, host=options.host)
    try:
        conn.send(proto.upload_header(len(payload), UPLOAD_NAME))
        conn.send_payload(payload)
        conn.send(proto.upload_trailer(payload))
        status, _ = conn.read_ack()
        if proto.ACK_NAMES[status] != "OK":
            raise SystemExit(f"baseline upload was rejected: {proto.ACK_NAMES[status]}")
        reference, crc_ok = conn.read_result()
        conn.send(proto.download_done())
    finally:
        conn.close()
    print(f"reference result.csv: {len(reference)} bytes, CRC {'ok' if crc_ok else 'MISMATCH'}")
    if not crc_ok:
        print("FAILED  the baseline result is already broken")
        return 1

    failures = 0
    print()
    print("=== resuming from four offsets ===")
    # 0 = 한 바이트도 못 받은 채 끊김 / 절반 / 끝-1 / 끝 = 전부 받고 끊김.
    # 양 끝을 넣는 이유는 "중간만 되는" 구현을 배제하기 위해서다. 특히 마지막 경우는
    # 보낼 본문이 0바이트인데도 헤더로 답해야 하는 경계라, 그 경로가 따로 있어야 한다.
    for label, cut in (("nothing received", 0),
                       ("half received", len(reference) // 2),
                       ("all but the last byte", len(reference) - 1),
                       ("everything received", len(reference))):
        problems, sent, payload_size = check_resume(options, payload, reference, cut)
        if problems:
            failures += 1
            print(f"  FAIL  {label} (offset {cut})")
            for problem in problems:
                print(f"          {problem}")
        else:
            print(f"  ok    {label} (offset {cut}) - recovered"
                  f" {len(reference) - cut} bytes by sending {sent},"
                  f" instead of re-sending {payload_size}")

    print()
    print("=== the claim has to match ===")
    # 청구표가 틀리면 결과를 주면 안 된다. 이게 없으면 재접속한 아무나 마지막 결과를
    # 받아가고, 그건 조용히 잘못된 결과라 가장 나쁜 실패다.
    kind, value, _ = request_result(options, payload, 0, claim_crc=zlib.crc32(payload) ^ 0xFF)
    if kind == "ack" and proto.ACK_NAMES[value[0]] == "NO_SUCH_RESULT":
        print("  ok    a wrong CRC in the claim is answered with NO_SUCH_RESULT")
    else:
        failures += 1
        print(f"  FAIL  a wrong CRC got {kind} {value if kind == 'ack' else ''}"
              f" - someone else's result can be handed out")

    kind, value, _ = request_result(options, payload, len(reference) + 1)
    if kind == "ack" and proto.ACK_NAMES[value[0]] == "PROTOCOL_ERROR":
        print("  ok    an offset past the end is answered with PROTOCOL_ERROR")
    else:
        failures += 1
        print(f"  FAIL  an offset past the end got {kind}"
              f" {proto.ACK_NAMES[value[0]] if kind == 'ack' else ''}")

    print()
    if failures:
        print(f"FAILED  ({failures} checks failed)")
        return 1
    print("The result survived a broken download and was recovered without sending the log")
    print("again - the response direction is resumable, not only the request direction.")
    print()
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
