// pbcopy / pbpaste for Windows.
// The executable name selects the command: pbcopy reads stdin onto the
// clipboard, pbpaste writes clipboard text to stdout.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace {

constexpr DWORD kIoChunk = 8192;
constexpr int kClipboardRetries = 20;
constexpr DWORD kClipboardRetryMs = 25;

bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
    if (left.empty() || right.empty()) {
        return left.empty() && right.empty();
    }
    if (left.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()),
               right.data(), static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

std::wstring ExecutableBaseName() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);

    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.erase(0, slash + 1);
    }
    return path;
}

bool IsPasteCommand() {
    const std::wstring name = ExecutableBaseName();
    return EqualsIgnoreCase(name, L"pbpaste.exe") ||
           EqualsIgnoreCase(name, L"pbpaste");
}

bool OpenClipboardRetry() {
    for (int attempt = 0; attempt < kClipboardRetries; ++attempt) {
        if (OpenClipboard(nullptr)) {
            return true;
        }
        Sleep(kClipboardRetryMs);
    }
    return false;
}

void StripUtf8Bom(std::string& bytes) {
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
}

// Git Bash pipes UTF-8. cmd.exe pipes the ANSI code page. Prefer strict UTF-8
// and fall back to the system ANSI code page when the bytes are not UTF-8.
std::wstring DecodeInputBytes(const std::string& bytes) {
    if (bytes.empty() ||
        bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    const auto convert = [&](UINT codePage, DWORD flags) -> std::wstring {
        const int count = MultiByteToWideChar(
            codePage, flags, bytes.data(), static_cast<int>(bytes.size()),
            nullptr, 0);
        if (count <= 0) {
            return {};
        }
        std::wstring text(static_cast<size_t>(count), L'\0');
        const int written = MultiByteToWideChar(
            codePage, flags, bytes.data(), static_cast<int>(bytes.size()),
            text.data(), count);
        if (written <= 0) {
            return {};
        }
        text.resize(static_cast<size_t>(written));
        return text;
    };

    const int utf8Count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (utf8Count > 0) {
        return convert(CP_UTF8, MB_ERR_INVALID_CHARS);
    }
    return convert(CP_ACP, 0);
}

std::string ReadAllStdin() {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) {
        return {};
    }

    DWORD consoleMode = 0;
    if (GetConsoleMode(input, &consoleMode)) {
        std::wstring wide;
        wchar_t buffer[2048];
        DWORD read = 0;
        while (ReadConsoleW(input, buffer, 2048, &read, nullptr) && read > 0) {
            wide.append(buffer, read);
        }

        if (wide.empty() ||
            wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return {};
        }
        const int count = WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            nullptr, 0, nullptr, nullptr);
        if (count <= 0) {
            return {};
        }
        std::string bytes(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            bytes.data(), count, nullptr, nullptr);
        return bytes;
    }

    std::string bytes;
    bytes.reserve(65536);
    char buffer[kIoChunk];
    DWORD read = 0;
    while (ReadFile(input, buffer, kIoChunk, &read, nullptr) && read > 0) {
        bytes.append(buffer, buffer + read);
    }
    return bytes;
}

bool SetClipboardText(const std::wstring& text) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        return false;
    }

    void* locked = GlobalLock(memory);
    if (!locked) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(locked, text.c_str(), bytes);
    GlobalUnlock(memory);

    if (!OpenClipboardRetry()) {
        GlobalFree(memory);
        return false;
    }

    if (!EmptyClipboard()) {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }

    // Ownership of the allocation transfers to the clipboard on success.
    // Windows synthesizes CF_TEXT / CF_OEMTEXT from CF_UNICODETEXT.
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

std::wstring WideFromCodePage(UINT codePage, const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return {};
    }
    const int count = MultiByteToWideChar(codePage, 0, text, -1, nullptr, 0);
    if (count <= 1) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(count - 1), L'\0');
    MultiByteToWideChar(codePage, 0, text, -1, wide.data(), count);
    return wide;
}

// Returns false when the clipboard has no text format to print.
bool GetClipboardText(std::wstring& text) {
    text.clear();
    if (!OpenClipboardRetry()) {
        return false;
    }

    if (const HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        const auto* chars = static_cast<const wchar_t*>(GlobalLock(data));
        if (chars != nullptr) {
            text.assign(chars);
            GlobalUnlock(data);
        }
        CloseClipboard();
        return true;
    }

    if (const HANDLE data = GetClipboardData(CF_TEXT)) {
        const auto* chars = static_cast<const char*>(GlobalLock(data));
        if (chars != nullptr) {
            text = WideFromCodePage(CP_ACP, chars);
            GlobalUnlock(data);
        }
        CloseClipboard();
        return true;
    }

    if (const HANDLE data = GetClipboardData(CF_OEMTEXT)) {
        const auto* chars = static_cast<const char*>(GlobalLock(data));
        if (chars != nullptr) {
            text = WideFromCodePage(CP_OEMCP, chars);
            GlobalUnlock(data);
        }
        CloseClipboard();
        return true;
    }

    CloseClipboard();
    return false;
}

void WriteStdout(const std::wstring& text) {
    if (text.empty()) {
        return;
    }

    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD consoleMode = 0;
    if (GetConsoleMode(output, &consoleMode)) {
        size_t offset = 0;
        while (offset < text.size()) {
            const DWORD chunk = static_cast<DWORD>(
                std::min(text.size() - offset, static_cast<size_t>(30000)));
            DWORD written = 0;
            if (!WriteConsoleW(output, text.data() + offset, chunk, &written, nullptr) ||
                written == 0) {
                return;
            }
            offset += written;
        }
        return;
    }

    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return;
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return;
    }

    std::string utf8(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), count, nullptr, nullptr);

    size_t offset = 0;
    while (offset < utf8.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min(utf8.size() - offset, static_cast<size_t>(1u << 20)));
        DWORD written = 0;
        if (!WriteFile(output, utf8.data() + offset, chunk, &written, nullptr) ||
            written == 0) {
            return;
        }
        offset += written;
    }
}

int CopyToClipboard() {
    std::string bytes = ReadAllStdin();
    StripUtf8Bom(bytes);
    const std::wstring text = DecodeInputBytes(bytes);
    if (!SetClipboardText(text)) {
        std::fputs("pbcopy: unable to open clipboard\n", stderr);
        return 1;
    }
    return 0;
}

int PasteFromClipboard() {
    std::wstring text;
    // No text format, an empty string, or a clipboard we cannot read: write
    // nothing and stay silent on both stdout and stderr.
    if (!GetClipboardText(text) || text.empty()) {
        return 0;
    }
    WriteStdout(text);
    return 0;
}

}  // namespace

int main() {
    if (IsPasteCommand()) {
        return PasteFromClipboard();
    }
    return CopyToClipboard();
}
