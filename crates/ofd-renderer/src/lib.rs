//! Flat C ABI over `invoice-engine`, for host applications that are not Rust
//! (C / C++ / C# / Python / Delphi …).
//!
//! Scope: rasterize **page 1** of an OFD document to a PNG in memory. That is
//! the *entire* contract — no structured invoice fields are extracted or
//! returned (the desktop app compiles the engine with the `fields` feature for
//! that; this DLL deliberately does not, which keeps it small and keeps field
//! extraction bugs from ever touching this surface).
//!
//! Multi-page and vector (SVG) output are intentionally out of scope.
//!
//! # Why a flat C ABI and not an exported C++ class
//!
//! Exporting `extern "C++"` classes ties the DLL to one compiler *and* version:
//! MSVC name decoration differs across releases, and on x86 the `__thiscall`
//! convention makes a hand-rolled vtable easy to get subtly wrong. Everything
//! here is therefore `extern "C"` with plain structs and opaque handles.
//! `include/ofd_renderer.h` layers a header-only RAII wrapper on top, so C++
//! callers get a class-shaped API without inheriting the ABI fragility.
//!
//! # Memory ownership
//!
//! Every pointer written into [`OfdRenderResult`] is allocated **inside this
//! DLL** and must be released by [`ofd_free_result`] — never by the caller's
//! `free`/`delete`. Host and DLL may link different C runtimes with separate
//! heaps; freeing across that boundary is a classic Windows crash.
//!
//! # Panics
//!
//! `extern "C"` functions must not unwind, so every entry point wraps its work
//! in [`catch_unwind`] and reports a panic as [`OFD_ERR_PANIC`].

use std::cell::RefCell;
use std::ffi::{c_char, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;

use invoice_engine::OfdPng;

// =====================================================
// Result codes
// =====================================================

/// Success.
pub const OFD_OK: i32 = 0;
/// A required pointer was null, or an argument was unusable (e.g. a path that
/// is not valid UTF-8 in the `_utf8` entry point).
pub const OFD_ERR_INVALID_ARG: i32 = -1;
/// The file could not be opened (missing, locked, no permission).
pub const OFD_ERR_FILE_OPEN: i32 = -2;
/// The bytes are not a usable OFD: not a ZIP, missing `OFD.xml`, malformed XML.
pub const OFD_ERR_PARSE: i32 = -3;
/// Parsing succeeded but rasterizing the page failed (SVG/PNG stage).
pub const OFD_ERR_RENDER: i32 = -4;
/// The worker panicked; the panic was caught at the boundary.
pub const OFD_ERR_PANIC: i32 = -99;

/// ABI revision of this header, bumped whenever the struct layouts or the
/// meaning of an existing entry point changes. Callers should compare it with
/// the value they compiled against and refuse to run on a mismatch.
///
/// v2: `OfdRenderResult` no longer carries invoice-field data (pure PNG
/// converter); field members were removed.
pub const OFD_RENDERER_ABI_VERSION: u32 = 2;

/// Version string, `\0`-terminated at compile time so returning it can never
/// panic.
const VERSION_STRING: &str = concat!(env!("CARGO_PKG_VERSION"), "\0");

// =====================================================
// C-visible types
// =====================================================

/// Call options. Zero-initialise it (`{0}` in C/C++, `OfdRenderOptions()` in
/// C++) to get every default.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct OfdRenderOptions {
    /// Rasterization density. `0` means 300 DPI, which is what the desktop app
    /// uses. Values ≤ 1 are treated as 0.
    pub dpi: u32,
    /// Reserved for future use; must be 0.
    pub reserved: u32,
}

/// Everything one call produces. Treat as write-only until the call returns
/// [`OFD_OK`]; use [`ofd_free_result`] to release it either way.
#[repr(C)]
pub struct OfdRenderResult {
    /// PNG bytes, allocated by the DLL.
    pub png_data: *mut u8,
    /// Length of `png_data` in bytes.
    pub png_size: usize,
    /// Raster width in pixels.
    pub width: u32,
    /// Raster height in pixels.
    pub height: u32,
    /// Page width in millimetres, from the OFD `PhysicalBox`.
    pub page_width_mm: f64,
    /// Page height in millimetres.
    pub page_height_mm: f64,
}

// =====================================================
// Last-error storage
// =====================================================

