/*
 * RPFM Serial Monitor - Win32 GDI
 * Reads USB CDC serial output from RP2350A and displays in a scrollable window.
 *
 * Build (MSYS2 mingw64 or Windows gcc):
 *   gcc -o rpfm_monitor.exe rpfm_monitor.c -mwindows
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>

#define ID_EDIT      1001
#define ID_CONNECT   1002
#define ID_CLEAR     1003
#define ID_PORT      1004
#define ID_BAUD      1005
#define ID_STATUS    1006
#define ID_RESCAN    1007
#define ID_COPY      1008
#define WM_SERIAL_DATA (WM_USER + 1)

#define MAX_LINES   8192
#define LINE_BUF    1024
#define READ_SIZE   4096
#define MAX_PORTS   64

static const char CLASS_NAME[] = "RPFMMonitor";

static HWND hEdit, hPortCombo, hBaud, hConnect, hClearBtn, hStatus, hRescanBtn, hCopyBtn;
static char lastPort[64] = "";
static HFONT hFont, hFontUI;
static HANDLE hSerial = INVALID_HANDLE_VALUE;
static OVERLAPPED ovRead;
static char readBuf[READ_SIZE];
static HWND hMainWnd;
static char ports[MAX_PORTS][64];
static int portCount = 0;

/* --- Line buffer --- */

static char *lines[MAX_LINES];
static int lineCount = 0;

static void clear_lines(void) {
    for (int i = 0; i < lineCount; i++) {
        HeapFree(GetProcessHeap(), 0, lines[i]);
    }
    lineCount = 0;
}

static void add_line(const char *text) {
    int len = (int)strlen(text);
    if (len == 0) return;

    if (lineCount >= MAX_LINES) {
        HeapFree(GetProcessHeap(), 0, lines[0]);
        memmove(&lines[0], &lines[1], (MAX_LINES - 1) * sizeof(char *));
        lineCount = MAX_LINES - 1;
    }

    lines[lineCount] = HeapAlloc(GetProcessHeap(), 0, len + 1);
    memcpy(lines[lineCount], text, len + 1);
    lineCount++;

    int totalLen = 0;
    for (int i = 0; i < lineCount; i++) {
        totalLen += (int)strlen(lines[i]) + 2;
    }

    char *buf = HeapAlloc(GetProcessHeap(), 0, totalLen + 1);
    char *p = buf;
    for (int i = 0; i < lineCount; i++) {
        int l = (int)strlen(lines[i]);
        memcpy(p, lines[i], l);
        p += l;
        *p++ = '\r';
        *p++ = '\n';
    }
    *p = '\0';

    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    GetScrollInfo(hEdit, SB_VERT, &si);
    BOOL atBottom = (si.nPos + (int)si.nPage >= si.nMax - 1);

    SendMessage(hEdit, WM_SETTEXT, 0, (LPARAM)buf);

    if (atBottom) {
        SendMessage(hEdit, WM_VSCROLL, SB_BOTTOM, 0);
    }

    HeapFree(GetProcessHeap(), 0, buf);
}

static void add_raw_data(const char *data, int len) {
    static char accum[LINE_BUF];
    static int accumLen = 0;

    for (int i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') {
            accum[accumLen] = '\0';
            add_line(accum);
            accumLen = 0;
        } else if (c == '\r') {
            /* skip */
        } else {
            if (accumLen < LINE_BUF - 2) {
                accum[accumLen++] = c;
            }
        }
    }
}

/* --- Registry: remember last port --- */

static const char *REG_KEY = "Software\\RPFMMonitor";

static void save_last_port(const char *port) {
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0,
                        KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "LastPort", 0, REG_SZ,
                       (const BYTE *)port, (DWORD)(strlen(port) + 1));
        RegCloseKey(hKey);
    }
}

static void load_last_port(void) {
    lastPort[0] = '\0';
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(lastPort) - 1;
        DWORD type;
        if (RegQueryValueExA(hKey, "LastPort", NULL, &type,
                             (LPBYTE)lastPort, &size) == ERROR_SUCCESS && type == REG_SZ) {
            lastPort[size] = '\0';
        } else {
            lastPort[0] = '\0';
        }
        RegCloseKey(hKey);
    }
}

