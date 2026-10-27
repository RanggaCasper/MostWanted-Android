// IDirectSound: listener, effects image, work pump
#include "sound/sound.h"
#include "xbe/xbe.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace sound;

namespace kernel {
extern const xbe::Image* g_image;
}
namespace dispatch {
X86Func* Find(uint32_t guest);
}

// exports with a host version
static bool Handled(uint32_t va) {
    static const uint32_t handled[] = {
        SYM(DirectSoundCreate), SYM(IDirectSound_Release), SYM(DirectSoundUseFullHRTF), SYM(DirectSoundUseLightHRTF),
        SYM(IDirectSound_GetSpeakerConfig), SYM(IDirectSound_SetEffectData), SYM(IDirectSound_EnableHeadphones),
        SYM(IDirectSound_SetMixBinHeadroom), SYM(IDirectSound_CommitDeferredSettings),
        SYM(IDirectSound_SetDopplerFactor), SYM(IDirectSound_SetRolloffFactor), SYM(IDirectSound_SetDistanceFactor),
        SYM(IDirectSound_SetVelocity), SYM(IDirectSound_SetI3DL2Listener), SYM(IDirectSound_SetPosition),
        SYM(IDirectSound_SetOrientation), SYM(IDirectSound_SynchPlayback), SYM(DirectSoundDoWork),
        SYM(IDirectSound_DownloadEffectsImage), SYM(IDirectSound_GetEffectData), SYM(IDirectSound_CommitEffectData),
        SYM(IDirectSound_GetOutputLevels), SYM(IDirectSoundBuffer_SetPlayRegion),
        SYM(IDirectSoundBuffer_SetAllParameters), SYM(IDirectSoundBuffer_SetFormat),
        SYM(IDirectSoundBuffer_SetOutputBuffer), SYM(IDirectSoundBuffer_SetRolloffCurve),
        SYM(IDirectSoundBuffer_StopEx), SYM(XAudioDownloadEffectsImage), SYM(DirectSoundCreateBuffer),
        SYM(IDirectSound_CreateSoundBuffer), SYM(IDirectSoundBuffer_Release), SYM(IDirectSoundBuffer_Unlock),
        SYM(IDirectSoundBuffer_SetBufferData), SYM(IDirectSoundBuffer_Play), SYM(IDirectSoundBuffer_Stop),
        SYM(IDirectSoundBuffer_SetLoopRegion), SYM(IDirectSoundBuffer_GetStatus),
        SYM(IDirectSoundBuffer_GetCurrentPosition), SYM(IDirectSoundBuffer_SetCurrentPosition),
        SYM(IDirectSoundBuffer_Lock), SYM(IDirectSoundBuffer_SetVolume), SYM(IDirectSoundBuffer_SetFrequency),
        SYM(IDirectSoundBuffer_SetFilter), SYM(IDirectSoundBuffer_SetHeadroom),
        SYM(IDirectSoundBuffer_SetMixBinVolumes), SYM(IDirectSoundBuffer_SetMixBins),
        SYM(IDirectSoundBuffer_SetMaxDistance), SYM(IDirectSoundBuffer_SetMinDistance),
        SYM(IDirectSoundBuffer_SetPosition), SYM(IDirectSoundBuffer_SetVelocity),
        SYM(IDirectSoundBuffer_SetI3DL2Source), SYM(DirectSoundCreateStream), SYM(CDirectSoundStream_AddRef),
        SYM(CDirectSoundStream_SetFormat), SYM(CDirectSoundStream_Release), SYM(CDirectSoundStream_GetInfo),
        SYM(CDirectSoundStream_GetStatus), SYM(CDirectSoundStream_Process), SYM(CDirectSoundStream_Discontinuity),
        SYM(CDirectSoundStream_Flush), SYM(CDirectSoundStream_FlushEx), SYM(CDirectSoundStream_Pause),
        SYM(CDirectSoundStream_SetVolume), SYM(CDirectSoundStream_SetFrequency), SYM(CDirectSoundStream_SetFilter),
        SYM(CDirectSoundStream_SetHeadroom), SYM(CDirectSoundStream_SetMixBinVolumes),
        SYM(CDirectSoundStream_SetMixBins), SYM(CDirectSoundStream_SetMaxDistance),
        SYM(CDirectSoundStream_SetMinDistance), SYM(CDirectSoundStream_SetPosition),
        SYM(CDirectSoundStream_SetVelocity), SYM(CDirectSoundStream_SetI3DL2Source)};
    return std::find(std::begin(handled), std::end(handled), va) != std::end(handled);
}

constexpr uint32_t kDsoundLibraryId = 0xB;  // DSOUND's library id, the high word of an .XTLID id
constexpr uint8_t kJmpRel32 = 0xE9;

