# ==========================================
# log-transfer-analyzer Server Base Image
# ==========================================
# 빌드 도구 + 3rdparty(libuv, Catch2)까지 담는 이미지.
# 서버 소스 복사와 빌드는 app.dockerfile이 담당한다.
#
#   docker build -t log-server-base:latest -f docker/log-server-base.dockerfile .
FROM ubuntu:24.04

# apt 설치 중 debconf가 대화형 질문(tzdata의 지역 선택 등)을 띄우면
# docker build는 입력을 받을 수 없어 멈춘다. 기본값으로 자동 진행시킨다.
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Seoul

# Ubuntu 24.04의 cmake(3.28)는 요구 버전(3.16)을 충족하므로 별도 저장소가 필요 없다.
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

RUN cmake --version

# ── 3rdparty: 번들 tarball에서 빌드 ────────────────────────────────────────
# apt의 libuv/catch2를 쓰지 않는 이유: 루트 CMakeLists.txt가 3rdparty/ 안의
# 번들 경로를 하드코딩하므로 시스템 패키지는 빌드에 사용되지 않는다.
# 경로를 /app/3rdparty로 두는 이유: app.dockerfile의 COPY . . 이 같은 위치에
# 소스를 겹쳐 넣고, build_project_linux.sh는 include/가 이미 있으면 3rdparty
# 빌드를 건너뛴다 — base에서 만든 산출물이 그대로 재사용된다.
# (imgui는 Windows 클라이언트 전용이라 base에 넣지 않는다.)
WORKDIR /app

COPY 3rdparty/libuv  /app/3rdparty/libuv
COPY 3rdparty/catch2 /app/3rdparty/catch2

RUN cd /app/3rdparty/libuv  && bash ./build_linux.sh
RUN cd /app/3rdparty/catch2 && bash ./build_linux.sh
