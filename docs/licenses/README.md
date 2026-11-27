# Third-party licences

STAR as a whole is under the GNU General Public License, version 3 (`LICENSE` at the top of the repository). It
includes or links the following, each under its own licence.

In the repository:

| Part | Where | Licence |
|---|---|---|
| PS2Recomp runtime and recompiler | `runtime/` | GPL-3.0 (`runtime/LICENSE`) |
| PCSX2's GS renderer and common code | `thirdparty/pcsx2/` | GPL-3.0 (`thirdparty/pcsx2/COPYING.GPLv3`) |
| Libraries inside PCSX2's tree (fmt, Dear ImGui, cpuinfo, Vulkan headers and VMA, xbyak, fast_float, WIL, xxhash) | `thirdparty/pcsx2/3rdparty/` | Each library's own licence file in its folder |
| DirectX headers | `thirdparty/directx-headers/` | MIT (`thirdparty/directx-headers/LICENSE`) |
| IPU movie decoder, from the Ace Combat 5 static recompilation | `runtime/ps2xRuntime/src/lib/ipu/` | Apache-2.0 (`AC5-Recomp-APACHE-2.0.txt`) |
| Roboto font | `thirdparty/pcsx2-gs/resources/fonts/` | Apache-2.0 (`Roboto-font-copyright.txt`; the licence text is in `shaderc-APACHE-2.0.txt`) |

Downloaded when STAR is built, and part of the program you run:

| Part | Licence |
|---|---|
| raylib (with GLFW and the stb libraries it carries) | zlib (`raylib-zlib.txt`, `glfw-zlib.txt`), MIT or public domain (`stb-MIT-or-public-domain.txt`) |
| SDL3 | zlib (`SDL3-zlib.txt`) |
| Dear ImGui | MIT (`dear-imgui-MIT.txt`) |
| rlImGui | zlib (`rlImGui-zlib.txt`) |
| shaderc, glslang, SPIRV-Tools, SPIRV-Headers | Apache-2.0, BSD-3 and others, MIT (`shaderc-APACHE-2.0.txt`, `glslang-BSD-3-and-others.txt`, `SPIRV-Tools-APACHE-2.0.txt`, `SPIRV-Headers-MIT.txt`) |
| libchdr, with the LZMA SDK, miniz and zstd it carries | BSD-3 (`libchdr-BSD-3.txt`), public domain (`lzma-sdk-public-domain.txt`), Unlicense (`miniz-unlicense.txt`), BSD (`zstd-BSD-3.txt`) |

Downloaded only to build the recompiler, and not part of the program you run: ELFIO, toml11, fmt, libdwarf,
rabbitizer. Their licences come with their sources in the build folder.