// DSOUND objects are the host's; .XTLID names linked exports that would still run console code on them
static void AuditExports() {
    std::string left;
    // the library's own section: the WMA decoder shares the export ids and touches no DSOUND object
    uint32_t lo = 0;
    uint32_t hi = 0;
    for (auto& s : g_image->sections) {
        if (SYM(DirectSoundCreate) - s.va >= s.vsize) continue;
        lo = s.va;
        hi = s.va + s.vsize;
    }
    for (auto& s : g_image->sections) {
        if (s.name != ".XTLID") continue;
        for (uint32_t o = 0; o + 8 <= std::min(s.vsize, s.rsize); o += 8) {
            uint32_t id = g_image->U32(s.raw + o);
            uint32_t va = g_image->U32(s.raw + o + 4);
            if (id >> 16 != kDsoundLibraryId || va < lo || va >= hi) continue;
            if (!dispatch::Find(va) || Handled(va)) continue;
            // a jmp to a host version is covered by it
            if (G<uint8_t>(va) == kJmpRel32 && Handled(va + 5 + R32(va + 1))) continue;
            char b[24];
            std::snprintf(b, sizeof b, " %05X at %08X", id, va);
            left += b;
        }
    }
    if (!left.empty()) LOGW("dsound: exports running their original code:%s", left.c_str());
}

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        AuditExports();
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
HLE_STDCALL(DirectSoundUseLightHRTF, 0) { return 0; }
// DSSPEAKER as the library keeps it (0x4CDB34 in Double Agent): the dashboard's XC_AUDIO_FLAGS
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    constexpr uint32_t kStereo = 0;
    constexpr uint32_t kMono = 1;  // DSSPEAKER_MONO
    constexpr uint32_t kSurroundAc3 = 0x2 | 0x10000;  // DSSPEAKER_SURROUND | DSSPEAKER_ENABLE_AC3
    uint32_t config = kStereo;
    if (audio::IsMono()) config = kMono;
    if (audio::IsSurround()) config = kSurroundAc3;
    W32(a[1], config);
    return 0;
}
HLE_STDCALL(IDirectSound_SetEffectData, 6) { return 0; }
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_headphones = a[1] != 0;
    return 0;
}
// (this, mix bin, headroom)
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (a[1] < 32) g_bin_headroom[a[1]] = a[2] & 7;
    return 0;
}
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
// (this, DSI3DL2LISTENER, apply): room, room HF, reflections and reverb are integers
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) {
    if (!a[1]) return 0;
    float p[12];
    for (int i = 0; i < 12; i++) {
        uint32_t v = R32(a[1] + i * 4);
        bool integer = i < 2 || i == 5 || i == 7;
        p[i] = integer ? float(int32_t(v)) : F32(v);
    }
    LOG_FIRST(32, "dsound: listener room %.0f reflections %.0f after %.3f s, reverb %.0f after %.3f s, decay %.2f s",
              p[0], p[5], p[6], p[7], p[8], p[3]);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_reverb.Set(p);
    return 0;
}
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_listener[i] = F32(a[1 + i]);
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) {
        g_front[i] = F32(a[1 + i]);
        g_top[i] = F32(a[4 + i]);
    }
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        done.assign(g_done.begin(), g_done.end());
        g_done.clear();
    }
    Run(ctx, done);
    return 0;
}
// starts together every voice played or unpaused with the synch playback flag
HLE_STDCALL(IDirectSound_SynchPlayback, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) v.sync = false;
    return 0;
}

// Effects run on the console's DSP, of those only the I3DL2 reverb is reproduced.
// the descriptor keeps the image's effect list so the title finds its indices, every segment points at zeroed memory
static uint32_t EffectsDesc(const std::vector<uint8_t>& img, const char* name) {
    XDK_REQUIRE("dsound", XDK_KEY(EffectsImage_Skip0), XDK_KEY(EffectsImage_Skip1), XDK_KEY(EffectsImage_Desc));
    auto u32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4);
        return v;
    };
    size_t t = XDK_EffectsImage_Desc + size_t(u32(XDK_EffectsImage_Skip0) + u32(XDK_EffectsImage_Skip1)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name, count);
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    // count, effect list, then per effect 4 segments of (address, size)
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    return desc;
}

HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for (auto& sec : g_image->sections) {
            if (sec.name != name) continue;
            const uint8_t* at = static_cast<uint8_t*>(HostPtr(sec.va));
            img.assign(at, at + sec.vsize);
        }
    } else {
        ReadGameFile(name, img);
    }
    uint32_t desc = EffectsDesc(img, name.c_str());
    if (a[3]) W32(a[3], desc);
    return 0;
}

// (this, image, size, location, descriptor out): the title loaded the image itself
HLE_STDCALL(IDirectSound_DownloadEffectsImage, 5) {
    const uint8_t* at = static_cast<uint8_t*>(HostPtr(a[1]));
    std::vector<uint8_t> img(at, at + a[2]);
    uint32_t desc = EffectsDesc(img, "from memory");
    if (a[4]) W32(a[4], desc);
    return 0;
}
// (this, effect, offset, data, size): SetEffectData keeps nothing, so effect memory reads as zero
HLE_STDCALL(IDirectSound_GetEffectData, 5) {
    if (a[3]) std::memset(HostPtr(a[3]), 0, a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_CommitEffectData, 1) { return 0; }
// (this, DSOUTPUTLEVELS out, reset peaks): 16 dwords of meters, silent
HLE_STDCALL(IDirectSound_GetOutputLevels, 3) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 64);
    return 0;
}
