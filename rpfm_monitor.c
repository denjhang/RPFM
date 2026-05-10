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

#define ID_EDIT      1001
#define ID_CONNECT   1002
#define ID_CLEAR     1003
#define ID_PORT      1004
#define ID_BAUD      1005
#define ID_STATUS    1006
#define WM_SERIAL_DATA (WM_USER + 1)

#define MAX_LINES   8192
#define LINE_BUF    1024
#define READ_SIZE   4096

static const char CLASS_NAME[] = "RPFMMonitor";

static HWND hEdit, hPort, hBaud, hConnect, hClearBtn, hStatus;
static HFONT hFont, hFontUI;
static HANDLE hSerial = INVALID_HANDLE_VALUE;
static OVERLAPPED ovRead;
static char readBuf[READ_SIZE];
static HWND hMainWnd;

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

    /* Rebuild full text */
    int totalLen = 0;
    for (int i = 0; i < lineCount; i++) {
        totalLen += (int)strlen(lines[i]) + 2; /* \r\n */
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

    /* Remember if user was scrolled to bottom */
    int scrollPos = (int)SendMessage(hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    int lineCountVis = (int)SendMessage(hEdit, EM_GETLINECOUNT, 0, 0);
    BOOL atBottom = (scrollPos + 20 >= lineCountVis);

    SendMessage(hEdit, WM_SETTEXT, 0, (LPARAM)buf);

    if (atBottom) {
        SendMessage(hEdit, EM_LINESCROLL, 0, 999999);
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

    char port[64];
    GetWindowTextA(hPort, port, sizeof(port));
    if (strlen(port) == 0) {
        MessageBoxA(hMainWnd, "Enter COM port (e.g. COM3)", "Error", MB_ICONERROR);
        return FALSE;
    }

    char fullPort[64];
    snprintf(fullPort, sizeof(fullPort), "\\\\.\\%s", port);

    hSerial = CreateFileA(fullPort, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Cannot open %s\nError: %lu", port, GetLastError());
        MessageBoxA(hMainWnd, msg, "Error", MB_ICONERROR);
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
    SetWindowTextA(hStatus, msg);
    SetWindowTextA(hConnect, "Disconnect");
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
}

/* --- Auto-detect Pico COM port --- */

static void auto_detect_port(void) {
    /* Enumerate COM ports via registry */
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    char valName[256];
    char portData[64];
    DWORD idx = 0;
    DWORD nameSize, dataSize;
    DWORD type;

    while (1) {
        nameSize = sizeof(valName);
        dataSize = sizeof(portData);
        LONG ret = RegEnumValueA(hKey, idx++, valName, &nameSize, NULL, &type,
                                 (LPBYTE)portData, &dataSize);
        if (ret != ERROR_SUCCESS) break;

        /* Pico USB CDC usually shows up as \Device\USBSER000 */
        if (type == REG_SZ && strstr(valName, "Prolific") != NULL ||
            strstr(valName, "USBSER") != NULL ||
            strstr(valName, "Serial") != NULL ||
            strstr(valName, "CH340") != NULL ||
            strstr(valName, "Silabser") != NULL) {
            SetWindowTextA(hPort, portData);
            RegCloseKey(hKey);
            return;
        }
    }

    /* If no match, try the last COM port (most recently added) */
    idx = 0;
    char lastPort[64] = "";
    while (1) {
        nameSize = sizeof(valName);
        dataSize = sizeof(portData);
        LONG ret = RegEnumValueA(hKey, idx++, valName, &nameSize, NULL, &type,
                                 (LPBYTE)portData, &dataSize);
        if (ret != ERROR_SUCCESS) break;
        strncpy(lastPort, portData, sizeof(lastPort));
    }

    if (strlen(lastPort) > 0) {
        SetWindowTextA(hPort, lastPort);
    }

    RegCloseKey(hKey);
}

/* --- Window layout --- */

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int m = 8;     /* margin */
    int bh = 26;   /* button/edit height */
    int sh = 22;   /* status bar height */

    int topY = m;

    /* Label: Port */
    MoveWindow(GetDlgItem(hwnd, 2001), m, topY + 4, 32, bh - 4, TRUE);
    MoveWindow(hPort, m + 34, topY, 80, bh, TRUE);

    /* Label: Baud */
    MoveWindow(GetDlgItem(hwnd, 2002), m + 120, topY + 4, 32, bh - 4, TRUE);
    MoveWindow(hBaud, m + 152, topY, 70, bh, TRUE);

    MoveWindow(hConnect, m + 230, topY, 80, bh, TRUE);
    MoveWindow(hClearBtn, m + 316, topY, 60, bh, TRUE);

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

        /* Labels */
        CreateWindowA("STATIC", "Port:", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                      0, 0, 32, 20, hwnd, (HMENU)2001, NULL, NULL);
        CreateWindowA("STATIC", "Baud:", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                      0, 0, 32, 20, hwnd, (HMENU)2002, NULL, NULL);

        hPort = CreateWindowA("EDIT", "COM3",
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                              0, 0, 80, 26, hwnd, (HMENU)ID_PORT, NULL, NULL);

        hBaud = CreateWindowA("EDIT", "115200",
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                              0, 0, 70, 26, hwnd, (HMENU)ID_BAUD, NULL, NULL);

        hConnect = CreateWindowA("BUTTON", "Connect",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 80, 26, hwnd, (HMENU)ID_CONNECT, NULL, NULL);

        hClearBtn = CreateWindowA("BUTTON", "Clear",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 60, 26, hwnd, (HMENU)ID_CLEAR, NULL, NULL);

        hEdit = CreateWindowA("EDIT", "",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                              ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                              ES_READONLY,
                              0, 0, 400, 300, hwnd, (HMENU)ID_EDIT, NULL, NULL);

        hStatus = CreateWindowA("STATIC", "Disconnected",
                                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_SUNKEN,
                                0, 0, 400, 22, hwnd, (HMENU)ID_STATUS, NULL, NULL);

        /* Set fonts */
        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(hPort, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hBaud, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hConnect, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hClearBtn, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(hStatus, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(GetDlgItem(hwnd, 2001), WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessage(GetDlgItem(hwnd, 2002), WM_SETFONT, (WPARAM)hFontUI, TRUE);

        auto_detect_port();
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
                               CW_USEDEFAULT, CW_USEDEFAULT, 700, 500,
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
