/**
 * ofd_renderer.h - C ABI / C++ facade over invoice-engine.
 *
 * Rasterizes page 1 of a Chinese electronic invoice in OFD format
 * (GB/T 33190-2016) into a PNG held in memory. Pure image conversion — no
 * invoice-field extraction: the desktop app compiles that separately, and this
 * DLL deliberately ships without it to stay small and field-bug-free.
 *
 * ---------------------------------------------------------------------------
 * Two layers, one ABI
 * ---------------------------------------------------------------------------
 * Everything below the `#ifdef __cplusplus` marker is plain C: flat functions,
 * POD structs, and an opaque ownership rule. That is the stable part — any
 * compiler, any language, any runtime can call it.
 *
 * The C++ block on top is a header-only RAII wrapper with no out-of-line
 * symbols, so a C++ project just includes this file and links the import
 * library. Nothing in it is ABI-sensitive: exporting a real C++ class instead
 * would tie the DLL to one compiler and version (MSVC name decoration differs
 * across releases, and x86 `__thiscall` makes hand-rolled vtables fragile).
 *
 * ---------------------------------------------------------------------------
 * Memory ownership - read this before writing a call
 * ---------------------------------------------------------------------------
 * Buffers and strings returned in OfdRenderResult are allocated *inside the
 * DLL* and must be released by ofd_free_result(). Never free() or delete them
 * from the host: host and DLL routinely link different C runtimes with
 * separate heaps, and freeing across that boundary is a hard crash.
 *
 * ofd_free_result() is safe on a zero-initialised struct, safe to call twice,
 * and safe on NULL, so the failure path needs no bookkeeping:
 *
 *     OfdRenderResult r = {0};          // or use the C++ wrapper
 *     int rc = ofd_render_png_from_file_utf8(path, 0, &r);
 *     if (rc == OFD_OK) { ...use r... }
 *     ofd_free_result(&r);              // always, on every path
 *
 * Error text comes from ofd_renderer_last_error(), which is per-thread and
 * lives until the next failing call on the same thread.
 *
 * ---------------------------------------------------------------------------
 * Threading
 * ---------------------------------------------------------------------------
 * Entry points are stateless and safe to call concurrently. System fonts are
 * loaded once, lazily, on first use.
 */

#ifndef OFD_RENDERER_H
#define OFD_RENDERER_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Export / calling convention                                               */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32)
#  if defined(OFD_RENDERER_BUILD)
#    define OFD_RENDERER_API __declspec(dllexport)
#  elif defined(OFD_RENDERER_NO_IMPORT)
/* Define OFD_RENDERER_NO_IMPORT if you load the DLL with LoadLibrary and
   resolve entry points with GetProcAddress — dllimport would then generate
   unresolved externals for every function you never call. */
#    define OFD_RENDERER_API
#  else
#    define OFD_RENDERER_API __declspec(dllimport)
#  endif
#else
#  define OFD_RENDERER_API
#endif

/* Pinned to __cdecl so a host project built with /Gz (default __stdcall)
   cannot silently mismatch the callee. Rust's extern "C" is __cdecl on x86. */
#if defined(_WIN32)
#  define OFD_RENDERER_CALL __cdecl
#else
#  define OFD_RENDERER_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Version / ABI                                                             */
/* ------------------------------------------------------------------------- */

/* Bumped whenever a struct layout changes or an existing entry point changes
   meaning. Compare it with the value you compiled against and refuse to run on
   a mismatch — that is the whole point of keeping it in the header.
   v2: OfdRenderResult no longer carries invoice fields (pure PNG converter). */
#define OFD_RENDERER_ABI_VERSION 2u

/* Result codes. Anything < 0 is a failure. */
#define OFD_OK                0
#define OFD_ERR_INVALID_ARG  (-1)  /* null pointer, unusable argument          */
#define OFD_ERR_FILE_OPEN    (-2)  /* missing, locked, no permission           */
#define OFD_ERR_PARSE        (-3)  /* not a usable OFD: ZIP/XML layer failed   */
#define OFD_ERR_RENDER       (-4)  /* parsed fine, rasterization failed        */
#define OFD_ERR_PANIC        (-99) /* internal panic, caught at the boundary   */

/* ABI revision implemented by the loaded DLL. */
OFD_RENDERER_API uint32_t OFD_RENDERER_CALL ofd_renderer_abi_version(void);

/* DLL version string, UTF-8, NUL-terminated, statically allocated.
   Do not free. */
OFD_RENDERER_API const char *OFD_RENDERER_CALL ofd_renderer_version(void);

