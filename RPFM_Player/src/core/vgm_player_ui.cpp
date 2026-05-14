// vgm_player_ui.cpp - Shared VGM player UI rendering

#include "vgm_player_ui.h"
#include "vgm_sync.h"
#include "imgui/imgui.h"
#include "midi/midi_player.h"
#include <map>
#include <chrono>
#include <cstring>
#include <cstdio>

// Shared scroll state for DrawScrollingText
static std::map<std::string, MidiPlayer::TextScrollState> s_scrollStates;

namespace VGMPlayerUI {

void DrawScrollingText(const char* id, const char* text, uint32_t col, float maxWidth) {
    ImVec2 textSize = ImGui::CalcTextSize(text);
    float availWidth = (maxWidth > 0.0f) ? maxWidth : ImGui::GetContentRegionAvail().x;
    bool needsScrolling = textSize.x > availWidth;

    if (needsScrolling) {
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        ImGui::Dummy(ImVec2(availWidth, lineH));

        auto& st = s_scrollStates[id];
        if (st.lastUpdateTime.time_since_epoch().count() == 0) {
            st.scrollOffset = 0.0f; st.scrollDirection = 1.0f;
            st.pauseTimer = 1.0f;
            st.lastUpdateTime = std::chrono::steady_clock::now();
        }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - st.lastUpdateTime).count();
        st.lastUpdateTime = now;
        if (st.pauseTimer > 0.0f) st.pauseTimer -= dt;
        else {
            float scrollSpeed = 30.0f;
            st.scrollOffset += st.scrollDirection * scrollSpeed * dt;
            float maxScroll = textSize.x - availWidth + 20.0f;
            if (st.scrollOffset >= maxScroll) { st.scrollOffset = maxScroll; st.scrollDirection = -1.0f; st.pauseTimer = 1.0f; }
            else if (st.scrollOffset <= 0.0f) { st.scrollOffset = 0.0f; st.scrollDirection = 1.0f; st.pauseTimer = 1.0f; }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + lineH), true);
        dl->AddText(ImVec2(cursorPos.x - st.scrollOffset, cursorPos.y), col, text);
        dl->PopClipRect();
    } else {
        ImGui::TextColored(ImVec4(
            ((col >>  0) & 0xFF) / 255.0f,
            ((col >>  8) & 0xFF) / 255.0f,
            ((col >> 16) & 0xFF) / 255.0f,
            ((col >> 24) & 0xFF) / 255.0f), "%s", text);
        s_scrollStates.erase(id);
    }
}

