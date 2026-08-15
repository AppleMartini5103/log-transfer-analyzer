#include "app/ServerConfig.h"
#include "util/Version.h"

#include <uv.h>

#include <cstdio>

// 서버 진입점.
// design 10번 초기화 순서 고정: 인자 파싱 → 데몬화(-d) → 로거 → uv_loop → 스레드 → 실행.
// 현재 구현된 단계는 [인자 파싱]까지 — 이후 단계는 다음 이슈에서 채운다.
int main(int argc, char** argv) {
    server::app::CommandLine cli;
    const server::app::ParseResult parsed = server::app::parseCommandLine(argc, argv, cli);
    if (!parsed.ok) {
        std::fprintf(stderr, "error: %s\n\n%s", parsed.error.c_str(),
                     server::app::usageText().c_str());
        return 1;
    }
    if (cli.helpRequested) {
        std::fputs(server::app::usageText().c_str(), stdout);
        return 0;
    }

    server::app::ServerConfig config;
    const server::app::ParseResult built = server::app::buildConfig(cli, config);
    if (!built.ok) {
        std::fprintf(stderr, "error: %s\n", built.error.c_str());
        return 1;
    }

    std::printf("%.*s (built %.*s, libuv %s)\n",
                static_cast<int>(common::kProjectName.size()), common::kProjectName.data(),
                static_cast<int>(common::buildDate().size()), common::buildDate().data(),
                uv_version_string());
    std::printf("  config file   : %s\n", config.configPath.c_str());
    std::printf("  port          : %u\n", static_cast<unsigned>(config.port));
    std::printf("  daemonize     : %s\n", config.daemonize ? "yes" : "no");
    std::printf("  chunk_size    : %zu\n", config.chunkSize);
    std::printf("  snd_buf_size  : %zu%s\n", config.sendBufferSize,
                config.sendBufferSize == 0 ? " (kernel autotuning)" : "");
    std::printf("  log_path      : %s\n", config.logPath.c_str());
    return 0;
}