/* --- COM Port enumeration --- */

static void scan_com_ports(void) {
    portCount = 0;
    memset(ports, 0, sizeof(ports));

    /* Method 1: SetupAPI - enumerate all serial ports */
    HDEVINFO hDevInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL,
                                             DIGCF_PRESENT);
    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devInfo;
        devInfo.cbSize = sizeof(devInfo);
        for (int i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfo); i++) {
            char friendlyName[256] = {0};
            if (SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devInfo,
                    SPDRP_FRIENDLYNAME, NULL, (PBYTE)friendlyName,
                    sizeof(friendlyName), NULL)) {
                /* Extract COM port number from friendly name like "USB Serial Device (COM10)" */
                char *com = strstr(friendlyName, "(COM");
                if (com) {
                    com += 1; /* skip '(' */
                    char *end = strchr(com, ')');
                    if (end) {
                        int len = (int)(end - com);
                        if (portCount < MAX_PORTS && len < 63) {
                            memcpy(ports[portCount], com, len);
                            ports[portCount][len] = '\0';
                            portCount++;
                        }
                    }
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    /* Method 2: Registry fallback - get any ports missed by SetupAPI */
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char valName[256], portData[64];
        DWORD idx = 0, nameSize, dataSize, type;
        while (1) {
            nameSize = sizeof(valName);
            dataSize = sizeof(portData);
            if (RegEnumValueA(hKey, idx++, valName, &nameSize, NULL, &type,
                              (LPBYTE)portData, &dataSize) != ERROR_SUCCESS) break;
            if (type != REG_SZ) continue;
            /* Check if already in list */
            int found = 0;
            for (int j = 0; j < portCount; j++) {
                if (strcmp(ports[j], portData) == 0) { found = 1; break; }
            }
            if (!found && portCount < MAX_PORTS) {
                strncpy(ports[portCount], portData, 63);
                ports[portCount][63] = '\0';
                portCount++;
            }
        }
        RegCloseKey(hKey);
    }
}

static void refresh_port_combo(void) {
    int prevSel = (int)SendMessage(hPortCombo, CB_GETCURSEL, 0, 0);
    char prevPort[64] = "";
    if (prevSel >= 0) {
        SendMessage(hPortCombo, CB_GETLBTEXT, prevSel, (LPARAM)prevPort);
    }

    SendMessage(hPortCombo, CB_RESETCONTENT, 0, 0);

    scan_com_ports();

    int newSel = 0;
    int matchedPrev = 0;
    for (int i = 0; i < portCount; i++) {
        SendMessage(hPortCombo, CB_ADDSTRING, 0, (LPARAM)ports[i]);
        if (strlen(prevPort) > 0 && strcmp(ports[i], prevPort) == 0) {
            newSel = i;
            matchedPrev = 1;
        }
    }

    /* If no previous selection, try to match the saved last port from registry */
    if (!matchedPrev && strlen(prevPort) == 0 && strlen(lastPort) > 0) {
        for (int i = 0; i < portCount; i++) {
            if (strcmp(ports[i], lastPort) == 0) {
                newSel = i;
                break;
            }
        }
    }

    if (portCount > 0) {
        SendMessage(hPortCombo, CB_SETCURSEL, newSel, 0);
    }
}

static const char *get_selected_port(void) {
    int sel = (int)SendMessage(hPortCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < portCount) {
        return ports[sel];
    }
    return NULL;
}

/* --- Serial --- */

static DWORD WINAPI serial_thread(LPVOID param) {
    (void)param;
    DWORD bytesRead;

    while (hSerial != INVALID_HANDLE_VALUE) {
        memset(&ovRead, 0, sizeof(ovRead));
        ovRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        BOOL ok = ReadFile(hSerial, readBuf, READ_SIZE - 1, &bytesRead, &ovRead);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForSingleObject(ovRead.hEvent, 500);
                if (waitResult == WAIT_OBJECT_0) {
                    if (GetOverlappedResult(hSerial, &ovRead, &bytesRead, FALSE) && bytesRead > 0) {
                        readBuf[bytesRead] = '\0';
                        PostMessage(hMainWnd, WM_SERIAL_DATA, bytesRead, 0);
                    }
                }
            } else {
                CloseHandle(ovRead.hEvent);
                break;
            }
        } else if (ok && bytesRead > 0) {
            readBuf[bytesRead] = '\0';
            PostMessage(hMainWnd, WM_SERIAL_DATA, bytesRead, 0);
        }

        CloseHandle(ovRead.hEvent);
    }
    return 0;
}

