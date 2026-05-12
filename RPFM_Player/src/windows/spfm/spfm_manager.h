// Stub: spfm_manager.h — maps SPFMManager to RPFM HID
#pragma once
#include "rpfm_hid.h"

namespace SPFMManager {

inline bool IsConnected() { return rpfm_hid_is_open(); }

inline bool ConnectDevice(int idx) {
    (void)idx;
    return rpfm_hid_open();
}

inline void DisconnectDevice(int idx, bool reset) {
    (void)idx; (void)reset;
    rpfm_hid_close();
}

inline void SendReset() {
    rpfm_send_reset();
}

inline void RawWrite(uint8_t* data, int len) {
    (void)data; (void)len;
    // Not needed for RPFM HID — register writes go through rpfm_write_reg_ay
}

inline void Init() {}
inline void Update() {}
inline void Shutdown() {}

}
