// Stub: modizer_viz.h — oscilloscope (no-op in RPFM Player)
#pragma once

struct ImDrawList;

class ModizerViz {
public:
    void Init() {}
    void ResetOffsets() {}
    void DrawChannel(int, ImDrawList*, float, float, float, float, float,
                     unsigned int, bool, float, int, int, int, bool, bool, int, bool) {}
};
