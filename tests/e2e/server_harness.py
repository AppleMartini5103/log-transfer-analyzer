"""서버 프로세스 기동·관찰·종료 (E2E 하네스).

각 시나리오는 깨끗한 임시 디렉토리에서 서버를 띄운다 — 서버가 실행 디렉토리 기준으로
result.csv / skip_report.txt / server.pid / server.log를 만들기 때문에(design 10번:
chdir("/") 생략), 디렉토리를 격리하면 시나리오 간 산출물이 섞이지 않는다.
"""

import os
import shutil
import socket
import subprocess
import tempfile
import time


def find_free_port():
    """커널에게 빈 포트를 받아온다 — 고정 포트는 병렬 실행·잔존 소켓과 충돌한다."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class ServerProcess:
    """포그라운드로 띄우고 SIGTERM으로 내린다 (데몬 모드는 별도 시나리오에서 검증)."""

    CLOCK_TICKS = os.sysconf("SC_CLK_TCK")

    def __init__(self, binary, daemon=False, extra_args=()):
        self.binary = os.path.abspath(binary)
        self.daemon = daemon
        self.extra_args = list(extra_args)
        self.port = find_free_port()
        self.workdir = tempfile.mkdtemp(prefix="byda-e2e-")
        self.process = None
        self._peak_rss_kb = 0
        self._stdout_file = None
        self._stdout_path = None

    def start(self, wait_timeout=5.0):
        args = [self.binary, "-p", str(self.port)] + self.extra_args
        if self.daemon:
            args.append("-d")
        # 포그라운드 모드는 로그가 stdout으로 나간다 — 파일로 받아둬야 실패 원인을 볼 수 있고
        # 소켓 버퍼 실제 적용값 같은 진단 로그도 읽을 수 있다 (데몬 모드는 server.log를 쓴다)
        self._stdout_path = os.path.join(self.workdir, "stdout.log")
        self._stdout_file = open(self._stdout_path, "wb")
        self.process = subprocess.Popen(
            args, cwd=self.workdir, stdout=self._stdout_file, stderr=subprocess.STDOUT
        )
        if self.daemon:
            self.process.wait(timeout=wait_timeout)  # 부모는 즉시 반환한다
        self._wait_until_listening(wait_timeout)
        return self

    def _wait_until_listening(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.3):
                    return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError(f"server did not listen on port {self.port} within {timeout}s")

    @property
    def pid(self):
        if self.daemon:
            with open(os.path.join(self.workdir, "server.pid")) as handle:
                return int(handle.read().strip())
        return self.process.pid

    def sample_rss_kb(self):
        """현재까지의 peak RSS(VmHWM). 50MB 상한 검증의 근거값."""
        try:
            with open(f"/proc/{self.pid}/status") as handle:
                for line in handle:
                    if line.startswith("VmHWM:"):
                        self._peak_rss_kb = int(line.split()[1])
        except (OSError, ValueError):
            pass
        return self._peak_rss_kb

    def cpu_seconds(self):
        """프로세스가 지금까지 쓴 CPU 시간 (utime+stime). 청크 스윕의 핵심 지표."""
        try:
            with open(f"/proc/{self.pid}/stat") as handle:
                fields = handle.read().rsplit(") ", 1)[1].split()
            # /proc(5): 닫는 괄호 뒤 3번째가 utime(14), 4번째가 stime(15)
            return (int(fields[11]) + int(fields[12])) / self.CLOCK_TICKS
        except (OSError, ValueError, IndexError, FileNotFoundError):
            return 0.0

    def is_alive(self):
        if self.daemon:
            try:
                os.kill(self.pid, 0)
                return True
            except (OSError, ValueError, FileNotFoundError):
                return False
        return self.process.poll() is None

    def artifact(self, name):
        path = os.path.join(self.workdir, name)
        return path if os.path.exists(path) else None

    def read_artifact(self, name):
        path = self.artifact(name)
        if path is None:
            return None
        with open(path, "rb") as handle:
            return handle.read()

    def log_text(self):
        """데몬이면 server.log, 포그라운드면 캡처한 stdout."""
        data = self.read_artifact("server.log")
        if data is None:
            data = self.read_artifact("stdout.log")
        return data.decode("utf-8", "replace") if data else ""

    def stop(self, timeout=5.0):
        """SIGTERM → 그레이스풀 종료. 종료 코드를 돌려준다 (신호로 죽었으면 음수)."""
        self.sample_rss_kb()  # 죽기 전에 마지막으로 읽어둔다
        if self.daemon:
            try:
                os.kill(self.pid, 15)
            except (OSError, ValueError, FileNotFoundError):
                return 0
            deadline = time.time() + timeout
            while time.time() < deadline and self.is_alive():
                time.sleep(0.05)
            return 0 if not self.is_alive() else -1
        if self.process.poll() is None:
            self.process.terminate()
            try:
                return self.process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.process.kill()
                return -9
        return self.process.returncode

    def cleanup(self):
        self.stop()
        if self._stdout_file is not None:
            self._stdout_file.close()
            self._stdout_file = None
        shutil.rmtree(self.workdir, ignore_errors=True)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc_info):
        self.cleanup()
