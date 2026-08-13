#pragma once

namespace avs::legacy_gdxg {

    // Select the adapter before avs::game::load_dll is called. This is kept
    // separate from DLL_NAME so existing GITADORA patch/module selection can
    // continue to use the canonical "gdxg.dll" identifier.
    void enable();
    bool enabled();

    void load();
    bool entry_init(char *sid_code, void *app_param);
    void entry_main();
}
