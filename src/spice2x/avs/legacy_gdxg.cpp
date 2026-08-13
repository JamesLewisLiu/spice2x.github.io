#include "legacy_gdxg.h"
#include "legacy_gdxg_vmr.h"

#include <windows.h>
#include <imm.h>
#include <mmsystem.h>
#include <objbase.h>

#include <string>

#include "avs/game.h"
#include "launcher/launcher.h"
#include "util/libutils.h"
#include "util/logging.h"

namespace avs::legacy_gdxg {

    static bool ENABLED = false;

#ifndef SPICE64
    using boot_avs_t = int (__cdecl *)();
    using boot_main_t = void (__cdecl *)(HWND);
    using boot_step_t = int (__cdecl *)();
    using boot_terminate_t = void (__cdecl *)();
    using game_initialize_t = int (__cdecl *)(HWND);
    using game_mainloop_t = int (__cdecl *)(HWND);
    using game_finalize_t = int (__cdecl *)(HWND);
    using set_cmdline_t = void (__cdecl *)(char *);
    using sys_window_initialize_t = void (__cdecl *)(HINSTANCE, HWND);
    using sys_debug_mem_initialize_t = void (__cdecl *)();
    using sys_input_dbgkey_initialize_t = void (__cdecl *)();
    using sys_input_dbgkey_finalize_t = void (__cdecl *)();
    using sys_input_dbgkey_event_t = void (__cdecl *)(unsigned int, long);
    using sys_input_dbgkey_update_t = void (__cdecl *)();
    using movie_set_render_engine_t = void (__cdecl *)(IUnknown *);

    static HMODULE boot_module = nullptr;
    static HMODULE game_module = nullptr;
    static HMODULE system_module = nullptr;
    static HMODULE gdbase_module = nullptr;
    static HMODULE gftools_module = nullptr;
    static HMODULE movie_module = nullptr;
    static HWND game_window = nullptr;
    static bool com_initialized = false;
    static std::string legacy_cmdline;

    static boot_avs_t boot_avs = nullptr;
    static boot_main_t boot_main = nullptr;
    static boot_step_t boot_step = nullptr;
    static boot_terminate_t boot_terminate = nullptr;
    static game_initialize_t game_initialize = nullptr;
    static game_mainloop_t game_mainloop = nullptr;
    static game_finalize_t game_finalize = nullptr;
    static sys_window_initialize_t sys_window_initialize = nullptr;
    static sys_debug_mem_initialize_t sys_debug_mem_initialize = nullptr;
    static sys_input_dbgkey_initialize_t sys_input_dbgkey_initialize = nullptr;
    static sys_input_dbgkey_finalize_t sys_input_dbgkey_finalize = nullptr;
    static sys_input_dbgkey_event_t sys_input_dbgkey_keydown = nullptr;
    static sys_input_dbgkey_event_t sys_input_dbgkey_keyup = nullptr;
    static sys_input_dbgkey_update_t sys_input_dbgkey_update = nullptr;
    static vmr::set_d3d_device_t set_d3d_device = nullptr;
    static movie_set_render_engine_t movie_set_render_engine = nullptr;

