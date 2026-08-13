# Legacy GDXG Loader POC

## Goal

Add a proof-of-concept path for older 32-bit GITADORA XG installations whose
game directory contains `gdxg.exe`, `boot.dll`, and `game.dll`, so that
`spice.exe` can host the game modules directly instead of starting `gdxg.exe`
and injecting a hook DLL into that process.

The reference installations examined for this POC are:

- Legacy XG1: `E:\HDD\XG1\bin`
- Later modular version: `M:\K32\contents\modules`

These paths are investigation inputs only. They are not embedded in the code.

## Initial findings

### Existing spice2x loading model

spice2x already acts as a module host. The normal path:

1. detects the game DLL in `launcher/launcher.cpp`;
2. loads AVS and AVS-EA3;
3. loads the selected game module in `avs/game.cpp`;
4. resolves `dll_entry_init` and `dll_entry_main`;
5. lets AVS-EA3 call `dll_entry_init`;
6. calls `dll_entry_main` from the launcher.

The current GITADORA detection selects `gdxg.dll`. This works for later modular
versions such as K32.

### PE comparison

All examined XG1/K32 binaries are PE32 (`IMAGE_FILE_MACHINE_I386`).

| File | Kind | Relevant exports |
| --- | --- | --- |
| XG1 `gdxg.exe` | EXE | none |
| XG1 `boot.dll` | DLL | `boot_avs`, `boot_main`, `boot_step`, `boot_terminate` |
| XG1 `game.dll` | DLL | `game_initialize`, `game_mainloop`, `game_finalize` |
| K32 `gdxg.dll` | DLL | `dll_entry_init`, `dll_entry_main` |
| K32 `boot.dll` | DLL | same boot-family entry points |
| K32 `game.dll` | DLL | same game-family entry points |

The XG1 executable imports the boot/game entry points directly and primarily
supplies command-line setup, window creation, message pumping, initialization,
main-loop dispatch, and cleanup.

XG1 `gdxg.exe` has no relocation section and uses image base `0x00400000`.
Consequently it cannot be treated as an ordinary DLL or safely loaded into the
spice process by renaming it or calling `LoadLibrary`.

### Why the K32 `gdxg.dll` cannot be copied into XG1

Although K32's `gdxg.dll` calls the same broad boot/game API, it imports newer
symbols absent from the XG1 modules, including:

- seven versioned `XC...` AVS exports;
- `sys_ikey_boot` and related license/account-key functions;
- `sys_code_set_develop_version_code`;
- newer `libgdbase` process/exception helpers;
- a newer `libgftools` iKey callback.

It therefore fails dependency resolution against the XG1 DLL set. A dedicated
legacy adapter is required.

### Existing XG support in spice2x

The 32-bit GITADORA implementation already contains XG-specific behavior:

- processor affinity is limited to the first two logical processors;
- cabinet type 3 is rejected for XG;
- J32/J33 input definitions and legacy device/extdev hooks are present.

The missing piece is the executable-to-module startup adapter, not the entire
game integration.

## POC design

The POC adds a legacy backend to `avs::game`:

- auto-detect `gdxg.exe + boot.dll + game.dll` only when `gdxg.dll` is absent;
- compile the backend only for 32-bit builds;
- load `boot.dll` and `game.dll` through the existing DLL loader;
- dynamically resolve the decorated legacy entry points;
- expose the existing `avs::game::load_dll`, `entry_init`, and `entry_main`
  interface to the rest of spice2x;
- keep legacy state and cleanup isolated from the standard module path.

The first runtime sequence is intentionally conservative. It will be refined
from logs and debugger traces rather than silently assuming that every XG
revision uses the same ordering.

## Risks and open questions

1. Exact `gdxg.exe` window-class, dimensions, styles, and focus behavior still
   need runtime tracing.
2. The exact relationship between `boot_avs`, AVS-EA3 initialization, and the
   legacy game loop must be confirmed on J32/J33.
3. Return-value semantics for `boot_step` and `game_mainloop` require runtime
   confirmation.
4. Shutdown ordering must be tested to avoid AVS threads or graphics objects
   surviving module cleanup.
5. Both GF (`J33`) and DM (`J32`) command-line/configuration paths must be
   validated.

## Work log

### 2026-08-13 - Investigation

- Inspected the spice2x launcher, AVS game loader, AVS-EA3 entry call, and
  GITADORA hooks.
- Enumerated XG1 and K32 module layouts.
- Parsed PE headers, imports, and exports without executing game binaries.
- Confirmed XG1's current scripts use `launcherx.exe gfdmhook.dll gdxg.exe ...`,
  which is the injection path this POC intends to replace.
- Confirmed K32 uses a standard `gdxg.dll` with `dll_entry_init` and
  `dll_entry_main`.
- Compared K32 `gdxg.dll` imports with XG1 exports and ruled out direct reuse.

### 2026-08-13 - Branch creation

- Requested branch: `codex/poc-legacy-gdxg-loader`.
- First branch creation attempt was blocked because the sandbox could not write
  `.git/refs`.
- Retried with explicit permission and successfully created/switched branches.
- The pre-existing worktree contained 48 modified files before this POC. Their
  diff statistics showed equal additions/deletions throughout, consistent with
  a broad line-ending conversion. They were preserved and were not reset.
- Because `launcher.cpp` and `games/gitadora/*` are among those pre-existing
  modifications, POC edits to them must remain narrow and be reviewed against
  both the branch base and the current worktree.

### 2026-08-13 - Implementation start

- Created this document before code changes so subsequent operations and
  findings can be appended as the POC progresses.

### 2026-08-13 - Initial adapter implementation

