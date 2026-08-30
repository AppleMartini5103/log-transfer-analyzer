# ==========================================
# log-transfer-analyzer Server Application Image
# ==========================================
# log-server-base 위에서 소스를 빌드하고 실행 설정을 담는다.
#
# 두 이미지를 순서대로 빌드하는 일은 docker/build_images.sh가 감싼다:
#
#   ./docker/build_images.sh
#   docker compose up
#
# 순서를 직접 칠 수도 있다 (base가 먼저여야 한다 — app이 FROM으로 참조한다):
#
#   docker build -t log-server-base:latest -f docker/log-server-base.dockerfile .
#   docker build -t log-server-app:latest  -f docker/log-server-app.dockerfile .
#   docker run --rm -p 23507:23507 -v "$(pwd)/out:/app/out" log-server-app:latest
FROM log-server-base:latest

WORKDIR /app

# .dockerignore가 호스트의 build/·logs/·3rdparty 산출물을 걸러낸다. COPY는 기존
# 파일을 지우지 않고 겹쳐 쓰므로, base가 /app/3rdparty에 만들어 둔 libuv/Catch2
# 산출물은 그대로 남는다.
COPY . .

# 기존 빌드 스크립트를 그대로 쓴다: CMake Release 빌드 → ctest.
# (3rdparty는 include/가 이미 있으므로 — base에서 빌드됨 — 스크립트가 건너뛴다.)
# 유닛 테스트가 실패하면 이미지 빌드 자체가 실패한다 — 테스트를 건너뛴 이미지는
# 존재할 수 없다.
RUN bash ./build_project_linux.sh

# 서버 바이너리의 RUNPATH는 $ORIGIN 기준이다 (이슈 62). 컨테이너 안에서는 빌드
# 경로가 /app으로 고정되어 두 번째 항목이 3rdparty의 libuv를 가리키므로 그대로 뜬다.

# 스모크 테스트: 로더가 libuv나 libstdc++를 찾지 못하면 여기서 이미지 빌드가 실패한다.
# 실행 시점에 발견하는 대신, 깨진 이미지는 애초에 만들어지지 않는다.
# -h는 소켓을 열기 전에 usage만 출력하고 0으로 끝나므로 포트가 필요 없다.
RUN /app/build/server/server -h > /dev/null

EXPOSE 23507

# 산출물(result.csv · skip_report.txt · logs/)은 전부 작업 디렉토리 기준 상대 경로다.
# 작업 디렉토리를 /app/out 한 곳으로 모아 두면 -v 하나로 셋 다 호스트에 회수된다.
#
# VOLUME은 선언하지 않는다. 익명 볼륨은 docker run --rm과 함께 삭제되어 보존 효과가
# 없고, 매 실행마다 찌꺼기만 남는다. 회수는 호스트가 -v로 명시할 일이다.
# (이전 판의 VOLUME ["/app/logs"]는 두 겹으로 무의미했다: 포그라운드 실행에서는
#  파일 로깅 자체가 켜지지 않고 — main.cpp의 openFile은 데몬 분기 안에 있다 —
#  정작 산출물인 result.csv는 /app 루트에 떨어져 그 볼륨 밖이었다.)
WORKDIR /app/out

# 컨테이너에서는 포그라운드로 실행한다. 데몬 모드(-d)는 PID 1이 fork 후 종료되어
# 컨테이너가 곧바로 내려가므로 쓰지 않는다. 백그라운드는 docker run -d의 몫이다.
# 작업 디렉토리가 /app/out이므로 절대 경로로 부른다.
CMD ["/app/build/server/server"]
