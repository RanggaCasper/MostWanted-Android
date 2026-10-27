#pragma once
// harness only: the Win32 names portable runtime and game files touch
#include <cstdint>

typedef long HRESULT;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HWND;
typedef void* HCURSOR;
typedef const wchar_t* PCWSTR;
typedef unsigned int UINT;
typedef int BOOL;
#define WINAPI
#ifndef TRUE
#define TRUE 1
#endif

inline HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }
inline void* GetProcAddress(HMODULE, const char*) { return nullptr; }
inline HANDLE GetCurrentThread() { return nullptr; }

// window messages and keys a game's GameInfo::message names; no window sends them here
enum : uint32_t {
    WM_ACTIVATE = 0x0006, WM_SETCURSOR = 0x0020, WM_INPUT = 0x00FF, WM_KEYDOWN = 0x0100, WM_SYSKEYDOWN = 0x0104,
    WM_LBUTTONUP = 0x0202,
};
enum { WA_INACTIVE = 0, HTCLIENT = 1 };
enum { VK_RETURN = 0x0D, VK_ESCAPE = 0x1B, VK_F3 = 0x72, VK_F10 = 0x79, VK_OEM_3 = 0xC0 };
#define LOWORD(v) ((uint16_t)((uintptr_t)(v) & 0xFFFF))
#define MAKEINTRESOURCEW(i) ((const wchar_t*)(uintptr_t)(uint16_t)(i))
inline HCURSOR LoadCursorW(void*, const wchar_t*) { return nullptr; }
inline HCURSOR SetCursor(HCURSOR) { return nullptr; }
inline short GetAsyncKeyState(int) { return 0; }
