# DDS texture tool

This dependency-free Rust utility validates DDS files and packs raw RGBA8 pixels into DDS with a complete mip chain. The experimental world texture-pack loader has been verified in a Windows native build: export an original texture, author a 2x replacement, and display it on a station wall. See [texture pack authoring](../../CONTROLS.md#texture-pack-authoring-experimental) for its opt-in flags and content-key filenames.

```powershell
cargo build --release --locked --manifest-path tools/texture_mods/Cargo.toml
tools/texture_mods/target/release/darkrecomp-texture-tool.exe inspect edited.dds
tools/texture_mods/target/release/darkrecomp-texture-tool.exe from-rgba 1024 1024 edited.rgba new.dds
```

`from-rgba` expects tightly packed rows of RGBA bytes. It averages each channel independently for lower mip levels, with no gamma, alpha or normal-map transform. Use an authored DDS chain when a material requires different filtering. The output must be a new file. Existing files are preserved; an I/O failure may leave a partial new output for inspection.

The accepted DDS subset is one 2D RGBA8 or BGRA8 UNORM image, with every floor-halved mip through 1x1. Both legacy channel masks and the DX10 UNORM formats are supported. Dimensions are limited to 4096 per axis and the pixel chain to 128 MiB. Cubes, volumes, arrays, compression, sRGB, premultiplied alpha, missing levels, truncated data and trailing bytes are rejected. BGRA input is normalized to owned RGBA bytes.

Builds use Rust 1.74 or newer, with no third-party dependencies. Qualification used Rust 1.90: 13 tests, formatting and clippy with warnings denied passed. A separate C++20 parser accepted the tool's synthetic DDS output and passed 822 format and rejection checks. The native build passed 18 focused renderer, worker, input and shutdown tests. In-game checks exported 45 valid DDS files and visibly replaced one opaque 512x512 wall texture with a procedural 1024x1024 image with 11 mip levels. The game exited normally and joined the texture worker; original assets and saved profiles were unchanged. This establishes that single-material workflow; broader material compatibility and performance remain unqualified. No game assets are included.

The layout follows Microsoft's [DDS programming guide](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide) and [DDS header definitions](https://github.com/microsoft/DirectXTex/blob/main/DirectXTex/DDS.h).
