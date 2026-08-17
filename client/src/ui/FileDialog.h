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

}  // namespace client
