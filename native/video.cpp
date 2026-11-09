// A replay or cinematic mode saved as a video (src/common/video.h). The frames come as BGRA from the renderer, are made
// NV12 and given to the phone's H.264 encoder (NDK MediaCodec) on a thread of their own, and its output is written into
// an MP4 (MediaMuxer) in Movies/MostWanted, which the media scanner is then told of so the gallery shows it.
//
// A replay plays at a fixed 1/30 s a frame while it is saved, so frame n is at n / fps whatever the game's own frame
// rate. The sound under it (the race's, recorded by replay.cpp) is made AAC whole, and its packets written as the
// frames reach them: a muxer takes no track after it starts, and the video's format is known only with its first
// output.
//
// Cinematic mode's is live: the game runs at its own pace, so a frame is stamped with the time it was presented, at
// most fps a second, and the sound is the game's mix as the audio thread plays it, made AAC as it comes. A frame the
// encoder has no room for is dropped: waiting for it held the renderer, and the game with it.
//
// Its photos: the frame's BGRA to RGB rows, filtered (Sub) and deflated into a PNG on a thread of their own.
#include "common/video.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"

#if defined(__ANDROID__) || defined(HAVE_ZLIB)
#include <zlib.h>
#define VIDEO_ZLIB 1
#endif

#ifdef __ANDROID__
#include <fcntl.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#endif

namespace video {

namespace {

using Clock = std::chrono::steady_clock;

struct Pending {
    std::vector<uint8_t> bgra;
    int64_t us;
};

std::mutex g_lock;
std::condition_variable g_wake;
std::deque<Pending> g_queue;
std::atomic<int> g_state{0};  // 0 idle, 1 capturing, 2 stopping (the renderer hands its last), 3 finishing
uint32_t g_w = 0, g_h = 0, g_fps = 30;
std::string g_result;
std::thread g_thread;
bool g_finish = false;
constexpr size_t kQueued = 6;  // frames waiting for the encoder; the renderer waits past it (a replay's)
// live: the frames' clock, the next frame's due time; a replay's: the frames stamped so far
bool g_live = false;
Clock::time_point g_t0;
int64_t g_due = 0;
uint64_t g_count = 0;

// the name a new file takes in dir: prefix, the time, ext; another second's number when that one is there
std::filesystem::path NewFile(const std::filesystem::path& dir, const char* prefix, const char* ext) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&now));
    std::filesystem::path p = dir / (std::string(prefix) + "_" + stamp + ext);
    for (int n = 2; std::filesystem::exists(p, ec) && n < 100; n++)
        p = dir / (std::string(prefix) + "_" + stamp + "_" + std::to_string(n) + ext);
    return p;
}

#ifdef __ANDROID__
constexpr int32_t kNv12 = 21;  // COLOR_FormatYUV420SemiPlanar
constexpr int32_t kI420 = 19;  // COLOR_FormatYUV420Planar
AMediaCodec* g_codec = nullptr;
AMediaMuxer* g_muxer = nullptr;
int g_fd = -1;
int32_t g_color = kNv12;
int32_t g_stride = 0, g_slice = 0;
ssize_t g_track = -1;
bool g_muxing = false;
uint64_t g_written = 0;
std::filesystem::path g_path;
// the sound: the PCM handed to Start, its AAC packets once made, the next to write and its track
std::vector<int16_t> g_pcm;
uint32_t g_pcm_rate = 0;
struct Packet {
    std::vector<uint8_t> data;
    int64_t us;
    uint32_t flags;
};
std::vector<Packet> g_packets;
size_t g_next_packet = 0;
// the sound is made AAC on a thread of its own as the frames go: made first, its minutes held the frames back, and the
// renderer waited on their queue (Frame) with the game still. g_audio_lock keeps its format and packets; the muxer
// waits only for the format (g_audio_known), the end for the thread
std::thread g_audio_thread;
std::mutex g_audio_lock;
std::condition_variable g_audio_cv;
bool g_audio_known = false;  // the format made, or no sound
AMediaFormat* g_audio_format = nullptr;
ssize_t g_audio_track = -1;
int64_t g_last_us = 0;  // the last frame written
// live: the mix as it plays, from the audio thread, till the end is asked for; the first block's time on the frames'
// clock
std::mutex g_live_lock;
std::condition_variable g_live_cv;
std::deque<int16_t> g_live_pcm;
bool g_live_end = false;
int64_t g_live_us0 = -1;
uint32_t g_live_rate = 0;
constexpr size_t kLiveMax = 48000 * 2 * 10;  // ten seconds the encoder has not taken: past it the oldest go

