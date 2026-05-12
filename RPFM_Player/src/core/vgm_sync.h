// Stub: vgm_sync.h — simplified slot management for RPFM Player
// In RPFM, slot 0 = first AY8910, slot 1 = second AY8910 (if any)
#pragma once
#include <stdint.h>
#include <functional>
#include <string>

namespace VGMSync {

static const int MAX_SLOTS = 4;
static const int CHIP_NONE = 0;
static const int CHIP_AY8910 = 1;

// Callback types
typedef std::function<void(uint16_t, uint8_t)> ChipStateUpdateFn;
typedef std::function<void(uint8_t, uint16_t, uint8_t)> ChipHwWriteFn;
typedef std::function<void()> ChipFlushFn;
typedef std::function<void()> ChipApplyStateFn;
typedef std::function<void()> ChipResetFn;
typedef std::function<void(const char*)> LogFn;

// Slot chip type — in RPFM, all slots are AY8910
inline int& GetSlotChipRef(int slot) {
    static int s_chipTypes[MAX_SLOTS] = {CHIP_AY8910, CHIP_AY8910, CHIP_NONE, CHIP_NONE};
    return s_chipTypes[slot];
}
inline int GetSlotChip(int slot) { return GetSlotChipRef(slot); }
inline void SetSlotChip(int slot, int chip) { GetSlotChipRef(slot) = chip; }

// Chip labels (for UI)
static const char* CHIP_LABELS[] = {"None", "AY8910"};
static const int CHIP_LABEL_COUNT = 2;

// No-op stubs for unified playback (not used in standalone RPFM Player)
inline void SetFlushThreshold(int) {}
inline void PreviewAssignSlots(const char*) {}
inline void SetTimerMode(int) {}
inline void SetTotalSamples(uint32_t) {}
inline void StartUnifiedPlayback(const char*, int) {}
inline void StopUnifiedPlayback() {}
inline void PauseUnifiedPlayback() {}
inline bool IsUnifiedPaused() { return false; }
inline void LoadWindowSlotConfig(const char*, int* = nullptr) {}
inline void SaveWindowSlotConfig(const char*, int* = nullptr) {}
inline void NotifyFileOpened(const char*) {}

inline void RegisterChipWriter(int, ChipStateUpdateFn, ChipHwWriteFn, ChipFlushFn, ChipApplyStateFn, ChipResetFn) {}
inline void RegisterChipWriter2(int, ChipStateUpdateFn) {}
inline void RegisterLogFn(LogFn) {}

inline bool IsUnifiedPlaying() { return false; }
inline const char* GetSharedFilePath() { return ""; }
inline uint32_t GetCurrentSamples() { return 0; }
inline bool IsUnifiedTrackEnded() { return false; }
inline int GetLoopCount() { return 0; }
inline int GetMaxLoops() { return 2; }
inline void SetMaxLoops(int) {}
inline float GetFadeout() { return 0.0f; }
inline void SetFadeout(float) {}
inline void SeekUnifiedPlayback(uint32_t) {}

}
