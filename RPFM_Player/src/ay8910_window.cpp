// ay8910_window.cpp - AY8910 (PSG) Hardware Control Window
// Controls real AY8910 chip via RPFM HID interface
// VGM hardware playback + debug test functions + file browser

#include "ay8910_window.h"
#include "core/chip_descriptions.h"
#include "windows/sn76489/spfm.h"
#include "windows/ym2413/ym2413_window.h"
#include "windows/sn76489/sn76489_window.h"
#include "windows/ym2163/chip_control.h"
#include "windows/ym2163/chip_window_ym2163.h"
#include "windows/spfm/spfm_manager.h"
#include "rpfm_hid.h"
#include "rpfm_protocol.h"
#include "midi/midi_player.h"
#include "core/vgm_sync.h"
#include "core/vgm_player_ui.h"
#include "core/modizer_viz.h"
#include "libvgm-modizer/emu/cores/ModizerVoicesData.h"
#include "imgui/imgui.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <atomic>
#include <string>
#include <math.h>
#include <commdlg.h>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <chrono>
#include <zlib.h>

namespace AY8910Window {

// ============ Constants ============
static const int AY_SAMPLE_RATE = 44100;
static double s_ay8910Clock = 1789773.0;

// Clock frequency selection
static int s_clockSelect = 0;
static double s_customClock = 1789773.0;
struct ClockEntry { const char* name; double hz; };
static const ClockEntry kClocks[] = {
    {"MSX (NTSC) - 1.789 MHz",   1789773.0},
    {"ZX Spectrum - 1.773 MHz",   1773400.0},
    {"Pentagon 128K - 1.750 MHz", 1750000.0},
    {"Atari ST - 2.000 MHz",      2000000.0},
    {"Custom",                     0.0},
};
static const int kClockCount = sizeof(kClocks) / sizeof(kClocks[0]);

// 3 tone + 1 noise + 1 envelope = 5 channels per chip, × 2 chips = 10
static const int AY_CH_PER_CHIP = 5;
static const int AY_NUM_CHANNELS = 10;

// Channel names
static const char* kChNames[10] = {
    "ChA", "ChB", "ChC", "Noise", "Env",
    "2ChA", "2ChB", "2ChC", "2Noise", "2Env"
};

// Note names for display
static const char* kNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// Envelope shape names
static const char* kEnvShapeNames[16] = {
    "\\\\_____", "/\\_\\_\\_\\_", "\\\\\\___", "\\/\\/\\\\/",
    "_______", "\\\\\\____", "\\/\\___", "Hold\\\\Alt",
    "Continu", "Attack2", "Alt\\\\Alt", "Hold\\\\Hld",
    "\\\\____/\\_", "/\\_\\\\___", "Hold\\\\Cont", "\\/\\\\___\\\\"
};

// 10 channel colors (5 per chip, chip1 uses different hues like SN76489)
static ImU32 kChColors[10] = {
    IM_COL32(160, 200, 160, 255), // ChA: green
    IM_COL32(160, 160, 220, 255), // ChB: blue
    IM_COL32(220, 160, 160, 255), // ChC: red
    IM_COL32(160, 160, 160, 255), // Noise: gray
    IM_COL32(255, 160,  60, 255), // Env: orange
    IM_COL32(200, 160, 200, 255), // 2ChA: purple
    IM_COL32(200, 200, 160, 255), // 2ChB: yellow
    IM_COL32(160, 200, 200, 255), // 2ChC: cyan
    IM_COL32(180, 140, 140, 255), // 2Noise: rose
    IM_COL32(200, 140, 100, 255), // 2Env: amber
};

// Custom channel colors (0 = use default)
static ImU32 kChColorsCustom[10] = {};

// ============ Connection State ============
static bool s_connected = false;
static int s_localSlots[VGMSync::MAX_SLOTS] = {VGMSync::CHIP_NONE};

static inline bool IsConnected() { return s_connected; }

// ============ Test State ============
static bool s_testRunning = false;
static int s_testType = 0;
static int s_testStep = 0;
static double s_testStepMs = 0.0;
static LARGE_INTEGER s_testStartTime;
static LARGE_INTEGER s_perfFreq;

// ============ VGM Playback State ============
static FILE* s_vgmFile = nullptr;
static char s_vgmPath[MAX_PATH] = "";
static bool s_vgmLoaded = false;
static bool s_vgmPlaying = false;
static HANDLE s_vgmThread = nullptr;
static volatile bool s_vgmThreadRunning = false;
static bool s_vgmPaused = false;
static bool s_vgmTrackEnded = false;
static UINT32 s_vgmTotalSamples = 0;
static UINT32 s_vgmCurrentSamples = 0;
static uint32_t s_fwTick = 0;  // firmware's current playback tick (from HID response)
static UINT32 s_vgmDataOffset = 0;

// Visualization thread: independent local VGM playback for responsive shadow register updates
static HANDLE s_vizThread = nullptr;
static volatile bool s_vizRunning = false;

static UINT32 s_vgmLoopOffset = 0;
static UINT32 s_vgmLoopSamples = 0;

// VGZ support: memory buffer for decompressed data
static std::vector<UINT8> s_memData;
static size_t s_memPos = 0;

static size_t vgmfread(void* buf, size_t sz, size_t cnt, FILE* f) {
    if (!s_memData.empty()) {
        size_t total = sz * cnt;
        if (s_memPos + total > s_memData.size()) {
            size_t avail = s_memData.size() - s_memPos;
            if (avail < sz) return 0;
            cnt = avail / sz;
            total = cnt * sz;
        }
        memcpy(buf, &s_memData[s_memPos], total);
        s_memPos += total;
        return cnt;
    }
    return fread(buf, sz, cnt, f);
}

static int vgmfseek(FILE* f, long off, int whence) {
    if (!s_memData.empty()) {
        switch (whence) {
            case SEEK_SET: s_memPos = (size_t)off; break;
            case SEEK_CUR: s_memPos += off; break;
            case SEEK_END: s_memPos = s_memData.size() + off; break;
        }
        if (s_memPos > s_memData.size()) s_memPos = s_memData.size();
        return 0;
    }
    return fseek(f, off, whence);
}

static std::vector<UINT8> InflateGzip(const UINT8* data, size_t size) {
    std::vector<UINT8> out;
    z_stream strm = {};
    strm.avail_in = (uInt)size;
    strm.next_in = (Bytef*)data;
    if (inflateInit2(&strm, 15 + 32) != Z_OK) return out;
    UINT8 chunk[16384];
    int ret;
    do {
        strm.avail_out = sizeof(chunk);
        strm.next_out = chunk;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&strm); return std::vector<UINT8>(); }
        out.insert(out.end(), chunk, chunk + sizeof(chunk) - strm.avail_out);
    } while (strm.avail_out == 0);
    inflateEnd(&strm);
    return out;
}
static int s_vgmLoopCount = 0;
static int s_vizLoopCount = 0;
static int s_vgmMaxLoops = 2;
static bool s_vgmLoopEnabled = true;       // true=展开循环播放完整长度, false=播到0x66停止
static volatile size_t s_vgmSeekPos = 0;   // seek后线程从该位置开始读取

// Playback Mode: 0=Live (real-time register writes), 1=Buffered (stream raw VGM to firmware)
// s_playbackMode declared with s_ayDelayNs near line 190
static uint16_t s_bufLevel = 0;
static uint32_t s_bufTotal = 2048;
static uint32_t s_streamSent = 0;
static uint32_t s_streamTotal = 0;
static int s_bufTargetKB = 0;  // 0=512B (default), 1=1KB, ..., 5=8KB
static const uint32_t kBufSizes[] = {512, 1024, 2048, 3072, 4096, 8192};
static const int kBufSizeCount = 6;
static HANDLE s_vgmStreamThread = nullptr;
static volatile bool s_vgmStreamRunning = false;

// Flush Mode & Timer Mode
static int s_flushMode = 2;   // 1=Register-Level, 2=Command-Level (default)
static int s_timerMode = 0;   // 0=H-Prec, 1=Hybrid, 2=MM-Timer, 3=VGMPlay, 7=OptVGMPlay
static int s_writeProtocol = 0;  // 0=Original (spfm_write_reg), 1=Fixed (spfm_write_raw path B)
static int s_flushThreshold = 4; // min wait samples to trigger mid-batch flush (1-735)
static int s_ayDelayNs = 1000;  // AY8910 PIO write delay (ns, 0-2000)
static int s_playbackMode = 0;  // 0=Live, 1=Buffered

// Seek & Fadeout
static int s_seekMode = 0;
static float s_fadeoutDuration = 3.0f;
static bool s_fadeoutActive = false;
static float s_fadeoutLevel = 1.0f;
static UINT32 s_fadeoutStartSample = 0;
static UINT32 s_fadeoutEndSample = 0;

// Channel mute/solo (10 channels: 5 per chip)
static bool s_chMuted[AY_NUM_CHANNELS] = {};
static int s_soloCh = -1;

// GD3 tags
static std::string s_trackName, s_gameName, s_systemName, s_artistName;
static UINT32 s_vgmVersion = 0;

// ============ AY8910 Register Shadow ============
static uint8_t s_regShadow[0x10] = {};

// Individual channel state for UI display (extracted from shadow)
static uint8_t s_toneFine[3] = {};     // Reg 0x00, 0x02, 0x04
static uint8_t s_toneCoarse[3] = {};   // Reg 0x01, 0x03, 0x05
static uint8_t s_noisePeriod = 0;     // Reg 0x06
static uint8_t s_mixer = 0;           // Reg 0x07
static uint8_t s_vol[3] = {};         // Reg 0x08, 0x09, 0x0A
static uint8_t s_envFine = 0;         // Reg 0x0B
static uint8_t s_envCoarse = 0;       // Reg 0x0C
static uint8_t s_envShape = 0;        // Reg 0x0D

// Tone/noise active state (from mixer reg, 0=on, 1=off)
static bool s_toneOn[3] = {};
static bool s_noiseOn[3] = {};
static bool s_chEnvOnly[3] = {};  // tone+noise both off but env mode (bit4) on
static bool s_chDacMode[3] = {};  // tone+noise both off but vol > 0 (DAC mode)

// ============ 2nd AY8910 Register Shadow ============
static uint8_t s2_toneFine[3] = {};
static uint8_t s2_toneCoarse[3] = {};
static uint8_t s2_noisePeriod = 0;
static uint8_t s2_mixer = 0;
static uint8_t s2_vol[3] = {};
static uint8_t s2_envFine = 0;
static uint8_t s2_envCoarse = 0;
static uint8_t s2_envShape = 0;
static bool s2_toneOn[3] = {};
static bool s2_noiseOn[3] = {};
static bool s2_chEnvOnly[3] = {};
static bool s2_chDacMode[3] = {};

// ============ Piano State ============
static const int AY_PIANO_LOW = 24;   // C1
static const int AY_PIANO_HIGH = 107; // B7
static const int AY_PIANO_KEYS = AY_PIANO_HIGH - AY_PIANO_LOW + 1;
static bool s_pianoKeyOn[AY_PIANO_KEYS] = {};
static float s_pianoKeyLevel[AY_PIANO_KEYS] = {};
static int s_pianoKeyChannel[AY_PIANO_KEYS] = {};
// Multi-channel per key: support vertical color splitting
static int s_pianoKeyChannels[AY_PIANO_KEYS][AY_NUM_CHANNELS]; // active channels per key
static float s_pianoKeyChLevels[AY_PIANO_KEYS][AY_NUM_CHANNELS]; // level per channel per key
static int s_pianoKeyChCount[AY_PIANO_KEYS]; // number of active channels per key
static float s_pitchOffset[AY_PIANO_KEYS] = {}; // portamento offset per key
static const bool s_isBlackNote[12] = {false, true, false, true, false, false, true, false, true, false, true, false};

// ============ Level Meter State ============
static float s_channelLevel[AY_NUM_CHANNELS] = {}; // 10 channels (5 per chip)

// Tone on/off edge detection (from mixer register)
static uint8_t s_prevMixer = 0;
static float s_chDecay[3] = {};         // per-channel decay envelope (tone)
static bool  s_chKeyOff[3] = {};        // key-off state per tone channel

// Noise on/off edge detection
static float s_noiseDecay = {};         // noise decay

// Envelope channel decay
static float s_envDecay = {};           // envelope channel decay

// Key-on edge detection for portamento fix
static bool s_chKeyOnEdge[3] = {};

// Portamento tracking via continuous frequency (smoothed poff)
static double s_chFreq[3] = {};        // chip0 current actual frequency
static double s_chFreqAnchor[3] = {};  // chip0 anchor frequency
static float s_chSmoothPoff[3] = {};   // chip0 smoothed pitch offset
static float s_chPrevRawPoff[3] = {};  // chip0 previous raw poff (for change detection)
static double s2_chFreq[3] = {};       // chip1
static double s2_chFreqAnchor[3] = {}; // chip1
static float s2_chSmoothPoff[3] = {};  // chip1 smoothed pitch offset
static float s2_chPrevRawPoff[3] = {}; // chip1 previous raw poff

// Portamento indicator toggle and threshold
static bool s_showPortamento = true;
static float s_portamentoThreshold = 0.05f;
static bool s_showMicrotonal = false;  // show static microtonal offsets

// ============ 2nd AY8910 UI State ============
static float s2_chDecay[3] = {};
static bool  s2_chKeyOff[3] = {};
static float s2_noiseDecay = {};
static float s2_envDecay = {};
static float s2_channelLevel[AY_CH_PER_CHIP] = {};
static bool  s2_chKeyOnEdge[3] = {};

// ============ Scope State ============
static ModizerViz s_scope;
static bool s_showScope = false;
static float s_scopeHeight = 80.0f;
static int s_voiceCh[AY_NUM_CHANNELS] = {}; // 10 channels for scope
static int s_scopeSamples = 441;
static float s_scopeAmplitude = 3.0f;

// ============ Log ============
static std::string s_log;
static char s_logDisplay[65536] = "";
static bool s_logAutoScroll = true;
static bool s_logScrollToBottom = false;
static size_t s_logLastSize = 0;

// ============ File Browser State ============
static char s_currentPath[MAX_PATH] = "";
static char s_pathInput[MAX_PATH] = "";
static bool s_pathEditMode = false;
static bool s_pathEditModeJustActivated = false;
static std::vector<std::string> s_navHistory;
static int s_navPos = -1;
static bool s_navigating = false;
static std::vector<std::string> s_folderHistory;
static std::vector<MidiPlayer::FileEntry> s_fileList;
static int s_selectedFileIndex = -1;
static std::string s_currentPlayingFilePath;
static std::map<std::string, float> s_pathScrollPositions;
static char s_histFilter[128] = "";
static char s_fileBrowserFilter[256] = "";
static int s_histSortMode = 0;
static std::vector<std::string> s_playlist;
static int s_playlistIndex = -1;
static bool s_autoPlayNext = true;
static bool s_isSequentialPlayback = true;

// ============ UI Collapse State ============
static bool s_ymPlayerCollapsed = false;
// History always expanded (no collapse persistence)
static bool s_showTestPopup = false;
static bool s_showPlayerSettings = false;

// ============ Config ============
static char s_configPath[MAX_PATH] = "";

// ============ Forward Declarations ============
static void ay8910_mute_all(void);
void ay8910_write_reg(uint8_t chipID, uint16_t reg, uint8_t data);
static void InitHardware(void);
static void ApplyShadowState(void);
static void ResetState(void);
static void StopTest(void);
static void LoadConfig(void);
static void SaveConfig(void);
static void GetExeDir(char* buf, int bufSize);
static void AddToFolderHistory(const char* path);
static void RefreshFileList(void);
static void NavigateTo(const char* rawPath);
static void NavBack(void);
static void NavForward(void);
static void NavToParent(void);
static void UpdateChannelLevels(void);
static void SeekVGM(uint32_t targetSample);
static void key_off_all(void);
static void RenderPianoKeyboard(void);
static void RenderLevelMeters(void);
static void RenderScopeArea(void);
static void RenderTestPopup(void);
static void RenderSidebar(void);
static VGMPlayerCallbacks GetPlayerCallbacks(void);
static void RenderPlayerBar(void);
static void RenderRegisterTable(void);
static void RenderFileBrowser(void);
static void RenderLogPanel(void);
static void RenderMain(void);

// ============ Log ============
static void DcLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_log += buf;
    if (s_log.size() > 64000) s_log = s_log.substr(s_log.size() - 32000);
    s_logScrollToBottom = true;
}

// ============ Hardware Helpers ============
static FILE* ym_fopen(const char* path, const char* mode) {
    std::wstring wPath = MidiPlayer::UTF8ToWide(std::string(path));
    std::wstring wMode = MidiPlayer::UTF8ToWide(std::string(mode));
    return _wfopen(wPath.c_str(), wMode.c_str());
}

static int s_wrLogCount = 0;
void ay8910_write_reg(uint8_t chipID, uint16_t reg, uint8_t data) {
    // Find the Nth AY8910 slot (chipID=0 → first, chipID=1 → second)
    int slot = -1;
    int found = 0;
    for (int i = 0; i < VGMSync::MAX_SLOTS; i++) {
        if (VGMSync::GetSlotChip(i) == VGMSync::CHIP_AY8910) {
            if (found == chipID) { slot = i; break; }
            found++;
        }
    }
    if (slot < 0) { if (s_wrLogCount < 20) { DcLog("[AY] write_reg: chipID=%d reg=0x%02X NO SLOT\n", chipID, reg); s_wrLogCount++; } return; }
    if (s_wrLogCount < 20) { DcLog("[AY] write_reg: chipID=%d slot=%d reg=0x%02X data=0x%02X\n", chipID, slot, reg, data); s_wrLogCount++; }
    if (s_writeProtocol == 0) {
        ::spfm_write_reg(slot, 0, reg, data);
    } else {
        // Experiment: direct raw write path B
        uint8_t cmd[6];
        cmd[0] = (uint8_t)slot; cmd[1] = 0x80; cmd[2] = reg;      // A0=0, write address
        cmd[3] = (uint8_t)slot; cmd[4] = 0x81; cmd[5] = data;     // A0=1, write data
        SPFMManager::RawWrite(cmd, 6);
    }
    if (s_flushMode == 1) {
        ::spfm_hw_wait(1);
    }
}

void safe_flush(void) {
    ::spfm_flush();
}

// Update AY8910 shadow state and UI state for a register write
// Returns the (possibly modified) data byte after fadeout/mute interception
UINT8 UpdateAY8910State(UINT16 reg, UINT8 data) {
    if (reg < 0x10) s_regShadow[reg] = data;

    if (reg <= 0x05) {
        // Tone period registers
        int ch = reg / 2;
        if (reg % 2 == 0) s_toneFine[ch] = data;
        else s_toneCoarse[ch] = data;
        // Update frequency for portamento detection
        { int p = (s_toneCoarse[ch] << 8) | s_toneFine[ch];
          s_chFreq[ch] = (p > 0) ? s_ay8910Clock / (16.0 * p) : 0.0; }
    } else if (reg == 0x06) {
        s_noisePeriod = data;
    } else if (reg == 0x07) {
        s_mixer = data;
        // Update tone/noise enable state (0=on, 1=off)
        for (int i = 0; i < 3; i++) {
            s_toneOn[i] = !(data & (1 << i));
            s_noiseOn[i] = !(data & (1 << (i + 3)));
        }
        // Key-on: volume > 0 regardless of tone/noise state
        for (int i = 0; i < 3; i++) {
            bool new_on = (s_vol[i] & 0x1F) > 0;
            if (new_on) {
                if (s_chKeyOff[i]) s_chKeyOnEdge[i] = true; // rising edge
                s_chDecay[i] = 1.0f; s_chKeyOff[i] = false;
            }
            else { s_chKeyOff[i] = true; }
        }
        // Noise key-on
        bool anyNoise = false;
        for (int i = 0; i < 3; i++) if (s_noiseOn[i] && (s_vol[i] & 0x0F) > 0) anyNoise = true;
        if (anyNoise) s_noiseDecay = 1.0f;
    } else if (reg >= 0x08 && reg <= 0x0A) {
        int ch = reg - 0x08;
        s_vol[ch] = data;
        // Key-on: volume > 0 regardless of tone/noise state
        bool new_on = (data & 0x1F) > 0;
        if (new_on) {
            if (s_chKeyOff[ch]) s_chKeyOnEdge[ch] = true; // rising edge
            s_chDecay[ch] = 1.0f; s_chKeyOff[ch] = false;
        }
        else { s_chKeyOff[ch] = true; }
        // Noise key-on for this channel
        if (s_noiseOn[ch] && (data & 0x0F) > 0) s_noiseDecay = 1.0f;
    } else if (reg == 0x0B) {
        s_envFine = data;
        // Trigger envelope decay when env period changes and any channel uses env
        for (int i = 0; i < 3; i++) {
            if (s_vol[i] & 0x10) { s_envDecay = 1.0f; break; }
        }
    } else if (reg == 0x0C) {
        s_envCoarse = data;
        for (int i = 0; i < 3; i++) {
            if (s_vol[i] & 0x10) { s_envDecay = 1.0f; break; }
        }
    } else if (reg == 0x0D) {
        s_envShape = data;
        s_envDecay = 1.0f; // shape change retriggers envelope
    }

    // Fadeout: intercept volume writes
    if (reg >= 0x08 && reg <= 0x0A && s_fadeoutActive && s_fadeoutLevel < 1.0f) {
        uint8_t origVol = data & 0x0F;
        uint8_t fadedVol = (uint8_t)(origVol * s_fadeoutLevel);
        data = (data & 0xF0) | (fadedVol & 0x0F);
        s_regShadow[reg] = data;
        s_vol[reg - 0x08] = data;
    }

    // Channel mute: intercept volume writes (MDPlayer: dData = mask ? 0 : dData)
    if (reg >= 0x08 && reg <= 0x0A) {
        int ch = reg - 0x08;
        if (s_chMuted[ch]) data = 0;
        if (s_chMuted[4]) data &= ~0x10;  // envelope mute
    }

    // Channel mute: intercept mixer writes (MDPlayer: maskData |= 0x9 << ch)
    if (reg == 0x07) {
        for (int ch = 0; ch < 3; ch++) {
            if (s_chMuted[ch]) data |= (0x09 << ch);
        }
        // Noise mute: disable all noise bits
        if (s_chMuted[3]) data |= 0x38;
    }

    // Envelope mute: disable envelope bit4 on all volume registers
    if (reg >= 0x08 && reg <= 0x0A) {
        if (s_chMuted[4]) data &= ~0x10;
    }

    return data;
}