void AudioKnown() {
    {
        std::lock_guard lock(g_audio_lock);
        g_audio_known = true;
    }
    g_audio_cv.notify_all();
}

AMediaCodec* OpenAac(uint32_t rate) {
    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, int32_t(rate));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 2);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, 128000);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_AAC_PROFILE, 2);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 16384);
    AMediaCodec* a = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    bool ok = a && AMediaCodec_configure(a, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK &&
              AMediaCodec_start(a) == AMEDIA_OK;
    AMediaFormat_delete(f);
    if (!ok) {
        if (a) AMediaCodec_delete(a);
        LOGR("video: no AAC encoder, the video is silent");
        return nullptr;
    }
    return a;
}

// one of the AAC encoder's outputs taken: its format, or a packet kept. False when there was none; done at the end
bool TakeAac(AMediaCodec* a, int64_t timeout_us, bool& done) {
    AMediaCodecBufferInfo info;
    ssize_t o = AMediaCodec_dequeueOutputBuffer(a, &info, timeout_us);
    if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        {
            std::lock_guard lock(g_audio_lock);
            if (g_audio_format) AMediaFormat_delete(g_audio_format);
            g_audio_format = AMediaCodec_getOutputFormat(a);
        }
        AudioKnown();
        return true;
    }
    if (o < 0) return false;
    size_t size = 0;
    uint8_t* data = AMediaCodec_getOutputBuffer(a, size_t(o), &size);
    if (data && info.size > 0 && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
        std::lock_guard lock(g_audio_lock);
        g_packets.push_back({std::vector<uint8_t>(data + info.offset, data + info.offset + info.size),
                             info.presentationTimeUs, info.flags & ~uint32_t(AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)});
    }
    AMediaCodec_releaseOutputBuffer(a, size_t(o), false);
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) done = true;
    return true;
}

// the whole sound made AAC (LC, 128 kb/s), its packets kept; false leaves the video silent
bool EncodeAudio() {
    struct Told {  // however it ends, the muxer waits no more
        ~Told() { AudioKnown(); }
    } told;
    if (g_pcm.empty() || !g_pcm_rate) return false;
    AMediaCodec* a = OpenAac(g_pcm_rate);
    if (!a) return false;
    const size_t total = g_pcm.size() / 2;
    size_t pos = 0;
    bool sent_end = false, done = false;
    for (int idle = 0; !done && idle < 2000;) {
        if (!sent_end) {
            ssize_t i = AMediaCodec_dequeueInputBuffer(a, 10000);
            if (i >= 0) {
                size_t room = 0;
                uint8_t* buf = AMediaCodec_getInputBuffer(a, size_t(i), &room);
                size_t n = buf ? std::min(room / 4, total - pos) : 0;
                if (n) std::memcpy(buf, g_pcm.data() + pos * 2, n * 4);
                uint64_t us = uint64_t(pos) * 1000000ull / g_pcm_rate;
                if (n == 0) {
                    AMediaCodec_queueInputBuffer(a, size_t(i), 0, 0, us, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    sent_end = true;
                } else {
                    AMediaCodec_queueInputBuffer(a, size_t(i), 0, n * 4, us, 0);
                    pos += n;
                }
            }
        }
        if (!TakeAac(a, sent_end ? 10000 : 0, done)) idle += sent_end;
    }
    AMediaCodec_stop(a);
    AMediaCodec_delete(a);
    g_pcm.clear();
    g_pcm.shrink_to_fit();
    LOGR("video: %zu AAC packets of %.1f s of sound", g_packets.size(), double(total) / g_pcm_rate);
    return g_audio_format && !g_packets.empty();
}

// live: the mix made AAC as Audio hands it, till the end is asked for and all of it is in
void EncodeLiveAudio() {
    struct Told {
        ~Told() { AudioKnown(); }
    } told;
    uint32_t rate = 0;
    {  // the rate is the first block's
        std::unique_lock lock(g_live_lock);
        g_live_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_live_rate || g_live_end; });
        rate = g_live_rate;
    }
    AMediaCodec* a = rate ? OpenAac(rate) : nullptr;
    if (!a) return;
    uint64_t sent = 0;  // frames given the encoder
    bool sent_end = false, done = false;
    for (int idle = 0; !done && idle < 2000;) {
        if (!sent_end) {
            bool have;
            {
                std::unique_lock lock(g_live_lock);
                g_live_cv.wait_for(lock, std::chrono::milliseconds(20), [] { return g_live_pcm.size() >= 4096 || g_live_end; });
                have = !g_live_pcm.empty() || g_live_end;
            }
            ssize_t i = have ? AMediaCodec_dequeueInputBuffer(a, 10000) : -1;
            if (i >= 0) {
                size_t room = 0;
                uint8_t* buf = AMediaCodec_getInputBuffer(a, size_t(i), &room);
                size_t n = 0;
                bool end;
                int64_t us0;
                {
                    std::lock_guard lock(g_live_lock);
                    n = buf ? std::min(room / 4, g_live_pcm.size() / 2) : 0;
                    auto* out = reinterpret_cast<int16_t*>(buf);
                    for (size_t k = 0; k < n * 2; k++) out[k] = g_live_pcm[k];
                    g_live_pcm.erase(g_live_pcm.begin(), g_live_pcm.begin() + std::ptrdiff_t(n * 2));
                    end = g_live_end && g_live_pcm.empty();
                    us0 = std::max<int64_t>(g_live_us0, 0);
                }
                int64_t us = us0 + int64_t(sent * 1000000ull / rate);
                if (n) {
                    AMediaCodec_queueInputBuffer(a, size_t(i), 0, n * 4, uint64_t(us), 0);
                    sent += n;
                } else {  // nothing more and the end asked for: the stream's end; else the buffer handed back empty
                    AMediaCodec_queueInputBuffer(a, size_t(i), 0, 0, uint64_t(us),
                                                 end ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
                    sent_end = end;
                }
            }
        }
        while (TakeAac(a, sent_end ? 10000 : 0, done) && !done) {}
        idle += sent_end;
    }
    AMediaCodec_stop(a);
    AMediaCodec_delete(a);
    LOGR("video: %zu AAC packets of %.1f s of live sound", g_packets.size(), double(sent) / rate);
}

