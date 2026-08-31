#pragma once

#include "overlay/windows/iopanel.h"

namespace overlay::windows {

    class DEAIOPanel : public IOPanel {
    public:
        DEAIOPanel(SpiceOverlay *overlay);

    protected:
        void build_io_panel() override;

    private:
        void find_dea_buttons();
        void draw_player(int player);
        void draw_button(const char *label, const ImVec2 &size, Button *button, Light *light, bool round);

        Button *start[2] {};
        Button *left[2] {};
        Button *right[2] {};
        Light *start_light[2] {};
        Light *lr_light[2] {};
    };
}
