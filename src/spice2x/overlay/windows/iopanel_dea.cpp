#include "iopanel_dea.h"

#include "cfg/api.h"
#include "games/dea/io.h"
#include "games/io.h"
#include "launcher/launcher.h"
#include "misc/eamuse.h"

namespace overlay::windows {

    DEAIOPanel::DEAIOPanel(SpiceOverlay *overlay) : IOPanel(overlay) {
        this->title = "Dance Evolution IO Panel";
        find_dea_buttons();
    }

    void DEAIOPanel::find_dea_buttons() {
        const auto buttons = games::get_buttons(eamuse_get_game());
        const auto lights = games::get_lights(eamuse_get_game());

        start[0] = &buttons->at(games::dea::Buttons::P1Start);
        left[0] = &buttons->at(games::dea::Buttons::P1Left);
        right[0] = &buttons->at(games::dea::Buttons::P1Right);
        start[1] = &buttons->at(games::dea::Buttons::P2Start);
        left[1] = &buttons->at(games::dea::Buttons::P2Left);
        right[1] = &buttons->at(games::dea::Buttons::P2Right);

        start_light[0] = &lights->at(games::dea::Lights::P1Start);
        lr_light[0] = &lights->at(games::dea::Lights::P1LRButton);
        start_light[1] = &lights->at(games::dea::Lights::P2Start);
        lr_light[1] = &lights->at(games::dea::Lights::P2LRButton);
    }

    void DEAIOPanel::build_io_panel() {
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();
        ImGui::PushID("P1");
        draw_player(0);
        ImGui::PopID();

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();
        ImGui::PushID("P2");
        draw_player(1);
        ImGui::PopID();
    }

    void DEAIOPanel::draw_player(int player) {
        const float diameter = ImGui::GetFrameHeightWithSpacing() + ImGui::GetFrameHeight();
        const ImVec2 size(diameter, diameter);

        draw_button("<", size, left[player], lr_light[player], false);
        ImGui::SameLine();
        draw_button(player == 0 ? "P1\nSTART" : "P2\nSTART", size, start[player], start_light[player], true);
        ImGui::SameLine();
        draw_button(">", size, right[player], lr_light[player], false);
    }

    void DEAIOPanel::draw_button(
        const char *label, const ImVec2 &size, Button *button, Light *light, bool round) {

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(label, size);

        if (ImGui::IsItemActivated()) {
            button->override_state = GameAPI::Buttons::BUTTON_PRESSED;
            button->override_velocity = 1.f;
            button->override_enabled = true;
        } else if (ImGui::IsItemDeactivated()) {
            button->override_enabled = false;
        }

        const bool lit = light && GameAPI::Lights::readLight(RI_MGR, *light) > 0.5f;
        const ImU32 color = ImGui::GetColorU32(lit ? ImGuiCol_ButtonActive : ImGuiCol_Button);
        const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
        const ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
        const float radius = size.x * 0.42f;
        auto *draw_list = ImGui::GetWindowDrawList();

        if (round) {
            draw_list->AddCircleFilled(center, radius, color);
            draw_list->AddCircle(center, radius, border);
        } else {
            draw_list->AddQuadFilled(
                ImVec2(center.x, center.y - radius), ImVec2(center.x + radius, center.y),
                ImVec2(center.x, center.y + radius), ImVec2(center.x - radius, center.y), color);
            draw_list->AddQuad(
                ImVec2(center.x, center.y - radius), ImVec2(center.x + radius, center.y),
                ImVec2(center.x, center.y + radius), ImVec2(center.x - radius, center.y), border);
        }

        const ImVec2 text_size = ImGui::CalcTextSize(label);
        draw_list->AddText(ImVec2(center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f),
                           ImGui::GetColorU32(ImGuiCol_Text), label);
    }
}