// the sound's packets up to the frames written
void WriteAudio(int64_t up_to_us) {
    if (g_audio_track < 0) return;
    std::lock_guard lock(g_audio_lock);
    while (g_next_packet < g_packets.size() && g_packets[g_next_packet].us <= up_to_us) {
        Packet& p = g_packets[g_next_packet++];
        AMediaCodecBufferInfo info{0, int32_t(p.data.size()), p.us, p.flags};
        AMediaMuxer_writeSampleData(g_muxer, size_t(g_audio_track), p.data.data(), &info);
    }
}

// BT.601 limited range, as the encoder takes it
void ToYuv(const uint8_t* bgra, uint8_t* out, size_t room) {
    const uint32_t w = g_w, h = g_h, stride = g_stride ? uint32_t(g_stride) : w, slice = g_slice ? uint32_t(g_slice) : h;
    if (size_t(stride) * slice * 3 / 2 > room) return;
    uint8_t* y_plane = out;
    uint8_t* uv = out + size_t(stride) * slice;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* p = bgra + size_t(y) * w * 4;
        uint8_t* yr = y_plane + size_t(y) * stride;
        for (uint32_t x = 0; x < w; x++, p += 4) yr[x] = uint8_t(((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) >> 8) + 16);
    }
    for (uint32_t y = 0; y < h / 2; y++) {
        const uint8_t* a = bgra + size_t(y * 2) * w * 4;
        const uint8_t* b = a + size_t(w) * 4;
        for (uint32_t x = 0; x < w / 2; x++) {
            const uint8_t* q[4] = {a + x * 8, a + x * 8 + 4, b + x * 8, b + x * 8 + 4};
            int r = 0, g = 0, bl = 0;
            for (const uint8_t* c : q) bl += c[0], g += c[1], r += c[2];
            r /= 4, g /= 4, bl /= 4;
            uint8_t u = uint8_t(((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128);
            uint8_t v = uint8_t(((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128);
            if (g_color == kNv12) {
                uv[size_t(y) * stride + x * 2] = u;
                uv[size_t(y) * stride + x * 2 + 1] = v;
            } else {
                uint8_t* up = uv;
                uint8_t* vp = uv + size_t(stride / 2) * (slice / 2);
                up[size_t(y) * (stride / 2) + x] = u;
                vp[size_t(y) * (stride / 2) + x] = v;
            }
        }
    }
}

// what the encoder has made, into the file; till the end of the stream when ending
void Drain(bool ending) {
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t i = AMediaCodec_dequeueOutputBuffer(g_codec, &info, ending ? 10000 : 0);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(g_codec);
            g_track = AMediaMuxer_addTrack(g_muxer, f);
            AMediaFormat_delete(f);
            // the muxer takes its tracks before it starts: the sound's format, which its encoder gives in moments
            // (live, once the audio thread has played a block or two; a silent video if it never comes)
            {
                std::unique_lock lock(g_audio_lock);
                g_audio_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_audio_known; });
                g_audio_track = g_audio_format ? AMediaMuxer_addTrack(g_muxer, g_audio_format) : -1;
            }
            g_muxing = g_track >= 0 && AMediaMuxer_start(g_muxer) == AMEDIA_OK;
            continue;
        }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) {
            if (ending && i == AMEDIACODEC_INFO_TRY_AGAIN_LATER) continue;
            return;
        }
        size_t size = 0;
        uint8_t* data = AMediaCodec_getOutputBuffer(g_codec, size_t(i), &size);
        bool config = info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
        if (data && info.size > 0 && !config && g_muxing) {
            AMediaMuxer_writeSampleData(g_muxer, size_t(g_track), data + info.offset, &info);
            g_written++;
            g_last_us = std::max(g_last_us, int64_t(info.presentationTimeUs));
            WriteAudio(g_last_us);
        }
        AMediaCodec_releaseOutputBuffer(g_codec, size_t(i), false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return;
    }
}

