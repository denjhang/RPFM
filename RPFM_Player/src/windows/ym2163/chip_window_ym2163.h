// Stub: chip_window_ym2163.h — no-op for standalone RPFM Player
#pragma once

namespace YM2163Window {
inline void Init() {}
inline void Shutdown() {}
inline void Update() {}
inline void Render() {}
inline bool WantsKeyboardCapture() { return false; }
}