- Added a 32-bit-only legacy adapter under `avs/legacy_gdxg.*`.
- Added automatic detection for `gdxg.exe + boot.dll + game.dll` when the
  standard `gdxg.dll` was not detected.
- Kept the canonical logical module name `gdxg.dll` so existing GITADORA heap,
  patch-manager extra-DLL, and game-selection behavior remains active.
- The adapter loads `boot.dll`, `game.dll`, `libsystem.dll`, and
  `libgdbase.dll`, then resolves all required decorated exports dynamically.
- The adapter creates a temporary Win32 host window and forwards the legacy
  GF/DM command line based on the J32/J33 soft ID received by `entry_init`.
- Implemented the first hypothesized lifecycle:
  `boot_avs -> boot_main -> repeated boot_step -> game_initialize -> repeated
  game_mainloop -> game_finalize -> boot_terminate`.
- K32's analysis report states that `boot_step` returns zero when boot is
  complete. `Report_game.md` states that `game_mainloop` returns zero when a
  module completes. The latter may represent a mode transition rather than
  total process termination, so this POC loop is deliberately logged and still
  requires live validation.
- No game executable or DLL was executed during implementation.

### 2026-08-13 - Static review and build-environment check

- Confirmed the new source file is present in `SOURCE_FILES` and the adapter is
  selected only in non-`SPICE64` launcher builds.
- Confirmed all required legacy entry points are resolved through the existing
  fatal `libutils::get_proc` helper, so an incompatible XG build fails with the
  missing symbol in the log instead of dereferencing a null function pointer.
- WSL was initially denied by the sandbox. After explicit permission it started
  successfully, but that environment has no `cmake`, `ninja`, or 32-bit MinGW
  compiler/toolchain file. No native Windows compiler was found either.
  Consequently the POC has not yet received a compile test in this workspace.
- No Capstone or iced-x86 Python module and no objdump/LLVM disassembler were
  available locally. The implementation therefore continues to rely on PE
  import/export inspection and the existing K32 analysis reports, with runtime
  sequence assumptions kept visible here.
- Corrected the first game-loop draft after reviewing `Report_game.md`: a zero
  return from `game_mainloop` marks completion of the current game/app module,
  not necessarily application exit. The adapter now finalizes and initializes
  the next module cycle until the host window is closed.

### 2026-08-13 - Direct disassembly of XG1 `gdxg.exe`

- Installed Capstone into the task-specific temporary visualization directory;
  no repository dependency was added.
- Read-only disassembly confirmed the legacy executable's main order:
  `timeBeginPeriod(1)`, high process priority, `boot_avs`, command-line
  forwarding, COM initialization, host/render setup, `boot_main`, message pump
  plus `boot_step` until zero, `game_initialize`, then a message pump that calls
  `sys_input_dbgkey_update` and `game_mainloop` every frame, followed by
  `game_finalize` on exit.
- Corrected the adapter: XG1 ignores the return from `game_mainloop`; it does not
  use it as the host-loop termination condition.
- Confirmed the original window is a visible popup at 1280x720, class `gfdm`,
  title `GFDM`, and calls `sys_window_initialize(HINSTANCE, HWND)` before
  `ShowWindow`. Updated the POC to match those externally visible properties.
- Matched the original COM ordering by calling `CoInitializeEx` after command
  line setup and balancing it with `CoUninitialize` during adapter cleanup.
- Disassembly exposed an important missing component: the EXE constructs a COM
  DirectShow/render graph, obtains a D3D device, calls `SetD3DDevice`, and after
  `game_initialize` passes an `IVMRSvrRenderEngine` pointer to
  `movie_set_render_engine`. The current POC does not yet reproduce that private
  render bridge and now emits an explicit warning before entering the game
  loop. This is the largest remaining blocker to a bootable implementation.

### 2026-08-13 - VMR COM ownership analysis

- Internet searches for the four embedded COM GUIDs returned no public matches.
- `E:\HDD\XG1\bin\required.reg` identifies two of them as XG1-local COM
  classes hosted by `libvmrsvr.dll`:
  - `{8D372F4D-E0C0-464F-BDA9-963924890A35}`: `Wizard`
  - `{9401081E-848A-4DA7-B6C1-9828BB493E4F}`: `RenderEngine`
- The other two GUIDs are the interfaces requested from those classes:
  - `{6D402155-D941-478A-BEBB-AF361F633851}`: Wizard interface
  - `{6E474394-49FD-416F-AED4-0EDAD1879DC7}`: RenderEngine interface
- Both class and interface GUIDs also occur in `libvmrsvr.dll`, confirming that
  they are supplied by the game package rather than the Windows SDK.
- The EXE does more than instantiate these classes: it constructs its own
  allocator/presenter object, passes that object to the render engine, connects
  Wizard/RenderEngine to the game HWND, obtains the D3D device, and forwards
  that device to `libgftools`. This custom object lives in `gdxg.exe`, not
  `libvmrsvr.dll`, so a complete loader must port that implementation or provide
  an ABI-compatible replacement.

## Current POC status

Implemented:

- legacy-layout auto-detection;
- 32-bit-only adapter selection;
- direct loading of XG1 module DLLs;
- decorated entry-point resolution with fatal diagnostics;
- J32/J33 GF/DM command-line forwarding;
- legacy-compatible host window creation and `sys_window_initialize` call;
- verified boot and game message-loop ordering;
- per-step logging for field validation.

Not yet implemented or validated:

- XG1's custom VMR surface allocator/image presenter bridge;
- successful execution against the real XG1 files;
- compilation, because no supported C++/MinGW toolchain is installed in the
  current Windows or WSL environment;
- clean shutdown and both J32/J33 modes on real hardware/files.

The branch is therefore a structural POC and investigation checkpoint, not yet
a claim of a bootable replacement for `gdxg.exe`.
