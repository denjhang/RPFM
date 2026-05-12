#ifndef AY8910_WINDOW_H
#define AY8910_WINDOW_H

namespace AY8910Window {
void Init();
void Shutdown();
void Update();
void Render();
bool WantsKeyboardCapture();
void MuteAll();
}

#endif