    static constexpr auto BOOT_AVS_NAME = "?boot_avs@@YAHXZ";
    static constexpr auto BOOT_MAIN_NAME = "?boot_main@@YAXPAUHWND__@@@Z";
    static constexpr auto BOOT_STEP_NAME = "?boot_step@@YAHXZ";
    static constexpr auto BOOT_TERMINATE_NAME = "?boot_terminate@@YAXXZ";
    static constexpr auto GAME_INITIALIZE_NAME = "?game_initialize@@YAHPAUHWND__@@@Z";
    static constexpr auto GAME_MAINLOOP_NAME = "?game_mainloop@@YAHPAUHWND__@@@Z";
    static constexpr auto GAME_FINALIZE_NAME = "?game_finalize@@YAHPAUHWND__@@@Z";

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_CREATE) {
            sys_input_dbgkey_initialize();
            return DefWindowProcA(hwnd, message, wparam, lparam);
        }
        if (message == WM_CLOSE) {
            vmr::stop_rendering();
            DestroyWindow(hwnd);
            return 0;
        }
        if (message == WM_DESTROY) {
            sys_input_dbgkey_finalize();
            PostQuitMessage(0);
            return 0;
        }
        if (message == WM_KEYDOWN) {
            sys_input_dbgkey_keydown(static_cast<unsigned int>(wparam), static_cast<long>(lparam));
            if (wparam == VK_ESCAPE) {
                SendMessageA(hwnd, WM_CLOSE, 0, 0);
            }
        } else if (message == WM_KEYUP) {
            sys_input_dbgkey_keyup(static_cast<unsigned int>(wparam), static_cast<long>(lparam));
        }
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }

    static bool pump_messages() {
        MSG message{};
        if (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return false;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        return IsWindow(game_window) != FALSE;
    }

    static HWND create_game_window() {
        static constexpr auto CLASS_NAME = "gfdm";
        WNDCLASSEXA window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_OWNDC | CS_BYTEALIGNCLIENT | CS_BYTEALIGNWINDOW;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(5));
        window_class.lpszClassName = CLASS_NAME;

        if (!RegisterClassExA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            log_fatal("legacy-gdxg", "RegisterClassExA failed: {}", GetLastError());
        }

        const DWORD style = WS_POPUP | WS_VISIBLE;
        auto hwnd = CreateWindowExA(
                0,
                CLASS_NAME,
                "GFDM",
                style,
                0,
                0,
                1280,
                720,
                nullptr,
                nullptr,
                window_class.hInstance,
                nullptr);
        if (!hwnd) {
            log_fatal("legacy-gdxg", "CreateWindowExA failed: {}", GetLastError());
        }

        RECT client_rect{};
        RECT window_rect{};
        if (GetClientRect(hwnd, &client_rect) && GetWindowRect(hwnd, &window_rect)) {
            SetWindowPos(
                    hwnd,
                    nullptr,
                    0,
                    0,
                    (window_rect.right - window_rect.left) - client_rect.right + 1280,
                    (window_rect.bottom - window_rect.top) - client_rect.bottom + 720,
                    SWP_NOMOVE | SWP_FRAMECHANGED);
        }
        ShowCursor(FALSE);
        ImmAssociateContext(hwnd, nullptr);
        return hwnd;
    }

    static void set_legacy_cmdline(const char *sid_code) {
        // J32 is DrumMania and J33 is GuitarFreaks in the examined XG1 set.
        const auto mode = sid_code && _strnicmp(sid_code, "J33", 3) == 0 ? "-g" : "-d";
        legacy_cmdline = GetCommandLineA();
        legacy_cmdline += ' ';
        legacy_cmdline += mode;
        log_info("legacy-gdxg", "legacy game mode argument: {}", mode);

        auto sys_code_set_cmdline = reinterpret_cast<set_cmdline_t>(
                GetProcAddress(system_module, "sys_code_set_cmdline"));
        auto sys_debug_set_cmdline = reinterpret_cast<set_cmdline_t>(
                GetProcAddress(system_module, "sys_debug_set_cmdline"));
        auto gdbase_set_cmdline = reinterpret_cast<set_cmdline_t>(
                GetProcAddress(gdbase_module, "?gdbase_set_cmdline@@YAXQAD@Z"));

        if (sys_code_set_cmdline) {
            sys_code_set_cmdline(legacy_cmdline.data());
        }
        if (sys_debug_set_cmdline) {
            sys_debug_set_cmdline(legacy_cmdline.data());
        }
        if (gdbase_set_cmdline) {
            gdbase_set_cmdline(legacy_cmdline.data());
        }
    }
