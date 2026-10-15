/**
 * ofd2png.cpp - standalone command-line tool: OFD invoice -> PNG file.
 *
 *     ofd2png <input.ofd> [output.png] [--dpi N] [--quiet]
 *
 * Loads ofd_renderer.dll at run time instead of linking its import library.
 * That keeps the executable free of link-time library dependencies and lets it
 * report a useful message when the DLL is missing, is the wrong architecture,
 * or implements an incompatible ABI - all failure modes a caller hits in
 * practice and none of which an implicit link can explain.
 *
 * The DLL is searched for next to the .exe first, then along the normal
 * Windows search order.
 *
 * Build:  build_dll.bat x64 && build_dll.bat example x64
 * Run:    dist\dll\x64\ofd2png.exe invoice.ofd            -> invoice.png
 *         dist\dll\x64\ofd2png.exe invoice.ofd out.png --dpi 600
 *
 * You can also drag an .ofd file onto the .exe in Explorer.
 *
 * Compiled with /utf-8 (see build_dll.bat) so the Chinese literals below reach
 * stdout as UTF-8. Pair that with SetConsoleOutputCP(CP_UTF8) and the console
 * renders them correctly; without either one they arrive as ANSI and look like
 * mojibake in any UTF-8 reader.
 */

#define OFD_RENDERER_NO_IMPORT
#include "ofd_renderer.h"

#include <shellapi.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_USAGE = 1;
constexpr int EXIT_DLL = 2;
constexpr int EXIT_ABI = 3;
constexpr int EXIT_RENDER = 4;
constexpr int EXIT_WRITE = 5;

/* Entry-point signatures. Spelled out explicitly rather than reusing the
   header's declarations, because those carry __declspec(dllimport) and cannot
   be used to form a function pointer for GetProcAddress. */
using FnAbiVersion = uint32_t(__cdecl *)(void);
using FnVersion = const char *(__cdecl *)(void);
using FnLastError = const char *(__cdecl *)(void);
using FnRenderFileW = int(__cdecl *)(const uint16_t *, size_t,
                                     const OfdRenderOptions *, OfdRenderResult *);
using FnRenderMemory = int(__cdecl *)(const uint8_t *, size_t,
                                      const OfdRenderOptions *, OfdRenderResult *);
using FnFreeResult = void(__cdecl *)(OfdRenderResult *);

/* RAII wrapper around the DLL handle plus the entry points we need. */
class Library {
public:
    Library() = default;
    ~Library() {
        if (m_handle != nullptr) ::FreeLibrary(m_handle);
    }
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;

    bool Load() {
        const std::wstring beside = ExeDirectory() + L"\\ofd_renderer.dll";
        m_handle = ::LoadLibraryW(beside.c_str());
        if (m_handle == nullptr) {
            /* Fall back to the standard search order, in case the DLL was
               installed somewhere on PATH. */
            m_handle = ::LoadLibraryW(L"ofd_renderer.dll");
        }
        if (m_handle == nullptr) {
            const DWORD err = ::GetLastError();
            m_error = "无法加载 ofd_renderer.dll（错误码 " + std::to_string(err) +
                      "）。请确认 DLL 与 ofd2png.exe 在同一目录，"
                      "且架构一致（x64 的 exe 必须配 x64 的 DLL）。";
            return false;
        }

        abiVersion = Resolve<FnAbiVersion>("ofd_renderer_abi_version");
        version = Resolve<FnVersion>("ofd_renderer_version");
        lastError = Resolve<FnLastError>("ofd_renderer_last_error");
        renderFileW = Resolve<FnRenderFileW>("ofd_render_png_from_file_utf16");
        renderMemory = Resolve<FnRenderMemory>("ofd_render_png_from_memory");
        freeResult = Resolve<FnFreeResult>("ofd_free_result");

        if (abiVersion == nullptr || version == nullptr || lastError == nullptr ||
            renderFileW == nullptr || renderMemory == nullptr ||
            freeResult == nullptr) {
            m_error = "ofd_renderer.dll 已加载，但缺少预期的导出函数——"
                      "这通常意味着 DLL 版本过旧或文件损坏。";
            return false;
        }
        return true;
    }

    const std::string &Error() const { return m_error; }

    FnAbiVersion abiVersion = nullptr;
    FnVersion version = nullptr;
    FnLastError lastError = nullptr;
    FnRenderFileW renderFileW = nullptr;
    FnRenderMemory renderMemory = nullptr;
    FnFreeResult freeResult = nullptr;

private:
    template <typename T>
    T Resolve(const char *name) {
        return reinterpret_cast<T>(
            ::GetProcAddress(m_handle, name));  // NOLINT: cast required by FFI
    }

    static std::wstring ExeDirectory() {
        std::vector<wchar_t> buf(MAX_PATH);
        for (;;) {
            const DWORD n =
                ::GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
            if (n == 0) return L".";
            if (n < buf.size()) break;
            buf.resize(buf.size() * 2);  // path longer than MAX_PATH
        }
        std::wstring path(buf.data());
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring(L".")
                                           : path.substr(0, slash);
    }

    HMODULE m_handle = nullptr;
    std::string m_error;
};

