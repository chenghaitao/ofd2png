# ofd2png

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64%20%7C%20x86-lightgrey.svg)](#构建)

把中国电子发票 OFD 文件（GB/T 33190-2016）的**第一页渲染为 PNG 位图**的 Windows 动态库与命令行工具。

- **`ofd_renderer.dll`** —— 一个无第三方运行时依赖的 DLL（C ABI），可被 C / C++ / C# / Python / Delphi 等任何语言调用
- **`ofd2png.exe`** —— 开箱即用的命令行转换工具

> 纯图像转换器：只做 OFD → PNG，**不提取发票结构化字段**（发票号码、金额等）。

## 特性

- **渲染质量与桌面版逐字节一致**：解析 → SVG 版面 → resvg 栅格化，中文（宋体/黑体/楷体/仿宋）走系统字体匹配，无方框
- **体积小**：x64 约 3.5 MB、x86 约 3.0 MB，全部静态编入（resvg / 字体库 / ZIP / XML），**部署只需一个 DLL**
- **三种输入**：UTF-8 路径、UTF-16（宽字符）路径（Windows 首选，原生支持中文路径）、内存字节（数据库 BLOB / HTTP 响应）
- **稳定 C ABI**：扁平 `extern "C"` + `__cdecl` 显式固定调用约定，结构体布局随 ABI 版本号发布；C++ 体验由 header-only 的 RAII 包装类提供，不绑定编译器版本
- **异常安全**：所有入口 `catch_unwind` 兜底，Rust panic 不会穿越边界杀死宿主进程

## 快速开始

### 命令行

```bat
dist\dll\x64\ofd2png.exe invoice.ofd                  :: 输出 invoice.png
dist\dll\x64\ofd2png.exe invoice.ofd out.png --dpi 600
dist\dll\x64\ofd2png.exe invoice.ofd --quiet          :: 只输出结果路径，便于脚本串联
dist\dll\x64\ofd2png.exe --help
```

也支持把 `.ofd` 文件直接拖到 `ofd2png.exe` 上。

退出码：`0` 成功 / `1` 参数错误 / `2` DLL 加载失败 / `3` ABI 不匹配 / `4` 渲染失败 / `5` 写文件失败。

### C++（推荐用包装类）

```cpp
#include "ofd_renderer.h"

ofd::Renderer renderer;              // 默认 300 DPI
ofd::PngImage img = renderer.RenderFile(L"D:\\发票\\test.ofd");

img.SaveToFile(L"D:\\out.png");
std::cout << img.width << "x" << img.height << std::endl;
```

链接（二选一）：

```bat
:: 隐式链接 —— 需要 ofd_renderer.dll.lib
cl /EHsc /std:c++17 /utf-8 main.cpp /link ofd_renderer.dll.lib
```

```cpp
// 显式加载 —— 不依赖导入库，DLL 缺失 / 架构不符 / ABI 不匹配时可给出自己的提示
// 参见 cpp/ofd2png.cpp 的完整实现
#define OFD_RENDERER_NO_IMPORT
#include "ofd_renderer.h"
```

> **编译务必加 `/utf-8`。** 不加时 MSVC 会把源码里的窄字符串字面量转成系统 ANSI（GBK），你自己写的中文提示会输出乱码。失败时 `Renderer` 抛 `std::runtime_error`；不想用异常就用 `TryRenderFile` / `TryRenderMemory`。

### C

```c
#include "ofd_renderer.h"

OfdRenderResult r = {0};
int rc = ofd_render_png_from_file_utf8("D:\\test.ofd", NULL, &r);
if (rc == OFD_OK) {
    FILE *f = fopen("out.png", "wb");
    fwrite(r.png_data, 1, r.png_size, f);
    fclose(f);
}
ofd_free_result(&r);              /* 每条路径上都要调用 */
```

## API 一览

| 函数 | 用途 |
| --- | --- |
| `ofd_render_png_from_file_utf16(path, len, opt, out)` | 宽字符路径，**Windows 上首选** |
| `ofd_render_png_from_file_utf8(path, opt, out)` | UTF-8 路径 |
| `ofd_render_png_from_memory(data, len, opt, out)` | 内存字节（数据库 BLOB、HTTP body） |
| `ofd_free_result(out)` | 释放结果，可重复调用，可传 NULL |
| `ofd_renderer_last_error()` | 本线程最近一次失败的原因（UTF-8） |
| `ofd_renderer_version()` / `ofd_renderer_abi_version()` | 版本 / ABI 号 |

参数说明：

- `OfdRenderOptions.dpi`：`0` 表示 300 DPI；`<= 1` 同样按 0 处理
- `options` 可以传 `NULL`，表示全默认
- 返回 `0`（`OFD_OK`）为成功，负数为错误码

| 错误码 | 值 | 含义 |
| --- | --- | --- |
| `OFD_OK` | 0 | 成功 |
| `OFD_ERR_INVALID_ARG` | -1 | 空指针或参数不可用（如非 UTF-8 路径） |
| `OFD_ERR_FILE_OPEN` | -2 | 文件打不开：不存在、被占用、无权限 |
| `OFD_ERR_PARSE` | -3 | 不是可用的 OFD：非 ZIP、缺 `OFD.xml`、XML 破损 |
| `OFD_ERR_RENDER` | -4 | 解析成功但栅格化失败 |
| `OFD_ERR_PANIC` | -99 | 内部 panic，已在边界处捕获 |

### 内存所有权（必读）

`OfdRenderResult` 里的指针**全部由 DLL 分配**，必须用 `ofd_free_result` 释放，**不要**在宿主侧 `free()` / `delete`——宿主和 DLL 往往链接不同的 C 运行时、各自持有独立堆，跨堆释放是 Windows 上的经典崩溃。

`ofd_free_result` 对全零结构体是空操作、可以调两次、可以传 `NULL`，失败路径无需记录"走到哪一步"。

`ofd_renderer_last_error()` 返回**线程局部**缓冲，指针在下一次本线程失败调用或线程退出后失效——需要保留就自己拷贝。

### 线程安全

所有入口无状态，可并发调用。系统字体在首次调用时懒加载一次，之后共享。

## 构建

前置条件：Rust 工具链（rustup）、Visual Studio 2022 C++ 生成工具、Windows SDK 10。

```bat
build_dll.bat            :: x64（默认）
build_dll.bat x86        :: 32 位（自动安装 i686-pc-windows-msvc 目标）
build_dll.bat all        :: 两个架构都出
build_dll.bat test       :: FFI 层单元测试
build_dll.bat testengine :: 引擎单元测试
build_dll.bat example    :: 编译 C++ 示例（默认 x64，example x86 编 32 位）
```

产物在 `dist\dll\<arch>\`：`ofd_renderer.dll` + `.dll.lib` + `ofd_renderer.h` + 两个示例源码 + `ofd2png.exe`。发布给第三方时把整个目录打包即可。

生成测试样本：

```bat
python scripts\make-test-ofd.py     :: 产出 dist\sample.ofd
```

推送 `v*` tag 时 GitHub Actions 会自动构建双架构 DLL 并发布 Release。

## 已知限制

| 限制 | 说明 |
| --- | --- |
| 只渲染第 1 页 | 多页 OFD 需要扩展引擎的页面遍历 |
| 输出只有 PNG | 引擎内部先生成矢量 SVG，当前不对外暴露 |
| 不提取发票字段 | 结构化字段（号码/金额/购销方）在引擎的 `fields` feature 里，本 DLL 编译时关闭 |
| 最大边长 20000 px | 防止畸形 `PhysicalBox` 申请上 GB 内存；300 DPI 下约 1.69 m |
| 需要系统中文字体 | 宋体/黑体等商业字体不随 DLL 分发，按 SVG fallback 链匹配系统字体（Windows 中文版自带） |

## 项目结构

```
ofd2png/
├── Cargo.toml                     # workspace 根（release 体积优化在此配置）
├── build_dll.bat                  # 一键构建脚本（x64/x86/测试/示例）
├── crates/
│   ├── invoice-engine/            # 核心引擎：OFD 解析 + SVG 版面 + resvg 栅格化
│   │   └── src/lib.rs             # （fields feature 承载字段提取，DLL 构建时关闭）
│   └── ofd-renderer/              # FFI crate → ofd_renderer.dll
│       ├── src/lib.rs             # C ABI 导出层：参数编解码与所有权转移
│       ├── include/ofd_renderer.h # C 声明 + header-only C++ RAII 包装
│       └── cpp/                   # ofd2png.cpp（显式加载）/ ofd_renderer_demo.cpp（隐式链接）
├── scripts/make-test-ofd.py       # 生成最小测试样本
└── .github/workflows/release.yml  # tag 触发的双架构构建与 Release
```

## 为什么不导出 C++ 类

直接 `extern "C++"` 导出类会把 ABI 绑定到**特定编译器 + 特定版本**：MSVC 名字修饰跨版本不一致，x86 成员函数的 `__thiscall` 手写 vtable 极易出错。所以分层是：

- **C ABI 是唯一的真实边界**——扁平函数、POD 结构体、`__cdecl` 显式固定调用约定，任何语言都能调
- **C++ 的 `ofd::Renderer` 是头文件内联包装**——有 RAII、`std::string`、异常，但没有一个 out-of-line 符号，不引入任何新的 ABI 依赖

## License

[MIT](LICENSE)
