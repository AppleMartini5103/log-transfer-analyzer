#include "util/Version.h"

#include <uv.h>

#include <cstdio>

// 뼈대 검증용 main — libuv 링크와 common 라이브러리 배선 확인.
// 실제 서버 수명주기(시그널/데몬화/루프)는 이후 이슈에서 구현.
int main() {
    std::printf("%.*s skeleton (built %.*s, libuv %s)\n",
                static_cast<int>(common::kProjectName.size()), common::kProjectName.data(),
                static_cast<int>(common::buildDate().size()), common::buildDate().data(),
                uv_version_string());
    return 0;
}
