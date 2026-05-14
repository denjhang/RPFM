// vgm_player_ui.h — VGM player UI for RPFM Player
// Copied from DMPlayer's vgm_player_ui.h to match VGMPlayerCallbacks struct layout

#ifndef VGM_PLAYER_UI_H
#define VGM_PLAYER_UI_H

#include <stdint.h>
#include <string>
#include <vector>

struct VGMPlayerCallbacks {
    const char* idSuffix;
    const char* playerTitle;
    bool* collapsed;
    const char* vgmPath;
    bool* vgmLoaded;
    bool* vgmPlaying;
    bool* vgmPaused;
    uint32_t* totalSamples;
    uint32_t* currentSamples;
    uint32_t* loopSamples;
    int* loopCount;
    bool* autoPlayNext;
    bool* sequentialPlayback;
    std::vector<std::string>* playlist;
    bool* showPlayerSettings;
    bool* vgmLoopEnabled;
    void (*startPlayback)();
    void (*stopPlayback)();
    void (*pausePlayback)();
    void (*seekToStart)();
    void (*seekToPosition)(uint32_t targetSample);
    void (*playNext)();
    void (*playPrev)();
    void (*openFileDialog)();
};

namespace VGMPlayerUI {

void RenderPlayerBar(const VGMPlayerCallbacks& cb);
void RenderPlayerSettingsPopup(const VGMPlayerCallbacks& cb);
void DrawScrollingText(const char* id, const char* text, uint32_t col = 0xFFFFFFFF, float maxWidth = 0.0f);

}

#endif