// Update 2nd AY8910 shadow state (no fadeout/mute interception — handled by chip0)
UINT8 UpdateAY8910State2(UINT16 reg, UINT8 data) {
    if (reg <= 0x05) {
        int ch = reg / 2;
        if (reg % 2 == 0) s2_toneFine[ch] = data;
        else s2_toneCoarse[ch] = data;
        { int p = (s2_toneCoarse[ch] << 8) | s2_toneFine[ch];
          s2_chFreq[ch] = (p > 0) ? s_ay8910Clock / (16.0 * p) : 0.0; }
    } else if (reg == 0x06) {
        s2_noisePeriod = data;
    } else if (reg == 0x07) {
        s2_mixer = data;
        // Channel mute for 2nd chip mixer
        for (int ch = 0; ch < 3; ch++) {
            if (s_chMuted[AY_CH_PER_CHIP + ch]) data |= (0x09 << ch);
        }
        if (s_chMuted[AY_CH_PER_CHIP + 3]) data |= 0x38;
        for (int i = 0; i < 3; i++) {
            s2_toneOn[i] = !(data & (1 << i));
            s2_noiseOn[i] = !(data & (1 << (i + 3)));
        }
        for (int i = 0; i < 3; i++) {
            bool new_on = (s2_vol[i] & 0x1F) > 0;
            if (new_on) {
                if (s2_chKeyOff[i]) s2_chKeyOnEdge[i] = true;
                s2_chDecay[i] = 1.0f; s2_chKeyOff[i] = false;
            }
            else { s2_chKeyOff[i] = true; }
        }
        bool anyNoise = false;
        for (int i = 0; i < 3; i++) if (s2_noiseOn[i] && (s2_vol[i] & 0x0F) > 0) anyNoise = true;
        if (anyNoise) s2_noiseDecay = 1.0f;
    } else if (reg >= 0x08 && reg <= 0x0A) {
        int ch = reg - 0x08;
        s2_vol[ch] = data;
        bool new_on = (data & 0x1F) > 0;
        if (new_on) {
            if (s2_chKeyOff[ch]) s2_chKeyOnEdge[ch] = true;
            s2_chDecay[ch] = 1.0f; s2_chKeyOff[ch] = false;
        }
        else { s2_chKeyOff[ch] = true; }
        if (s2_noiseOn[ch] && (data & 0x0F) > 0) s2_noiseDecay = 1.0f;
        // Channel mute for 2nd chip
        if (s_chMuted[AY_CH_PER_CHIP + ch]) data = 0;
        if (s_chMuted[AY_CH_PER_CHIP + 4]) data &= ~0x10;
    } else if (reg == 0x0B) {
        s2_envFine = data;
        for (int i = 0; i < 3; i++) {
            if (s2_vol[i] & 0x10) { s2_envDecay = 1.0f; break; }
        }
    } else if (reg == 0x0C) {
        s2_envCoarse = data;
        for (int i = 0; i < 3; i++) {
            if (s2_vol[i] & 0x10) { s2_envDecay = 1.0f; break; }
        }
    } else if (reg == 0x0D) {
        s2_envShape = data;
        s2_envDecay = 1.0f;
    }
    return data;
}

// ============ Timer Mode Sleep Helpers ============

static void sleep_precise_us(unsigned int usec) {
    if (usec < 100) {
        LARGE_INTEGER start, cur;
        QueryPerformanceCounter(&start);
        LONGLONG target = start.QuadPart + (s_perfFreq.QuadPart * usec) / 1000000;
        do { QueryPerformanceCounter(&cur); } while (cur.QuadPart < target);
    } else {
        HANDLE h = CreateWaitableTimer(NULL, TRUE, NULL);
        if (h) {
            LARGE_INTEGER due;
            due.QuadPart = -((LONGLONG)usec * 10);
            SetWaitableTimer(h, &due, 0, NULL, NULL, 0);
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        }
    }
}

static void sleep_hybrid_us(unsigned int usec) {
    LARGE_INTEGER start, cur;
    QueryPerformanceCounter(&start);
    LONGLONG target = start.QuadPart + (s_perfFreq.QuadPart * usec) / 1000000;
    if (usec > 1000) Sleep((usec - 1000) / 1000);
    do { QueryPerformanceCounter(&cur); } while (cur.QuadPart < target);
}

static void mm_timer_callback(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)uTimerID; (void)uMsg; (void)dwUser; (void)dw1; (void)dw2;
}

static void sleep_mm_timer_us(unsigned int usec) {
    unsigned int ms = usec / 1000;
    if (ms == 0) ms = 1;
    MMRESULT id = timeSetEvent(ms, 1, (LPTIMECALLBACK)mm_timer_callback, 0,
                               TIME_ONESHOT);
    if (id) {
        Sleep(ms + 5);
        timeKillEvent(id);
    } else {
        sleep_hybrid_us(usec);
    }
}

static void timer_sleep_1ms(void) {
    switch (s_timerMode) {
        case 1: sleep_hybrid_us(1000); break;
        case 2: sleep_mm_timer_us(1000); break;
        default: Sleep(1); break;
    }
}

static void ay8910_mute_all(void) {
    if (!s_connected) return;
    // Count AY8910 slots
    int ayCount = 0;
    for (int i = 0; i < VGMSync::MAX_SLOTS; i++) {
        if (VGMSync::GetSlotChip(i) == VGMSync::CHIP_AY8910) ayCount++;
    }
    if (ayCount == 0) ayCount = 1;
    for (int id = 0; id < ayCount; id++) {
        ay8910_write_reg(id, 0x07, 0x3F);
        for (int i = 0; i < 3; i++)
            ay8910_write_reg(id, 0x08 + i, 0x00);
        ay8910_write_reg(id, 0x06, 0x00);
        ay8910_write_reg(id, 0x0B, 0x00);
        ay8910_write_reg(id, 0x0C, 0x00);
        ay8910_write_reg(id, 0x0D, 0x00);
    }
    safe_flush();
}

void MuteAll() {
    if (!SPFMManager::IsConnected()) return;
    ay8910_mute_all();
}

static void InitHardware(void) {
    if (!s_connected) return;
    DcLog("[AY] InitHardware\n");
    ay8910_mute_all();
    // Count AY8910 slots and zero all registers
    int ayCount = 0;
    for (int i = 0; i < VGMSync::MAX_SLOTS; i++) {
        if (VGMSync::GetSlotChip(i) == VGMSync::CHIP_AY8910) ayCount++;
    }
    if (ayCount == 0) ayCount = 1;
    for (int id = 0; id < ayCount; id++) {
        for (int i = 0; i < 14; i++) {
            ay8910_write_reg(id, i, 0x00);
        }
    }
    safe_flush();
    DcLog("[AY] Init done (%d AY chip(s))\n", ayCount);
}

static void ApplyShadowState(void) {
    if (!s_connected) return;
    // Reset all chips first
    InitHardware();

    // Write chip0 shadow state (R0-R13)
    for (int i = 0; i < 14; i++) {
        ay8910_write_reg(0, i, s_regShadow[i]);
    }
    safe_flush();

    // Check if 2nd AY chip exists
    int ayCount = 0;
    for (int i = 0; i < VGMSync::MAX_SLOTS; i++) {
        if (VGMSync::GetSlotChip(i) == VGMSync::CHIP_AY8910) ayCount++;
    }
    if (ayCount >= 2) {
        // Write chip1 shadow state
        ay8910_write_reg(1, 0, s2_toneFine[0]);
        ay8910_write_reg(1, 1, s2_toneCoarse[0]);
        ay8910_write_reg(1, 2, s2_toneFine[1]);
        ay8910_write_reg(1, 3, s2_toneCoarse[1]);
        ay8910_write_reg(1, 4, s2_toneFine[2]);
        ay8910_write_reg(1, 5, s2_toneCoarse[2]);
        ay8910_write_reg(1, 6, s2_noisePeriod);
        ay8910_write_reg(1, 7, s2_mixer);
        ay8910_write_reg(1, 8, s2_vol[0]);
        ay8910_write_reg(1, 9, s2_vol[1]);
        ay8910_write_reg(1, 10, s2_vol[2]);
        ay8910_write_reg(1, 11, s2_envFine);
        ay8910_write_reg(1, 12, s2_envCoarse);
        ay8910_write_reg(1, 13, s2_envShape);
        safe_flush();
    }
}

static void ResetState(void) {
    s_testRunning = false; s_testType = 0; s_testStep = 0; s_testStepMs = 0.0;
    memset(s_regShadow, 0, sizeof(s_regShadow));
    memset(s_toneFine, 0, sizeof(s_toneFine));
    memset(s_toneCoarse, 0, sizeof(s_toneCoarse));
    s_noisePeriod = 0;
    s_mixer = 0;
    memset(s_vol, 0, sizeof(s_vol));
    s_envFine = 0;
    s_envCoarse = 0;
    s_envShape = 0;
    memset(s_toneOn, 0, sizeof(s_toneOn));
    memset(s_noiseOn, 0, sizeof(s_noiseOn));
    // 2nd chip
    memset(s2_toneFine, 0, sizeof(s2_toneFine));
    memset(s2_toneCoarse, 0, sizeof(s2_toneCoarse));
    s2_noisePeriod = 0;
    s2_mixer = 0;
    memset(s2_vol, 0, sizeof(s2_vol));
    s2_envFine = 0;
    s2_envCoarse = 0;
    s2_envShape = 0;
    memset(s2_toneOn, 0, sizeof(s2_toneOn));
    memset(s2_noiseOn, 0, sizeof(s2_noiseOn));
    // Piano
    memset(s_pianoKeyOn, 0, sizeof(s_pianoKeyOn));
    memset(s_pianoKeyLevel, 0, sizeof(s_pianoKeyLevel));
    memset(s_pianoKeyChannel, -1, sizeof(s_pianoKeyChannel));
    memset(s_channelLevel, 0, sizeof(s_channelLevel));
    memset(s_chDecay, 0, sizeof(s_chDecay));
    memset(s_chKeyOff, false, sizeof(s_chKeyOff));
    memset(s_chKeyOnEdge, false, sizeof(s_chKeyOnEdge));
    memset(s_chFreq, 0, sizeof(s_chFreq));
    memset(s_chFreqAnchor, 0, sizeof(s_chFreqAnchor));
    memset(s_chSmoothPoff, 0, sizeof(s_chSmoothPoff));
    memset(s_chPrevRawPoff, 0, sizeof(s_chPrevRawPoff));
    memset(s_chEnvOnly, false, sizeof(s_chEnvOnly));
    memset(s_chDacMode, false, sizeof(s_chDacMode));
    s_noiseDecay = 0;
    s_envDecay = 0;
    // 2nd chip UI
    memset(s2_chDecay, 0, sizeof(s2_chDecay));
    memset(s2_chKeyOff, false, sizeof(s2_chKeyOff));
    memset(s2_chKeyOnEdge, false, sizeof(s2_chKeyOnEdge));
    memset(s2_chFreq, 0, sizeof(s2_chFreq));
    memset(s2_chFreqAnchor, 0, sizeof(s2_chFreqAnchor));
    memset(s2_chSmoothPoff, 0, sizeof(s2_chSmoothPoff));
    memset(s2_chPrevRawPoff, 0, sizeof(s2_chPrevRawPoff));
    memset(s2_chEnvOnly, false, sizeof(s2_chEnvOnly));
    memset(s2_chDacMode, false, sizeof(s2_chDacMode));
    s2_noiseDecay = 0;
    s2_envDecay = 0;
    memset(s2_channelLevel, 0, sizeof(s2_channelLevel));
    s_prevMixer = 0;
    memset(s_chMuted, false, sizeof(s_chMuted));
    s_soloCh = -1;
    s_fadeoutActive = false;
    s_fadeoutLevel = 1.0f;
    s_scope.ResetOffsets();
}

// ============ Period to MIDI Note Conversion ============
static int period_to_midi_note(int ch) {
    // AY8910 tone period: 12-bit = coarse(4bit) << 8 | fine(8bit)
    int period = (s_toneCoarse[ch] << 8) | s_toneFine[ch];
    if (period == 0) return -1;

    double freq = s_ay8910Clock / (16.0 * period);
    int midiNote = (int)round(69.0 + 12.0 * log2(freq / 440.0));
    return midiNote;
}

static int period_to_midi_note2(int ch) {
    int period = (s2_toneCoarse[ch] << 8) | s2_toneFine[ch];
    if (period == 0) return -1;
    double freq = s_ay8910Clock / (16.0 * period);
    return (int)round(69.0 + 12.0 * log2(freq / 440.0));
}

// ============ Connection (managed by SPFMManager) ============
static void SyncConnectionState(void) {
    bool wasConnected = s_connected;
    s_connected = SPFMManager::IsConnected();
    if (s_connected && !wasConnected) {
        ResetState();
        InitHardware();
        DcLog("[AY] Hardware connected via SPFMManager\n");
    }
    if (!s_connected && wasConnected) {
        DcLog("[AY] Hardware disconnected\n");
    }
}

