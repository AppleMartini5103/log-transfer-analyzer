#pragma once

#include <cstdint>
#include <string>

namespace client {

// Win32 파일 다이얼로그 래퍼.
//
// ImGui에는 파일 선택 창이 없어 Win32 API를 직접 부른다 (design 5번 확정).
// GetOpenFileNameW/GetSaveFileNameW는 스택 구조체만 쓰므로 동적 할당이 없다 (컨벤션 1번).
// 반환 경로는 UTF-16이라 프로토콜 헤더에 넣기 전에 UTF-8로 변환한다 (컨벤션 10번).
struct SelectedFile {
    std::string path;      // UTF-8 전체 경로 (로컬 파일 열기용)
    std::string name;      // UTF-8 파일명만 — UploadHeader의 filename 필드로 나간다
    std::uint64_t size = 0;
};

// 열기 다이얼로그 + 존재·크기·파일명 검증까지. 취소하거나 검증에 걸리면 false를 반환하고
// error에 사용자에게 보여줄 사유를 채운다 (예외 대신 반환값 — 컨벤션 3번).
bool openFileDialog(void* ownerWindow, SelectedFile& selected, std::string& error);

// 저장 다이얼로그 — result.csv를 어디에 저장할지 (design 12번: Download 버튼 = 저장).
// 취소 시 false를 반환하고 error는 비운다.
bool saveFileDialog(void* ownerWindow, const std::string& defaultName, std::string& path,
                    std::string& error);

// 전송 시작 확인 (design 88행 "전송시 전송을 할 것인지 물어보는 message box" — 유지 항목).
//
// [왜 확인을 받는가]
//   500MB 전송은 실질적으로 되돌릴 수 없다. Cancel은 있지만 그 시점에 서버 세션 하나가
//   이미 소비되고(1:1이라 그 사이 다른 클라이언트는 대기한다) 전송한 바이트는 버려진다.
//   그래서 "무엇을 어디로" 둘 다 보여주고 시작 전에 한 번 묻는다.
//
// [모달이어도 되는 이유]
//   전송 시작 전이라 진행 중인 작업이 없다. 파일 다이얼로그도 같은 방식으로 UI 스레드를
//   막는다 — PDF가 금지하는 것은 "500MB 전송 중" 프리즈다.
//
// 사용자가 취소하면 false. 물어볼 수 없는 상황(대화상자 생성 실패)에서도 false를 돌려
// 전송하지 않는다 — 확인받지 못했으면 보내지 않는 쪽이 안전하다.
bool confirmUpload(void* ownerWindow, const std::string& fileName, std::uint64_t fileSize,
                   const std::string& destination);

}  // namespace client