void Encode(const Pending& p) {
    for (int tries = 0; tries < 200; tries++) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(g_codec, 10000);
        if (i < 0) {
            Drain(false);
            continue;
        }
        size_t room = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(g_codec, size_t(i), &room);
        if (buf) ToYuv(p.bgra.data(), buf, room);
        uint32_t stride = g_stride ? uint32_t(g_stride) : g_w, slice = g_slice ? uint32_t(g_slice) : g_h;
        AMediaCodec_queueInputBuffer(g_codec, size_t(i), 0, size_t(stride) * slice * 3 / 2, uint64_t(p.us), 0);
        Drain(false);
        return;
    }
}

// the gallery told of the new file (MediaScannerConnection.scanFile)
void Scan(const std::string& path, const char* mime) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return;
    jclass scanner = env->FindClass("android/media/MediaScannerConnection");
    jclass string = env->FindClass("java/lang/String");
    if (scanner && string) {
        jmethodID scan = env->GetStaticMethodID(scanner, "scanFile",
            "(Landroid/content/Context;[Ljava/lang/String;[Ljava/lang/String;"
            "Landroid/media/MediaScannerConnection$OnScanCompletedListener;)V");
        jobjectArray paths = env->NewObjectArray(1, string, env->NewStringUTF(path.c_str()));
        jobjectArray types = env->NewObjectArray(1, string, env->NewStringUTF(mime));
        if (scan) env->CallStaticVoidMethod(scanner, scan, activity, paths, types, nullptr);
        env->DeleteLocalRef(paths);
        env->DeleteLocalRef(types);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (scanner) env->DeleteLocalRef(scanner);
    if (string) env->DeleteLocalRef(string);
    env->DeleteLocalRef(activity);
}

void Close() {
    g_packets.clear();
    g_packets.shrink_to_fit();
    if (g_audio_format) AMediaFormat_delete(g_audio_format);
    g_audio_format = nullptr;
    g_audio_track = -1;
    if (g_codec) {
        AMediaCodec_stop(g_codec);
        AMediaCodec_delete(g_codec);
        g_codec = nullptr;
    }
    if (g_muxer) {
        if (g_muxing) AMediaMuxer_stop(g_muxer);
        AMediaMuxer_delete(g_muxer);
        g_muxer = nullptr;
    }
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
}

void EndLiveAudio() {
    {
        std::lock_guard lock(g_live_lock);
        g_live_end = true;
    }
    g_live_cv.notify_all();
}