std::string U8(const std::wstring &s) { return ofd::detail::WideToUtf8(s); }

/* invoice.ofd -> invoice.png */
std::wstring DefaultOutputFor(const std::wstring &input) {
    const size_t dot = input.find_last_of(L'.');
    const size_t slash = input.find_last_of(L"\\/");
    if (dot != std::wstring::npos &&
        (slash == std::wstring::npos || dot > slash)) {
        return input.substr(0, dot) + L".png";
    }
    return input + L".png";
}

void PrintUsage(std::ostream &os) {
    os << "用法: ofd2png <input.ofd> [output.png] [--dpi N] [--quiet]\n"
          "\n"
          "  input.ofd    要渲染的 OFD 发票文件\n"
          "  output.png   输出路径，缺省为「输入文件名.png」\n"
          "  --dpi N      渲染精度，缺省 300（与桌面版一致）\n"
          "  --quiet      只输出结果路径，便于脚本串联\n"
          "\n"
          "示例: ofd2png 发票.ofd 发票.png --dpi 600\n";
}

}  // namespace

int main() {
    ::SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    LPWSTR *argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr || argc < 2) {
        PrintUsage(std::cout);
        if (argv) ::LocalFree(argv);
        return EXIT_USAGE;
    }
    /* Copy everything out of argv before freeing: the array itself must not
       outlive LocalFree. */
    std::vector<std::wstring> args(argv, argv + argc);
    ::LocalFree(argv);

    std::wstring input;
    std::wstring output;
    unsigned int dpi = 0; /* 0 = DLL default (300) */
    bool quiet = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring &a = args[i];
        if (a == L"--dpi") {
            if (i + 1 >= args.size()) {
                std::cerr << "--dpi 需要一个数值参数\n";
                return EXIT_USAGE;
            }
            unsigned long v = 0;
            try {
                v = std::stoul(args[++i]);
            } catch (...) {
                std::cerr << "--dpi 的值不是有效数字\n";
                return EXIT_USAGE;
            }
            if (v == 0 || v > 2400) {
                std::cerr << "--dpi 超出合理范围（1..2400）\n";
                return EXIT_USAGE;
            }
            dpi = static_cast<unsigned int>(v);
        } else if (a == L"--quiet" || a == L"-q") {
            quiet = true;
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            PrintUsage(std::cout);
            return EXIT_OK;
        } else if (input.empty()) {
            input = a;
        } else if (output.empty()) {
            output = a;
        } else {
            std::cerr << "多出的参数: " << U8(a) << "\n";
            PrintUsage(std::cerr);
            return EXIT_USAGE;
        }
    }

    if (input.empty()) {
        PrintUsage(std::cerr);
        return EXIT_USAGE;
    }
    if (output.empty()) output = DefaultOutputFor(input);

    Library lib;
    if (!lib.Load()) {
        std::cerr << lib.Error() << "\n";
        return EXIT_DLL;
    }

    if (!quiet) {
        std::cout << "ofd_renderer.dll " << (lib.version ? lib.version() : "?")
                  << ", ABI " << (lib.abiVersion ? lib.abiVersion() : 0u) << "\n";
    }

    if (lib.abiVersion() != OFD_RENDERER_ABI_VERSION) {
        std::cerr << "ABI 不匹配: ofd2png 按 ABI " << OFD_RENDERER_ABI_VERSION
                  << " 编译，DLL 报告 " << lib.abiVersion()
                  << "。请重新编译 ofd2png（build_dll.bat example）。\n";
        return EXIT_ABI;
    }

    OfdRenderOptions opt;
    opt.dpi = dpi;
    opt.reserved = 0;

    OfdRenderResult result = {0};
    const int rc = lib.renderFileW(
        reinterpret_cast<const uint16_t *>(input.c_str()), input.size(), &opt,
        &result);

    if (rc != OFD_OK) {
        const char *msg = lib.lastError();
        std::cerr << "渲染失败（错误码 " << rc << "）";
        if (msg != nullptr && *msg != '\0') std::cerr << ": " << msg;
        std::cerr << "\n";
        lib.freeResult(&result); /* zeroed on failure, but free unconditionally */
        return EXIT_RENDER;
    }

    ofd::PngImage image;
    image.data.assign(result.png_data, result.png_data + result.png_size);
    image.width = result.width;
    image.height = result.height;
    image.pageWidthMm = result.page_width_mm;
    image.pageHeightMm = result.page_height_mm;

    lib.freeResult(&result);

    if (!image.SaveToFile(output)) {
        std::cerr << "无法写入 " << U8(output) << "\n";
        return EXIT_WRITE;
    }

    if (quiet) {
        std::cout << U8(output) << "\n";
    } else {
        std::cout << "输入  : " << U8(input) << "\n";
        std::cout << "输出  : " << U8(output) << "\n";
        std::cout << "页面  : " << image.pageWidthMm << "x" << image.pageHeightMm
                  << " mm -> " << image.width << "x" << image.height << " px, "
                  << image.data.size() << " bytes\n";
        std::cout << "OK\n";
    }
    return EXIT_OK;
}
