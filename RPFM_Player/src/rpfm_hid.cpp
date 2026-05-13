#include "rpfm_hid.h"
#include "rpfm_protocol.h"

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <hidpi.h>
#include <string.h>
#include <stdio.h>

static HANDLE s_handle = INVALID_HANDLE_VALUE;
static uint8_t s_seq = 0;

bool rpfm_hid_open(void) {
    if (s_handle != INVALID_HANDLE_VALUE) return true;

    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO dev_info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA iface_data;
    iface_data.cbSize = sizeof(iface_data);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, i, &iface_data); i++) {
        DWORD req_size = 0;
        SetupDiGetDeviceInterfaceDetailA(dev_info, &iface_data, NULL, 0, &req_size, NULL);

        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(req_size);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &iface_data, detail, req_size, NULL, NULL)) {
            free(detail);
            continue;
        }

        HANDLE h = CreateFileA(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
        free(detail);

        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr) &&
            attr.VendorID == RPFM_VID && attr.ProductID == RPFM_PID) {
            SetupDiDestroyDeviceInfoList(dev_info);
            s_handle = h;
            return true;
        }
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return false;
}

void rpfm_hid_close(void) {
    if (s_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(s_handle);
        s_handle = INVALID_HANDLE_VALUE;
    }
}

bool rpfm_hid_is_open(void) {
    return s_handle != INVALID_HANDLE_VALUE;
}

bool rpfm_hid_send_frame(uint8_t cmd, uint8_t seq,
                          const uint8_t *payload, uint8_t plen,
                          rpfm_resp_t *resp) {
    if (s_handle == INVALID_HANDLE_VALUE) return false;

    // Build 65-byte buffer: [report_id=0][frame 64 bytes]
    uint8_t buf[65];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0; // report ID
    rpfm_make_frame(buf + 1, cmd, seq, payload, plen);

    if (!HidD_SetOutputReport(s_handle, buf, 65))
        return false;

    // Read response only when explicitly requested (not during VGM playback)
    if (resp) {
        uint8_t rbuf[65];
        memset(rbuf, 0, sizeof(rbuf));
        rbuf[0] = 0;
        if (HidD_GetInputReport(s_handle, rbuf, 65)) {
            uint8_t *r = rbuf + 1; // skip report ID
            resp->ack_seq = r[0];
            resp->status = r[1];
            resp->buf_level = r[2] | (r[3] << 8);
        } else {
            memset(resp, 0, sizeof(*resp));
        }
    }

    return true;
}

bool rpfm_write_reg(uint8_t slot, uint8_t addr, uint8_t data, rpfm_resp_t *resp) {
    s_seq = (s_seq + 1) & 0xFF;
    uint8_t payload[3] = {slot, addr, data};
    return rpfm_hid_send_frame(CMD_WRITE_REG, s_seq, payload, 3, resp);
}

bool rpfm_write_reg_ay(uint8_t slot, uint8_t addr, uint8_t data, rpfm_resp_t *resp) {
    s_seq = (s_seq + 1) & 0xFF;
    uint8_t payload[3] = {slot, addr, data};
    return rpfm_hid_send_frame(CMD_WRITE_AY, s_seq, payload, 3, resp);
}

bool rpfm_send_bootsel(void) {
    return rpfm_hid_send_frame(CMD_BOOTSEL, 0, NULL, 0, NULL);
}

bool rpfm_send_reset(void) {
    uint8_t payload[1] = {0x0F};
    return rpfm_hid_send_frame(CMD_RESET, 1, payload, 1, NULL);
}

bool rpfm_set_ay_delay(uint8_t delay_100ns) {
    if (delay_100ns > 200) delay_100ns = 200;
    uint8_t payload[1] = {delay_100ns};
    return rpfm_hid_send_frame(CMD_SET_DELAY, 0, payload, 1, NULL);
}

bool rpfm_send_vgm_data(const uint8_t *data, uint8_t len, uint16_t *buf_level) {
    s_seq = (s_seq + 1) & 0xFF;
    rpfm_resp_t resp;
    bool ok = rpfm_hid_send_frame(CMD_VGM_DATA, s_seq, data, len, &resp);
    if (ok && buf_level) *buf_level = resp.buf_level;
    return ok;
}

bool rpfm_vgm_start(uint16_t loop_offset, uint8_t *status) {
    s_seq = (s_seq + 1) & 0xFF;
    uint8_t payload[2] = { (uint8_t)(loop_offset & 0xFF), (uint8_t)(loop_offset >> 8) };
    rpfm_resp_t resp;
    bool ok = rpfm_hid_send_frame(CMD_VGM_START, s_seq, payload, 2, &resp);
    if (ok && status) *status = resp.status;
    return ok;
}

bool rpfm_vgm_stop(void) {
    s_seq = (s_seq + 1) & 0xFF;
    return rpfm_hid_send_frame(CMD_VGM_STOP, s_seq, NULL, 0, NULL);
}
