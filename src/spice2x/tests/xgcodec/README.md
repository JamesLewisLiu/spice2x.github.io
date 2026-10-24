# XG codec tests

Standalone Windows x86 tests for the production `games/gitadora/xg_codec.cpp`.
The test includes that file directly, supplies launcher stubs and uses the
repository's fmt headers in header-only mode. It does not launch a game, register
a COM server, or create or modify any registry entry.

Configure with a 32-bit llvm-mingw toolchain and Ninja:

```powershell
cmake -S src/spice2x/tests/xgcodec -B ../xg-codec-tests-32 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=/path/to/llvm-mingw-x86.cmake `
  -DXG_CODEC_AX=M:/K32/contents/CLVsd.ax
cmake --build ../xg-codec-tests-32
ctest --test-dir ../xg-codec-tests-32 --output-on-failure
```

Omit `XG_CODEC_AX` to run just the registry tests. Those cover the six DWORD
overrides on a nonexistent subkey, size/type/existence queries, invalid and short
buffers, unknown values, case sensitivity and prefix boundaries, plus 40,000
queries racing against virtual-handle closure.

The optional integration test patches the real AX imports, configures the same
module twice, obtains its class factory, and creates/releases a decoder object.
It also exercises production `create_instance`: supported classes must use the
local factory; unsupported classes must preserve the native COM fallback. The
minimal test IAT patcher deliberately operates on real PE imports.

Two separate child processes exercise production `load_local` with AX copies:
the EXE directory wins when both locations contain a decoder, and the modules
directory works when the EXE directory has none. Each child starts from a third
working directory. Temporary copies are removed after successful probes.

These checks establish COM construction, import overrides, and loader behavior.
They do not construct a DirectShow graph or validate video playback/DXVA on a GPU.
