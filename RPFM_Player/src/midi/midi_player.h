// Stub: midi_player.h — file browser utilities for RPFM Player
#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <windows.h>

namespace MidiPlayer {

struct TextScrollState {
    float scrollOffset;
    float scrollDirection;
    float pauseTimer;
    std::chrono::steady_clock::time_point lastUpdateTime;
};

struct FileEntry {
    std::string name;
    std::string fullPath;
    bool isDirectory;
    FileEntry() : isDirectory(false) {}
};

inline std::wstring UTF8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

inline std::string WideToUTF8(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, NULL, NULL);
    return s;
}

inline std::vector<std::string> SplitPath(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] == '\\' || path[i] == '/') {
            if (i > start) parts.push_back(path.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < path.size()) parts.push_back(path.substr(start));
    return parts;
}

static inline void InitializeFileBrowser() {}
static inline void UpdateMIDIPlayback() {}

// Globals stubs
static HWND g_mainWindow = nullptr;
static bool g_isInputActive = false;
static bool g_isWindowDragging = false;
static bool g_enableGlobalMediaKeys = false;

}
