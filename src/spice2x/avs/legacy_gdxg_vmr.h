#pragma once

#include <windows.h>

struct IDirect3DDevice9;
struct IUnknown;

namespace avs::legacy_gdxg::vmr {

    using set_d3d_device_t = void (__cdecl *)(IDirect3DDevice9 *device);

    bool initialize(HWND window, set_d3d_device_t set_d3d_device);
    IUnknown *render_engine();
    void stop_rendering();
    void shutdown();
}
