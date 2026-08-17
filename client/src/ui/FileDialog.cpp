#include "ui/FileDialog.h"

#include <windows.h>
#include <commdlg.h>

#include <array>

#include "protocol/protocol.h"

namespace client {
namespace {

// 다이얼로그가 돌려주는 경로 길이 상한. MAX_PATH를 넘는 경로도 있으므로 넉넉히 잡되
// 고정 버퍼로 둔다 (동적 할당 없이 스택만 사용 — 컨벤션 1번).
constexpr std::size_t kPathBufferChars = 4096;

std::string toUtf8(const wchar_t* wide, int wideLength) {
    if (wideLength <= 0) {
        return std::string{};
    }
    const int bytes =
        ::WideCharToMultiByte(CP_UTF8, 0, wide, wideLength, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::string{};
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, wideLength, utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

std::string toUtf8(const std::wstring& wide) {
    return toUtf8(wide.c_str(), static_cast<int>(wide.size()));
}

// design 8번의 filename 검증을 클라이언트 쪽에서 먼저 건다 — 서버가 거절할 헤더를
// 500MB 전송 후에 알게 되는 것보다, 파일을 고르는 순간 알려주는 편이 낫다.
bool isAcceptableFileName(const std::string& name, std::string& error) {
    if (name.empty() || name.size() > common::protocol::kMaxFilenameLen) {
        error = "File name must be 1 to 255 bytes when encoded as UTF-8.";
        return false;
    }
    for (const unsigned char ch : name) {
        if (ch < 0x20 || ch == 0x7F) {
            error = "File name contains a control character.";
            return false;
        }
        if (ch == '/' || ch == '\\') {
            error = "File name contains a path separator.";
            return false;
        }
    }
    return true;
}

bool queryFileSize(const std::wstring& path, std::uint64_t& size, std::string& error) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) == 0) {
        error = "Cannot read the selected file (it may have been moved or locked).";
        return false;
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        error = "A directory was selected instead of a file.";
        return false;
    }

    ULARGE_INTEGER value{};
    value.HighPart = attributes.nFileSizeHigh;
    value.LowPart = attributes.nFileSizeLow;
    size = value.QuadPart;

    // 서버가 헤더 단계에서 거절하는 상한 (design 8번) — 여기서 먼저 막는다.
    if (size > common::protocol::kMaxFileSize) {
        error = "File is larger than the 8 GiB protocol limit.";
        return false;
    }
    return true;
}

}  // namespace

bool openFileDialog(void* ownerWindow, SelectedFile& selected, std::string& error) {
    error.clear();

    std::array<wchar_t, kPathBufferChars> buffer{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(ownerWindow);
    dialog.lpstrFilter = L"Log files (*.log)\0*.log\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (::GetOpenFileNameW(&dialog) == FALSE) {
        return false;  // 사용자가 취소 — 에러가 아니므로 error는 비워 둔다
    }

    const std::wstring widePath(buffer.data());
    // nFileOffset은 경로에서 파일명이 시작하는 위치 — 직접 자르는 것보다 정확하다.
    const std::wstring wideName = widePath.substr(dialog.nFileOffset);

    SelectedFile result;
    result.path = toUtf8(widePath);
    result.name = toUtf8(wideName);
    if (result.path.empty() || result.name.empty()) {
        error = "Failed to convert the file path to UTF-8.";
        return false;
    }
    if (!isAcceptableFileName(result.name, error)) {
        return false;
    }
    if (!queryFileSize(widePath, result.size, error)) {
        return false;
    }

    selected = result;
    return true;
}

bool saveFileDialog(void* ownerWindow, const std::string& defaultName, std::string& path,
                    std::string& error) {
    error.clear();

    std::array<wchar_t, kPathBufferChars> buffer{};
    const int wideLength = ::MultiByteToWideChar(CP_UTF8, 0, defaultName.c_str(),
                                                 static_cast<int>(defaultName.size()),
                                                 buffer.data(), static_cast<int>(buffer.size() - 1));
    if (wideLength > 0) {
        buffer[static_cast<std::size_t>(wideLength)] = L'\0';
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(ownerWindow);
    dialog.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
    dialog.lpstrDefExt = L"csv";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (::GetSaveFileNameW(&dialog) == FALSE) {
        return false;
    }

    path = toUtf8(std::wstring(buffer.data()));
    if (path.empty()) {
        error = "Failed to convert the save path to UTF-8.";
        return false;
    }
    return true;
}

}  // namespace client