// ============ Config Persistence ============
static void GetExeDir(char* buf, int bufSize) {
    wchar_t wbuf[MAX_PATH];
    GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(wbuf, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    std::string s = MidiPlayer::WideToUTF8(std::wstring(wbuf));
    snprintf(buf, bufSize, "%s", s.c_str());
}

static void LoadConfig(void) {
    char exeDir[MAX_PATH];
    GetExeDir(exeDir, MAX_PATH);
    snprintf(s_configPath, MAX_PATH, "%s\\ay8910_config.ini", exeDir);

    char buf[MAX_PATH] = "";
    GetPrivateProfileStringA("Settings", "CurrentPath", "", buf, MAX_PATH, s_configPath);
    if (buf[0]) snprintf(s_currentPath, MAX_PATH, "%s", buf);

    s_ymPlayerCollapsed = GetPrivateProfileIntA("Settings", "PlayerCollapsed", 0, s_configPath) != 0;
    // History always expanded — removed HistoryCollapsed INI read
    s_autoPlayNext = GetPrivateProfileIntA("Settings", "AutoPlayNext", 1, s_configPath) != 0;
    s_isSequentialPlayback = GetPrivateProfileIntA("Settings", "SequentialPlayback", 1, s_configPath) != 0;
    s_showPortamento = GetPrivateProfileIntA("Settings", "ShowPortamento", 1, s_configPath) != 0;
    s_portamentoThreshold = GetPrivateProfileIntA("Settings", "PortamentoThreshold", 5, s_configPath) / 100.0f;
    s_showMicrotonal = GetPrivateProfileIntA("Settings", "ShowMicrotonal", 0, s_configPath) != 0;

    // Clock correction
    s_clockSelect = GetPrivateProfileIntA("Settings", "ClockSelect", 0, s_configPath);
    if (s_clockSelect < 0 || s_clockSelect >= kClockCount) s_clockSelect = 0;
    {
        char clkVal[32] = "";
        GetPrivateProfileStringA("Settings", "CustomClock", "1789773", clkVal, sizeof(clkVal), s_configPath);
        s_customClock = atof(clkVal);
        if (s_customClock < 1000.0) s_customClock = 1789773.0;
    }
    if (s_clockSelect < kClockCount - 1)
        s_ay8910Clock = kClocks[s_clockSelect].hz;
    else
        s_ay8910Clock = s_customClock;

    // Folder history
    s_folderHistory.clear();
    for (int i = 0; i < 200; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Folder%d", i);
        char val[MAX_PATH] = "";
        GetPrivateProfileStringA("YmFolderHistory", key, "", val, MAX_PATH, s_configPath);
        if (val[0] == '\0') break;
        s_folderHistory.push_back(std::string(val));
    }

    // Seek & fadeout
    s_seekMode = GetPrivateProfileIntA("Settings", "SeekMode", 0, s_configPath);
    if (s_seekMode < 0 || s_seekMode > 1) s_seekMode = 0;
    s_flushMode = GetPrivateProfileIntA("Settings", "FlushMode", 2, s_configPath);
    if (s_flushMode != 1 && s_flushMode != 2) s_flushMode = 2;
    s_timerMode = GetPrivateProfileIntA("Settings", "TimerMode", 0, s_configPath);
    if (s_timerMode != 0 && s_timerMode != 1 && s_timerMode != 2
        && s_timerMode != 3 && s_timerMode != 7) s_timerMode = 0;
    s_writeProtocol = GetPrivateProfileIntA("Settings", "WriteProtocol", 0, s_configPath);
    if (s_writeProtocol != 0 && s_writeProtocol != 1) s_writeProtocol = 0;
    s_flushThreshold = GetPrivateProfileIntA("Settings", "FlushThreshold", 4, s_configPath);
    if (s_flushThreshold < 1) s_flushThreshold = 1;
    if (s_flushThreshold > 735) s_flushThreshold = 735;
    VGMSync::SetFlushThreshold(s_flushThreshold);

    // PIO delay & playback mode
    s_ayDelayNs = GetPrivateProfileIntA("Settings", "AyPioDelay", 1000, s_configPath);
    if (s_ayDelayNs < 0) s_ayDelayNs = 0;
    if (s_ayDelayNs > 2000) s_ayDelayNs = 2000;
    s_playbackMode = GetPrivateProfileIntA("Settings", "PlaybackMode", 0, s_configPath);
    if (s_playbackMode < 0 || s_playbackMode > 1) s_playbackMode = 0;
    s_bufTargetKB = GetPrivateProfileIntA("Settings", "BufTargetKB", 5, s_configPath);
    if (s_bufTargetKB < 0) s_bufTargetKB = 0;
    if (s_bufTargetKB >= kBufSizeCount) s_bufTargetKB = kBufSizeCount - 1;
    s_bufTotal = kBufSizes[s_bufTargetKB];
    {
        char val[32] = "";
        GetPrivateProfileStringA("Settings", "FadeoutDuration", "3.0", val, sizeof(val), s_configPath);
        s_fadeoutDuration = (float)atof(val);
        if (s_fadeoutDuration < 0.0f) s_fadeoutDuration = 0.0f;
    }

    // Channel colors
    for (int i = 0; i < AY_NUM_CHANNELS; i++) {
        char key[64];
        snprintf(key, sizeof(key), "ChColor%d", i);
        char val[32] = "";
        GetPrivateProfileStringA("Colors", key, "", val, sizeof(val), s_configPath);
        if (val[0]) {
            unsigned int c = (unsigned int)strtoul(val, NULL, 16);
            if (c > 0) kChColorsCustom[i] = (ImU32)c;
        }
    }

    DcLog("[AY] Config loaded\n");
}

static void SaveConfig(void) {
    WritePrivateProfileStringA("Settings", "CurrentPath", s_currentPath, s_configPath);
    WritePrivateProfileStringA("Settings", "PlayerCollapsed", s_ymPlayerCollapsed ? "1" : "0", s_configPath);
    // History always expanded — removed HistoryCollapsed INI write
    WritePrivateProfileStringA("Settings", "AutoPlayNext", s_autoPlayNext ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "SequentialPlayback", s_isSequentialPlayback ? "1" : "0", s_configPath);
    WritePrivateProfileStringA("Settings", "ShowPortamento", s_showPortamento ? "1" : "0", s_configPath);
    { char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", (int)(s_portamentoThreshold * 100));
      WritePrivateProfileStringA("Settings", "PortamentoThreshold", tmp, s_configPath); }
    WritePrivateProfileStringA("Settings", "ShowMicrotonal", s_showMicrotonal ? "1" : "0", s_configPath);
    { char clkTmp[16]; snprintf(clkTmp, sizeof(clkTmp), "%d", s_clockSelect);
      WritePrivateProfileStringA("Settings", "ClockSelect", clkTmp, s_configPath); }
    { char clkTmp2[32]; snprintf(clkTmp2, sizeof(clkTmp2), "%.0f", s_customClock);
      WritePrivateProfileStringA("Settings", "CustomClock", clkTmp2, s_configPath); }

    WritePrivateProfileStringA("YmFolderHistory", NULL, NULL, s_configPath);
    for (int i = 0; i < (int)s_folderHistory.size() && i < 200; i++) {
        char key[64];
        snprintf(key, sizeof(key), "Folder%d", i);
        WritePrivateProfileStringA("YmFolderHistory", key, s_folderHistory[i].c_str(), s_configPath);
    }

    // Seek & fadeout
    WritePrivateProfileStringA("Settings", "SeekMode", std::to_string(s_seekMode).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "FlushMode", std::to_string(s_flushMode).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "TimerMode", std::to_string(s_timerMode).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "WriteProtocol", std::to_string(s_writeProtocol).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "FlushThreshold", std::to_string(s_flushThreshold).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "AyPioDelay", std::to_string(s_ayDelayNs).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "PlaybackMode", std::to_string(s_playbackMode).c_str(), s_configPath);
    WritePrivateProfileStringA("Settings", "BufTargetKB", std::to_string(s_bufTargetKB).c_str(), s_configPath);
    {
        char val[32];
        snprintf(val, sizeof(val), "%.1f", s_fadeoutDuration);
        WritePrivateProfileStringA("Settings", "FadeoutDuration", val, s_configPath);
    }

    // Channel colors
    WritePrivateProfileStringA("Colors", NULL, NULL, s_configPath);
    for (int i = 0; i < AY_NUM_CHANNELS; i++) {
        char key[64], val[32];
        snprintf(key, sizeof(key), "ChColor%d", i);
        if (kChColorsCustom[i] != 0) {
            snprintf(val, sizeof(val), "%08X", kChColorsCustom[i]);
            WritePrivateProfileStringA("Colors", key, val, s_configPath);
        }
    }
}

// ============ File Browser ============
static bool IsSupportedExt(const wchar_t* filename) {
    const wchar_t* dot = wcsrchr(filename, L'.');
    if (!dot) return false;
    return (_wcsicmp(dot, L".vgm") == 0 || _wcsicmp(dot, L".vgz") == 0);
}

static void AddToFolderHistory(const char* path) {
    for (int i = (int)s_folderHistory.size() - 1; i >= 0; i--) {
        if (s_folderHistory[i] == std::string(path)) {
            s_folderHistory.erase(s_folderHistory.begin() + i);
            break;
        }
    }
    s_folderHistory.insert(s_folderHistory.begin(), std::string(path));
    if (s_folderHistory.size() > 200) s_folderHistory.resize(200);
}

static void RefreshFileList(void) {
    s_fileList.clear();
    s_selectedFileIndex = -1;
    s_playlist.clear();
    s_playlistIndex = -1;

    // Parent entry
    if (strlen(s_currentPath) > 3) {
        MidiPlayer::FileEntry parent;
        parent.name = "..";
        parent.fullPath = "";
        parent.isDirectory = true;
        s_fileList.push_back(parent);
    }

    std::wstring wCurrentPath = MidiPlayer::UTF8ToWide(s_currentPath);
    std::wstring wSearchPath = wCurrentPath + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wSearchPath.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<MidiPlayer::FileEntry> dirs, files;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        MidiPlayer::FileEntry entry;
        entry.name = MidiPlayer::WideToUTF8(std::wstring(fd.cFileName));
        std::wstring wFullPath = wCurrentPath + L"\\" + fd.cFileName;
        entry.fullPath = MidiPlayer::WideToUTF8(wFullPath);
        entry.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (entry.isDirectory) {
            dirs.push_back(entry);
        } else if (IsSupportedExt(fd.cFileName)) {
            files.push_back(entry);
            s_playlist.push_back(entry.fullPath);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(dirs.begin(), dirs.end(), [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b) { return a.name < b.name; });
    std::sort(files.begin(), files.end(), [](const MidiPlayer::FileEntry& a, const MidiPlayer::FileEntry& b) { return a.name < b.name; });
    for (auto& d : dirs) s_fileList.push_back(d);
    for (auto& f : files) s_fileList.push_back(f);

    // Preview: auto-assign slots from first VGM in folder
    if (!s_playlist.empty()) {
        VGMSync::PreviewAssignSlots(s_playlist[0].c_str());
    }
}

static void NavigateTo(const char* rawPath) {
    std::wstring wRaw = MidiPlayer::UTF8ToWide(rawPath);
    wchar_t wCanon[MAX_PATH];
    if (_wfullpath(wCanon, wRaw.c_str(), MAX_PATH) == nullptr)
        wcsncpy(wCanon, wRaw.c_str(), MAX_PATH);
    std::string canon = MidiPlayer::WideToUTF8(std::wstring(wCanon));
    snprintf(s_currentPath, MAX_PATH, "%s", canon.c_str());

    if (!s_navigating) {
        if (s_navPos < (int)s_navHistory.size() - 1)
            s_navHistory.erase(s_navHistory.begin() + s_navPos + 1, s_navHistory.end());
        s_navHistory.push_back(canon);
        s_navPos++;
    }
    s_navigating = false;

    RefreshFileList();
    AddToFolderHistory(s_currentPath);
    SaveConfig();
}

static void NavBack(void) {
    if (s_navPos > 0) {
        s_navPos--;
        s_navigating = true;
        NavigateTo(s_navHistory[s_navPos].c_str());
    }
}

static void NavForward(void) {
    if (s_navPos < (int)s_navHistory.size() - 1) {
        s_navPos++;
        s_navigating = true;
        NavigateTo(s_navHistory[s_navPos].c_str());
    }
}

static void NavToParent(void) {
    char parentPath[MAX_PATH];
    strncpy(parentPath, s_currentPath, MAX_PATH);
    int len = (int)strlen(parentPath);
    while (len > 0 && parentPath[len - 1] == '\\') { parentPath[--len] = '\0'; }
    char* lastSlash = strrchr(parentPath, '\\');
    if (lastSlash && lastSlash != parentPath) {
        *lastSlash = '\0';
        NavigateTo(parentPath);
    }
}

// ============ Piano / Level Helpers ============

// Helper: process tone channels for one chip
static void UpdateToneChannelLevels(
    int chBase, // 0 for chip0, 5 for chip1
    const uint8_t* vol, const bool* toneOn, const bool* noiseOn,
    float* chDecay, bool* chKeyOff, bool* chKeyOnEdge,
    float* channelLevel,
    double* chFreq, double* chFreqAnchor, float* smoothPoff, float* prevRawPoff,
    int (*midiNoteFn)(int),
    bool* chEnvOnly, bool* chDacMode)
{
    for (int i = 0; i < 3; i++) {
        int chIdx = chBase + i;
        bool useEnv = vol[i] & 0x10;
        uint8_t v = vol[i] & 0x0F;
        bool ton = toneOn[i];
        bool noi = noiseOn[i];
        bool anyOn = ton || noi;

        // Update mode flags
        chEnvOnly[i] = (!anyOn && useEnv);
        chDacMode[i] = (!anyOn && !useEnv && v > 0);

        // Level meter: real volume + fast decay when vol drops to 0
        float rawLv = v / 15.0f;
        if (v > 0) {
            chDecay[i] = rawLv;
        } else {
            chDecay[i] *= 0.85f;
            if (chDecay[i] < 0.01f) chDecay[i] = 0.0f;
        }
        channelLevel[chIdx] = chDecay[i];

        if (s_chMuted[chIdx]) continue;

        // Piano keyboard: show key when volume > 0 or ENV mode active
        float pianoLv = (v > 0) ? rawLv : (useEnv ? 1.0f : 0.0f);
        if (pianoLv > 0.01f) {
            int midi = midiNoteFn(i);
            if (midi >= AY_PIANO_LOW && midi <= AY_PIANO_HIGH) {
                int idx = midi - AY_PIANO_LOW;
                int& cnt = s_pianoKeyChCount[idx];
                if (cnt < AY_NUM_CHANNELS) {
                    s_pianoKeyChannels[idx][cnt] = chIdx;
                    s_pianoKeyChLevels[idx][cnt] = pianoLv;
                    cnt++;
                }
                if (!s_pianoKeyOn[idx] || pianoLv > s_pianoKeyLevel[idx]) {
                    s_pianoKeyOn[idx] = true;
                    s_pianoKeyLevel[idx] = pianoLv;
                    s_pianoKeyChannel[idx] = chIdx;
                }

                // Portamento: continuous frequency tracking with smoothed output
                if (s_showPortamento && chFreq[i] > 1.0) {
                    // Key-on edge: set anchor, clear smooth
                    if (chKeyOnEdge[i]) {
                        chFreqAnchor[i] = chFreq[i];
                        smoothPoff[i] = 0.0f;
                        prevRawPoff[i] = 0.0f;
                        chKeyOnEdge[i] = false;
                    }
                    // Calculate raw semitone offset from anchor
                    if (chFreqAnchor[i] > 1.0) {
                        float rawPoff = (float)(12.0 * log(chFreq[i] / chFreqAnchor[i]) / log(2.0));
                        if (rawPoff > 1.0f) rawPoff = 1.0f;
                        if (rawPoff < -1.0f) rawPoff = -1.0f;
                        // Detect if pitch is actually changing (dynamic) vs static microtonal
                        float poffDelta = fabsf(rawPoff - prevRawPoff[i]);
                        prevRawPoff[i] = rawPoff;
                        bool isDynamic = poffDelta > 0.005f; // frequency is changing
                        // Smooth the offset
                        smoothPoff[i] += (rawPoff - smoothPoff[i]) * 0.3f;
                        // Only show if dynamic (pitch changing) or microtonal enabled
                        if (isDynamic || s_showMicrotonal)
                            s_pitchOffset[idx] = smoothPoff[i];
                    }
                }
            }
        }
        if (pianoLv <= 0.01f) {
            chFreqAnchor[i] = 0.0;
            smoothPoff[i] = 0.0f;
            prevRawPoff[i] = 0.0f;
        }
    }
}

// Helper: process noise channel for one chip
static void UpdateNoiseChannelLevel(
    int chIdx, // 3 or 8
    const uint8_t noisePeriod,
    const bool* noiseOn, const uint8_t* vol,
    float& noiseDecay, float* channelLevel)
{
    bool anyNoiseOn = false;
    for (int i = 0; i < 3; i++) if (noiseOn[i]) { anyNoiseOn = true; break; }

    float maxNVol = 0.0f;
    if (anyNoiseOn) {
        for (int i = 0; i < 3; i++) {
            if (noiseOn[i]) {
                float v = (vol[i] & 0x0F) / 15.0f;
                if (v > maxNVol) maxNVol = v;
            }
        }
    }
    channelLevel[chIdx] = anyNoiseOn ? maxNVol : 0.0f;

    // Noise piano key mapping
    if (!s_chMuted[chIdx] && channelLevel[chIdx] > 0.01f) {
        int nfrq = noisePeriod & 0x1F;
        int noise_freq = nfrq ? (nfrq << 1) : 2;
        if (noise_freq > 0) {
            double nFreq = s_ay8910Clock / (512.0 * noise_freq);
            int nMidi = (int)round(69.0 + 12.0 * log2(nFreq / 440.0));
            if (nMidi >= AY_PIANO_LOW && nMidi <= AY_PIANO_HIGH) {
                int idx = nMidi - AY_PIANO_LOW;
                int& cnt = s_pianoKeyChCount[idx];
                if (cnt < AY_NUM_CHANNELS) {
                    s_pianoKeyChannels[idx][cnt] = chIdx;
                    s_pianoKeyChLevels[idx][cnt] = channelLevel[chIdx];
                    cnt++;
                }
                if (!s_pianoKeyOn[idx] || channelLevel[chIdx] > s_pianoKeyLevel[idx]) {
                    s_pianoKeyOn[idx] = true;
                    s_pianoKeyLevel[idx] = channelLevel[chIdx];
                    s_pianoKeyChannel[idx] = chIdx;
                }
            }
        }
    }
}

// Helper: process envelope channel for one chip
static void UpdateEnvChannelLevel(
    int chIdx, // 4 or 9
    const uint8_t* vol, uint8_t envFine, uint8_t envCoarse, uint8_t envShape,
    float& envDecay, float* channelLevel)
{
    UINT16 envPeriod = ((UINT16)envCoarse << 8) | envFine;
    int envSteps = 32;
    if ((envShape & 0x0F) == 10 || (envShape & 0x0F) == 14) envSteps = 64;

    // Check if any channel has ENV mode on
    bool envActive = false;
    float maxEVol = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (vol[i] & 0x10) {
            envActive = true;
            float v = (vol[i] & 0x0F) / 15.0f;
            if (v > maxEVol) maxEVol = v;
        }
    }

    // ENV channel level: borrow max vol from ENV-enabled channels, full amplitude if all vol=0
    float envLv = 0.0f;
    if (envActive) {
        envLv = (maxEVol > 0) ? maxEVol : 1.0f;
    }
    channelLevel[chIdx] = envLv;

    // Envelope piano key mapping
    if (!s_chMuted[chIdx] && envActive && channelLevel[chIdx] > 0.01f && envPeriod > 0) {
        double eFreq = s_ay8910Clock / (8.0 * envPeriod * envSteps);
        if (eFreq > 0.0) {
            int eMidi = (int)round(69.0 + 12.0 * log2(eFreq / 440.0));
            if (eMidi >= AY_PIANO_LOW && eMidi <= AY_PIANO_HIGH) {
                int idx = eMidi - AY_PIANO_LOW;
                int& cnt = s_pianoKeyChCount[idx];
                if (cnt < AY_NUM_CHANNELS) {
                    s_pianoKeyChannels[idx][cnt] = chIdx;
                    s_pianoKeyChLevels[idx][cnt] = channelLevel[chIdx];
                    cnt++;
                }
                if (!s_pianoKeyOn[idx] || channelLevel[chIdx] > s_pianoKeyLevel[idx]) {
                    s_pianoKeyOn[idx] = true;
                    s_pianoKeyLevel[idx] = channelLevel[chIdx];
                    s_pianoKeyChannel[idx] = chIdx;
                }
            }
        }
    }
}

static void UpdateChannelLevels(void) {
    // Memory barrier: ensure GUI thread sees shadow register updates from viz thread
    std::atomic_thread_fence(std::memory_order_acquire);

    // Clear all piano keys first
    for (int i = 0; i < AY_PIANO_KEYS; i++) {
        s_pianoKeyOn[i] = false;
        s_pianoKeyLevel[i] = 0.0f;
        s_pianoKeyChannel[i] = -1;
        s_pianoKeyChCount[i] = 0;
        s_pitchOffset[i] = 0.0f;
    }

    // Chip 0 (channels 0-4)
    UpdateToneChannelLevels(0, s_vol, s_toneOn, s_noiseOn,
        s_chDecay, s_chKeyOff, s_chKeyOnEdge,
        s_channelLevel, s_chFreq, s_chFreqAnchor, s_chSmoothPoff, s_chPrevRawPoff,
        period_to_midi_note, s_chEnvOnly, s_chDacMode);
    UpdateNoiseChannelLevel(3, s_noisePeriod, s_noiseOn, s_vol,
        s_noiseDecay, s_channelLevel);
    UpdateEnvChannelLevel(4, s_vol, s_envFine, s_envCoarse, s_envShape,
        s_envDecay, s_channelLevel);

    // Chip 1 (channels 5-9)
    UpdateToneChannelLevels(5, s2_vol, s2_toneOn, s2_noiseOn,
        s2_chDecay, s2_chKeyOff, s2_chKeyOnEdge,
        s_channelLevel, s2_chFreq, s2_chFreqAnchor, s2_chSmoothPoff, s2_chPrevRawPoff,
        period_to_midi_note2, s2_chEnvOnly, s2_chDacMode);
    UpdateNoiseChannelLevel(8, s2_noisePeriod, s2_noiseOn, s2_vol,
        s2_noiseDecay, s_channelLevel);
    UpdateEnvChannelLevel(9, s2_vol, s2_envFine, s2_envCoarse, s2_envShape,
        s2_envDecay, s_channelLevel);
}

// ============ VGM Player ============
static UINT32 ReadLE32(FILE* f) {
    UINT8 b[4]; vgmfread(b, 1, 4, f);
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}

static std::string ReadGD3String(const UINT8*& ptr, const UINT8* end) {
    std::string result;
    while (ptr + 1 < end) {
        UINT16 ch = ptr[0] | (ptr[1] << 8); ptr += 2;
        if (ch == 0) break;
        if (ch < 128) result += (char)ch;
        else if (ch < 0x800) { result += (char)(0xC0 | (ch >> 6)); result += (char)(0x80 | (ch & 0x3F)); }
        else { result += (char)(0xE0 | (ch >> 12)); result += (char)(0x80 | ((ch >> 6) & 0x3F)); result += (char)(0x80 | (ch & 0x3F)); }
    }
    if ((ptr - (end - (end - ptr))) % 2 != 0) ptr++;
    return result;
}

static void ParseGD3Tags(FILE* f, UINT32 offset) {
    if (offset == 0) return;
    vgmfseek(f, offset, SEEK_SET);
    char sig[4]; vgmfread(sig, 1, 4, f);
    if (memcmp(sig, "gd3", 3) != 0) return;
    vgmfseek(f, offset + 4, SEEK_SET);
    UINT32 strLen = ReadLE32(f);
    UINT32 dataOff = offset + 12;
    vgmfseek(f, dataOff, SEEK_SET);
    std::vector<UINT8> buf(strLen);
    vgmfread(buf.data(), 1, strLen, f);
    const UINT8* ptr = buf.data();
    const UINT8* end = buf.data() + strLen;
    s_trackName = ReadGD3String(ptr, end);
    std::string trackJp = ReadGD3String(ptr, end);
    if (s_trackName.empty() && !trackJp.empty()) s_trackName = trackJp;
    s_gameName = ReadGD3String(ptr, end);
    std::string gameJp = ReadGD3String(ptr, end);
    if (s_gameName.empty() && !gameJp.empty()) s_gameName = gameJp;
    s_systemName = ReadGD3String(ptr, end);
    std::string sysJp = ReadGD3String(ptr, end);
    if (s_systemName.empty() && !sysJp.empty()) s_systemName = sysJp;
    s_artistName = ReadGD3String(ptr, end);
    std::string artJp = ReadGD3String(ptr, end);
    if (s_artistName.empty() && !artJp.empty()) s_artistName = artJp;
}

static void StopVGMPlayback(void);

static bool LoadVGMFile(const char* path) {
    StopTest();
    StopVGMPlayback();
    if (s_vgmFile) { fclose(s_vgmFile); s_vgmFile = nullptr; }
    s_memData.clear();
    s_memData.shrink_to_fit();
    s_memPos = 0;
    s_vgmLoaded = false;
    s_trackName.clear(); s_gameName.clear(); s_systemName.clear(); s_artistName.clear();
    s_vgmTotalSamples = 0; s_vgmCurrentSamples = 0;
    s_vgmTrackEnded = false;

    FILE* f = ym_fopen(path, "rb");
    if (!f) { DcLog("[VGM] Cannot open: %s\n", path); return false; }

    // Detect gzip header (VGZ)
    UINT8 hdr[2];
    if (fread(hdr, 1, 2, f) != 2) { fclose(f); return false; }
    if (hdr[0] == 0x1F && hdr[1] == 0x8B) {
        // VGZ: read entire file, decompress to memory
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<UINT8> compressed(fsize);
        if ((long)fread(compressed.data(), 1, fsize, f) != fsize) { fclose(f); DcLog("[VGM] VGZ read failed\n"); return false; }
        fclose(f); f = nullptr;
        s_memData = InflateGzip(compressed.data(), compressed.size());
        if (s_memData.empty()) { DcLog("[VGM] VGZ decompress failed\n"); return false; }
        DcLog("[VGM] VGZ decompressed: %ld -> %u bytes\n", fsize, (unsigned)s_memData.size());
    } else {
        // Plain VGM: use FILE* directly
        fseek(f, 0, SEEK_SET);
    }

    char sig[4]; vgmfread(sig, 1, 4, f);
    if (memcmp(sig, "Vgm ", 4) != 0) {
        if (f) fclose(f);
        s_memData.clear(); DcLog("[VGM] Not a VGM file\n"); return false;
    }

    vgmfseek(f, 0x08, SEEK_SET);
    s_vgmVersion = ReadLE32(f);

    // AY8910 clock at offset 0x74
    vgmfseek(f, 0x74, SEEK_SET);
    UINT32 ay8910Clock = ReadLE32(f);

    // GD3 tags at 0x14
    vgmfseek(f, 0x14, SEEK_SET);
    UINT32 gd3RelOff = ReadLE32(f);
    UINT32 gd3Off = gd3RelOff ? (gd3RelOff + 0x14) : 0;
    s_vgmTotalSamples = ReadLE32(f);
    UINT32 loopRelOff = ReadLE32(f);
    s_vgmLoopOffset = loopRelOff ? (loopRelOff + 0x1C) : 0;
    s_vgmLoopSamples = ReadLE32(f);

    UINT32 dataOff = 0x40;
    if (s_vgmVersion >= 0x150) {
        vgmfseek(f, 0x34, SEEK_SET);
        UINT32 hdrDataOff = ReadLE32(f);
        if (hdrDataOff > 0) dataOff = hdrDataOff + 0x34;
    }
    s_vgmDataOffset = dataOff;

    DcLog("[VGM] hdr: ver=0x%X dataAbs=0x%X loopAbs=0x%X gd3Abs=0x%X AY8910Clk=%u\n",
        s_vgmVersion, s_vgmDataOffset, s_vgmLoopOffset, gd3Off, ay8910Clock);

    ParseGD3Tags(f, gd3Off);

    if (s_memData.empty()) {
        fclose(f);
        s_vgmFile = ym_fopen(path, "rb");
        if (!s_vgmFile) { DcLog("[VGM] Reopen failed\n"); return false; }
    } else {
        // VGZ or expanded data: already in memory, reset position for playback
        s_memPos = 0;
    }

    // Loop expansion: duplicate loop section N-1 times in memory
    // VGM: [intro][loop], expanded: [intro][loop]×maxLoops
    // totalExpanded = (totalSamples - loopSamples) + loopSamples × maxLoops
    if (s_vgmLoopEnabled && s_vgmLoopSamples > 0 && s_vgmLoopOffset > 0 && s_vgmMaxLoops > 1
        && s_vgmLoopOffset < s_memData.size()) {
        size_t loopDataLen = s_memData.size() - s_vgmLoopOffset;
        size_t origSize = s_memData.size();
        size_t extraBytes = loopDataLen * (s_vgmMaxLoops - 1);
        s_memData.resize(origSize + extraBytes);
        for (int i = 1; i < s_vgmMaxLoops; i++) {
            memcpy(&s_memData[origSize + loopDataLen * (i - 1)],
                   &s_memData[s_vgmLoopOffset], loopDataLen);
        }
        s_vgmTotalSamples = (s_vgmTotalSamples - s_vgmLoopSamples)
                           + s_vgmLoopSamples * s_vgmMaxLoops;
        s_vgmLoopOffset = 0;  // expanded, no firmware loop
        DcLog("[VGM] Loop expanded: %d loops, %u -> %u samples (%.1fs)\n",
              s_vgmMaxLoops, (unsigned)origSize, (unsigned)s_memData.size(),
              (double)s_vgmTotalSamples / 44100.0);
    } else if (!s_vgmLoopEnabled) {
        s_vgmLoopOffset = 0;  // no loop
    }

    snprintf(s_vgmPath, MAX_PATH, "%s", path);
    s_vgmLoaded = true;
    s_vgmCurrentSamples = 0;
    s_vgmLoopCount = 0;
    s_vgmPaused = false;
    s_currentPlayingFilePath = path;
    for (int i = 0; i < (int)s_playlist.size(); i++) {
        if (s_playlist[i] == std::string(path)) { s_playlistIndex = i; break; }
    }

    DcLog("[VGM] Loaded: %s\n", path);
    DcLog("[VGM] Clock=%u Total=%.1fs\n", ay8910Clock, (double)s_vgmTotalSamples / 44100.0);
    if (!s_trackName.empty()) DcLog("[VGM] Track: %s\n", s_trackName.c_str());
    if (!s_gameName.empty()) DcLog("[VGM] Game: %s\n", s_gameName.c_str());
    return true;
}

static int s_vgmCmdCount = 0;

// VGM Command Length Table for AY8910 player
// cmdLen = total bytes including opcode. 0 = invalid/special.
static const UINT8 VGM_CMD_LEN[0x100] = {
    // 0x00-0x1F: invalid
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x20-0x2F: 20=AY8910(3), 22-2F=unknown(2)
    0x03,0x02,0x02,0x02,0x02,0x02,0x02,0x02, 0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    // 0x30-0x3F: 30=SN76489_2nd(2), 31=AY8910_stereo(2), 3F=GG_stereo_2nd(2)
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02, 0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    // 0x40-0x4F: 40=Mikey(3), 4F=GG_stereo(2)
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03, 0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x02,
    // 0x50-0x5F: 50=SN76489(2), 51=YM2413(3), A0=AY8910(3), 52-5F=YM chips(3)
    0x02,0x03,0x03,0x03,0x03,0x03,0x03,0x03, 0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    // 0x60-0x6F: 61=wait_N(3), 62=735(1), 63=882(1), 66=end(special), 67=datablock(special), 68=PCM_RAM(12)
    0x00,0x03,0x01,0x01,0x00,0x00,0x00,0x00, 0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0x70-0x7F: short wait (op&0x0F)+1 samples, 1 byte each
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01, 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    // 0x80-0x8F: YM2612 DAC write + wait (2 bytes each)
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02, 0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    // 0x90-0x9F: DAC stream control
    0x05,0x05,0x06,0x0B,0x02,0x05, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    // 0xA0-0xAF: 2nd chip writes (3 bytes each)
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03, 0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    // 0xB0-0xBF: block chip writes (3 bytes each)
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03, 0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    // 0xC0-0xCF: memory/bank writes (4-5 bytes)
    0x05,0x05,0x05,0x04,0x04,0x04,0x04,0x04, 0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
    // 0xD0-0xDF: port/register writes (4-5 bytes)
    0x04,0x04,0x04,0x05,0x05,0x05,0x05,0x04, 0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
    // 0xE0-0xFF: PCM seek(5), C352(5), unknown(5)
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05, 0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05, 0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
};

// Table-driven VGM command processor for AY8910
// Returns: wait samples (>0), 0 = no wait, -1 = EOF/error
static int VGMProcessCommand(void) {
    UINT8 cmd;
    if (vgmfread(&cmd, 1, 1, s_vgmFile) != 1) return -1;

    if (s_vgmCmdCount < 50) {
        DcLog("[VGM] cmd=0x%02X at %ld\n", cmd, ftell(s_vgmFile) - 1);
    }

    // Special commands with unique behavior
    switch (cmd) {
        case 0xA0: { // AY8910 register write: [0xA0, register, data]  (3 bytes)
            UINT8 reg_raw, data;
            if (vgmfread(&reg_raw, 1, 1, s_vgmFile) != 1) return -1;
            if (vgmfread(&data, 1, 1, s_vgmFile) != 1) return -1;
            s_vgmCmdCount++;

            UINT8 chipID = (reg_raw & 0x80) >> 7;  // bit7 = chip select
            UINT8 reg = reg_raw & 0x7F;             // actual register
            if (chipID == 0) data = UpdateAY8910State(reg, data);
            else data = UpdateAY8910State2(reg, data);

            // Hardware write
            if (s_connected) {
                ay8910_write_reg(chipID, reg, data);
                if (s_flushMode == 2) {
                    safe_flush();
                }
            }
            return 0;
        }
        case 0x61: { // Wait N samples (3 bytes)
            UINT16 wait; if (vgmfread(&wait, 1, 2, s_vgmFile) != 2) return -1;
            return wait;
        }
        case 0x62: return 735;  // Wait 735 samples
        case 0x63: return 882;  // Wait 882 samples
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            return (cmd & 0x0F) + 1;
        case 0x66: { // End of data
            if (s_vgmLoopOffset > 0 && (s_vgmMaxLoops == 0 || s_vgmLoopCount < s_vgmMaxLoops)) {
                // Fadeout trigger: on the penultimate 0x66
                if (s_vgmMaxLoops > 0 && s_vgmLoopCount >= s_vgmMaxLoops - 1
                    && s_fadeoutDuration > 0 && !s_fadeoutActive) {
                    s_fadeoutActive = true;
                    s_fadeoutLevel = 1.0f;
                    s_fadeoutStartSample = s_vgmCurrentSamples;
                    UINT32 fadeoutSamples = (UINT32)(s_fadeoutDuration * 44100.0);
                    if (s_vgmLoopSamples > 0 && fadeoutSamples > s_vgmLoopSamples)
                        fadeoutSamples = s_vgmLoopSamples;
                    s_fadeoutEndSample = s_vgmCurrentSamples + fadeoutSamples;
                }
                vgmfseek(s_vgmFile, s_vgmLoopOffset, SEEK_SET);
                s_vgmLoopCount++;
                return 0;
            }
            return -1;
        }
        case 0x67: { // Data block (variable length)
            UINT8 compat; if (vgmfread(&compat, 1, 1, s_vgmFile) != 1) return -1;
            if (compat != 0x66) return -1;
            UINT8 type; if (vgmfread(&type, 1, 1, s_vgmFile) != 1) return -1;
            UINT32 size; if (vgmfread(&size, 4, 1, s_vgmFile) != 1) return -1;
            vgmfseek(s_vgmFile, size, SEEK_CUR);
            return 0;
        }
    }

    // All other commands: skip using VGM_CMD_LEN table
    UINT8 cmdLen = VGM_CMD_LEN[cmd];
    if (cmdLen <= 1) return 0;
    UINT8 skip = cmdLen - 1;
    if (vgmfseek(s_vgmFile, skip, SEEK_CUR) != 0) return -1;
    return 0;
}

// Optimized VGMPlay: lookahead batch — reads consecutive 0xA0 writes,
// updates shadow/UI state, sends via ay8910_write_reg.
// Returns number of register writes processed.
// *outWait: wait samples (>0), -1 = EOF, -2 = need fallback to VGMProcessCommand.
static int VGMProcessBatch(int* outWait) {
    *outWait = 0;
    const int MAX_BATCH = 64;
    int count = 0;

    while (count < MAX_BATCH && s_vgmThreadRunning && s_vgmPlaying && !s_vgmPaused) {
        UINT8 cmd;
        if (vgmfread(&cmd, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }

        if (cmd == 0xA0) {
            UINT8 reg_raw, data;
            if (vgmfread(&reg_raw, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            if (vgmfread(&data, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            s_vgmCmdCount++;
            UINT8 chipID = (reg_raw & 0x80) >> 7;
            UINT8 reg = reg_raw & 0x7F;
            if (chipID == 0) data = UpdateAY8910State(reg, data);
            else data = UpdateAY8910State2(reg, data);
            if (s_connected) {
                ay8910_write_reg(chipID, reg, data);
            }
            count++;
        } else if (cmd == 0x62) {
            *outWait = 735; break;
        } else if (cmd == 0x63) {
            *outWait = 882; break;
        } else if (cmd >= 0x70 && cmd <= 0x7F) {
            *outWait = (cmd & 0x0F) + 1; break;
        } else if (cmd == 0x61) {
            UINT16 wait;
            if (vgmfread(&wait, 1, 2, s_vgmFile) != 2) { *outWait = -1; break; }
            *outWait = wait; break;
        } else if (cmd == 0x66) {
            if (s_vgmLoopOffset > 0 && (s_vgmMaxLoops == 0 || s_vgmLoopCount < s_vgmMaxLoops)) {
                if (s_vgmMaxLoops > 0 && s_vgmLoopCount >= s_vgmMaxLoops - 1
                    && s_fadeoutDuration > 0 && !s_fadeoutActive) {
                    s_fadeoutActive = true;
                    s_fadeoutLevel = 1.0f;
                    s_fadeoutStartSample = s_vgmCurrentSamples;
                    UINT32 fadeoutSamples = (UINT32)(s_fadeoutDuration * 44100.0);
                    if (s_vgmLoopSamples > 0 && fadeoutSamples > s_vgmLoopSamples)
                        fadeoutSamples = s_vgmLoopSamples;
                    s_fadeoutEndSample = s_vgmCurrentSamples + fadeoutSamples;
                }
                vgmfseek(s_vgmFile, s_vgmLoopOffset, SEEK_SET);
                s_vgmLoopCount++;
                // Loop jump produces no wait, continue reading
                continue;
            } else {
                *outWait = -1;
            }
            break;
        } else if (cmd == 0x67) {
            UINT8 compat; if (vgmfread(&compat, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            if (compat != 0x66) { *outWait = -1; break; }
            UINT8 type; if (vgmfread(&type, 1, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            UINT32 size; if (vgmfread(&size, 4, 1, s_vgmFile) != 1) { *outWait = -1; break; }
            vgmfseek(s_vgmFile, size, SEEK_CUR);
            // Data block produces no wait, continue reading
            continue;
        } else {
            // Unknown or non-batchable command: put back, signal fallback
            vgmfseek(s_vgmFile, -1, SEEK_CUR);
            *outWait = -2;
            break;
        }
    }

    // Flush after batch
    if (count > 0 && s_connected) {
        safe_flush();
    }
    return count;
}

// Local VGM reader for visualization thread (avoids touching shared s_vgmFile/s_memPos)
struct VizVGMReader {
    std::vector<UINT8> data;
    size_t pos = 0;
    bool eof = false;

    bool open(const char* path, UINT32 dataOffset) {
        if (!s_memData.empty()) {
            data = s_memData;
        } else {
            FILE* f = ym_fopen(path, "rb");
            if (!f) return false;
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            data.resize(fsize);
            if ((long)fread(data.data(), 1, fsize, f) != fsize) { fclose(f); return false; }
            fclose(f);
        }
        pos = dataOffset;
        eof = false;
        return true;
    }

    int readByte() {
        if (pos >= data.size()) { eof = true; return -1; }
        return data[pos++];
    }

    void seek(size_t p) { pos = p; }
};

// Process one VGM command from local reader - updates shadow registers only, no HID
static int VizProcessCommand(VizVGMReader& r) {
    int cmd = r.readByte();
    if (cmd < 0) return -1;

    switch (cmd) {
        case 0xA0: {
            int reg_raw = r.readByte(); if (reg_raw < 0) return -1;
            int data = r.readByte();    if (data < 0) return -1;
            uint8_t chipID = (reg_raw & 0x80) >> 7;
            uint8_t reg = reg_raw & 0x7F;
            if (chipID == 0) UpdateAY8910State(reg, (UINT8)data);
            else UpdateAY8910State2(reg, (UINT8)data);
            return 0;
        }
        case 0x61: {
            int lo = r.readByte(); if (lo < 0) return -1;
            int hi = r.readByte(); if (hi < 0) return -1;
            return lo | (hi << 8);
        }
        case 0x62: return 735;
        case 0x63: return 882;
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            return (cmd & 0x0F) + 1;
        case 0x66: {
            if (s_vgmLoopOffset > 0 && (s_vgmMaxLoops == 0 || s_vizLoopCount < s_vgmMaxLoops)) {
                if (s_vgmMaxLoops > 0 && s_vizLoopCount >= s_vgmMaxLoops - 1
                    && s_fadeoutDuration > 0 && !s_fadeoutActive) {
                    s_fadeoutActive = true;
                    s_fadeoutLevel = 1.0f;
                    s_fadeoutStartSample = s_vgmCurrentSamples;
                    UINT32 fadeoutSamples = (UINT32)(s_fadeoutDuration * 44100.0);
                    if (s_vgmLoopSamples > 0 && fadeoutSamples > s_vgmLoopSamples)
                        fadeoutSamples = s_vgmLoopSamples;
                    s_fadeoutEndSample = s_vgmCurrentSamples + fadeoutSamples;
                }
                r.seek(s_vgmLoopOffset);
                s_vizLoopCount++;
                return 0;
            }
            return -1;
        }
        case 0x67: {
            int compat = r.readByte(); if (compat < 0) return -1;
            if (compat != 0x66) return -1;
            int type = r.readByte();   if (type < 0) return -1;
            int b0 = r.readByte(); if (b0 < 0) return -1;
            int b1 = r.readByte(); if (b1 < 0) return -1;
            int b2 = r.readByte(); if (b2 < 0) return -1;
            int b3 = r.readByte(); if (b3 < 0) return -1;
            UINT32 size = (UINT32)b0 | ((UINT32)b1 << 8) | ((UINT32)b2 << 16) | ((UINT32)b3 << 24);
            r.pos += size;
            return 0;
        }
        default: {
            UINT8 len = VGM_CMD_LEN[cmd];
            if (len <= 1) return 0;
            r.pos += len - 1;
            return 0;
        }
    }
}


// Fadeout helper: shared logic for volume ramp
static bool DoFadeoutUpdate(void) {
    if (!s_fadeoutActive || s_fadeoutDuration <= 0) return false;
    UINT32 fadeRange = s_fadeoutEndSample - s_fadeoutStartSample;
    if (fadeRange == 0) fadeRange = 1;
    float progress = (float)(s_vgmCurrentSamples - s_fadeoutStartSample) / (float)fadeRange;
    if (s_vgmCurrentSamples >= s_fadeoutEndSample) {
        s_fadeoutLevel = 0.0f;
        s_fadeoutActive = false;
    } else {
        s_fadeoutLevel = 1.0f - progress;
    }
    static UINT32 lastFadeSample = 0;
    if (s_vgmCurrentSamples - lastFadeSample >= 441 || s_fadeoutLevel <= 0.0f) {
        lastFadeSample = s_vgmCurrentSamples;
        if (s_connected) {
            for (int ch = 0; ch < 3; ch++) {
                uint8_t origVol = s_vol[ch] & 0x0F;
                uint8_t fadedVol = (uint8_t)(origVol * s_fadeoutLevel);
                uint8_t regData = (s_vol[ch] & 0xF0) | (fadedVol & 0x0F);
                ay8910_write_reg(0, 0x08 + ch, regData);
            }
            safe_flush();
        }
    }
    if (s_fadeoutLevel <= 0.0f) {
        s_vgmTrackEnded = true;
        s_vgmPlaying = false;
        return true;
    }
    return false;
}

// Tick-based shadow register queue: syncs UI visualization with firmware playback
// Each update is tagged with its VGM sample tick; flushed when fwTick catches up.
struct ShadowRegUpdate {
    uint8_t reg;
    uint8_t data;
    uint8_t chipID;   // 0 or 1
    uint32_t tick;     // VGM sample position (44100 Hz)
};

struct ShadowRegDelayQueue {
    static constexpr int CAP = 512;
    ShadowRegUpdate buf[CAP];
    int head = 0, tail = 0, count = 0;

    void push(uint8_t reg, uint8_t data, uint8_t chipID, uint32_t tick) {
        if (count >= CAP) flushTo(tick);  // overflow: flush oldest
        auto& t = buf[tail];
        t.reg = reg; t.data = data; t.chipID = chipID; t.tick = tick;
        tail = (tail + 1) % CAP;
        count++;
    }

    // Apply all updates up to (and including) the given firmware tick
    void flushTo(uint32_t fwTick) {
        while (count > 0 && buf[head].tick <= fwTick) {
            auto& t = buf[head];
            if (t.chipID == 0) UpdateAY8910State(t.reg, t.data);
            else UpdateAY8910State2(t.reg, t.data);
            head = (head + 1) % CAP;
            count--;
        }
    }

    void flushAll() {
        while (count > 0) {
            auto& t = buf[head];
            if (t.chipID == 0) UpdateAY8910State(t.reg, t.data);
            else UpdateAY8910State2(t.reg, t.data);
            head = (head + 1) % CAP;
            count--;
        }
    }

    void clear() { head = tail = count = 0; }
};

static ShadowRegDelayQueue s_shadowQueue;

// VGM parse state machine for shadow register sync in buffered mode
// Visualization thread: independent local VGM playback at real speed (44100Hz)
// Same architecture as live-mode VGMPlaybackThread, but only updates shadow registers
static DWORD WINAPI VGMVisualizationThread(LPVOID param) {
    size_t seekPos = (size_t)param;
    size_t startPos = seekPos > 0 ? seekPos : s_vgmDataOffset;

    VizVGMReader reader;
    if (!reader.open(s_vgmPath, startPos)) { DcLog("[Viz] Failed to open VGM\n"); return 1; }
    std::atomic_thread_fence(std::memory_order_acquire);

    LARGE_INTEGER perfFreq;
    QueryPerformanceFrequency(&perfFreq);
    double samplesPerTick = 44100.0 / perfFreq.QuadPart;

    HANDLE mmEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    MMRESULT mmTimer = timeSetEvent(1, 1, (LPTIMECALLBACK)mmEvent, 0,
                                     TIME_PERIODIC | TIME_CALLBACK_EVENT_SET);
    if (!mmTimer) { CloseHandle(mmEvent); return 1; }

    LARGE_INTEGER last;
    QueryPerformanceCounter(&last);
    double samplesToProcess = 0.0;

    while (s_vizRunning && s_vgmPlaying) {
        if (s_vgmPaused) { Sleep(10); QueryPerformanceCounter(&last); continue; }
        WaitForSingleObject(mmEvent, INFINITE);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        samplesToProcess += (now.QuadPart - last.QuadPart) * samplesPerTick;
        last = now;

        int run = (int)samplesToProcess;

        if (run > 0) {
            int processed = 0;
            while (processed < run && s_vizRunning && s_vgmPlaying && !s_vgmPaused) {
                int w = VizProcessCommand(reader);
                if (w < 0) {
                    // EOF — don't stop playback, let stream thread decide
                    break;
                }
                if (w > 0) processed += w;
            }
            samplesToProcess -= processed;
            s_vgmCurrentSamples += processed;
            std::atomic_thread_fence(std::memory_order_release);

            if (s_fadeoutActive && s_fadeoutDuration > 0) {
                UINT32 fadeRange = s_fadeoutEndSample - s_fadeoutStartSample;
                if (fadeRange == 0) fadeRange = 1;
                float progress = (float)(s_vgmCurrentSamples - s_fadeoutStartSample) / (float)fadeRange;
                if (s_vgmCurrentSamples >= s_fadeoutEndSample) {
                    s_fadeoutLevel = 0.0f;
                    s_fadeoutActive = false;
                } else {
                    s_fadeoutLevel = 1.0f - progress;
                }
                if (s_fadeoutLevel <= 0.0f) break;
            }
        }
    }

    timeKillEvent(mmTimer);
    CloseHandle(mmEvent);
    return 0;
}

// Buffered mode: stream raw VGM bytes to firmware via CMD_VGM_DATA
static DWORD WINAPI VGMStreamThread(LPVOID param) {
    size_t seekPos = (size_t)param;

    // Lower thread priority so GUI main thread gets priority for rendering
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // 1ms multimedia timer for precise polling (replaces Sleep(1) which drifts under load)
    HANDLE mmEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    MMRESULT mmTimer = timeSetEvent(1, 1, (LPTIMECALLBACK)mmEvent, 0,
                                     TIME_PERIODIC | TIME_CALLBACK_EVENT_SET);
    if (!mmTimer) { DcLog("[VGM-Stream] Failed to create mm timer\n"); CloseHandle(mmEvent); s_vgmStreamRunning = false; s_vgmPlaying = false; s_vgmTrackEnded = true; return 0; }

    // Use local copy of VGM data — don't touch shared s_vgmFile/s_memPos
    std::vector<UINT8> localData;
    size_t localPos = 0;
    uint32_t localTotal = 0;

    if (!s_memData.empty()) {
        // Copy from already-decompressed memory
        localData = s_memData;
        size_t startPos = seekPos > 0 ? seekPos : s_vgmDataOffset;
        localPos = startPos;
        localTotal = (uint32_t)(localData.size() - startPos);
    } else {
        // Read entire file into local buffer
        FILE* f = ym_fopen(s_vgmPath, "rb");
        if (!f) { s_vgmStreamRunning = false; return 0; }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        localData.resize(fsize);
        if ((long)fread(localData.data(), 1, fsize, f) != fsize) {
            fclose(f); s_vgmStreamRunning = false; return 0;
        }
        fclose(f);
        size_t startPos = seekPos > 0 ? seekPos : s_vgmDataOffset;
        localPos = startPos;
        localTotal = (uint32_t)(localData.size() - startPos);
    }

    if (localTotal == 0) {
        DcLog("[VGM-Stream] No VGM data to stream\n");
        s_vgmStreamRunning = false;
        return 0;
    }

    s_streamSent = 0;
    s_streamTotal = localTotal;

    uint32_t bufTargetBytes = kBufSizes[s_bufTargetKB];
    if (bufTargetBytes > s_bufTotal) bufTargetBytes = s_bufTotal;

    DcLog("[VGM-Stream] Total data: %u bytes, startPos=%u\n",
          localTotal, (unsigned)localPos);

    // Phase 1: Pre-fill firmware buffer before starting timer
    uint32_t prefillAmount = (localTotal > bufTargetBytes / 4) ? bufTargetBytes / 4 : localTotal;
    int prefillFails = 0;
    while (s_streamSent < prefillAmount && s_vgmStreamRunning && s_vgmPlaying) {
        uint32_t remain = prefillAmount - s_streamSent;
        size_t toSend = (remain > 60) ? 60 : remain;
        uint16_t newBufLevel = 0;
        if (!rpfm_send_vgm_data(&localData[localPos], (uint8_t)toSend, &newBufLevel, nullptr)) {
            prefillFails++;
            if (prefillFails >= 10) {
                DcLog("[VGM-Stream] Prefill failed 10 times, aborting\n");
                s_vgmStreamRunning = false; s_vgmPlaying = false; s_vgmTrackEnded = true; return 0;
            }
            DcLog("[VGM-Stream] Prefill HID fail #%d, retrying...\n", prefillFails);
            Sleep(5);
            continue;
        }
        prefillFails = 0;
        s_bufLevel = newBufLevel;
        localPos += toSend;
        s_streamSent += (uint32_t)toSend;
    }

    DcLog("[VGM-Stream] Prefilled %u bytes, starting playback\n", s_streamSent);

    // Now send VGM_START — loops are expanded by host, so loopOff=0
    uint16_t loopOff = 0;

    if (!rpfm_vgm_start(loopOff, NULL)) {
        DcLog("[VGM-Stream] VGM start failed\n");
        s_vgmStreamRunning = false;
        return 0;
    }

    // Phase 2: Continue streaming remaining data
    bool allDataSent = (s_streamSent >= localTotal);
    uint32_t fwTick = 0;
    int streamFails = 0;

    while (s_vgmStreamRunning && s_vgmPlaying) {
        if (s_vgmPaused) { Sleep(10); continue; }

        // Backpressure: wait if firmware buffer is >50% of target
        if (s_bufLevel > bufTargetBytes / 2) {
            // Only poll HID every ~10ms to avoid starving GUI thread
            static int backpressureCount = 0;
            backpressureCount++;
            if (backpressureCount >= 10) {
                backpressureCount = 0;
                uint8_t dummy = 0;
                uint16_t newLevel = 0;
                if (rpfm_send_vgm_data(&dummy, 0, &newLevel, &fwTick))
                    s_bufLevel = newLevel;
            }
            WaitForSingleObject(mmEvent, INFINITE);
            continue;
        }

        if (!allDataSent) {
            uint32_t remain = localTotal - s_streamSent;
            if (remain == 0) {
                allDataSent = true;
                DcLog("[VGM-Stream] All %u bytes sent, waiting for firmware to finish\n", s_streamSent);
            } else {
                size_t toSend = (remain > 60) ? 60 : remain;
                uint16_t newBufLevel = 0;
                if (!rpfm_send_vgm_data(&localData[localPos], (uint8_t)toSend, &newBufLevel, &fwTick)) {
                    streamFails++;
                    if (streamFails >= 20) {
                        DcLog("[VGM-Stream] HID send failed 20 times, stopping\n");
                        s_vgmPlaying = false; s_vgmTrackEnded = true;
                        break;
                    }
                    DcLog("[VGM-Stream] HID send fail #%d, retrying...\n", streamFails);
                    Sleep(3);
                    continue;
                }
                streamFails = 0;
                s_bufLevel = newBufLevel;
                localPos += toSend;
                s_streamSent += (uint32_t)toSend;

                // Use firmware tick for accurate position tracking (progress bar uses s_fwTick)
                s_fwTick = fwTick;
            }
        }

        if (allDataSent) {
            // Poll firmware buffer: if drained, playback is complete
            uint8_t dummy = 0;
            uint16_t newLevel = 0;
            if (rpfm_send_vgm_data(&dummy, 0, &newLevel, &fwTick))
                s_bufLevel = newLevel;
            else
                Sleep(3);
            if (s_bufLevel == 0) {
                DcLog("[VGM-Stream] Firmware buffer drained, playback complete\n");
                break;
            }
            Sleep(10);
        }
    }

    s_vgmStreamRunning = false;
    s_vizRunning = false;
    if (s_vizThread) { WaitForSingleObject(s_vizThread, 2000); CloseHandle(s_vizThread); s_vizThread = nullptr; }
    s_vgmPlaying = false;
    s_vgmTrackEnded = true;
    timeKillEvent(mmTimer);
    CloseHandle(mmEvent);
    return 0;
}

static DWORD WINAPI VGMPlaybackThread(LPVOID) {
    QueryPerformanceFrequency(&s_perfFreq);
    double samplesPerTick = 44100.0 / s_perfFreq.QuadPart;

    if (s_timerMode == 3 || s_timerMode == 7) {
        // VGMPlay / OptVGMPlay: 1ms periodic multimedia timer + QPC
        HANDLE mmEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        MMRESULT timerId = timeSetEvent(1, 1, (LPTIMECALLBACK)mmEvent, 0,
                                        TIME_PERIODIC | TIME_CALLBACK_EVENT_SET);
        if (!timerId) { CloseHandle(mmEvent); s_vgmThreadRunning = false; return 0; }

        LARGE_INTEGER last;
        QueryPerformanceCounter(&last);
        double samplesToProcess = 0.0;

        while (s_vgmThreadRunning && s_vgmPlaying) {
            if (s_vgmPaused) { Sleep(10); QueryPerformanceCounter(&last); continue; }
            WaitForSingleObject(mmEvent, INFINITE);

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            samplesToProcess += (now.QuadPart - last.QuadPart) * samplesPerTick;
            last = now;

            int run = (int)samplesToProcess;
            if (run > 0) {
                int processed = 0;
                while (processed < run && s_vgmThreadRunning && s_vgmPlaying && !s_vgmPaused) {
                    int s;
                    if (s_timerMode == 7) {
                        // Optimized: lookahead batch
                        int batchWait;
                        VGMProcessBatch(&batchWait);
                        if (batchWait == -2) {
                            // Non-batchable command encountered, fallback
                            s = VGMProcessCommand();
                        } else if (batchWait < 0) {
                            s_vgmTrackEnded = true;
                            s_vgmPlaying = false;
                            break;
                        } else {
                            s = batchWait;
                        }
                    } else {
                        s = VGMProcessCommand();
                        if (s < 0) {
                            s_vgmTrackEnded = true;
                            s_vgmPlaying = false;
                            break;
                        }
                    }
                    if (s > 0) processed += s;
                }
                samplesToProcess -= processed;
                s_vgmCurrentSamples += processed;

                if (DoFadeoutUpdate()) break;
                safe_flush();
            }
        }
        timeKillEvent(timerId);
        CloseHandle(mmEvent);
    } else {
        // Modes 0/1/2: QPC + periodic sleep
        LARGE_INTEGER last;
        QueryPerformanceCounter(&last);
        double samplesToProcess = 0.0;

        while (s_vgmThreadRunning && s_vgmPlaying) {
            if (s_vgmPaused) { Sleep(10); QueryPerformanceCounter(&last); continue; }

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            samplesToProcess += (now.QuadPart - last.QuadPart) * samplesPerTick;
            last = now;

            int run = (int)samplesToProcess;
            if (run > 0) {
                int processed = 0;
                while (processed < run && s_vgmThreadRunning && s_vgmPlaying && !s_vgmPaused) {
                    int s = VGMProcessCommand();
                    if (s < 0) {
                        s_vgmTrackEnded = true;
                        s_vgmPlaying = false;
                        break;
                    }
                    if (s > 0) processed += s;
                }
                samplesToProcess -= processed;
                s_vgmCurrentSamples += processed;

                if (DoFadeoutUpdate()) break;
                safe_flush();
            }
            timer_sleep_1ms();
        }
    }
    safe_flush();
    s_vgmThreadRunning = false;
    return 0;
}

static size_t s_startSeekPos = 0;

static void StartVGMPlayback(void) {
    size_t seekPos = s_startSeekPos;
    s_startSeekPos = 0;
    if (!s_vgmLoaded) return;
    StopTest();

    if (seekPos == 0 && s_connected) InitHardware();

    // Stop existing threads
    if (s_vgmThreadRunning) {
        s_vgmThreadRunning = false;
        if (s_vgmThread) { WaitForSingleObject(s_vgmThread, 2000); CloseHandle(s_vgmThread); s_vgmThread = nullptr; }
    }
    if (s_vgmStreamRunning) {
        s_vgmStreamRunning = false;
        if (s_vgmStreamThread) { WaitForSingleObject(s_vgmStreamThread, 2000); CloseHandle(s_vgmStreamThread); s_vgmStreamThread = nullptr; }
    }
    if (s_vizRunning) {
        s_vizRunning = false;
        if (s_vizThread) { WaitForSingleObject(s_vizThread, 2000); CloseHandle(s_vizThread); s_vizThread = nullptr; }
    }

    if (seekPos == 0) {
        s_vgmCurrentSamples = 0;
        s_vgmLoopCount = 0;
        s_vizLoopCount = 0;
    }
    s_vgmPlaying = true;
    s_vgmPaused = false;
    s_vgmTrackEnded = false;

    if (s_playbackMode == 1) {
        // Buffered mode: VGMStreamThread handles HID streaming, VizThread handles local visualization
        s_vizLoopCount = 0;
        s_vizRunning = true;
        s_vizThread = CreateThread(NULL, 0, VGMVisualizationThread, (LPVOID)(uintptr_t)seekPos, 0, NULL);

        s_bufLevel = 0;
        s_streamSent = 0;
        s_streamTotal = 0;
        s_vgmStreamRunning = true;
        s_vgmStreamThread = CreateThread(NULL, 0, VGMStreamThread, (LPVOID)(uintptr_t)seekPos, 0, NULL);
    } else {
        // Live mode: seek to data start, use VGMPlaybackThread
        if (s_memData.empty()) {
            if (s_vgmFile) fclose(s_vgmFile);
            s_vgmFile = ym_fopen(s_vgmPath, "rb");
            if (!s_vgmFile) return;
            vgmfseek(s_vgmFile, s_vgmDataOffset, SEEK_SET);
        } else {
            s_memPos = s_vgmDataOffset;
        }
        s_vgmThreadRunning = true;
        s_vgmThread = CreateThread(NULL, 0, VGMPlaybackThread, NULL, 0, NULL);
    }
}

static void StopVGMPlayback(void) {
    s_vgmPlaying = false;
    s_vgmPaused = false;
    s_vgmThreadRunning = false;
    s_vgmStreamRunning = false;
    if (s_vgmThread) {
        WaitForSingleObject(s_vgmThread, 2000);
        CloseHandle(s_vgmThread);
        s_vgmThread = nullptr;
    }
    if (s_vgmStreamThread) {
        WaitForSingleObject(s_vgmStreamThread, 2000);
        CloseHandle(s_vgmStreamThread);
        s_vgmStreamThread = nullptr;
    }
    // Always stop firmware VGM player and clear buffer when connected
    if (s_connected) {
        rpfm_vgm_stop();
    }
    if (s_connected) InitHardware();
}

static void PauseVGMPlayback(void) {
    if (!s_vgmLoaded) return;

    if (!s_vgmPaused) {
        // 暂停 = 停止播放，记住位置
        uint32_t pos = s_vgmCurrentSamples;
        StopVGMPlayback();
        s_vgmCurrentSamples = pos;
        s_vgmPaused = true;
    } else {
        // 恢复 = seek 到暂停位置继续播放
        s_vgmPaused = false;
        SeekVGM(s_vgmCurrentSamples);
    }
}

static void OpenVGMFileDialog(void) {
    char path[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "VGM Files\0*.vgm;*.vgz\0All Files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        LoadVGMFile(path);
        VGMSync::LoadWindowSlotConfig("AY8910");
        VGMSync::NotifyFileOpened(path);
    }
}

static void SeekVGMToStart(void) {
    SeekVGM(0);
}

static void SeekVGM(uint32_t targetSample) {
    DcLog("[Seek] enter: targetSample=%u totalSamples=%u\n", targetSample, s_vgmTotalSamples);
    if (!s_vgmLoaded) return;
    targetSample = (std::min)(targetSample, s_vgmTotalSamples);

    // 1. Stop playback (same as StartVGMPlayback preamble)
    s_vgmPlaying = false;
    s_vgmPaused = false;
    s_vizRunning = false;
    s_vgmStreamRunning = false;
    s_vgmThreadRunning = false;
    if (s_vizThread) { WaitForSingleObject(s_vizThread, 2000); CloseHandle(s_vizThread); s_vizThread = nullptr; }
    if (s_vgmStreamThread) { WaitForSingleObject(s_vgmStreamThread, 2000); CloseHandle(s_vgmStreamThread); s_vgmStreamThread = nullptr; }
    if (s_vgmThread) { WaitForSingleObject(s_vgmThread, 2000); CloseHandle(s_vgmThread); s_vgmThread = nullptr; }
    if (s_connected) { rpfm_vgm_stop(); InitHardware(); }

    // 2. Silent fast-forward to target sample (updates shadow registers)
    VizVGMReader seekReader;
    if (!seekReader.open(s_vgmPath, s_vgmDataOffset)) return;
    uint32_t skipSamples = 0;
    while (skipSamples < targetSample) {
        int w = VizProcessCommand(seekReader);
        if (w < 0) break;
        if (w > 0) {
            skipSamples += w;
            if (skipSamples > targetSample) { skipSamples = targetSample; break; }
        }
    }

    // 3. Set state for StartVGMPlayback
    s_vgmCurrentSamples = targetSample;
    s_fwTick = targetSample;
    s_vgmLoopCount = 0;
    s_vizLoopCount = 0;
    s_vgmTrackEnded = false;
    s_fadeoutActive = false;
    s_fadeoutLevel = 1.0f;
    s_vgmSeekPos = seekReader.pos;

    // 4. Write shadow registers to hardware so chip state matches seek position
    if (s_connected) ApplyShadowState();

    // 5. Start playback from seek position
    s_startSeekPos = seekReader.pos;
    DcLog("[Seek] skipSamples=%u seekReader.pos=%u, calling StartVGMPlayback\n",
          skipSamples, (unsigned)seekReader.pos);
    StartVGMPlayback();
    DcLog("[Seek] done: playing=%d currentSamples=%u\n", s_vgmPlaying, s_vgmCurrentSamples);
}

static void PlayPlaylistNext(void) {
    if (s_playlist.empty()) return;
    const char* nextPath;
    if (s_isSequentialPlayback) {
        int next = s_playlistIndex + 1;
        if (next >= (int)s_playlist.size()) next = 0;
        nextPath = s_playlist[next].c_str();
    } else {
        int next = rand() % (int)s_playlist.size();
        nextPath = s_playlist[next].c_str();
    }
    if (LoadVGMFile(nextPath)) {
        VGMSync::LoadWindowSlotConfig("AY8910");
        VGMSync::NotifyFileOpened(nextPath);
        StartVGMPlayback();
    }
}

static void PlayPlaylistPrev(void) {
    if (s_playlist.empty()) return;
    int prev = s_playlistIndex - 1;
    if (prev < 0) prev = (int)s_playlist.size() - 1;
    const char* prevPath = s_playlist[prev].c_str();
    if (LoadVGMFile(prevPath)) {
        VGMSync::LoadWindowSlotConfig("AY8910");
        VGMSync::NotifyFileOpened(prevPath);
        StartVGMPlayback();
    }
}

// ============ Test Functions ============
static double GetTestElapsedMs(void) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - s_testStartTime.QuadPart) / s_perfFreq.QuadPart * 1000.0;
}

static double GetStepDurationMs(int type) {
    switch (type) {
        case 0: return 500.0;  // Instrument demo
        case 1: return 300.0;  // Scale
        default: return 300.0;
    }
}

// Helper: convert MIDI note to AY8910 tone period
static void midi_to_period(int midiNote, int& fine, int& coarse) {
    double freq = 440.0 * pow(2.0, (midiNote - 69.0) / 12.0);
    int period = (int)round(s_ay8910Clock / (8.0 * freq));
    if (period < 1) period = 1;
    if (period > 4095) period = 4095;
    fine = period & 0xFF;
    coarse = (period >> 8) & 0x0F;
}

// Key off all tone channels
static void key_off_all(void) {
    // Mute all tone channels via mixer
    ay8910_write_reg(0, 0x07, 0x3F);
    safe_flush();
}

static void TestStep(void) {
    switch (s_testType) {
        case 0: {
            // Tone channel demo: play C major scale across 3 channels
            static const uint8_t notes[] = {60, 62, 64, 65, 67, 69, 71, 72};
            if (s_testStep >= 8) { s_testRunning = false; key_off_all(); return; }

            int fine, coarse;
            midi_to_period(notes[s_testStep], fine, coarse);

            // Key off all first
            key_off_all();

            // Set volume on channel 0
            ay8910_write_reg(0, 0x08, 0x0F); // max volume
            // Set period
            ay8910_write_reg(0, 0x00, fine);
            ay8910_write_reg(0, 0x01, coarse);
            // Key-on: enable tone for channel A
            ay8910_write_reg(0, 0x07, ~(1 << 0)); // enable tone ch A
            safe_flush();

            // Update shadow state
            s_vol[0] = 0x0F;
            s_toneFine[0] = fine;
            s_toneCoarse[0] = coarse;
            s_mixer = ~(1 << 0);
            s_toneOn[0] = true;
            s_chDecay[0] = 1.0f;
            s_chKeyOff[0] = false;
            break;
        }
        case 1: {
            // Noise channel test
            if (s_testStep >= 8) { s_testRunning = false; key_off_all(); return; }

            key_off_all();

            // Enable noise on channel A
            ay8910_write_reg(0, 0x08, 0x0F); // max volume
            ay8910_write_reg(0, 0x06, s_testStep * 2 + 1); // varying noise period
            ay8910_write_reg(0, 0x07, ~(1 << 3)); // enable noise ch A
            safe_flush();

            s_vol[0] = 0x0F;
            s_noisePeriod = s_testStep * 2 + 1;
            s_mixer = ~(1 << 3);
            s_noiseOn[0] = true;
            s_noiseDecay = 1.0f;
            break;
        }
        default: s_testRunning = false; return;
    }
    safe_flush(); s_testStep++;
}

static void StopTest(void) {
    if (!s_testRunning) return;
    s_testRunning = false;
    if (s_connected) InitHardware();
    ResetState();
}

static void StartTest(int type) {
    if (!s_connected || s_vgmPlaying) return;
    StopTest();
    s_testType = type; s_testStep = 0; s_testStepMs = 0.0;
    QueryPerformanceCounter(&s_testStartTime);
    s_testRunning = true;
}

// ============ Public API ============
void Init() {
    QueryPerformanceFrequency(&s_perfFreq);
    s_scope.Init();
    LoadConfig();

    // Auto-connect RPFM device
    if (!s_connected) {
        s_connected = SPFMManager::ConnectDevice(0);
        if (s_connected) {
            DcLog("[AY] Auto-connected RPFM device\n");
        }
    }

    // Register unified playback callbacks
    VGMSync::RegisterChipWriter(VGMSync::CHIP_AY8910,
        (VGMSync::ChipStateUpdateFn)UpdateAY8910State,
        (VGMSync::ChipHwWriteFn)ay8910_write_reg,
        (VGMSync::ChipFlushFn)safe_flush,
        (VGMSync::ChipApplyStateFn)ApplyShadowState,
        (VGMSync::ChipResetFn)ResetState);
    // Register 2nd chip instance state update
    VGMSync::RegisterChipWriter2(VGMSync::CHIP_AY8910,
        (VGMSync::ChipStateUpdateFn)UpdateAY8910State2);
    VGMSync::RegisterLogFn(DcLog);

    if (s_currentPath[0] != '\0') {
        RefreshFileList();
    } else {
        GetExeDir(s_currentPath, MAX_PATH);
        RefreshFileList();
    }
    VGMSync::LoadWindowSlotConfig("AY8910", s_localSlots);
}

void Shutdown() {
    StopVGMPlayback();
    SaveConfig();
    if (s_connected) {
        InitHardware();
    }
    rpfm_hid_close();
    s_connected = false;
}

void Update() {
    SyncConnectionState();
    UpdateChannelLevels();

    // Live mode: detect VGM thread finished
    if (s_playbackMode == 0) {
        if (s_vgmPlaying && s_vgmTrackEnded) {
            s_vgmPlaying = false;
            s_vgmPaused = false;
            s_vgmThreadRunning = false;
            if (s_vgmThread) { CloseHandle(s_vgmThread); s_vgmThread = nullptr; }
            if (s_autoPlayNext && !s_playlist.empty()) PlayPlaylistNext();
        }
        if (s_vgmPlaying && !s_vgmThreadRunning && s_vgmThread) {
            CloseHandle(s_vgmThread); s_vgmThread = nullptr;
            s_vgmPlaying = false; s_vgmPaused = false;
        }
    }

    // Buffered mode: detect stream thread finished
    // Only check after stream has actually started sending data (s_streamTotal > 0)
    if (s_playbackMode == 1 && s_vgmPlaying && !s_vgmStreamRunning
        && s_vgmStreamThread && s_streamTotal > 0) {
        WaitForSingleObject(s_vgmStreamThread, 2000);
        CloseHandle(s_vgmStreamThread);
        s_vgmStreamThread = nullptr;
        s_vgmPlaying = false;
        s_vgmPaused = false;
        // Only auto-next if thread exited normally (all data sent and consumed)
        // Don't auto-next on early failures (prefill error, HID send failure, etc.)
        if (s_streamSent >= s_streamTotal) {
            s_vgmTrackEnded = true;
            if (s_autoPlayNext && !s_playlist.empty()) PlayPlaylistNext();
        }
    }

    // Update scope voice channel offsets
    static int scopeCheckCounter = 0;
    if (++scopeCheckCounter >= 60) {
        scopeCheckCounter = 0;
        ScopeChipSlot *slot = scope_find_slot("AY8910", 0);
        if (slot) {
            for (int i = 0; i < AY_CH_PER_CHIP; i++) s_voiceCh[i] = slot->slot_base + i;
        } else {
            for (int i = 0; i < AY_CH_PER_CHIP; i++) s_voiceCh[i] = -1;
        }
    }

    // Test
    if (s_testRunning) {
        double elapsed = GetTestElapsedMs();
        if (elapsed >= s_testStepMs) { TestStep(); s_testStepMs += GetStepDurationMs(s_testType); }
    }
}

// ============ Piano Keyboard ============
static ImU32 getChColor(int ch) {
    if (ch >= 0 && ch < AY_NUM_CHANNELS) {
        if (kChColorsCustom[ch] != 0) return kChColorsCustom[ch];
        return kChColors[ch];
    }
    return IM_COL32(160, 200, 160, 255);
}

static ImU32 blendKey(ImU32 col, float lv, bool isBlack) {
    float blendLv = 0.2f + 0.8f * lv;
    int r = (col >>  0) & 0xFF;
    int g = (col >>  8) & 0xFF;
    int b = (col >> 16) & 0xFF;
    int br = isBlack ? 20 : 255;
    return IM_COL32(
        br + (int)((r - br) * blendLv),
        br + (int)((g - br) * blendLv),
        br + (int)((b - br) * blendLv), 255);
}

static ImU32 getKeyColor(int idx, float level) {
    int ch = s_pianoKeyChannel[idx];
    ImU32 col = (ch >= 0) ? getChColor(ch) : IM_COL32(160, 200, 160, 255);
    return blendKey(col, level, false);
}

static ImU32 getKeyColorBlack(int idx, float level) {
    int ch = s_pianoKeyChannel[idx];
    ImU32 col = (ch >= 0) ? getChColor(ch) : IM_COL32(160, 200, 160, 255);
    return blendKey(col, level, true);
}

static void RenderPianoKeyboard(void) {
    ImGui::BeginChild("AY_Piano", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float whiteKeyH = 100.0f;
    float blackKeyH = 60.0f;

    const int kMinNote = AY_PIANO_LOW;
    const int kMaxNote = AY_PIANO_HIGH;

    int numWhiteKeys = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) if (!s_isBlackNote[n % 12]) numWhiteKeys++;

    float whiteKeyW = availW / (float)numWhiteKeys;
    if (whiteKeyW < 6.0f) whiteKeyW = 6.0f;
    float blackKeyW = whiteKeyW * 0.65f;

    // Pre-compute key center X positions for portamento interpolation
    float keyCenterX[AY_PIANO_KEYS];
    {
        int wk = 0;
        for (int n = kMinNote; n <= kMaxNote; n++) {
            int idx = n - AY_PIANO_LOW;
            if (!s_isBlackNote[n % 12]) {
                keyCenterX[idx] = p.x + wk * whiteKeyW + whiteKeyW * 0.5f;
                wk++;
            } else {
                float bkLeft = p.x + (wk - 1) * whiteKeyW + whiteKeyW - blackKeyW * 0.5f;
                keyCenterX[idx] = bkLeft + blackKeyW * 0.5f;
            }
        }
    }

    // Pass 1: white keys
    int wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (s_isBlackNote[n % 12]) continue;
        int idx = n - AY_PIANO_LOW;
        float x = p.x + wkIdx * whiteKeyW;

        if (s_pianoKeyOn[idx] && s_pianoKeyChCount[idx] > 1) {
            // Multi-channel vertical split
            int cnt = s_pianoKeyChCount[idx];
            float segH = whiteKeyH / (float)cnt;
            for (int s = 0; s < cnt; s++) {
                ImU32 segCol = blendKey(getChColor(s_pianoKeyChannels[idx][s]),
                    s_pianoKeyChLevels[idx][s], false);
                dl->AddRectFilled(ImVec2(x, p.y + s * segH),
                    ImVec2(x + whiteKeyW - 1, p.y + (s + 1) * segH), segCol);
            }
        } else {
            ImU32 fillCol = s_pianoKeyOn[idx] ? getKeyColor(idx, s_pianoKeyLevel[idx]) : IM_COL32(255, 255, 255, 255);
            dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + whiteKeyW - 1, p.y + whiteKeyH), fillCol);
        }
        dl->AddRect(ImVec2(x, p.y), ImVec2(x + whiteKeyW, p.y + whiteKeyH), IM_COL32(0, 0, 0, 255));
        if (n % 12 == 0) {
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "C%d", n / 12 - 1);
            dl->AddText(ImVec2(x + 2, p.y + whiteKeyH - 18), IM_COL32(0, 0, 0, 255), lbl);
        }
        wkIdx++;
    }

    // Pass 2: black keys
    wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (!s_isBlackNote[n % 12]) { wkIdx++; continue; }
        int idx = n - AY_PIANO_LOW;
        float x = p.x + (wkIdx - 1) * whiteKeyW + whiteKeyW - blackKeyW * 0.5f;

        if (s_pianoKeyOn[idx] && s_pianoKeyChCount[idx] > 1) {
            int cnt = s_pianoKeyChCount[idx];
            float segH = blackKeyH / (float)cnt;
            for (int s = 0; s < cnt; s++) {
                ImU32 segCol = blendKey(getChColor(s_pianoKeyChannels[idx][s]),
                    s_pianoKeyChLevels[idx][s], true);
                dl->AddRectFilled(ImVec2(x, p.y + s * segH),
                    ImVec2(x + blackKeyW, p.y + (s + 1) * segH), segCol);
            }
        } else {
            ImU32 fillCol = (idx >= 0 && idx < AY_PIANO_KEYS && s_pianoKeyOn[idx])
                ? getKeyColorBlack(idx, s_pianoKeyLevel[idx]) : IM_COL32(0, 0, 0, 255);
            dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + blackKeyW, p.y + blackKeyH), fillCol);
        }
        dl->AddRect(ImVec2(x, p.y), ImVec2(x + blackKeyW, p.y + blackKeyH), IM_COL32(128, 128, 128, 255));
    }

    // Pass 3: Portamento/Vibrato indicators (continuous frequency tracking)
    if (s_showPortamento)
    {
        auto noteToX = [&](float fnote) -> float {
            int n0 = (int)floorf(fnote);
            int n1 = n0 + 1;
            float frac = fnote - (float)n0;
            float x0 = (n0 >= kMinNote && n0 <= kMaxNote) ? keyCenterX[n0 - AY_PIANO_LOW] : keyCenterX[0];
            float x1 = (n1 >= kMinNote && n1 <= kMaxNote) ? keyCenterX[n1 - AY_PIANO_LOW] : keyCenterX[kMaxNote - AY_PIANO_LOW];
            return x0 + (x1 - x0) * frac;
        };
        for (int n = kMinNote; n <= kMaxNote; n++) {
            int idx = n - AY_PIANO_LOW;
            if (!s_pianoKeyOn[idx]) continue;
            float poff = s_pitchOffset[idx];
            if (fabsf(poff) < s_portamentoThreshold) continue;
            ImU32 col = getChColor(s_pianoKeyChannel[idx]);
            float hl_w = whiteKeyW * 0.3f;
            float kh = s_isBlackNote[n % 12] ? blackKeyH : whiteKeyH;
            float fnote_target = (float)n + poff;
            float hx = noteToX(fnote_target);
            dl->AddRectFilled(
                ImVec2(hx - hl_w * 0.5f, p.y),
                ImVec2(hx + hl_w * 0.5f, p.y + kh),
                col);
        }
    }

    ImGui::EndChild();
}

// ============ Level Meters ============
static void ApplyChannelMute(int i) {
    int chip = i / AY_CH_PER_CHIP;
    int ch = i % AY_CH_PER_CHIP;
    if (ch < 3) {
        const uint8_t* volArr = (chip == 0) ? s_vol : s2_vol;
        uint8_t volReg;
        if (s_chMuted[i]) {
            volReg = (volArr[ch] & 0xF0) | 0x00;
        } else {
            volReg = volArr[ch];
        }
        if (s_connected) {
            ay8910_write_reg((uint8_t)chip, 0x08 + ch, volReg);
            uint8_t mixer = (chip == 0) ? s_mixer : s2_mixer;
            for (int c = 0; c < 3; c++) {
                if (s_chMuted[chip * AY_CH_PER_CHIP + c]) {
                    mixer |= (0x9 << c);
                }
            }
            if (s_chMuted[chip * AY_CH_PER_CHIP + 3]) mixer |= 0x38;
            ay8910_write_reg((uint8_t)chip, 0x07, mixer);
            // Envelope mute: clear bit4 on all volume regs
            if (s_chMuted[chip * AY_CH_PER_CHIP + 4]) {
                const uint8_t* vol = (chip == 0) ? s_vol : s2_vol;
                for (int c = 0; c < 3; c++) {
                    ay8910_write_reg((uint8_t)chip, 0x08 + c, vol[c] & ~0x10);
                }
            }
            safe_flush();
        }
    }
}

static void RenderLevelMeters(void) {
    ImGui::BeginChild("AY_LevelMeters", ImVec2(0, 0), true);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    float groupW = availW / (float)AY_NUM_CHANNELS;
    float meterW = groupW - 4.0f;
    if (meterW > 28.0f) meterW = 28.0f;
    float labelH = 20.0f;
    float volTextH = 18.0f;
    float meterH = availH - labelH - volTextH - 4.0f;
    if (meterH < 10.0f) meterH = 10.0f;

    auto levelToDB = [](float level) -> float {
        if (level <= 0.0f) return 0.0f;
        float db = 20.0f * log10f(level);
        if (db < -24.0f) db = -24.0f;
        return (db + 24.0f) / 24.0f;
    };

    auto getLevelColor = [](float level) -> ImU32 {
        if (level <= 0.0f) return IM_COL32(40, 40, 40, 255);
        if (level < 0.33f) {
            float t = level / 0.33f;
            return IM_COL32(0, (int)(100 + 155 * t), (int)(255 - 155 * t), 255);
        } else if (level < 0.66f) {
            float t = (level - 0.33f) / 0.33f;
            return IM_COL32((int)(255 * t), 255, (int)(100 - 100 * t), 255);
        } else {
            float t = (level - 0.66f) / 0.34f;
            return IM_COL32(255, (int)(255 - 155 * t), 0, 255);
        }
    };

    // Scroll wheel invert all
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) {
        for (int j = 0; j < AY_NUM_CHANNELS; j++) s_chMuted[j] = !s_chMuted[j];
        s_soloCh = -1;
        for (int j = 0; j < AY_NUM_CHANNELS; j++) ApplyChannelMute(j);
    }

    for (int i = 0; i < AY_NUM_CHANNELS; i++) {
        float centerX = p.x + i * groupW + groupW * 0.5f;
        float mY = p.y + labelH;
        float meterLeft = centerX - meterW * 0.5f;
        float meterRight = centerX + meterW * 0.5f;
        float meterBottom = mY + meterH;

        // Channel label
        const char* label = kChNames[i];
        ImU32 labelCol = getChColor(i);
        bool isMuted = s_chMuted[i];
        bool isSolo = (s_soloCh == i);

        ImVec2 textSize = ImGui::CalcTextSize(label);
        float btnW = textSize.x + 6.0f;
        float btnH = textSize.y + 4.0f;
        ImGui::SetCursorScreenPos(ImVec2(centerX - btnW * 0.5f, p.y + 1));

        // Style button per state
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        if (isMuted) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 50, 50, 200));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 70, 70, 220));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 90, 90, 240));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 200, 255));
        } else if (isSolo) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 160, 30, 200));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 200, 50, 220));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 240, 70, 240));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 200, 255));
        } else {
            ImColor col(labelCol);
            float bright = 0.3f + 0.7f * s_channelLevel[i];
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(col.Value.x * bright * 255, col.Value.y * bright * 255, col.Value.z * bright * 255, 180));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(col.Value.x * 0.6f * 255, col.Value.y * 0.6f * 255, col.Value.z * 0.6f * 255, 220));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(col.Value.x * 0.8f * 255, col.Value.y * 0.8f * 255, col.Value.z * 0.8f * 255, 240));
            ImGui::PushStyleColor(ImGuiCol_Text, labelCol);
        }

        char btnId[16];
        snprintf(btnId, sizeof(btnId), "##aych%d", i);
        if (ImGui::Button(btnId, ImVec2(btnW, btnH))) {
            if (s_soloCh >= 0) {
                s_soloCh = -1;
                s_chMuted[i] = !s_chMuted[i];
            } else {
                s_chMuted[i] = !s_chMuted[i];
            }
            ApplyChannelMute(i);
        }
        if (ImGui::IsItemClicked(1)) {
            if (s_soloCh == i) {
                s_soloCh = -1;
                for (int j = 0; j < AY_NUM_CHANNELS; j++) { s_chMuted[j] = false; ApplyChannelMute(j); }
            } else {
                s_soloCh = i;
                for (int j = 0; j < AY_NUM_CHANNELS; j++) {
                    s_chMuted[j] = (j != i);
                    ApplyChannelMute(j);
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Left-click: Mute/Unmute\nRight-click: Solo\nScroll: Invert all");
            ImGui::EndTooltip();
        }

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        // Overlay label text
        dl->AddText(ImVec2(centerX - textSize.x * 0.5f, p.y + 3),
                     isMuted ? IM_COL32(255, 200, 200, 255) : labelCol, label);

        // Background
        dl->AddRectFilled(ImVec2(meterLeft, mY), ImVec2(meterRight, meterBottom), IM_COL32(30, 30, 30, 255));
        dl->AddRect(ImVec2(meterLeft, mY), ImVec2(meterRight, meterBottom), IM_COL32(100, 100, 100, 255));

        float level = s_channelLevel[i];
        float displayLevel = levelToDB(level);
        if (displayLevel > 0.01f) {
            float barH = meterH * displayLevel;
            float barY = mY + meterH - barH;
            int segs = 20;
            for (int s = 0; s < segs; s++) {
                float segH = barH / segs;
                float segY = barY + s * segH;
                float segLvl = (float)(segs - s) / segs * displayLevel;
                dl->AddRectFilled(ImVec2(meterLeft + 1, segY), ImVec2(meterRight - 1, segY + segH), getLevelColor(segLvl));
            }
        }

        // Mute overlay + X
        if (s_chMuted[i]) {
            dl->AddRectFilled(ImVec2(meterLeft, mY), ImVec2(meterRight, meterBottom), IM_COL32(0, 0, 0, 120));
            dl->AddLine(ImVec2(meterLeft + 2, mY + 2), ImVec2(meterRight - 2, meterBottom - 2), IM_COL32(255, 80, 80, 200), 2);
            dl->AddLine(ImVec2(meterRight - 2, mY + 2), ImVec2(meterLeft + 2, meterBottom - 2), IM_COL32(255, 80, 80, 200), 2);
        }
        // Solo highlight
        if (s_soloCh == i) {
            dl->AddRect(ImVec2(meterLeft - 1, mY - 1), ImVec2(meterRight + 1, meterBottom + 1), IM_COL32(255, 220, 50, 200), 0, 0, 2);
        }

        // Volume / status text
        char volStr[16];
        {
            int chip = i / AY_CH_PER_CHIP;
            int ch = i % AY_CH_PER_CHIP;
            if (ch < 3) {
                const uint8_t* pVol = (chip == 0) ? s_vol : s2_vol;
                const bool* pToneOn = (chip == 0) ? s_toneOn : s2_toneOn;
                const bool* pNoiseOn = (chip == 0) ? s_noiseOn : s2_noiseOn;
                const bool* pEnvOnly = (chip == 0) ? s_chEnvOnly : s2_chEnvOnly;
                uint8_t rawVol = pVol[ch];
                uint8_t v = rawVol & 0x0F;
                bool ton = pToneOn[ch];
                bool noi = pNoiseOn[ch];
                bool anyOn = ton || noi;
                if (!anyOn && v > 0) {
                    snprintf(volStr, sizeof(volStr), "DAC:%d", v);  // DAC mode: tone+noise off but vol > 0
                } else {
                    snprintf(volStr, sizeof(volStr), "%d", v);  // show real volume register value
                }
            } else if (ch == 3) {
                // Noise ch: show max volume of noise-enabled tone channels
                const uint8_t* pVol = (chip == 0) ? s_vol : s2_vol;
                const bool* pNoiseOn = (chip == 0) ? s_noiseOn : s2_noiseOn;
                uint8_t maxVol = 0;
                for (int j = 0; j < 3; j++) {
                    if (pNoiseOn[j]) {
                        uint8_t v = pVol[j] & 0x0F;
                        if (v > maxVol) maxVol = v;
                    }
                }
                snprintf(volStr, sizeof(volStr), "%d", maxVol);
            } else {
                // Env ch: show max volume of env-enabled tone channels
                const uint8_t* pVol = (chip == 0) ? s_vol : s2_vol;
                uint8_t maxVol = 0;
                bool envActive = false;
                for (int j = 0; j < 3; j++) {
                    if (pVol[j] & 0x10) {
                        envActive = true;
                        uint8_t v = pVol[j] & 0x0F;
                        if (v > maxVol) maxVol = v;
                    }
                }
                if (envActive && maxVol == 0)
                    snprintf(volStr, sizeof(volStr), "ENV");  // ENV mode but vol=0: full amplitude
                else if (envActive)
                    snprintf(volStr, sizeof(volStr), "%d", maxVol);  // borrow vol from env channels
                else
                    snprintf(volStr, sizeof(volStr), "0");
            }
        }
        ImVec2 volSize = ImGui::CalcTextSize(volStr);
        ImU32 volCol = s_chMuted[i] ? IM_COL32(180, 60, 60, 255) : IM_COL32(180, 180, 180, 255);
        dl->AddText(ImVec2(centerX - volSize.x * 0.5f, mY + meterH + 2), volCol, volStr);
    }

    ImGui::EndChild();
}