/* Message for the most recent failure *on the calling thread*, UTF-8 and
   NUL-terminated. Valid until the next failing call on this thread or thread
   exit — copy it if you need to keep it. Empty when nothing has failed. */
OFD_RENDERER_API const char *OFD_RENDERER_CALL ofd_renderer_last_error(void);

/* ------------------------------------------------------------------------- */
/* Types                                                                     */
/* ------------------------------------------------------------------------- */

/* Call options. Zero-initialise (`{0}` in C, `{}` in C++) for all defaults. */
typedef struct OfdRenderOptions {
    /* Rasterization density. 0 means 300 DPI, matching the desktop app.
       Values <= 1 are treated as 0. */
    uint32_t dpi;
    /* Reserved. Must be 0. */
    uint32_t reserved;
} OfdRenderOptions;

/* Output of one render call. Treat as write-only until the call returns
   OFD_OK, and pass it to ofd_free_result() on every path. */
typedef struct OfdRenderResult {
    uint8_t *png_data;          /* PNG bytes, allocated by the DLL            */
    size_t   png_size;          /* length of png_data in bytes                */
    uint32_t width;             /* raster width in pixels                     */
    uint32_t height;            /* raster height in pixels                    */
    double   page_width_mm;     /* page width from the OFD PhysicalBox        */
    double   page_height_mm;    /* page height from the OFD PhysicalBox       */
} OfdRenderResult;

#if defined(__cplusplus)
static_assert(sizeof(OfdRenderOptions) == 8,
              "OfdRenderOptions layout drifted from the Rust definition");
#endif

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

/* Render page 1 of an OFD identified by a UTF-8 filesystem path.
 *
 * `options` may be NULL for defaults. `out` must point to writable storage.
 * Returns OFD_OK or a negative code; on success `out` must be released with
 * ofd_free_result(). */
OFD_RENDERER_API int OFD_RENDERER_CALL ofd_render_png_from_file_utf8(
    const char *path_utf8,
    const OfdRenderOptions *options,
    OfdRenderResult *out);

/* Render page 1 of an OFD identified by a UTF-16 (wide) path.
 *
 * Preferred on Windows: the path reaches the filesystem without a UTF-8
 * round-trip, so astral characters and unpaired surrogates survive.
 *
 * `path_len` is a code-unit count. Pass 0 to treat `path_utf16` as
 * NUL-terminated. */
OFD_RENDERER_API int OFD_RENDERER_CALL ofd_render_png_from_file_utf16(
    const uint16_t *path_utf16,
    size_t path_len,
    const OfdRenderOptions *options,
    OfdRenderResult *out);

/* Render page 1 of an OFD already held in memory — bytes pulled from a
 * database BLOB or an HTTP response, with no file on disk.
 *
 * `data` is read only for the duration of the call; the DLL keeps no
 * reference, so the caller may reuse or release its buffer as soon as this
 * returns. */
OFD_RENDERER_API int OFD_RENDERER_CALL ofd_render_png_from_memory(
    const uint8_t *data,
    size_t len,
    const OfdRenderOptions *options,
    OfdRenderResult *out);

/* Release everything a render call wrote into `out`, then zero the struct.
 *
 * Safe on a zeroed struct, safe twice, and a no-op on NULL. */
OFD_RENDERER_API void OFD_RENDERER_CALL ofd_free_result(OfdRenderResult *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ========================================================================= */
/* C++ convenience layer (header-only, no out-of-line symbols)               */
/* ========================================================================= */

#ifdef __cplusplus

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ofd {

namespace detail {

/* UTF-8 <-> UTF-16. Empty input maps to empty output; a conversion failure
   also yields empty rather than throwing, since it is only ever used for
   display and path transport. */
inline std::wstring Utf8ToWide(const std::string &s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          &out[0], n);
    return out;
}

inline std::string WideToUtf8(const std::wstring &s) {
    if (s.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()),
                                        NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          &out[0], n, NULL, NULL);
    return out;
}

/* Most recent error text reported by the DLL, as UTF-8. */
inline std::string LastErrorMessage() {
    const char *msg = ofd_renderer_last_error();
    return msg ? std::string(msg) : std::string();
}

} /* namespace detail */

/* A rendered page: PNG bytes plus the geometry needed to place them. */
class PngImage {
public:
    std::vector<unsigned char> data;
    unsigned int width = 0;
    unsigned int height = 0;
    double pageWidthMm = 0.0;
    double pageHeightMm = 0.0;

    bool Empty() const { return data.empty(); }

