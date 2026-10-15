/**
 * ofd_renderer_demo.cpp - minimal C++ consumer of ofd_renderer.dll.
 *
 * Exercises all three input paths so a packaging mistake shows up here rather
 * than in your application:
 *   1. wide-character filesystem path
 *   2. in-memory bytes (what you get from a database BLOB)
 *   3. a deliberate failure, to show the error surface
 *
 * Build:
 *     build_dll.bat x64
 *     build_dll.bat example
 * Run:
 *     dist\dll\x64\ofd_renderer_demo.exe <path-to.ofd>
 *
 * The DLL is linked implicitly through ofd_renderer.dll.lib, so the DLL must
 * sit next to the .exe - which is where the build script puts it.
 */

#include "ofd_renderer.h"

#include <shellapi.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace {

/* Read a whole file. Returns false when the file cannot be opened - the demo
   reports that as a setup problem, not as a DLL failure. */
bool ReadAllBytes(const std::wstring &path, std::vector<unsigned char> &out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size <= 0) return false;

    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char *>(out.data()), size);
    return f.good() || f.eof();
}

void ReportImage(const char *label, const ofd::PngImage &img) {
    std::cout << label << ": " << img.width << "x" << img.height << " px, "
              << img.data.size() << " bytes, page " << img.pageWidthMm << "x"
              << img.pageHeightMm << " mm\n";
}

}  // namespace

int main() {
    /* Console output carries Chinese field values; switch the console to UTF-8
       so they do not turn into mojibake on a GBK code page. */
    ::SetConsoleOutputCP(CP_UTF8);

    std::cout << "ofd_renderer DLL version " << ofd::Renderer::Version()
              << ", ABI " << ofd::Renderer::AbiVersion() << "\n";

    if (!ofd::Renderer::Compatible()) {
        std::cerr << "ABI mismatch: header expects "
                  << OFD_RENDERER_ABI_VERSION << ", DLL reports "
                  << ofd::Renderer::AbiVersion()
                  << ". Rebuild the demo against this DLL's header.\n";
        return 2;
    }

    int argc = 0;
    LPWSTR *argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr || argc < 2) {
        std::cerr << "usage: ofd_renderer_demo.exe <path-to.ofd>\n";
        if (argv) ::LocalFree(argv);
        return 1;
    }
    const std::wstring input = argv[1];
    ::LocalFree(argv);

    ofd::Renderer renderer;  /* default 300 DPI */

    try {
        /* --- 1. wide-character path --- */
        std::cout << "\n[1] RenderFile(wchar_t path)\n";
        const ofd::PngImage fromFile = renderer.RenderFile(input);
        ReportImage("    png", fromFile);

        const std::wstring outFile = input + L".png";
        if (fromFile.SaveToFile(outFile)) {
            /* Narrow stream + explicit UTF-8 conversion: std::wcout would pass
               the wide string through the "C" locale and mangle non-ASCII. */
            std::cout << "    saved -> " << ofd::detail::WideToUtf8(outFile)
                      << "\n";
        } else {
            std::cerr << "    could not write "
                      << ofd::detail::WideToUtf8(outFile) << "\n";
        }

        /* --- 2. in-memory bytes --- */
        std::cout << "\n[2] RenderMemory(bytes)\n";
        std::vector<unsigned char> bytes;
        if (!ReadAllBytes(input, bytes)) {
            std::cerr << "    could not read the file into memory\n";
            return 3;
        }
        const ofd::PngImage fromMemory =
            renderer.RenderMemory(bytes.data(), bytes.size());
        ReportImage("    png", fromMemory);

        if (fromMemory.data == fromFile.data) {
            std::cout << "    byte-identical to the path-based render\n";
        } else {
            std::cerr << "    MISMATCH: path and memory renders differ\n";
            return 4;
        }

        /* --- 3. failure surface --- */
        std::cout << "\n[3] failure path\n";
        try {
            renderer.RenderFile(std::wstring(L"Z:\\definitely\\missing.ofd"));
            std::cerr << "    expected an exception, got none\n";
            return 5;
        } catch (const std::exception &e) {
            std::cout << "    threw as expected: " << e.what() << "\n";
        }
    } catch (const std::exception &e) {
        std::cerr << "\nFAILED: " << e.what() << "\n";
        return 6;
    }

    std::cout << "\nOK\n";
    return 0;
}