void Run() {
    for (;;) {
        Pending p;
        bool finish = false;
        {
            std::unique_lock lock(g_lock);
            g_wake.wait(lock, [] { return !g_queue.empty() || g_finish; });
            if (!g_queue.empty()) {
                p = std::move(g_queue.front());
                g_queue.pop_front();
            } else {
                finish = true;
            }
        }
        g_wake.notify_all();
        if (!finish) {
            Encode(p);
            continue;
        }
        // the end of the stream, what is left drained, the file closed and shown
        ssize_t i = -1;
        for (int tries = 0; tries < 100 && i < 0; tries++) i = AMediaCodec_dequeueInputBuffer(g_codec, 10000);
        if (i >= 0) AMediaCodec_queueInputBuffer(g_codec, size_t(i), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        Drain(true);
        EndLiveAudio();
        if (g_audio_thread.joinable()) g_audio_thread.join();  // all the sound made before its last packets go in
        WriteAudio(g_last_us + int64_t(1000000 / g_fps));  // the sound to the end of the last frame, no further
        bool any = g_muxing && g_written;
        Close();
        std::string result;
        if (any) {
            Scan(g_path.string(), "video/mp4");
            result = g_path.string();
            LOGR("video: %llu frames into %s", (unsigned long long)g_written, result.c_str());
        } else {
            std::error_code ec;
            std::filesystem::remove(g_path, ec);
            result = "";
            LOGR("video: nothing was written");
        }
        std::lock_guard lock(g_lock);
        g_result = any ? result : std::string("!");
        g_finish = false;
        g_state = 0;
        return;
    }
}

bool Open(uint32_t w, uint32_t h, uint32_t fps, int32_t color) {
    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, int32_t(w));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, int32_t(h));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, color);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, int32_t(w * h * 8));  // about 7 Mb/s at 1280 x 720
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, int32_t(fps));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    g_codec = AMediaCodec_createEncoderByType("video/avc");
    bool ok = g_codec && AMediaCodec_configure(g_codec, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK &&
              AMediaCodec_start(g_codec) == AMEDIA_OK;
    AMediaFormat_delete(f);
    if (!ok) {
        if (g_codec) AMediaCodec_delete(g_codec);
        g_codec = nullptr;
        return false;
    }
    g_color = color;
    g_stride = g_slice = 0;
    if (AMediaFormat* in = AMediaCodec_getInputFormat(g_codec)) {
        AMediaFormat_getInt32(in, "stride", &g_stride);
        AMediaFormat_getInt32(in, "slice-height", &g_slice);
        AMediaFormat_delete(in);
    }
    if (g_stride < int32_t(w)) g_stride = int32_t(w);
    if (g_slice < int32_t(h)) g_slice = int32_t(h);
    return true;
}

bool Begin(uint32_t w, uint32_t h, uint32_t fps, bool live, const char* prefix) {
    if (g_state != 0) return false;
    g_last_us = 0;
    if (g_thread.joinable()) g_thread.join();
    if (g_audio_thread.joinable()) g_audio_thread.join();
    {
        std::lock_guard lock(g_audio_lock);
        g_packets.clear();
        g_next_packet = 0;
        g_audio_known = false;
        g_audio_track = -1;
        if (g_audio_format) AMediaFormat_delete(g_audio_format);
        g_audio_format = nullptr;
    }
    {
        std::lock_guard lock(g_live_lock);
        g_live_pcm.clear();
        g_live_end = false;
        g_live_us0 = -1;
        g_live_rate = 0;
    }
    g_w = w, g_h = h, g_fps = fps;
    g_live = live;
    g_count = 0;
    g_due = 0;
    g_path = NewFile("/storage/emulated/0/Movies/MostWanted", prefix, ".mp4");
    g_fd = open(g_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (g_fd < 0) {
        LOGR("video: cannot write %s", g_path.c_str());
        g_result = "!";
        return false;
    }
    if (!Open(w, h, fps, kNv12) && !Open(w, h, fps, kI420)) {
        LOGR("video: no H.264 encoder takes %ux%u", w, h);
        close(g_fd);
        g_fd = -1;
        std::error_code ec;
        std::filesystem::remove(g_path, ec);
        g_result = "!";
        return false;
    }
    g_muxer = AMediaMuxer_new(g_fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    g_track = -1;
    g_muxing = false;
    g_written = 0;
    {
        std::lock_guard lock(g_lock);
        g_queue.clear();
        g_finish = false;
        g_result.clear();
    }
    g_t0 = Clock::now();
    g_state = 1;
    g_audio_thread = live ? std::thread(EncodeLiveAudio) : std::thread([] { EncodeAudio(); });
    g_thread = std::thread(Run);
    LOGR("video: %ux%u at %u fps%s, colour %d, stride %d, slice %d, into %s", w, h, fps, live ? " live" : "", g_color,
         g_stride, g_slice, g_path.c_str());
    return true;
}
#endif

int64_t SinceStart() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_t0).count(); }