thread_local! {
    /// Per-thread error text. Entry points are callable concurrently, so a
    /// process-wide buffer would race and hand one thread another's message.
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn set_last_error(msg: &str) {
    LAST_ERROR.with(|cell| {
        // Interior NULs cannot survive in a C string; drop them rather than
        // failing the error report itself.
        let cleaned = msg.replace('\0', "");
        *cell.borrow_mut() = CString::new(cleaned)
            .unwrap_or_else(|_| CString::new("error message unavailable").unwrap());
    });
}

// =====================================================
// Internal plumbing
// =====================================================

/// A failure carrying the code handed back to the caller.
struct FfiError {
    code: i32,
    msg: String,
}

impl FfiError {
    fn new(code: i32, msg: impl Into<String>) -> Self {
        Self { code, msg: msg.into() }
    }
}

fn panic_text(payload: &(dyn std::any::Any + Send)) -> String {
    if let Some(s) = payload.downcast_ref::<&'static str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic payload".to_string()
    }
}

fn dpi_from(options: *const OfdRenderOptions) -> f64 {
    // Null options means "all defaults".
    let raw = if options.is_null() {
        0
    } else {
        // SAFETY: the caller promises a readable `OfdRenderOptions` (or null,
        // handled above); we only copy the POD out of it.
        unsafe { (*options).dpi }
    };
    if raw == 0 || raw <= 1 {
        invoice_engine::DEFAULT_RENDER_DPI
    } else {
        raw as f64
    }
}

/// What one successful call yields — the PNG plus the page's physical size.
struct RenderedPage {
    png: OfdPng,
    page_width_mm: f64,
    page_height_mm: f64,
}

/// Parse + rasterize from a filesystem path.
fn render_path(path: &PathBuf, dpi: f64) -> Result<RenderedPage, FfiError> {
    let file = std::fs::File::open(path)
        .map_err(|e| FfiError::new(OFD_ERR_FILE_OPEN, format!("打开 OFD 文件失败: {}", e)))?;
    let parsed = invoice_engine::parse_ofd_from_reader(file)
        .map_err(|e| FfiError::new(OFD_ERR_PARSE, e))?;
    render_parsed(parsed, dpi)
}

/// Parse + rasterize from an in-memory buffer.
fn render_memory(data: &[u8], dpi: f64) -> Result<RenderedPage, FfiError> {
    let parsed = invoice_engine::parse_ofd_from_bytes(data)
        .map_err(|e| FfiError::new(OFD_ERR_PARSE, e))?;
    render_parsed(parsed, dpi)
}

fn render_parsed(
    parsed: invoice_engine::OfdResult,
    dpi: f64,
) -> Result<RenderedPage, FfiError> {
    let png = invoice_engine::svg_to_png(
        &parsed.svg,
        parsed.page_width,
        parsed.page_height,
        dpi,
    )
    .map_err(|e| FfiError::new(OFD_ERR_RENDER, e))?;

    Ok(RenderedPage {
        png,
        page_width_mm: parsed.page_width,
        page_height_mm: parsed.page_height,
    })
}

/// Writes a successful render into `out`.
///
/// # Safety
/// `out` must be a valid, properly aligned, writable `OfdRenderResult`.
unsafe fn fill_result(out: *mut OfdRenderResult, render: RenderedPage) -> i32 {
    let r = &mut *out;

    // `into_boxed_slice` makes capacity == len, so the matching free can
    // rebuild the Box from (ptr, len) without knowing the original capacity.
    let png = render.png.data.into_boxed_slice();
    r.png_size = png.len();
    r.png_data = Box::into_raw(png) as *mut u8;
    r.width = render.png.width;
    r.height = render.png.height;
    r.page_width_mm = render.page_width_mm;
    r.page_height_mm = render.page_height_mm;

    OFD_OK
}

/// Runs `f`, zeroes `out` up front, and converts the outcome into a code.
///
/// Zeroing before the work starts is what makes failure safe: `ofd_free_result`
/// on an all-zero struct is a no-op, so callers can free unconditionally
/// without tracking how far the call got.
///
/// # Safety
/// `out` must be a valid, writable `OfdRenderResult`.
unsafe fn run_into<F>(out: *mut OfdRenderResult, f: F) -> i32
where
    F: FnOnce() -> Result<RenderedPage, FfiError>,
{
    std::ptr::write_bytes(out, 0, 1);

    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(render)) => fill_result(out, render),
        Ok(Err(e)) => {
            set_last_error(&e.msg);
            e.code
        }
        Err(payload) => {
            set_last_error(&format!("内部异常: {}", panic_text(&*payload)));
            OFD_ERR_PANIC
        }
    }
}