void RenderPlayerSettingsPopup(const VGMPlayerCallbacks& cb) {
    if (!*cb.showPlayerSettings) return;
    char winId[128];
    snprintf(winId, sizeof(winId), "Player Settings%s_settings", cb.idSuffix);
    ImGui::SetNextWindowSize(ImVec2(320, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(winId, cb.showPlayerSettings)) { ImGui::End(); return; }

    float fadeout = VGMSync::GetFadeout();
    ImGui::TextDisabled("Loop Fadeout");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fade out volume before final loop end\n0 = disabled");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    char fadeId[64];
    snprintf(fadeId, sizeof(fadeId), "%s_fadeout", cb.idSuffix);
    if (ImGui::DragFloat(fadeId, &fadeout, 0.1f, 0.0f, 30.0f, "%.1f sec")) {
        if (fadeout < 0.0f) fadeout = 0.0f;
        VGMSync::SetFadeout(fadeout);
    }

    ImGui::End();
}

void RenderPlayerBar(const VGMPlayerCallbacks& cb) {
    bool hasFile = *cb.vgmLoaded;
    bool isPlaying = hasFile && *cb.vgmPlaying && !*cb.vgmPaused;
    bool isPaused = hasFile && *cb.vgmPlaying && *cb.vgmPaused;
    int maxL = VGMSync::GetMaxLoops();

    // Collapsible header
    char headerId[128];
    snprintf(headerId, sizeof(headerId), "%s%s_vgmplayer", cb.playerTitle, cb.idSuffix);
    ImGui::SetNextItemAllowOverlap();
    bool playerOpen = ImGui::TreeNodeEx(headerId,
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        (*cb.collapsed ? 0 : ImGuiTreeNodeFlags_DefaultOpen));
    if (playerOpen) *cb.collapsed = false;
    else *cb.collapsed = true;

    // Progress bar when collapsed
    if (!playerOpen && hasFile && *cb.totalSamples > 0) {
        double posSec = (double)*cb.currentSamples / 44100.0;
        double durSec;
        if (*cb.loopSamples > 0 && maxL > 0) {
            durSec = (double)(*cb.totalSamples - *cb.loopSamples) / 44100.0
                   + (double)*cb.loopSamples / 44100.0 * maxL;
        } else {
            durSec = (double)*cb.totalSamples / 44100.0;
        }
        float progress = (durSec > 0.0) ? (float)(posSec / durSec) : 0.0f;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        ImVec2 rectMin = ImGui::GetItemRectMin();
        ImVec2 rectMax = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(rectMin, rectMax, true);
        float fillW = (rectMax.x - rectMin.x) * progress;
        dl->AddRectFilled(rectMin, ImVec2(rectMin.x + fillW, rectMax.y), IM_COL32(100, 180, 255, 50));
        dl->PopClipRect();
    }

    // Title bar mini controls
    char buf[64];
    ImGui::SameLine(0, 12);
    snprintf(buf, sizeof(buf), "<<%s_miniprev", cb.idSuffix);
    if (ImGui::SmallButton(buf)) {
        if (!cb.playlist->empty()) cb.playPrev();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous");
    ImGui::SameLine();
    snprintf(buf, sizeof(buf), "||%s_minipause", cb.idSuffix);
    if (isPlaying) {
        if (ImGui::SmallButton(buf)) cb.pausePlayback();
    } else {
        snprintf(buf, sizeof(buf), ">%s_minipause", cb.idSuffix);
        if (isPaused) {
            if (ImGui::SmallButton(buf)) cb.pausePlayback();
        } else if (hasFile) {
            if (ImGui::SmallButton(buf)) cb.startPlayback();
        } else {
            ImGui::SmallButton(buf);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play / Pause");
    ImGui::SameLine();
    snprintf(buf, sizeof(buf), ">>%s_mininext", cb.idSuffix);
    if (ImGui::SmallButton(buf)) {
        if (!cb.playlist->empty()) cb.playNext();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next");
    ImGui::SameLine();

    // Scrolling filename
    if (hasFile) {
        const char* fname = cb.vgmPath;
        const char* slash = strrchr(fname, '\\');
        if (!slash) slash = strrchr(fname, '/');
        const char* displayName = slash ? slash + 1 : fname;

        ImU32 nameCol;
        if (isPlaying) nameCol = IM_COL32(100, 255, 100, 255);
        else if (isPaused) nameCol = IM_COL32(255, 255, 100, 255);
        else nameCol = IM_COL32(220, 220, 220, 255);

        float maxNameW = ImGui::CalcTextSize("ABCDEFGHIJKLMNOPQRSTUVWXABCDEFGHIJKLMNOP").x;
        snprintf(buf, sizeof(buf), "%s_minifilename", cb.idSuffix);
        DrawScrollingText(buf, displayName, nameCol, maxNameW);
    } else {
        ImGui::TextDisabled("(no file)");
    }

    // Time display when collapsed
    if (!playerOpen && hasFile && *cb.totalSamples > 0) {
        double posSec = (double)*cb.currentSamples / 44100.0;
        double durSec;
        if (*cb.loopSamples > 0 && maxL > 0) {
            durSec = (double)(*cb.totalSamples - *cb.loopSamples) / 44100.0
                   + (double)*cb.loopSamples / 44100.0 * maxL;
        } else {
            durSec = (double)*cb.totalSamples / 44100.0;
        }
        int curMin = (int)posSec / 60; int curSecI = (int)posSec % 60;
        int totMin = (int)durSec / 60; int totSecI = (int)durSec % 60;
        char timeStr[64];
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d / %02d:%02d", curMin, curSecI, totMin, totSecI);
        float timeW = ImGui::CalcTextSize(timeStr).x;
        float contentRight = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        ImGui::SameLine(contentRight - timeW);
        ImGui::Text("%s", timeStr);
    }

    if (playerOpen) {
        // Expanded controls
        float buttonWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 3.0f;
        snprintf(buf, sizeof(buf), "Play%s_play", cb.idSuffix);
        if (ImGui::Button(buf, ImVec2(buttonWidth, 30))) {
            if (!hasFile) { /* nothing */ }
            else if (isPaused) cb.pausePlayback();
            else { cb.seekToStart(); cb.startPlayback(); }
        }
        ImGui::SameLine();
        snprintf(buf, sizeof(buf), "Pause%s_pause", cb.idSuffix);
        if (ImGui::Button(buf, ImVec2(buttonWidth, 30))) {
            if (isPlaying) cb.pausePlayback();
        }
        ImGui::SameLine();
        snprintf(buf, sizeof(buf), "Stop%s_stop", cb.idSuffix);
        if (ImGui::Button(buf, ImVec2(buttonWidth, 30))) {
            cb.stopPlayback();
        }

        float navWidth = (ImGui::GetContentRegionAvail().x - 10.0f) / 2.0f;
        snprintf(buf, sizeof(buf), "<< Prev%s_prev", cb.idSuffix);
        if (ImGui::Button(buf, ImVec2(navWidth, 25))) {
            if (!cb.playlist->empty()) cb.playPrev();
        }
        ImGui::SameLine();
        snprintf(buf, sizeof(buf), "Next >>%s_next", cb.idSuffix);
        if (ImGui::Button(buf, ImVec2(navWidth, 25))) {
            if (!cb.playlist->empty()) cb.playNext();
        }

        snprintf(buf, sizeof(buf), "Auto-play%s", cb.idSuffix);
        ImGui::Checkbox(buf, cb.autoPlayNext);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto-play next track when current finishes");
        ImGui::SameLine();
        const char* modeText = *cb.sequentialPlayback ? "Seq" : "Rnd";
        snprintf(buf, sizeof(buf), "%s_mode", cb.idSuffix);
        if (ImGui::Button(modeText, ImVec2(35, 0))) *cb.sequentialPlayback = !*cb.sequentialPlayback;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sequential / Random");
        ImGui::SameLine();
        snprintf(buf, sizeof(buf), "Open%s_open", cb.idSuffix);
        if (ImGui::Button(buf)) cb.openFileDialog();
        ImGui::SameLine();
        snprintf(buf, sizeof(buf), "Settings%s_settingsbtn", cb.idSuffix);
        if (ImGui::Button(buf)) *cb.showPlayerSettings = true;

        // Progress bar with seek
        if (hasFile && *cb.totalSamples > 0) {
            double posSec = (double)*cb.currentSamples / 44100.0;
            double durSec;
            if (*cb.loopSamples > 0 && maxL > 0) {
                double introSec = (double)(*cb.totalSamples - *cb.loopSamples) / 44100.0;
                double loopSec = (double)*cb.loopSamples / 44100.0;
                durSec = introSec + loopSec * maxL;
            } else {
                durSec = (double)*cb.totalSamples / 44100.0;
            }
            float progress = (float)(posSec / durSec);
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            int curMin = (int)posSec / 60; int curSecI = (int)posSec % 60;
            int totMin = (int)durSec / 60; int totSecI = (int)durSec % 60;
            char posStr[32], durStr[32];
            snprintf(posStr, sizeof(posStr), "%02d:%02d", curMin, curSecI);
            snprintf(durStr, sizeof(durStr), "%02d:%02d", totMin, totSecI);
            ImGui::Text("%s / %s", posStr, durStr);
            ImGui::SameLine();
            ImGui::TextDisabled("L:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            char loopId[64];
            snprintf(loopId, sizeof(loopId), "%s_maxloops", cb.idSuffix);
            if (ImGui::InputInt(loopId, &maxL, 1, 5)) {
                if (maxL < 1) maxL = 1;
                VGMSync::SetMaxLoops(maxL);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Loop count (requires reload)");
            ImGui::SameLine();
            char loopChkId[64];
            snprintf(loopChkId, sizeof(loopChkId), "%s_loopen", cb.idSuffix);
            if (cb.vgmLoopEnabled) {
                ImGui::Checkbox(loopChkId, cb.vgmLoopEnabled);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable loop (requires reload)");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("F:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(55);
            char fadeId[64];
            snprintf(fadeId, sizeof(fadeId), "%s_fadeout", cb.idSuffix);
            float fadeout = VGMSync::GetFadeout();
            if (ImGui::DragFloat(fadeId, &fadeout, 0.1f, 0.0f, 30.0f, "%.1fs")) {
                if (fadeout < 0.0f) fadeout = 0.0f;
                VGMSync::SetFadeout(fadeout);
            }

            static float seek_progress = 0.0f;
            snprintf(buf, sizeof(buf), "%s_seek", cb.idSuffix);
            ImGui::SliderFloat(buf, &seek_progress, 0.0f, 1.0f, "");
            if (ImGui::IsItemActive()) {
                // Dragging
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
                double totalDurSec;
                if (*cb.loopSamples > 0 && maxL > 0) {
                    totalDurSec = (double)(*cb.totalSamples - *cb.loopSamples) / 44100.0
                                + (double)*cb.loopSamples / 44100.0 * maxL;
                } else {
                    totalDurSec = (double)*cb.totalSamples / 44100.0;
                }
                uint32_t targetSample = (uint32_t)(seek_progress * totalDurSec * 44100.0);
                if (cb.seekToPosition) cb.seekToPosition(targetSample);
            } else {
                seek_progress = progress;
            }
        } else {
            ImGui::ProgressBar(0.0f, ImVec2(-1, 20), "");
        }

        // Status text
        if (hasFile) {
            const char* fname = cb.vgmPath;
            const char* slash = strrchr(fname, '\\');
            if (!slash) slash = strrchr(fname, '/');
            if (isPlaying) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Playing:");
                ImGui::SameLine();
                ImGui::Text("%s", slash ? slash + 1 : fname);
            } else if (isPaused) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused:");
                ImGui::SameLine();
                ImGui::Text("%s", slash ? slash + 1 : fname);
            } else {
                ImGui::TextDisabled("Ready:");
                ImGui::SameLine();
                ImGui::Text("%s", slash ? slash + 1 : fname);
            }
        }
    }
}

} // namespace VGMPlayerUI
