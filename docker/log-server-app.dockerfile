# ==========================================
# log-transfer-analyzer Server Application Image
# ==========================================
# log-server-base 위에서 소스를 빌드하고 실행 설정을 담는다. base를 먼저 빌드할 것:
#
#   docker build -t log-server-base:latest -f docker/log-server-base.dockerfile .
#   docker build -t log-server-app:latest  -f docker/log-server-app.dockerfile .
#   docker run --rm -p 23507:23507 log-server-app:latest
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

# 서버 바이너리의 RUNPATH는 빌드 트리의 libuv.so 경로를 가리킨다 (README "Deployment
# notes"). 컨테이너 안에서는 빌드 경로가 /app으로 고정되므로 재배치 문제가 없다.

# 산출물(result.csv, skip_report.txt, logs/)은 작업 디렉토리 기준으로 쓰인다.
RUN mkdir -p /app/logs

EXPOSE 23507

VOLUME ["/app/logs"]

# 컨테이너에서는 포그라운드로 실행한다. 데몬 모드(-d)는 PID 1이 fork 후 종료되어
# 컨테이너가 곧바로 내려가므로 쓰지 않는다.
CMD ["./build/server/server"]