// ---- photos

std::atomic<int> g_photo_state{0};  // 0 idle, 1 asked for (the renderer's next frame), 2 writing
std::atomic<float> g_photo_aspect{0};
std::mutex g_photo_lock;
std::string g_photo_result;

uint32_t Crc(uint32_t crc, const uint8_t* p, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void Be32(std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

void Chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t n) {
    Be32(out, uint32_t(n));
    size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    if (n) out.insert(out.end(), data, data + n);
    Be32(out, Crc(0, out.data() + at, n + 4));
}

// the rows deflated: zlib's, or stored blocks where a PC build has none
std::vector<uint8_t> Deflate(const std::vector<uint8_t>& raw) {
#ifdef VIDEO_ZLIB
    {
        uLongf n = compressBound(uLong(raw.size()));
        std::vector<uint8_t> z(n);
        if (compress2(z.data(), &n, raw.data(), uLong(raw.size()), 6) == Z_OK) {
            z.resize(n);
            return z;
        }
    }
#endif
    std::vector<uint8_t> out{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    for (size_t at = 0; at < raw.size() || at == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - at);
        bool last = at + n >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(uint8_t(n)), out.push_back(uint8_t(n >> 8));
        out.push_back(uint8_t(~n)), out.push_back(uint8_t(~n >> 8));
        out.insert(out.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
        at += n;
        if (last) break;
    }
    Be32(out, (b << 16) | a);
    return out;
}

// a PNG of the BGRA rows, RGB (the frame's alpha is no picture's); each row Sub filtered, which a photo deflates far
// better than as it is
bool WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
    const size_t row = size_t(w) * 3 + 1;
    std::vector<uint8_t> raw(row * h);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* s = bgra.data() + size_t(y) * w * 4;
        uint8_t* d = raw.data() + row * y;
        d[0] = 1;
        uint8_t prev[3] = {0, 0, 0};
        for (uint32_t x = 0; x < w; x++, s += 4) {
            const uint8_t rgb[3] = {s[2], s[1], s[0]};
            for (int c = 0; c < 3; c++) {
                d[1 + x * 3 + c] = uint8_t(rgb[c] - prev[c]);
                prev[c] = rgb[c];
            }
        }
    }
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    Be32(ihdr, w);
    Be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bit RGB, deflate, adaptive filters, no interlace
    Chunk(png, "IHDR", ihdr.data(), ihdr.size());
    std::vector<uint8_t> idat = Deflate(raw);
    Chunk(png, "IDAT", idat.data(), idat.size());
    Chunk(png, "IEND", nullptr, 0);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return bool(f);
}

std::filesystem::path PhotoDir() {
#ifdef __ANDROID__
    return "/storage/emulated/0/Pictures/MostWanted";
#else
    return app::ExeDir() / "screenshots";
#endif
}

}  // namespace

#ifdef __ANDROID__
bool Available() { return true; }

bool Start(uint32_t w, uint32_t h, uint32_t fps, std::vector<int16_t> pcm, uint32_t pcm_rate) {
    if (g_state != 0) return false;
    g_pcm = std::move(pcm);
    g_pcm_rate = pcm_rate;
    return Begin(w, h, fps, false, "Replay");
}

bool StartLive(uint32_t w, uint32_t h, uint32_t fps) {
    if (g_state != 0) return false;
    g_pcm.clear();
    g_pcm_rate = 0;
    return Begin(w, h, fps, true, "Cinematic");
}