static BOOL open_serial(void) {
    if (hSerial != INVALID_HANDLE_VALUE) {
        CancelIo(hSerial);
        CloseHandle(hSerial);
        hSerial = INVALID_HANDLE_VALUE;
    }

    const char *port = get_selected_port();
    if (!port) {
        refresh_port_combo();
        MessageBoxA(hMainWnd, "No COM port found.\nRescanned ports.", "Error", MB_ICONERROR);
        return FALSE;
    }

    char fullPort[64];
    snprintf(fullPort, sizeof(fullPort), "\\\\.\\%s", port);

    hSerial = CreateFileA(fullPort, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Cannot open %s\nPath: %s\nError: %lu",
                 port, fullPort, err);
        MessageBoxA(hMainWnd, msg, "Error", MB_ICONERROR);
        refresh_port_combo();
        return FALSE;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    GetCommState(hSerial, &dcb);

    char baudText[32];
    GetWindowTextA(hBaud, baudText, sizeof(baudText));
    DWORD baud = atol(baudText);
    if (baud == 0) baud = 115200;

    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(hSerial, &dcb)) {
        CloseHandle(hSerial);
        hSerial = INVALID_HANDLE_VALUE;
        MessageBoxA(hMainWnd, "SetCommState failed", "Error", MB_ICONERROR);
        return FALSE;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    SetCommTimeouts(hSerial, &timeouts);

    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    char msg[128];
    snprintf(msg, sizeof(msg), "Connected: %s @ %lu", port, baud);
    save_last_port(port);
    SetWindowTextA(hStatus, msg);
    SetWindowTextA(hConnect, "Disconnect");
    EnableWindow(hPortCombo, FALSE);
    EnableWindow(hRescanBtn, FALSE);
    add_line(msg);
    add_line("---");

    CreateThread(NULL, 0, serial_thread, NULL, 0, NULL);
    return TRUE;
}

static void close_serial(void) {
    if (hSerial != INVALID_HANDLE_VALUE) {
        CancelIo(hSerial);
        CloseHandle(hSerial);
        hSerial = INVALID_HANDLE_VALUE;
    }
    SetWindowTextA(hConnect, "Connect");
    SetWindowTextA(hStatus, "Disconnected");
    EnableWindow(hPortCombo, TRUE);
    EnableWindow(hRescanBtn, TRUE);
}

/* --- Window layout --- */

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int m = 8;
    int bh = 26;
    int sh = 22;

    int topY = m;

    MoveWindow(GetDlgItem(hwnd, 2001), m, topY + 4, 32, bh - 4, TRUE);
    MoveWindow(hPortCombo, m + 34, topY, 120, 200, TRUE);
    MoveWindow(hRescanBtn, m + 158, topY, 50, bh, TRUE);

    MoveWindow(GetDlgItem(hwnd, 2002), m + 218, topY + 4, 32, bh - 4, TRUE);
    MoveWindow(hBaud, m + 250, topY, 70, bh, TRUE);

    MoveWindow(hConnect, m + 328, topY, 80, bh, TRUE);
    MoveWindow(hClearBtn, m + 414, topY, 60, bh, TRUE);
    MoveWindow(hCopyBtn, m + 480, topY, 60, bh, TRUE);

    int editY = topY + bh + m;
    MoveWindow(hEdit, m, editY, w - 2 * m, h - editY - sh - m, TRUE);
    MoveWindow(hStatus, 0, h - sh, w, sh, TRUE);
}