#endif

    void enable() {
        ENABLED = true;
    }

    bool enabled() {
        return ENABLED;
    }

    void load() {
#ifdef SPICE64
        log_fatal("legacy-gdxg", "legacy gdxg.exe modules require the 32-bit spice executable");
#else
        log_info("legacy-gdxg", "loading legacy XG boot.dll/game.dll adapter");
        boot_module = libutils::load_library(MODULE_PATH / "boot.dll");
        game_module = libutils::load_library(MODULE_PATH / "game.dll");
        system_module = libutils::load_library(MODULE_PATH / "libsystem.dll");
        gdbase_module = libutils::load_library(MODULE_PATH / "libgdbase.dll");
        gftools_module = libutils::load_library(MODULE_PATH / "libgftools.dll");
        movie_module = libutils::load_library(MODULE_PATH / "libmovie.dll");

        boot_avs = libutils::get_proc<boot_avs_t>(boot_module, BOOT_AVS_NAME);
        boot_main = libutils::get_proc<boot_main_t>(boot_module, BOOT_MAIN_NAME);
        boot_step = libutils::get_proc<boot_step_t>(boot_module, BOOT_STEP_NAME);
        boot_terminate = libutils::get_proc<boot_terminate_t>(boot_module, BOOT_TERMINATE_NAME);
        game_initialize = libutils::get_proc<game_initialize_t>(game_module, GAME_INITIALIZE_NAME);
        game_mainloop = libutils::get_proc<game_mainloop_t>(game_module, GAME_MAINLOOP_NAME);
        game_finalize = libutils::get_proc<game_finalize_t>(game_module, GAME_FINALIZE_NAME);
        sys_window_initialize = libutils::get_proc<sys_window_initialize_t>(
                system_module, "sys_window_initialize");
        sys_debug_mem_initialize = libutils::get_proc<sys_debug_mem_initialize_t>(
                system_module, "sys_debug_mem_initialize");
        sys_input_dbgkey_initialize = libutils::get_proc<sys_input_dbgkey_initialize_t>(
                system_module, "?sys_input_dbgkey_initialize@@YAXXZ");
        sys_input_dbgkey_finalize = libutils::get_proc<sys_input_dbgkey_finalize_t>(
                system_module, "?sys_input_dbgkey_finalize@@YAXXZ");
        sys_input_dbgkey_keydown = libutils::get_proc<sys_input_dbgkey_event_t>(
                system_module, "?sys_input_dbgkey_keydown@@YAXIJ@Z");
        sys_input_dbgkey_keyup = libutils::get_proc<sys_input_dbgkey_event_t>(
                system_module, "?sys_input_dbgkey_keyup@@YAXIJ@Z");
        sys_input_dbgkey_update = libutils::get_proc<sys_input_dbgkey_update_t>(
                system_module, "?sys_input_dbgkey_update@@YAXXZ");
        set_d3d_device = libutils::get_proc<vmr::set_d3d_device_t>(
                gftools_module, "?SetD3DDevice@@YAXPAUIDirect3DDevice9@@@Z");
        movie_set_render_engine = libutils::get_proc<movie_set_render_engine_t>(
                movie_module, "?movie_set_render_engine@@YAXPAUIVMRSvrRenderEngine@@@Z");

        // The legacy game has no gdxg.dll module. Use game.dll as the closest
        // equivalent hook target for code that consults DLL_INSTANCE.
        avs::game::DLL_INSTANCE = game_module;
        log_info("legacy-gdxg", "legacy entry points resolved successfully");
#endif
    }

    bool entry_init(char *sid_code, void *app_param) {
#ifdef SPICE64
        return false;
#else
        (void) app_param;
        // boot_avs is a legacy stub in the examined builds. Keep the call for
        // parity with gdxg.exe and log its result instead of treating zero as a
        // failure.
        const auto boot_avs_result = boot_avs();
        log_info("legacy-gdxg", "boot_avs returned {}", boot_avs_result);
        sys_debug_mem_initialize();

        set_legacy_cmdline(sid_code);
        timeBeginPeriod(1);
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        // gdxg.exe passes the literal value 8 (COINIT_SPEED_OVER_MEMORY with
        // the default multithreaded apartment). Keep the literal for parity
        // and compatibility with older MinGW headers.
        const auto com_result = CoInitializeEx(nullptr, 8);
        com_initialized = SUCCEEDED(com_result);
        log_info("legacy-gdxg", "CoInitializeEx returned 0x{:08x}", static_cast<unsigned>(com_result));
        if (FAILED(com_result)) {
            log_fatal("legacy-gdxg", "CoInitializeEx failed: {}", FMT_HRESULT(com_result));
        }
        game_window = create_game_window();
        if (!vmr::initialize(game_window, set_d3d_device)) {
            log_fatal("legacy-gdxg", "failed to initialize the XG1 VMR render bridge");
        }
        sys_window_initialize(GetModuleHandleW(nullptr), game_window);
        ShowWindow(game_window, SW_SHOWNORMAL);
        SetForegroundWindow(game_window);
        boot_main(game_window);
        log_info("legacy-gdxg", "boot_main completed; deferring state loops to entry_main");
        return true;
#endif
    }

    void entry_main() {
#ifndef SPICE64
        log_info("legacy-gdxg", "starting legacy boot state loop");
        while (pump_messages() && boot_step() != 0) {
        }

        if (IsWindow(game_window)) {
            const auto init_result = game_initialize(game_window);
            log_info("legacy-gdxg", "game_initialize returned {}", init_result);
            movie_set_render_engine(vmr::render_engine());
            log_info("legacy-gdxg", "legacy movie render engine installed");
            while (pump_messages()) {
                sys_input_dbgkey_update();
                game_mainloop(game_window);
            }
            const auto finalize_result = game_finalize(game_window);
            log_info("legacy-gdxg", "game_finalize returned {}", finalize_result);
        }

        boot_terminate();
        vmr::shutdown();
        if (IsWindow(game_window)) {
            DestroyWindow(game_window);
        }
        game_window = nullptr;
        if (com_initialized) {
            CoUninitialize();
            com_initialized = false;
        }
        timeEndPeriod(1);
        log_info("legacy-gdxg", "legacy main loop finished");
#endif
    }
}