// =====================================================
// Exported entry points
// =====================================================

/// ABI revision of this DLL. Compare against `OFD_RENDERER_ABI_VERSION`.
#[no_mangle]
pub extern "C" fn ofd_renderer_abi_version() -> u32 {
    OFD_RENDERER_ABI_VERSION
}

/// `\0`-terminated version string. Statically allocated — do not free.
#[no_mangle]
pub extern "C" fn ofd_renderer_version() -> *const c_char {
    VERSION_STRING.as_ptr() as *const c_char
}

/// Message for the most recent failure **on the calling thread**.
///
/// The pointer stays valid until the next failing call on this thread, or
/// thread exit — copy it if you need to keep it. Returns an empty string when
/// nothing has failed yet.
#[no_mangle]
pub extern "C" fn ofd_renderer_last_error() -> *const c_char {
    LAST_ERROR.with(|cell| cell.borrow().as_ptr())
}

/// Render page 1 of an OFD identified by a UTF-8 filesystem path.
///
/// Returns [`OFD_OK`] or a negative error code. On success `out` owns the PNG
/// and must be released with [`ofd_free_result`].
///
/// # Safety
/// `path_utf8` must be a valid `\0`-terminated string; `options` may be null;
/// `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn ofd_render_png_from_file_utf8(
    path_utf8: *const c_char,
    options: *const OfdRenderOptions,
    out: *mut OfdRenderResult,
) -> i32 {
    if out.is_null() {
        set_last_error("out 参数为空指针");
        return OFD_ERR_INVALID_ARG;
    }
    if path_utf8.is_null() {
        std::ptr::write_bytes(out, 0, 1);
        set_last_error("path 参数为空指针");
        return OFD_ERR_INVALID_ARG;
    }

    let path = match CStr::from_ptr(path_utf8).to_str() {
        Ok(s) => PathBuf::from(s),
        Err(_) => {
            std::ptr::write_bytes(out, 0, 1);
            set_last_error("路径不是合法的 UTF-8");
            return OFD_ERR_INVALID_ARG;
        }
    };

    run_into(out, move || render_path(&path, dpi_from(options)))
}

/// Render page 1 of an OFD identified by a UTF-16 (wide) filesystem path.
///
/// Preferred on Windows: paths go through `OsString` untouched, so characters
/// outside the BMP and unpaired surrogates never have to survive a UTF-8
/// round-trip.
///
/// # Safety
/// `path_utf16` must point to `path_len` readable `u16` code units forming a
/// `\0`-terminated or length-counted path; `options` may be null; `out` must be
/// writable.
#[no_mangle]
pub unsafe extern "C" fn ofd_render_png_from_file_utf16(
    path_utf16: *const u16,
    path_len: usize,
    options: *const OfdRenderOptions,
    out: *mut OfdRenderResult,
) -> i32 {
    if out.is_null() {
        set_last_error("out 参数为空指针");
        return OFD_ERR_INVALID_ARG;
    }
    if path_utf16.is_null() {
        std::ptr::write_bytes(out, 0, 1);
        set_last_error("path 参数为空指针");
        return OFD_ERR_INVALID_ARG;
    }

    // `path_len == 0` means "treat it as NUL-terminated"; anything else is
    // taken literally, which lets callers pass non-terminated buffers.
    let units: &[u16] = if path_len == 0 {
        let mut n = 0usize;
        while *path_utf16.add(n) != 0 {
            n += 1;
        }
        std::slice::from_raw_parts(path_utf16, n)
    } else {
        std::slice::from_raw_parts(path_utf16, path_len)
    };

    use std::os::windows::ffi::OsStringExt;
    let path = PathBuf::from(std::ffi::OsString::from_wide(units));

    run_into(out, move || render_path(&path, dpi_from(options)))
}

