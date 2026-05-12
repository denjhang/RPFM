/*
 * RPFM HID debug tool — Windows only, no dependencies
 * Compile: gcc -o hid_test.exe hid_test.c -lhid -lsetupapi
 *
 * Usage:
 *   hid_test.exe              — scan for RPFM device
 *   hid_test.exe write <slot> <addr> <data>  — write register
 *   hid_test.exe ay           — play AY8910 test tone
 *   hid_test.exe ym           — play YM2413 test tone
 *   hid_test.exe reset        — IC# reset
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <hidpi.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

#define VID 0x2E8A
#define PID 0x1090

// RPFM protocol
#define CMD_WRITE_REG  0x01
#define CMD_RESET      0x03
#define CMD_BOOTSEL    0x20
#define CMD_NOP        0xFF

static uint8_t crc8(const uint8_t *data, int len) {
    uint8_t crc = 0x00;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x31;
            else crc <<= 1;
        }
    }
    return crc;
}

static HANDLE find_and_open_rpfm(void) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO dev_info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed\n");
        return INVALID_HANDLE_VALUE;
    }

    SP_DEVICE_INTERFACE_DATA iface_data;
    iface_data.cbSize = sizeof(iface_data);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, i, &iface_data); i++) {
        DWORD req_size = 0;
        SetupDiGetDeviceInterfaceDetailA(dev_info, &iface_data, NULL, 0, &req_size, NULL);

        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail = malloc(req_size);
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
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == VID && attr.ProductID == PID) {
            SetupDiDestroyDeviceInfoList(dev_info);
            printf("Found RPFM: VID=%04X PID=%04X\n", attr.VendorID, attr.ProductID);
            return h;
        }
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return INVALID_HANDLE_VALUE;
}

static BOOL send_frame(HANDLE h, uint8_t cmd, uint8_t seq, const uint8_t *payload, uint8_t plen) {
    uint8_t buf[65];  // [report_id, frame[64]]
    memset(buf, 0, sizeof(buf));
    buf[0] = 0;  // report ID
    buf[1] = cmd;
    buf[2] = seq;
    buf[3] = plen;
    if (plen > 0 && payload)
        memcpy(buf + 4, payload, plen);
    buf[64] = crc8(buf + 1, 63);

    // Use SetOutputReport for devices without OUT endpoint
    return HidD_SetOutputReport(h, buf, 65);
}

static BOOL recv_frame(HANDLE h, uint8_t *resp) {
    // Use GetInputReport for quick status read
    memset(resp, 0, 65);
    resp[0] = 0;  // report ID
    return HidD_GetInputReport(h, resp, 65);
}

static void cmd_write_reg(HANDLE h, uint8_t slot, uint8_t addr, uint8_t data) {
    static uint8_t seq = 0;
    uint8_t payload[3] = {slot, addr, data};
    seq = (seq + 1) & 0xFF;

    if (!send_frame(h, CMD_WRITE_REG, seq, payload, 3)) {
        printf("Write failed\n");
        return;
    }

    uint8_t resp[65] = {0};
    if (recv_frame(h, resp)) {
        // resp[0] = report ID, resp[1..] = frame data
        uint8_t *r = resp + 1;
        printf("ACK seq=%d st=0x%02X | raw[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]\n",
            r[0], r[1],
            r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13]);
    } else {
        printf("No response (timeout)\n");
    }
    Sleep(10);  // 10ms between writes
}

static void cmd_reset(HANDLE h) {
    uint8_t payload[1] = {0x0F};  // all slots
    if (!send_frame(h, CMD_RESET, 1, payload, 1))
        printf("Reset failed\n");
    else
        printf("Reset sent\n");
}

static void cmd_ay8910_test(HANDLE h) {
    uint8_t slot = 0;
    // Reference: AY-3-8910_test.ino, 2MHz clock
    // period=223 fine + 1 coarse = 479, freq ≈ 2M/(16*479) ≈ 261Hz (C4)
    int fine = 223;
    int coarse = 1;

    printf("AY8910 test tone: C4 (~261Hz), slot %d, period=%d\n", slot, fine + coarse * 256);

    // Enable tone on channel A only (bit0=0 = tone A on)
    cmd_write_reg(h, slot, 0x07, 0x3E);
    // Volume channel A = max
    cmd_write_reg(h, slot, 0x08, 0x0F);
    // Tone period channel A
    cmd_write_reg(h, slot, 0x00, fine);
    cmd_write_reg(h, slot, 0x01, coarse);

    printf("Playing... press Enter to stop\n");
    getchar();

    // Mute
    cmd_write_reg(h, slot, 0x07, 0x3F);
    printf("Muted\n");
}

static void cmd_ym2413_test(HANDLE h) {
    uint8_t slot = 0;
    printf("YM2413 test tone: Organ C4 (261Hz), slot %d\n", slot);

    // Set instrument + volume for channel 0
    cmd_write_reg(h, slot, 0x30, 0x80);  // instrument=8(Organ), vol=0(max)
    // Frequency: C4, block=3, fnum=262
    cmd_write_reg(h, slot, 0x10, 0x06);  // fnum low
    cmd_write_reg(h, slot, 0x20, 0x27);  // block=3, fnum high=1, key off
    cmd_write_reg(h, slot, 0x20, 0x37);  // key on

    printf("Playing... press Enter to stop\n");
    getchar();

    // Key off
    cmd_write_reg(h, slot, 0x20, 0x17);
    printf("Stopped\n");
}

int main(int argc, char *argv[]) {
    HANDLE h = find_and_open_rpfm();
    if (h == INVALID_HANDLE_VALUE) {
        printf("RPFM device not found (VID=%04X PID=%04X)\n", VID, PID);
        return 1;
    }

    if (argc < 2) {
        printf("Commands:\n");
        printf("  write <slot> <addr> <data>  — write register\n");
        printf("  ay                          — AY8910 test tone\n");
        printf("  ym                          — YM2413 test tone\n");
        printf("  reset                       — IC# reset\n");
        printf("  bootsel                     — enter BOOTSEL mode\n");
        CloseHandle(h);
        return 0;
    }

    if (strcmp(argv[1], "write") == 0 && argc >= 5) {
        cmd_write_reg(h, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    } else if (strcmp(argv[1], "ay") == 0) {
        cmd_ay8910_test(h);
    } else if (strcmp(argv[1], "ym") == 0) {
        cmd_ym2413_test(h);
    } else if (strcmp(argv[1], "reset") == 0) {
        cmd_reset(h);
    } else if (strcmp(argv[1], "bootsel") == 0) {
        printf("Sending BOOTSEL...\n");
        send_frame(h, CMD_BOOTSEL, 0, NULL, 0);
        printf("Device should enter BOOTSEL mode\n");
        // Don't close handle — device is gone
        return 0;
    } else {
        printf("Unknown command: %s\n", argv[1]);
    }

    CloseHandle(h);
    return 0;
}