void Audio(const float* stereo, uint32_t frames, uint32_t rate) {
    if (!g_live || g_state != 1) return;
    {
        std::lock_guard lock(g_live_lock);
        if (g_live_end) return;
        if (g_live_us0 < 0) g_live_us0 = SinceStart(), g_live_rate = rate;
        if (rate != g_live_rate) return;
        for (uint32_t i = 0; i < frames * 2; i++)
            g_live_pcm.push_back(int16_t(std::lround(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f)));
        if (g_live_pcm.size() > kLiveMax)
            g_live_pcm.erase(g_live_pcm.begin(), g_live_pcm.begin() + std::ptrdiff_t(g_live_pcm.size() - kLiveMax));
    }
    g_live_cv.notify_all();
}
#else
bool Available() { return false; }
bool Start(uint32_t, uint32_t, uint32_t, std::vector<int16_t>, uint32_t) { return false; }
bool StartLive(uint32_t, uint32_t, uint32_t) { return false; }
void Audio(const float*, uint32_t, uint32_t) {}
#endif

bool Capturing() { return g_state == 1; }
bool Stopping() { return g_state == 2; }
bool Busy() { return g_state != 0; }

void Size(uint32_t& w, uint32_t& h) {
    w = g_w;
    h = g_h;
}

void SizeFor(uint32_t height, float aspect, uint32_t& w, uint32_t& h) {
    aspect = std::max(aspect, 0.5f);
    h = height;
    w = uint32_t(std::lround(float(h) * aspect / 16.0f)) * 16;
    if (w > 1920) {  // wider than encoders take: 1920 wide, the height brought down with it
        w = 1920;
        h = std::max(16u, uint32_t(std::lround(1920.0f / aspect / 16.0f)) * 16);
    }
}

int64_t Stamp() {
    if (!g_live) return int64_t(g_count++ * 1000000ull / g_fps);
    const int64_t now = SinceStart(), period = 1000000 / int64_t(g_fps);
    if (now < g_due) return -1;
    // the next one a period on; a frame late (a slow one) leaves the next a period from this one, not at once
    g_due = std::max(g_due + period, now + period / 2);
    return now;
}

double Seconds() { return g_state == 1 ? double(SinceStart()) / 1e6 : 0.0; }

void Frame(const uint8_t* bgra, uint32_t w, uint32_t h, int64_t us) {
    if (w != g_w || h != g_h || (g_state != 1 && g_state != 2)) return;
    std::unique_lock lock(g_lock);
    if (g_live) {
        if (g_queue.size() >= kQueued) return;  // the encoder behind: this one dropped, the game not held
    } else {
        g_wake.wait(lock, [] { return g_queue.size() < kQueued; });  // the encoder's pace: the renderer waits for it
    }
    g_queue.push_back({std::vector<uint8_t>(bgra, bgra + size_t(w) * h * 4), us});
    lock.unlock();
    g_wake.notify_all();
}

void Stop() {
    int capturing = 1;
    g_state.compare_exchange_strong(capturing, 2);
}

void Finish() {
    if (g_state != 2) return;
    g_state = 3;
    {
        std::lock_guard lock(g_lock);
        g_finish = true;
    }
    g_wake.notify_all();
}

std::string Result() {
    std::lock_guard lock(g_lock);
    return g_result;
}

void TakePhoto(float aspect) {
    int idle = 0;
    g_photo_aspect = aspect;
    g_photo_state.compare_exchange_strong(idle, 1);
}

bool PhotoWanted(float& aspect) {
    if (g_photo_state != 1) return false;
    aspect = g_photo_aspect;
    return true;
}

void Photo(const uint8_t* bgra, uint32_t w, uint32_t h) {
    int asked = 1;
    if (!g_photo_state.compare_exchange_strong(asked, 2)) return;
    std::vector<uint8_t> copy(bgra, bgra + size_t(w) * h * 4);
    // detached: one at a time (the state), and a thread object left joinable at the app's end would abort it
    std::thread([copy = std::move(copy), w, h] {
        std::filesystem::path p = NewFile(PhotoDir(), "MostWanted", ".png");
        bool ok = WritePng(p, copy, w, h);
        std::string result = ok ? p.string() : std::string("!");
        if (ok) {
#ifdef __ANDROID__
            Scan(result, "image/png");
#endif
            LOGR("photo: %ux%u into %s", w, h, result.c_str());
        } else {
            std::error_code ec;
            std::filesystem::remove(p, ec);
            LOGR("photo: cannot write %s", p.string().c_str());
        }
        {
            std::lock_guard lock(g_photo_lock);
            g_photo_result = result;
        }
        g_photo_state = 0;
    }).detach();
}

bool PhotoBusy() { return g_photo_state != 0; }

std::string PhotoResult() {
    std::lock_guard lock(g_photo_lock);
    return g_photo_result;
}

}  // namespace video
