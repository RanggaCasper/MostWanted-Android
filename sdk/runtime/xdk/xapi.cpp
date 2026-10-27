// XAPI functions that talk to hardware: controller ports and the hard disk cache partition
#include <chrono>

#include "core/input.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace {

// XPP device type records: CurrentConnected, ChangeConnected, PreviousConnected
constexpr uint32_t kGamepadType = SYM(XPP_GamepadType);
constexpr uint32_t kMemoryUnitType = SYM(XPP_MemoryUnitType);

std::mutex g_ports_lock;
bool g_poller = false;

void UpdatePorts() {
    std::lock_guard<std::mutex> lock(g_ports_lock);
    uint32_t cur = 0;
    for (int p = 0; p < 4; p++)
        if (input::Poll(p).connected) cur |= 1u << p;
    uint32_t old = R32(kGamepadType + XDK_XPP_Current);
    if (cur != old) {
        W32(kGamepadType + XDK_XPP_Changed, R32(kGamepadType + XDK_XPP_Changed) | (cur ^ old));
        W32(kGamepadType + XDK_XPP_Current, cur);
        LOGI("xapi: controller ports now %X", cur);
    }
}

struct Handle {
    uint32_t port;
    uint32_t packet;
    input::Pad last;
};
std::unordered_map<uint32_t, Handle> g_handles;

}  // namespace

HLE_STDCALL(XMountUtilityDrive, 1) {
    CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    return 1;
}

HLE_STDCALL(XInitDevices, 2) {
    XDK_REQUIRE("xapi", XDK_KEY(XPP_Current), XDK_KEY(XPP_Changed), XDK_KEY(XPP_Previous), XDK_DATA(XPP_GamepadType));
    W32(kGamepadType + XDK_XPP_Current, 0);
    W32(kGamepadType + XDK_XPP_Changed, 0);
    W32(kGamepadType + XDK_XPP_Previous, 0);
    if constexpr (HAS_XPP_MemoryUnitType != 0) {  // titles without memory unit support have no record
        W32(kMemoryUnitType + XDK_XPP_Current, 0);
        W32(kMemoryUnitType + XDK_XPP_Changed, 0);
        W32(kMemoryUnitType + XDK_XPP_Previous, 0);
    }
    // no UpdatePorts here: on the console the pads enumerate after the init, and a title that takes the devices
    // XGetDevices finds then (none) opens the rest as XGetDeviceChanges reports them inserted (Shaolin Monks)
    if (!g_poller) {
        g_poller = true;
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                UpdatePorts();
            }
        }).detach();
    }
    return 0;
}

HLE_STDCALL(XInputOpen, 4) {
    uint32_t type = a[0], port = a[1];
    LOG_FIRST(16, "xapi: XInputOpen type %08X port %u", type, port);
    if (type != kGamepadType || port > 3) return 0;
    uint32_t h = mem::AllocSystem(16);
    std::lock_guard<std::mutex> lock(g_ports_lock);
    g_handles[h] = {port, 0, {}};
    return h;
}

HLE_STDCALL(XInputClose, 1) {
    std::lock_guard<std::mutex> lock(g_ports_lock);
    g_handles.erase(a[0]);
    mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(XInputGetState, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;  // ERROR_INVALID_HANDLE
        port = it->second.port;
    }
    input::Pad p = input::Poll(int(port));
    LOG_FIRST(8, "xapi: XInputGetState port %u connected %d", port, int(p.connected));
    if (p.buttons || p.analog[0]) LOG_FIRST(16, "xapi: port %u buttons %04X A %02X", port, p.buttons, p.analog[0]);
    if (!p.connected) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
    std::lock_guard<std::mutex> lock(g_ports_lock);
    Handle& h = g_handles[a[0]];
    if (std::memcmp(&p, &h.last, sizeof(p))) h.packet++;
    h.last = p;
    uint32_t s = a[1];
    W32(s, h.packet);  // XINPUT_STATE: packet, buttons, 8 analog buttons, thumbs
    W16(s + 4, p.buttons);
    std::memcpy(HostPtr(s + 6), p.analog, 8);
    W16(s + 14, uint16_t(p.lx));
    W16(s + 16, uint16_t(p.ly));
    W16(s + 18, uint16_t(p.rx));
    W16(s + 20, uint16_t(p.ry));
    return 0;
}

// XINPUT_CAPABILITIES, packed to 25 bytes: SubType, Reserved (2), the gamepad's controls (wButtons, 8 analog
// buttons, 4 thumb axes) and the rumble's motors, each nonzero where the pad has it. The library's own reads its
// device extension, which a host handle has none of
#if HAS_XInputGetCapabilities
HLE_STDCALL(XInputGetCapabilities, 2) {
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        if (!g_handles.count(a[0])) return 6;  // ERROR_INVALID_HANDLE
    }
    uint32_t c = a[1];
    std::memset(HostPtr(c), 0, 25);
    *static_cast<uint8_t*>(HostPtr(c)) = 1;  // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    W16(c + 3, 0x00FF);    // dpad, start, back, thumbs
    std::memset(HostPtr(c + 5), 0xFF, 8);
    std::memset(HostPtr(c + 13), 0xFF, 8);
    W16(c + 21, 0xFFFF);  // rumble, both motors
    W16(c + 23, 0xFFFF);
    return 0;
}
#endif

HLE_STDCALL(XInputSetState, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;
        port = it->second.port;
    }
    uint32_t fb = a[1];
    // XINPUT_FEEDBACK_HEADER is dwStatus, hEvent and 58 reserved bytes, the rumble follows
    input::Rumble(int(port), R16(fb + 66), R16(fb + 68));
    W32(fb, 0);
    if (uint32_t ev = R32(fb + 4))
        if (ObjectRef o = FromHandle(ev)) SetEvent(*o);
    return 997;  // ERROR_IO_PENDING, already complete
}

// A recompiled image launches through the XAPI, then a process restart into its executable; any other
// (the dashboard, updates) is refused and the title stays in its menu.
X86_EXTERN_FUNC(GUEST_ORIG(XLaunchNewImageA));
X86_FUNC(GUEST(XLaunchNewImageA)) {
    std::string path = ReadCString(R32(ctx.esp + 4));
    if (ImageExe(path)) {
        GUEST_ORIG(XLaunchNewImageA)(ctx, base);
        return;
    }
    LOGW("xapi: launch of %s refused, it is not recompiled", path.c_str());
    Return(ctx, 2, 2);  // ERROR_FILE_NOT_FOUND
}
