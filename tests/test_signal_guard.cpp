#include "app/Daemon.h"

#include <catch_amalgamated.hpp>

// 테스트 프로세스의 시그널 프롤로그 — 서버 실행 파일과 같은 전제를 테스트에도 세운다.
//
// 왜 필요한가(①등급): common/net의 전송 계층은 libuv의 uv__try_write를 타고 내려가
// MSG_NOSIGNAL 없는 write(2)를 호출한다. 따라서 끊긴 소켓에 write하는 순간 SIGPIPE
// 기본 동작(프로세스 즉사)이 발동한다. 서버는 main() 첫 문장의
// installProcessSignalDefaults()로 이를 막지만(design 10번), 테스트 바이너리는 그 main을
// 링크하지 않으므로 같은 전제를 스스로 세워야 한다.
//
// 이 파일이 없던 동안 테스트는 test_daemon의 raise(SIGPIPE) 케이스가 남긴 프로세스 전역
// SIG_IGN에 우연히 의존했다. 실행 순서가 무작위이므로 그 케이스가 나중에 뽑히는 런에서는
// 클라이언트 강제 단절 시나리오가 SIGPIPE로 즉사해 스위트 전체가 중단됐다 — 20회 반복
// 측정에서 12회 중 7회 실패(약 58%). 테스트 결과가 다른 테스트의 부수효과에 의존해서는
// 안 된다는 것이 이 파일의 존재 이유다.
//
// 정적 초기화 객체 대신 리스너를 쓴 이유: 두 방식의 메커니즘은 동일하다
// (CATCH_REGISTER_LISTENER도 익명 namespace의 전역 객체로 전개되며, TEST_CASE 자신도
// 같은 방식이다). 링크 보장의 차이는 없고, "테스트 실행 시작 시점의 준비"라는 의도가
// 코드에 드러나는 쪽을 택했다.
namespace {

class SignalGuardListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
        server::app::installProcessSignalDefaults();
    }
};

}  // namespace

CATCH_REGISTER_LISTENER(SignalGuardListener)