// ============ Scope (placeholder) ============
static void RenderScopeArea(void) {
    if (!s_showScope) return;

    if (s_scopeHeight < 10.0f) s_scopeHeight = 10.0f;
    ImGui::BeginChild("AY_Scope", ImVec2(0, s_scopeHeight), true);

    // Only show scope for chip0 channels (0-4) to keep layout manageable
    bool hasScope = false;
    for (int i = 0; i < AY_CH_PER_CHIP; i++) {
        if (s_voiceCh[i] >= 0) { hasScope = true; break; }
    }

    if (hasScope) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y;
        float chW = availW / (float)AY_CH_PER_CHIP - 4.0f;

        for (int i = 0; i < AY_CH_PER_CHIP; i++) {
            if (s_voiceCh[i] < 0) continue;
            float x = p.x + i * (chW + 4.0f) + 2.0f;
            bool keyOn = false;
            if (i < 3) keyOn = s_chDecay[i] > 0.01f;
            else if (i == 3) keyOn = s_noiseDecay > 0.01f;
            else keyOn = s_envDecay > 0.01f;

            s_scope.DrawChannel(s_voiceCh[i], dl, x, p.y + 16, chW, availH - 16,
                s_scopeAmplitude, kChColors[i], keyOn, s_channelLevel[i],
                s_scopeSamples, 0, 735, true, true, 1, false);
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Scope requires libvgm AY8910 core integration)");
    }

    ImGui::EndChild();
}