    /* Write the PNG to disk. Wide path avoids the ANSI code-page limit that
       breaks non-ASCII directories in narrow-path APIs. */
    bool SaveToFile(const std::wstring &path) const {
        std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (!data.empty()) {
            f.write(reinterpret_cast<const char *>(&data[0]),
                    static_cast<std::streamsize>(data.size()));
        }
        return f.good();
    }

    bool SaveToFile(const std::string &utf8Path) const {
        return SaveToFile(detail::Utf8ToWide(utf8Path));
    }
};

/* RAII renderer. Construct, optionally set the DPI, call one render method.
 *
 * Failures throw std::runtime_error carrying the DLL's own message. Use the
 * Try* overloads when exceptions are unwelcome. */
class Renderer {
public:
    Renderer() : m_dpi(0) {}
    explicit Renderer(unsigned int dpi) : m_dpi(dpi) {}

    void SetDpi(unsigned int dpi) { m_dpi = dpi; }
    unsigned int Dpi() const { return m_dpi; }

    const std::string &LastError() const { return m_error; }

    /* --- wide path (preferred on Windows) --- */
    PngImage RenderFile(const std::wstring &path) {
        PngImage img;
        if (!TryRenderFile(path, img)) {
            throw std::runtime_error(m_error);
        }
        return img;
    }

    bool TryRenderFile(const std::wstring &path, PngImage &out) {
        OfdRenderOptions opt = MakeOptions();
        OfdRenderResult raw = {0};
        const int rc = ofd_render_png_from_file_utf16(
            reinterpret_cast<const uint16_t *>(path.c_str()), path.size(),
            &opt, &raw);
        return Finish(rc, raw, out);
    }

    /* --- UTF-8 path --- */
    PngImage RenderFile(const std::string &utf8Path) {
        PngImage img;
        if (!TryRenderFile(utf8Path, img)) {
            throw std::runtime_error(m_error);
        }
        return img;
    }

    bool TryRenderFile(const std::string &utf8Path, PngImage &out) {
        OfdRenderOptions opt = MakeOptions();
        OfdRenderResult raw = {0};
        const int rc = ofd_render_png_from_file_utf8(
            utf8Path.c_str(), &opt, &raw);
        return Finish(rc, raw, out);
    }

    /* --- in-memory bytes (database BLOB, HTTP body, …) --- */
    PngImage RenderMemory(const void *data, size_t size) {
        PngImage img;
        if (!TryRenderMemory(data, size, img)) {
            throw std::runtime_error(m_error);
        }
        return img;
    }

    bool TryRenderMemory(const void *data, size_t size, PngImage &out) {
        OfdRenderOptions opt = MakeOptions();
        OfdRenderResult raw = {0};
        const int rc = ofd_render_png_from_memory(
            reinterpret_cast<const uint8_t *>(data), size, &opt, &raw);
        return Finish(rc, raw, out);
    }

    /* --- introspection --- */
    static std::string Version() {
        const char *v = ofd_renderer_version();
        return v ? std::string(v) : std::string();
    }

    static unsigned int AbiVersion() { return ofd_renderer_abi_version(); }

    /* Convenience: true when the DLL is present and compatible. */
    static bool Compatible() {
        return ofd_renderer_abi_version() == OFD_RENDERER_ABI_VERSION;
    }

    static std::wstring Utf8ToWide(const std::string &s) {
        return detail::Utf8ToWide(s);
    }
    static std::string WideToUtf8(const std::wstring &s) {
        return detail::WideToUtf8(s);
    }

private:
    OfdRenderOptions MakeOptions() const {
        OfdRenderOptions opt;
        opt.dpi = m_dpi;
        opt.reserved = 0;
        return opt;
    }

    /* Transfers a raw result into `out` and releases it, whatever happened. */
    bool Finish(int rc, OfdRenderResult &raw, PngImage &out) {
        if (rc == OFD_OK) {
            m_error.clear();
            out.data.assign(
                raw.png_data, raw.png_data + raw.png_size);
            out.width = raw.width;
            out.height = raw.height;
            out.pageWidthMm = raw.page_width_mm;
            out.pageHeightMm = raw.page_height_mm;
            ofd_free_result(&raw);
            return true;
        }

        m_error = detail::LastErrorMessage();
        if (m_error.empty()) {
            m_error = "OFD 渲染失败（错误码 " + std::to_string(rc) + "）";
        }
        ofd_free_result(&raw);
        return false;
    }

    unsigned int m_dpi;
    std::string m_error;
};

} /* namespace ofd */

#endif /* __cplusplus */

#endif /* OFD_RENDERER_H */