/// Render page 1 of an OFD already held in memory.
///
/// `data` is only read for the duration of the call — the DLL keeps no
/// reference to it, so the caller is free to reuse or release its buffer as
/// soon as this returns.
///
/// # Safety
/// `data` must point to `len` readable bytes; `options` may be null; `out`
/// must be writable.
#[no_mangle]
pub unsafe extern "C" fn ofd_render_png_from_memory(
    data: *const u8,
    len: usize,
    options: *const OfdRenderOptions,
    out: *mut OfdRenderResult,
) -> i32 {
    if out.is_null() {
        set_last_error("out 参数为空指针");
        return OFD_ERR_INVALID_ARG;
    }
    if data.is_null() || len == 0 {
        std::ptr::write_bytes(out, 0, 1);
        set_last_error("data 为空或长度为 0");
        return OFD_ERR_INVALID_ARG;
    }

    let bytes = std::slice::from_raw_parts(data, len);
    run_into(out, move || render_memory(bytes, dpi_from(options)))
}

/// Releases everything [`ofd_render_png_from_file_utf8`] (and friends) wrote
/// into `out`, then zeroes the struct.
///
/// Safe to call on a zeroed struct and safe to call twice. Passing a null
/// `out` is a no-op.
///
/// # Safety
/// `out` must either be null or point to a struct this DLL produced (or one
/// that was zero-initialised).
#[no_mangle]
pub unsafe extern "C" fn ofd_free_result(out: *mut OfdRenderResult) {
    if out.is_null() {
        return;
    }
    let r = &mut *out;

    if !r.png_data.is_null() && r.png_size > 0 {
        let slice = std::ptr::slice_from_raw_parts_mut(r.png_data, r.png_size);
        drop(Box::from_raw(slice));
    }

    std::ptr::write_bytes(out, 0, 1);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn zeroed() -> OfdRenderResult {
        // SAFETY: `OfdRenderResult` is a plain repr(C) struct of pointers and
        // scalars, for which an all-zero bit pattern is a valid value.
        unsafe { std::mem::zeroed() }
    }

    #[test]
    fn version_is_terminated() {
        let ptr = ofd_renderer_version();
        let s = unsafe { CStr::from_ptr(ptr) }.to_str().unwrap();
        assert_eq!(s, env!("CARGO_PKG_VERSION"));
    }

    #[test]
    fn null_out_is_rejected() {
        let code = unsafe {
            ofd_render_png_from_memory(
                b"x".as_ptr(),
                1,
                std::ptr::null(),
                std::ptr::null_mut(),
            )
        };
        assert_eq!(code, OFD_ERR_INVALID_ARG);
    }

    #[test]
    fn free_is_idempotent_on_zeroed() {
        let mut r = zeroed();
        unsafe { ofd_free_result(&mut r) };
        unsafe { ofd_free_result(&mut r) };
        unsafe { ofd_free_result(std::ptr::null_mut()) };
        assert!(r.png_data.is_null());
    }

    #[test]
    fn garbage_bytes_report_parse_error() {
        let mut r = zeroed();
        let junk = b"not a zip archive at all";
        let code = unsafe {
            ofd_render_png_from_memory(
                junk.as_ptr(),
                junk.len(),
                std::ptr::null(),
                &mut r,
            )
        };
        assert_eq!(code, OFD_ERR_PARSE);
        assert!(r.png_data.is_null());

        let msg = unsafe { CStr::from_ptr(ofd_renderer_last_error()) }
            .to_str()
            .unwrap()
            .to_string();
        assert!(!msg.is_empty(), "failure should leave a message behind");

        unsafe { ofd_free_result(&mut r) };
    }

    #[test]
    fn missing_file_reports_open_error() {
        let mut r = zeroed();
        let path = CString::new("Z:/definitely/not/here.ofd").unwrap();
        let code = unsafe {
            ofd_render_png_from_file_utf8(path.as_ptr(), std::ptr::null(), &mut r)
        };
        assert_eq!(code, OFD_ERR_FILE_OPEN);
        unsafe { ofd_free_result(&mut r) };
    }

    #[test]
    fn empty_dpi_means_default() {
        let o = OfdRenderOptions { dpi: 0, reserved: 0 };
        assert_eq!(dpi_from(&o), invoice_engine::DEFAULT_RENDER_DPI);
        assert_eq!(dpi_from(std::ptr::null()), invoice_engine::DEFAULT_RENDER_DPI);

        let o = OfdRenderOptions { dpi: 600, reserved: 0 };
        assert_eq!(dpi_from(&o), 600.0);
    }
}