// ============ Test & Channel Controls Window ============
static void RenderTestPopup(void) {
    if (!s_showTestPopup) return;
    ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AY8910 Debug##aytestwin", &s_showTestPopup)) { ImGui::End(); return; }

    // Hardware Tests
    if (ImGui::CollapsingHeader("Hardware Tests##aytest", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Tone Scale##aytest0", ImVec2(-1, 0))) StartTest(0);
        if (ImGui::Button("Noise Test##aytest1", ImVec2(-1, 0))) StartTest(1);
        if (s_testRunning) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Stop Test##ayteststop", ImVec2(-1, 0))) StopTest();
            ImGui::PopStyleColor();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running...");
            if (s_testType == 0) {
                ImGui::Text("Note %d/8", s_testStep);
            } else if (s_testType == 1) {
                ImGui::Text("Noise period %d/8", s_testStep * 2 + 1);
            }
        }
    }

    // Quick note test
    if (ImGui::CollapsingHeader("Quick Note##ayqnote", nullptr, 0)) {
        static int qNote = 60;
        static int qVol = 15;
        static int qCh = 0;
        ImGui::SliderInt("Note##ayqn", &qNote, 24, 96);
        ImGui::SliderInt("Volume##ayqv", &qVol, 0, 15);
        ImGui::SliderInt("Channel##ayqc", &qCh, 0, 2);
        ImGui::SameLine(); ImGui::Text("%s", kChNames[qCh]);
        if (ImGui::Button("Play Note##ayqplay", ImVec2(-1, 0))) {
            if (s_connected && !s_vgmPlaying) {
                int fine, coarse;
                midi_to_period(qNote, fine, coarse);
                key_off_all();
                ay8910_write_reg(0, 0x08 + qCh, (uint8_t)(qVol & 0x0F));
                ay8910_write_reg(0, qCh * 2, (uint8_t)fine);
                ay8910_write_reg(0, qCh * 2 + 1, (uint8_t)coarse);
                // Enable tone for selected channel
                uint8_t mixer = 0x3F;
                mixer &= ~(1 << qCh); // enable tone
                ay8910_write_reg(0, 0x07, mixer);
                safe_flush();

                // Update shadow state
                s_vol[qCh] = (uint8_t)(qVol & 0x0F);
                s_toneFine[qCh] = (uint8_t)fine;
                s_toneCoarse[qCh] = (uint8_t)coarse;
                s_mixer = mixer;
                s_toneOn[qCh] = true;
                s_chDecay[qCh] = 1.0f;
                s_chKeyOff[qCh] = false;
            }
        }
        if (ImGui::Button("Key Off##ayqoff", ImVec2(-1, 0))) {
            if (s_connected) { key_off_all(); safe_flush(); }
        }
    }

    ImGui::End();
}