/* --- WndProc --- */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        hFont = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                            FIXED_PITCH | FF_MODERN, "Consolas");
        hFontUI = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        CreateWindowA("STATIC", "Port:", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                      0, 0, 32, 20, hwnd, (HMENU)2001, NULL, NULL);
        CreateWindowA("STATIC", "Baud:", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                      0, 0, 32, 20, hwnd, (HMENU)2002, NULL, NULL);

        hPortCombo = CreateWindowA("COMBOBOX", "",
                                   WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                                   WS_VSCROLL | CBS_AUTOHSCROLL,
                                   0, 0, 120, 200, hwnd, (HMENU)ID_PORT, NULL, NULL);

        hBaud = CreateWindowA("EDIT", "115200",
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                              0, 0, 70, 26, hwnd, (HMENU)ID_BAUD, NULL, NULL);

        hRescanBtn = CreateWindowA("BUTTON", "Scan",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 50, 26, hwnd, (HMENU)ID_RESCAN, NULL, NULL);

        hConnect = CreateWindowA("BUTTON", "Connect",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 80, 26, hwnd, (HMENU)ID_CONNECT, NULL, NULL);

        hClearBtn = CreateWindowA("BUTTON", "Clear",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 60, 26, hwnd, (HMENU)ID_CLEAR, NULL, NULL);

        hCopyBtn = CreateWindowA("BUTTON", "Copy",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 60, 26, hwnd, (HMENU)ID_COPY, NULL, NULL);

        hEdit = CreateWindowA("EDIT", "",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                              ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                              ES_READONLY,
                              0, 0, 400, 300, hwnd, (HMENU)ID_EDIT, NULL, NULL);

        hStatus = CreateWindowA("STATIC", "Disconnected",
                                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_SUNKEN,
                                0, 0, 400, 22, hwnd, (HMENU)ID_STATUS, NULL, NULL);

        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(hPortCombo, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hBaud, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hRescanBtn, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hConnect, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hClearBtn, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hCopyBtn, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hStatus, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(GetDlgItem(hwnd, 2001), WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(GetDlgItem(hwnd, 2002), WM_SETFONT, (WPARAM)hFontUI, TRUE);

        load_last_port();
        refresh_port_combo();
        layout(hwnd);
        return 0;
    }

    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        if ((HWND)lParam == hStatus) {
            SetBkColor(hdc, RGB(240, 240, 240));
            SetTextColor(hdc, RGB(0, 0, 0));
            return (INT_PTR)GetStockObject(LTGRAY_BRUSH);
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        if ((HWND)lParam == hEdit) {
            SetBkColor(hdc, RGB(20, 20, 30));
            SetTextColor(hdc, RGB(0, 220, 100));
            static HBRUSH hBr = NULL;
            if (!hBr) hBr = CreateSolidBrush(RGB(20, 20, 30));
            return (INT_PTR)hBr;
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_RESCAN:
            refresh_port_combo();
            break;
        case ID_CONNECT:
            if (hSerial != INVALID_HANDLE_VALUE) {
                close_serial();
            } else {
                open_serial();
            }
            break;
        case ID_CLEAR:
            clear_lines();
            SendMessage(hEdit, WM_SETTEXT, 0, (LPARAM)"");
            break;
        case ID_COPY: {
            int textLen = (int)SendMessage(hEdit, WM_GETTEXTLENGTH, 0, 0);
            if (textLen > 0) {
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE,
                                           textLen + 1);
                if (hMem) {
                    char *p = (char *)GlobalLock(hMem);
                    SendMessage(hEdit, WM_GETTEXT, textLen + 1, (LPARAM)p);
                    GlobalUnlock(hMem);
                    if (OpenClipboard(hwnd)) {
                        EmptyClipboard();
                        SetClipboardData(CF_TEXT, hMem);
                        CloseClipboard();
                    } else {
                        GlobalFree(hMem);
                    }
                }
            }
            break;
        }
        }
        break;

    case WM_SERIAL_DATA: {
        DWORD bytes = (DWORD)wParam;
        if (bytes > 0 && bytes < READ_SIZE) {
            readBuf[bytes] = '\0';
            add_raw_data(readBuf, bytes);
        }
        break;
    }

    case WM_DESTROY:
        close_serial();
        if (hFont) DeleteObject(hFont);
        if (hFontUI) DeleteObject(hFontUI);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    (void)hPrev; (void)cmdLine; (void)nShow;

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExA(&wc);

    hMainWnd = CreateWindowExA(0, CLASS_NAME, "RPFM Serial Monitor",
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 720, 500,
                               NULL, NULL, hInst, NULL);

    ShowWindow(hMainWnd, nShow);
    UpdateWindow(hMainWnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