// ============ UI Rendering ============
static void RenderSidebar(void) {
    if (!ImGui::CollapsingHeader("AY8910 Hardware##ay", nullptr, ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // Connection status
    if (SPFMManager::IsConnected()) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Connected");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Disconnected");
    }

    // Connect / Disconnect / Reset buttons
    {
        if (SPFMManager::IsConnected()) {
            if (ImGui::Button("Disconnect##ay", ImVec2(-1, 0))) {
                SPFMManager::DisconnectDevice(0, true);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Disconnect RPFM device");
            if (ImGui::Button("Reset All##ay", ImVec2(-1, 0))) {
                AY8910Window::MuteAll();
                SPFMManager::SendReset();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute AY8910 and send RPFM reset");
        } else {
            if (ImGui::Button("Connect##ay", ImVec2(-1, 0))) {
                SPFMManager::ConnectDevice(0);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Connect RPFM HID device");
        }
    }

    // SPFM Slot assignment
    {
        ImGui::Spacing();
        for (int s = 0; s < 4; s++) {
            int cur = s_localSlots[s];
            ImGui::Text("Slot %d:", s); ImGui::SameLine();
            if (ImGui::Combo(("##slot_ay_"+std::to_string(s)).c_str(), &cur, VGMSync::CHIP_LABELS, VGMSync::CHIP_LABEL_COUNT)) {
                s_localSlots[s] = cur;
                VGMSync::SaveWindowSlotConfig("AY8910", s_localSlots);
            }
        }
    }

    ImGui::Spacing(); ImGui::Separator();

    // Playback Mode
    ImGui::TextDisabled("Playback Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Live: real-time register writes from PC\n"
        "Buffered: stream raw VGM to firmware timer");
    if (ImGui::RadioButton("Live##aymode", &s_playbackMode, 0)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("Buffered##aymode", &s_playbackMode, 1)) SaveConfig();

    // Buffer status in buffered mode
    if (s_playbackMode == 1 && s_vgmPlaying) {
        ImGui::Spacing();
        if (s_bufTotal < 1024)
            ImGui::Text("Buffer: %u / %u B", s_bufLevel, s_bufTotal);
        else
            ImGui::Text("Buffer: %.1f / %.0f KB", s_bufLevel / 1024.0, s_bufTotal / 1024.0);
        float bufRatio = (float)s_bufLevel / (float)s_bufTotal;
        ImGui::ProgressBar(bufRatio, ImVec2(-1, 0));
        ImGui::Text("Stream: %u / %u KB", s_streamSent / 1024, s_streamTotal / 1024);
        float streamRatio = (s_streamTotal > 0) ? (float)s_streamSent / (float)s_streamTotal : 0.0f;
        ImGui::ProgressBar(streamRatio, ImVec2(-1, 0));
        // Tick display: firmware position / total VGM samples
        double fwSec = (s_vgmTotalSamples > 0) ? (double)s_fwTick / 44100.0 : 0.0;
        double totSec = (double)s_vgmTotalSamples / 44100.0;
        ImGui::Text("Tick: %u / %u", s_fwTick, s_vgmTotalSamples);
        ImGui::Text("Time: %.1fs / %.1fs", fwSec, totSec);
    }

    ImGui::Spacing(); ImGui::Separator();

    // PIO Write Delay
    ImGui::TextDisabled("PIO Write Delay: %d ns", s_ayDelayNs);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "AY8910 /WR pulse width in nanoseconds.\n"
        "Higher = slower but more reliable timing.\n"
        "0 = fastest (PIO full speed).");
    if (ImGui::SliderInt("##aydelay", &s_ayDelayNs, 0, 2000, "%d ns")) {
        if (SPFMManager::IsConnected()) {
            rpfm_set_ay_delay((uint8_t)((s_ayDelayNs + 50) / 100));
        }
        SaveConfig();
    }

    // Buffer target size (affects prefill and backpressure threshold)
    ImGui::TextDisabled("Buffer Size");
    const char* bufLabels[] = {"512 B","1 KB","2 KB","3 KB","4 KB","8 KB"};
    if (ImGui::SliderInt("##aybufsz", &s_bufTargetKB, 0, kBufSizeCount - 1, bufLabels[s_bufTargetKB])) {
        s_bufTotal = kBufSizes[s_bufTargetKB];
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Firmware buffer target size.\n"
        "Smaller = lower latency, larger = more stable.\n"
        "Affects prefill amount and backpressure threshold.");

    ImGui::Spacing(); ImGui::Separator();

    // Test popup button
    if (s_testRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Test Running...##aytestbtn", ImVec2(-1, 0))) s_showTestPopup = true;
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("Debug Test##aytestbtn", ImVec2(-1, 0))) s_showTestPopup = true;
    }

    ImGui::Spacing(); ImGui::Separator();

    // Seek mode
    ImGui::TextDisabled("Seek Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fast-forward: replay commands from start\nDirect: skip without HW, reset chip on target");
    ImGui::RadioButton("Fast-forward##ayseek", &s_seekMode, 0); ImGui::SameLine();
    ImGui::RadioButton("Direct##ayseek", &s_seekMode, 1);

    ImGui::Spacing(); ImGui::Separator();

    // Flush Mode
    ImGui::TextDisabled("Flush Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Register: flush after each reg write\nCommand: flush after each VGM command");
    if (ImGui::RadioButton("Register##ayflush", &s_flushMode, 1)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("Command##ayflush", &s_flushMode, 2)) SaveConfig();

    // Write Protocol
    ImGui::TextDisabled("Write Protocol");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Original: RPFM HID register write\n"
        "Experiment: raw write path B");
    if (ImGui::RadioButton("Original##ayprotocol", &s_writeProtocol, 0)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("Experiment##ayprotocol", &s_writeProtocol, 1)) SaveConfig();

    // Flush Threshold
    ImGui::TextDisabled("Flush Threshold");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Min wait samples to trigger mid-batch flush.\n1 = most precise, 735 = 50Hz frame boundary only.\nLower = smoother for high-speed VGM (SNDH etc).");
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("##ayflushthr", &s_flushThreshold, 1, 10)) {
        if (s_flushThreshold < 1) s_flushThreshold = 1;
        if (s_flushThreshold > 735) s_flushThreshold = 735;
        VGMSync::SetFlushThreshold(s_flushThreshold);
        SaveConfig();
    }

    // Timer Mode
    ImGui::TextDisabled("Timer Mode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("H-Prec: waitable timer+spin\nHybrid: Sleep+spin\nMM-Timer: timeSetEvent\nVGMPlay: 1ms periodic timer\nOptVGMPlay: periodic+batch lookahead");
    if (ImGui::RadioButton("H-Prec##aytimer", &s_timerMode, 0)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("Hybrid##aytimer", &s_timerMode, 1)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("MM-Timer##aytimer", &s_timerMode, 2)) SaveConfig();
    if (ImGui::RadioButton("VGMPlay##aytimer", &s_timerMode, 3)) SaveConfig();
    ImGui::SameLine();
    if (ImGui::RadioButton("OptVGMPlay##aytimer", &s_timerMode, 7)) SaveConfig();

    ImGui::Spacing(); ImGui::Separator();

    // Clock Correction
    if (ImGui::CollapsingHeader("Clock Correction##ayclk")) {
        for (int i = 0; i < kClockCount; i++) {
            if (i == kClockCount - 1) {
                ImGui::RadioButton(kClocks[i].name, &s_clockSelect, i);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                char clkBuf[32];
                snprintf(clkBuf, sizeof(clkBuf), "%.0f", s_customClock);
                if (ImGui::InputText("##aycustomclk", clkBuf, sizeof(clkBuf), ImGuiInputTextFlags_CharsDecimal)) {
                    double v = atof(clkBuf);
                    if (v > 1000.0) { s_customClock = v; s_ay8910Clock = v; SaveConfig(); }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Custom AY8910 clock frequency in Hz");
            } else {
                if (ImGui::RadioButton(kClocks[i].name, &s_clockSelect, i)) {
                    s_ay8910Clock = kClocks[i].hz;
                    SaveConfig();
                }
            }
        }
        if (s_clockSelect == kClockCount - 1) {
            s_ay8910Clock = s_customClock;
        }
        ImGui::TextDisabled("Current: %.0f Hz", s_ay8910Clock);
    }

    ImGui::Spacing(); ImGui::Separator();

    // Portamento/Vibrato indicator
    if (ImGui::Checkbox("Portamento Indicator##ay", &s_showPortamento)) SaveConfig();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show pitch change indicator on piano keyboard\nDetects portamento, vibrato, and pitch bends");
    if (s_showPortamento) {
        ImGui::Indent();
        if (ImGui::Checkbox("Microtonal##ay", &s_showMicrotonal)) SaveConfig();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also show static microtonal offsets (frequency quantization error)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::SliderFloat("Threshold##ayport", &s_portamentoThreshold, 0.02f, 0.50f, "%.2f"))
            SaveConfig();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Minimum pitch change (semitones) to show indicator");
        ImGui::Unindent();
    }

    ImGui::Spacing(); ImGui::Separator();

    // Scope settings
    if (ImGui::CollapsingHeader("Scope##ayscope", nullptr, 0)) {
        ImGui::Checkbox("Show Scope##ay", &s_showScope);
        if (s_showScope) {
            ImGui::SliderFloat("Height##ay", &s_scopeHeight, 20, 300);
            ImGui::SliderFloat("Amplitude##ay", &s_scopeAmplitude, 0.5f, 10.0f);
            ImGui::SliderInt("Samples##ay", &s_scopeSamples, 100, 1000);
        }
    }

    // Channel Colors
    if (ImGui::CollapsingHeader("Channel Colors##aychcolors")) {
        for (int i = 0; i < AY_NUM_CHANNELS; i++) {
            ImGui::PushID(i);
            float colF[4];
            ImU32 curCol = kChColorsCustom[i];
            ImU32 defCol = kChColors[i];
            if (curCol != 0) {
                colF[0] = ((curCol >> 0) & 0xFF) / 255.0f;
                colF[1] = ((curCol >> 8) & 0xFF) / 255.0f;
                colF[2] = ((curCol >> 16) & 0xFF) / 255.0f;
                colF[3] = ((curCol >> 24) & 0xFF) / 255.0f;
            } else {
                colF[0] = ((defCol >> 0) & 0xFF) / 255.0f;
                colF[1] = ((defCol >> 8) & 0xFF) / 255.0f;
                colF[2] = ((defCol >> 16) & 0xFF) / 255.0f;
                colF[3] = 1.0f;
            }
            if (ImGui::ColorEdit4(("##ayclredit" + std::to_string(i)).c_str(), colF, ImGuiColorEditFlags_NoInputs)) {
                kChColorsCustom[i] = IM_COL32(
                    (int)(colF[0] * 255 + 0.5f), (int)(colF[1] * 255 + 0.5f),
                    (int)(colF[2] * 255 + 0.5f), (int)(colF[3] * 255 + 0.5f));
                SaveConfig();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(kChNames[i]);
            if (kChColorsCustom[i] == 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("(auto)");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(("Reset##rclr" + std::to_string(i)).c_str())) {
                kChColorsCustom[i] = 0;
                SaveConfig();
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Reset All Colors##ayrclrall")) {
            memset(kChColorsCustom, 0, sizeof(kChColorsCustom));
            SaveConfig();
        }
    }

    ImGui::Spacing(); ImGui::Separator();

    // VGM File Info
    if (ImGui::CollapsingHeader("VGM File Info##ayinfo", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (s_vgmLoaded) {
            if (s_vgmVersion) ImGui::TextDisabled("VGM v%X.%02X", (s_vgmVersion >> 8) & 0xFF, s_vgmVersion & 0xFF);
            double durSec = (double)s_vgmTotalSamples / 44100.0;
            int durMin = (int)durSec / 60; int durSecI = (int)durSec % 60;
            ImGui::TextDisabled("Duration:"); ImGui::SameLine();
            ImGui::Text("%02d:%02d", durMin, durSecI);

            if (s_vgmLoopSamples > 0) {
                ImGui::TextDisabled("Loop: Yes");
                ImGui::SameLine();
                ImGui::TextDisabled("(%u / %s)", VGMSync::GetLoopCount(), VGMSync::GetMaxLoops() == 0 ? "inf" : std::to_string(VGMSync::GetMaxLoops()).c_str());
            } else {
                ImGui::TextDisabled("Loop: No");
            }

            // Mixer status
            ImGui::TextDisabled("Mixer:"); ImGui::SameLine();
            ImGui::Text("0x%02X", s_mixer);

            // Envelope shape
            ImGui::TextDisabled("EnvShape:"); ImGui::SameLine();
            ImGui::Text("%s", (s_envShape < 16) ? kEnvShapeNames[s_envShape] : "---");

            ImGui::Spacing();
            if (!s_trackName.empty()) {
                ImGui::TextDisabled("Track:");
                ImGui::Indent();
                VGMPlayerUI::DrawScrollingText("##aytrack", s_trackName.c_str(), IM_COL32(255, 255, 102, 255));
                ImGui::Unindent();
            }
            if (!s_gameName.empty()) {
                ImGui::TextDisabled("Game:");
                ImGui::SameLine();
                VGMPlayerUI::DrawScrollingText("##aygame", s_gameName.c_str(), IM_COL32(200, 200, 200, 255));
            }
            if (!s_systemName.empty()) {
                ImGui::TextDisabled("System:");
                ImGui::SameLine();
                ImGui::Text("%s", s_systemName.c_str());
            }
            if (!s_artistName.empty()) {
                ImGui::TextDisabled("Artist:");
                ImGui::SameLine();
                VGMPlayerUI::DrawScrollingText("##ayartist", s_artistName.c_str(), IM_COL32(200, 200, 200, 255));
            }
        } else {
            ImGui::TextDisabled("No VGM file loaded");
        }
    }
}

static VGMPlayerCallbacks GetPlayerCallbacks(void) {
    return {
        "##ay", "AY8910 VGM Player", &s_ymPlayerCollapsed,
        s_vgmPath, &s_vgmLoaded, &s_vgmPlaying, &s_vgmPaused,
        &s_vgmTotalSamples, &s_vgmCurrentSamples, &s_vgmLoopSamples, &s_vgmLoopCount,
        &s_autoPlayNext, &s_isSequentialPlayback, &s_playlist, &s_showPlayerSettings,
        &s_vgmLoopEnabled,
        StartVGMPlayback, StopVGMPlayback, PauseVGMPlayback, SeekVGMToStart, SeekVGM,
        PlayPlaylistNext, PlayPlaylistPrev, OpenVGMFileDialog
    };
}

static void RenderPlayerBar(void) {
    VGMPlayerUI::RenderPlayerBar(GetPlayerCallbacks());
}

static void RenderRegisterTableForChip(
    const char* title, const char* tblId, const char* infoId, const char* regsId,
    const uint8_t* toneFine, const uint8_t* toneCoarse, uint8_t noisePeriod,
    uint8_t mixer, const uint8_t* vol, uint8_t envFine, uint8_t envCoarse, uint8_t envShape,
    int (*midiNoteFn)(int))
{
    static const char* kEnvShape[8] = {
        "\\___","/___","\\\\\\\\","\\___","\\/\\/","\\````","////","/````"
    };
    uint8_t etype = envShape & 0x0F;

    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "%s", title);
    ImGui::Separator();

    if (ImGui::BeginTable(tblId, 7, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("CH",    ImGuiTableColumnFlags_WidthFixed, 28.f);
        ImGui::TableSetupColumn("Note",  ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableSetupColumn("Per",   ImGuiTableColumnFlags_WidthFixed, 44.f);
        ImGui::TableSetupColumn("Vol",   ImGuiTableColumnFlags_WidthFixed, 28.f);
        ImGui::TableSetupColumn("Env",   ImGuiTableColumnFlags_WidthFixed, 52.f);
        ImGui::TableSetupColumn("Tone",  ImGuiTableColumnFlags_WidthFixed, 38.f);
        ImGui::TableSetupColumn("Noise", ImGuiTableColumnFlags_WidthFixed, 38.f);
        ImGui::TableHeadersRow();
        for (int ch = 0; ch < 3; ch++) {
            uint16_t period = ((uint16_t)(toneCoarse[ch] & 0x0F) << 8) | toneFine[ch];
            uint8_t v = vol[ch] & 0x1F;
            bool tone_on = !((mixer >> ch) & 1);
            bool noise_on = !((mixer >> (ch + 3)) & 1);
            bool env_on = (v & 0x10) != 0;
            int midi = (tone_on && period > 0) ? midiNoteFn(ch) : -1;
            char noteBuf[8] = "---";
            if (midi >= 0 && midi < 128)
                snprintf(noteBuf, sizeof(noteBuf), "%s%d", kNoteNames[midi % 12], midi / 12 - 1);
            ImVec4 col = (tone_on || noise_on) ? ImVec4(0.4f, 1.0f, 0.6f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextColored(col, "CH%c", 'A' + ch);
            ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", noteBuf);
            ImGui::TableSetColumnIndex(2); ImGui::TextColored(col, "%04X", period);
            ImGui::TableSetColumnIndex(3); ImGui::TextColored(col, "%X", v & 0x0F);
            ImGui::TableSetColumnIndex(4); ImGui::TextColored(col, "%s", env_on ? kEnvShape[etype & 7] : "--");
            ImGui::TableSetColumnIndex(5); ImGui::TextColored(col, "%s", tone_on ? "ON" : "--");
            ImGui::TableSetColumnIndex(6); ImGui::TextColored(col, "%s", noise_on ? "ON" : "--");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    {
        uint8_t nfrq = noisePeriod & 0x1F;
        uint16_t efrq = ((uint16_t)envCoarse << 8) | envFine;
        bool envHold = (etype >> 3) & 1;
        bool envAlt  = (etype >> 2) & 1;
        bool envAtt  = (etype >> 1) & 1;
        bool envCont = etype & 1;
        if (ImGui::BeginTable(infoId, 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Item",   ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Dec",    ImGuiTableColumnFlags_WidthFixed, 50.f);
            ImGui::TableSetupColumn("Hex",    ImGuiTableColumnFlags_WidthFixed, 44.f);
            ImGui::TableSetupColumn("Info",   ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Noise");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", nfrq);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%02X", nfrq);
            ImGui::TableSetColumnIndex(3); ImGui::Text("N/%d", 1 << (nfrq & 7));
            {
                int envSteps = 32;
                if (etype == 10 || etype == 14) envSteps = 64;
                float eFreq = (efrq > 0) ? (float)(s_ay8910Clock / (8.0 * efrq * envSteps)) : 0.0f;
                int ntEnv = -1;
                if (eFreq > 0.0f) {
                    double semitones = 12.0 * log(eFreq / 440.0) / log(2.0);
                    ntEnv = (int)(semitones + 0.5) + 69;
                    if (ntEnv < 0) ntEnv = -1;
                }
                char noteEnv[8] = "---";
                if (ntEnv >= 0 && ntEnv < 128)
                    snprintf(noteEnv, sizeof(noteEnv), "%s%d", kNoteNames[ntEnv % 12], ntEnv / 12 - 1);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("Env Freq");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", noteEnv);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%04X", efrq);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f Hz", eFreq);
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Env Type");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", etype);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%X", etype);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s%s%s%s",
                envAtt ? "ATT " : "", envAlt ? "ALT " : "", envCont ? "CONT " : "", envHold ? "HOLD" : "");
            ImGui::EndTable();
        }
    }
    (void)regsId; // regs dump removed for brevity in dual-chip mode
}

static void RenderRegisterTable(void) {
    // Chip 0
    RenderRegisterTableForChip(
        "AY8910 #1", "##ay8910tbl", "##ay8910info", "##ay8910allregs",
        s_toneFine, s_toneCoarse, s_noisePeriod, s_mixer, s_vol,
        s_envFine, s_envCoarse, s_envShape, period_to_midi_note);

    // Check if 2nd chip is active
    int ayCount = 0;
    for (int i = 0; i < VGMSync::MAX_SLOTS; i++)
        if (VGMSync::GetSlotChip(i) == VGMSync::CHIP_AY8910) ayCount++;

    if (ayCount >= 2) {
        ImGui::Spacing();
        RenderRegisterTableForChip(
            "AY8910 #2", "##ay8910tbl2", "##ay8910info2", "##ay8910allregs2",
            s2_toneFine, s2_toneCoarse, s2_noisePeriod, s2_mixer, s2_vol,
            s2_envFine, s2_envCoarse, s2_envShape, period_to_midi_note2);
    }
}

static void RenderFileBrowser(void) {
    ImGui::SetNextItemAllowOverlap();
    bool browserOpen = ImGui::TreeNodeEx("AY8910 File Browser##ayfb",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        ImGuiTreeNodeFlags_DefaultOpen);

    ImGui::SameLine(0, 12);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##ayfbfilter", "Filter...", s_fileBrowserFilter, sizeof(s_fileBrowserFilter));

    if (!browserOpen) return;

    // Navigation buttons
    if (ImGui::Button("<##ayfbback", ImVec2(25, 0))) NavBack();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back");
    ImGui::SameLine();
    if (ImGui::Button(">##ayfbfwd", ImVec2(25, 0))) NavForward();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward");
    ImGui::SameLine();
    if (ImGui::Button("^##ayfbup", ImVec2(25, 0))) NavToParent();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Up to parent directory");
    ImGui::SameLine();

    // Breadcrumb path bar
    if (!s_pathEditMode) {
        float availWidth = ImGui::GetContentRegionAvail().x;
        std::vector<std::string> segments = MidiPlayer::SplitPath(s_currentPath);
        std::vector<float> buttonWidths;
        std::vector<std::string> accumulatedPaths;
        std::string accumulatedPath;
        ImGuiStyle& style = ImGui::GetStyle();
        float framePaddingX = style.FramePadding.x;
        float itemSpacingX = style.ItemSpacing.x;
        float buttonBorderSize = style.FrameBorderSize;
        for (size_t i = 0; i < segments.size(); i++) {
            if (i == 0) accumulatedPath = segments[i];
            else { if (accumulatedPath.back() != '\\') accumulatedPath += "\\"; accumulatedPath += segments[i]; }
            accumulatedPaths.push_back(accumulatedPath);
            ImVec2 textSize = ImGui::CalcTextSize(segments[i].c_str());
            float bw = textSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
            buttonWidths.push_back(bw);
        }
        ImVec2 separatorTextSize = ImGui::CalcTextSize(">");
        float separatorWidth = separatorTextSize.x + itemSpacingX * 2.0f;
        ImVec2 ellipsisTextSize = ImGui::CalcTextSize("...");
        float ellipsisButtonWidth = ellipsisTextSize.x + framePaddingX * 2.0f + buttonBorderSize * 2.0f + 4.0f;
        float ellipsisWidth = ellipsisButtonWidth + separatorWidth;
        float safeAvailWidth = availWidth - 10.0f;
        int firstVisibleSegment = (int)segments.size() - 1;
        float usedWidth = (segments.size() > 0) ? buttonWidths.back() : 0.0f;
        for (int i = (int)segments.size() - 2; i >= 0; i--) {
            float segWidth = buttonWidths[i] + separatorWidth;
            float neededEllipsis = (i > 0) ? ellipsisWidth : 0.0f;
            if (usedWidth + segWidth + neededEllipsis > safeAvailWidth) break;
            else { usedWidth += segWidth; firstVisibleSegment = i; }
        }

        ImVec2 barStartPos = ImGui::GetCursorScreenPos();
        float barHeight = ImGui::GetFrameHeight();
        ImGui::BeginGroup();
        if (firstVisibleSegment > 0) {
            if (ImGui::Button("...##ayellipsis")) {
                s_pathEditMode = true; s_pathEditModeJustActivated = true;
                snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
            }
            ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine();
        }
        for (int i = firstVisibleSegment; i < (int)segments.size(); i++) {
            std::string btnId = segments[i] + "##ayseg" + std::to_string(i);
            if (ImGui::Button(btnId.c_str())) NavigateTo(accumulatedPaths[i].c_str());
            if (i < (int)segments.size() - 1) { ImGui::SameLine(); ImGui::Text(">"); ImGui::SameLine(); }
        }
        ImGui::EndGroup();
        ImVec2 barEndPos = ImGui::GetItemRectMax();
        float emptySpaceWidth = barStartPos.x + availWidth - barEndPos.x;
        if (emptySpaceWidth > 0) {
            ImGui::SameLine();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(barEndPos.x, barStartPos.y), ImVec2(barEndPos.x + emptySpaceWidth, barStartPos.y + barHeight),
                ImGui::GetColorU32(ImGuiCol_FrameBg));
            ImGui::InvisibleButton("##aypathEmpty", ImVec2(emptySpaceWidth, barHeight));
            if (ImGui::IsItemClicked(0)) {
                s_pathEditMode = true; s_pathEditModeJustActivated = true;
                snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
            }
        }
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ayPathInput", s_pathInput, MAX_PATH, ImGuiInputTextFlags_EnterReturnsTrue)) {
            NavigateTo(s_pathInput);
            s_pathEditMode = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_pathEditMode = false; s_pathEditModeJustActivated = false;
            snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
        } else if (!s_pathEditModeJustActivated && !ImGui::IsItemActive() && !ImGui::IsItemFocused()) {
            s_pathEditMode = false;
            snprintf(s_pathInput, MAX_PATH, "%s", s_currentPath);
        }
        if (s_pathEditModeJustActivated) {
            ImGui::SetKeyboardFocusHere(-1);
            s_pathEditModeJustActivated = false;
        }
    }

    // File list
    float fileAreaHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("AYFileList##ayfl", ImVec2(-1, fileAreaHeight), true);

    for (int i = 0; i < (int)s_fileList.size(); i++) {
        const MidiPlayer::FileEntry& entry = s_fileList[i];
        ImGui::PushID(i);

        if (s_fileBrowserFilter[0] != '\0') {
            std::string lowerName = entry.name;
            std::string lowerFilter = s_fileBrowserFilter;
            for (auto& c : lowerName) c = tolower(c);
            for (auto& c : lowerFilter) c = tolower(c);
            if (lowerName.find(lowerFilter) == std::string::npos) {
                ImGui::PopID(); continue;
            }
        }

        char label[512];
        if (entry.isDirectory) {
            if (entry.name == "..") snprintf(label, sizeof(label), "[UP] %s", entry.name.c_str());
            else snprintf(label, sizeof(label), "[DIR] %s", entry.name.c_str());
        } else {
            snprintf(label, sizeof(label), "%s", entry.name.c_str());
        }

        bool isPlaying = (entry.fullPath == s_currentPlayingFilePath && s_vgmPlaying);
        if (isPlaying)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));

        bool selected = (i == s_selectedFileIndex);
        if (ImGui::Selectable(label, selected)) {
            s_selectedFileIndex = i;
            if (entry.isDirectory) {
                if (entry.name == "..") NavToParent();
                else NavigateTo(entry.fullPath.c_str());
            } else {
                s_currentPlayingFilePath = entry.fullPath;
                for (int pi = 0; pi < (int)s_playlist.size(); pi++) {
                    if (s_playlist[pi] == entry.fullPath) { s_playlistIndex = pi; break; }
                }
                if (LoadVGMFile(entry.fullPath.c_str())) {
                    VGMSync::LoadWindowSlotConfig("AY8910");
                    VGMSync::NotifyFileOpened(entry.fullPath.c_str());
                    StartVGMPlayback();
                }
            }
        }

        if (isPlaying) ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::EndChild();
}

static void RenderLogPanel(void) {
    // Debug Log
    if (ImGui::CollapsingHeader("AY8910 Debug Log##aylog", nullptr, 0)) {
        ImGui::Checkbox("Auto-scroll##aylog", &s_logAutoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear##aylog")) {
            s_log.clear(); s_logDisplay[0] = '\0'; s_logLastSize = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy All##aylog")) {
            ImGui::SetClipboardText(s_log.c_str());
        }
        size_t copyLen = s_log.size() < sizeof(s_logDisplay) - 1 ? s_log.size() : sizeof(s_logDisplay) - 1;
        memcpy(s_logDisplay, s_log.c_str(), copyLen);
        s_logDisplay[copyLen] = '\0';
        bool changed = (s_log.size() != s_logLastSize);
        s_logLastSize = s_log.size();
        if (s_logAutoScroll && changed) s_logScrollToBottom = true;

        float logH = 150;
        ImGui::BeginChild("AYDebugLogRegion", ImVec2(0, logH), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 tsz = ImGui::CalcTextSize(s_logDisplay, NULL, false, -1.0f);
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float minH = ImGui::GetContentRegionAvail().y;
        float inH = (tsz.y > minH) ? tsz.y + lineH * 2 : minH;
        ImGui::InputTextMultiline("##YmLogText", s_logDisplay, sizeof(s_logDisplay),
            ImVec2(-1, inH), ImGuiInputTextFlags_ReadOnly);
        if (s_logScrollToBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
            s_logScrollToBottom = false;
        }
        ImGui::EndChild();
    }

    ImGui::Separator();

    // Folder History
    ImGui::SetNextItemAllowOverlap();
    bool historyOpen = ImGui::TreeNodeEx("AY8910 Folder History##ayhist",
        ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_AllowOverlap |
        ImGuiTreeNodeFlags_DefaultOpen);

    ImGui::SameLine(0, 12);
    if (ImGui::SmallButton("Clear##ayHistClear")) {
        s_folderHistory.clear(); SaveConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear All");
    ImGui::SameLine();
    if (s_histSortMode == 0) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Time##ayhistSortTime")) s_histSortMode = 0;
    if (s_histSortMode == 0) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by time");
    ImGui::SameLine();
    if (s_histSortMode == 1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
    if (ImGui::SmallButton("Freq##ayhistSortFreq")) s_histSortMode = 1;
    if (s_histSortMode == 1) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by frequency");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 8);
    ImGui::InputTextWithHint("##ayHistFilter", "Filter...", s_histFilter, sizeof(s_histFilter));

    if (historyOpen) {
        struct HistEntry { std::string name; int idx; int fileCount; };
        std::vector<HistEntry> entries;
        std::set<std::string> seen;
        for (int i = 0; i < (int)s_folderHistory.size(); i++) {
            size_t lastSlash = s_folderHistory[i].find_last_of("\\/");
            std::string folderName = (lastSlash != std::string::npos) ? s_folderHistory[i].substr(lastSlash + 1) : s_folderHistory[i];
            if (s_histFilter[0] != '\0') {
                std::string lowerName = folderName;
                std::string lowerFilter = s_histFilter;
                for (auto& c : lowerName) c = tolower(c);
                for (auto& c : lowerFilter) c = tolower(c);
                if (lowerName.find(lowerFilter) == std::string::npos) continue;
            }
            if (seen.insert(folderName).second) {
                int fileCount = 0;
                const char* exts[] = { "\\*.vgm", "\\*.vgz", "\\*.VGM", "\\*.VGZ" };
                for (int e = 0; e < 4; e++) {
                    std::string searchPath = s_folderHistory[i] + exts[e];
                    std::wstring wSearchPath = MidiPlayer::UTF8ToWide(searchPath);
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW(wSearchPath.c_str(), &fd);
                    if (h != INVALID_HANDLE_VALUE) {
                        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) fileCount++; } while (FindNextFileW(h, &fd));
                        FindClose(h);
                    }
                }
                entries.push_back({folderName, i, fileCount});
            }
        }
        if (s_histSortMode == 1) {
            std::sort(entries.begin(), entries.end(), [](const HistEntry& a, const HistEntry& b) { return a.fileCount > b.fileCount; });
        }

        ImGui::Separator();
        float historyHeight = ImGui::GetContentRegionAvail().y - 5;
        ImGui::BeginChild("AYHistoryRegion", ImVec2(0, historyHeight), true, ImGuiWindowFlags_HorizontalScrollbar);

        if (entries.empty()) {
            ImGui::TextDisabled("No matching folders...");
        } else {
            for (const auto& e : entries) {
                const std::string& path = s_folderHistory[e.idx];
                ImGui::PushID(e.idx);
                char label[512];
                if (e.fileCount > 0)
                    snprintf(label, sizeof(label), "[%d] %s", e.fileCount, e.name.c_str());
                else
                    snprintf(label, sizeof(label), "[DIR] %s", e.name.c_str());
                if (ImGui::Selectable(label, false)) NavigateTo(path.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Remove from history")) {
                        s_folderHistory.erase(s_folderHistory.begin() + e.idx);
                        SaveConfig();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
}

static void RenderMain(void) {
    float pianoHeight      = 150;
    float levelMeterHeight = 200;
    float statusAreaWidth  = 460;
    float topSectionHeight = pianoHeight + levelMeterHeight;

    // Top section: Piano + LevelMeters (left) | Registers (right)
    ImGui::BeginGroup();
    ImGui::BeginChild("AY_PianoArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, pianoHeight), false);
    RenderPianoKeyboard();
    ImGui::EndChild();
    ImGui::BeginChild("AY_LevelMeterArea", ImVec2(ImGui::GetContentRegionAvail().x - statusAreaWidth, levelMeterHeight), false);
    RenderLevelMeters();
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginChild("AY_StatusArea", ImVec2(statusAreaWidth - 10, topSectionHeight), false);
    RenderRegisterTable();
    ImGui::EndChild();

    // Bottom section: Player + FileBrowser | Log+History
    ImGui::BeginChild("AY_BottomLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), true);
    RenderPlayerBar();
    RenderFileBrowser();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("AY_BottomRight", ImVec2(0, 0), true);
    RenderLogPanel();
    ImGui::EndChild();

    // Scope (optional, very bottom)
    RenderScopeArea();
}

void Render() {
    // Full-viewport window — fill the entire main window
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("AY8910(PSG)", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImGui::PopStyleVar(3);
    ChipDescriptions::RenderChipTooltip("AY8910(PSG)", "PSG - Programmable Sound Generator");
    ImGui::BeginChild("AY_LeftPane", ImVec2(300, 0), true);
    RenderSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("AY_RightPane", ImVec2(0, 0), false);
    RenderMain();
    ImGui::EndChild();
    ImGui::End();

    // Popups
    RenderTestPopup();
    VGMPlayerUI::RenderPlayerSettingsPopup(GetPlayerCallbacks());
}

bool WantsKeyboardCapture() { return false; }

} // namespace AY8910Window
