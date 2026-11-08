// A replay saved as a video (src/common/video.h). The frames come as BGRA from the renderer, are made NV12 and given
// to the phone's H.264 encoder (NDK MediaCodec) on a thread of their own, and its output is written into an MP4
// (MediaMuxer) in Movies/MostWanted, which the media scanner is then told of so the gallery shows it. The replay plays
// at a fixed 1/30 s a frame while it is saved, so frame n is at n / fps whatever the game's own frame rate. The sound
// under it (the race's, recorded by replay.cpp) is made AAC first, whole, and its packets written as the frames reach
// them: a muxer takes no track after it starts, and the video's format is known only with its first output.
#include "common/video.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"

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

struct Pending {
    std::vector<uint8_t> bgra;
    uint64_t n;
};

std::mutex g_lock;
std::condition_variable g_wake;
std::deque<Pending> g_queue;
std::atomic<int> g_state{0};  // 0 idle, 1 capturing, 2 stopping (the renderer hands its last), 3 finishing
uint32_t g_w = 0, g_h = 0, g_fps = 30;
std::string g_result;
std::thread g_thread;
bool g_finish = false;
constexpr size_t kQueued = 6;  // frames waiting for the encoder; the renderer waits past it

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

void AudioKnown() {
    {
        std::lock_guard lock(g_audio_lock);
        g_audio_known = true;
    }
    g_audio_cv.notify_all();
}

// the whole sound made AAC (LC, 128 kb/s), its packets kept; false leaves the video silent
bool EncodeAudio() {
    struct Told {  // however it ends, the muxer waits no more
        ~Told() { AudioKnown(); }
    } told;
    if (g_pcm.empty() || !g_pcm_rate) return false;
    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, int32_t(g_pcm_rate));
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
        return false;
    }
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
        AMediaCodecBufferInfo info;
        ssize_t o = AMediaCodec_dequeueOutputBuffer(a, &info, sent_end ? 10000 : 0);
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            {
                std::lock_guard lock(g_audio_lock);
                if (g_audio_format) AMediaFormat_delete(g_audio_format);
                g_audio_format = AMediaCodec_getOutputFormat(a);
            }
            AudioKnown();
            continue;
        }
        if (o < 0) {
            idle += sent_end;
            continue;
        }
        size_t size = 0;
        uint8_t* data = AMediaCodec_getOutputBuffer(a, size_t(o), &size);
        if (data && info.size > 0 && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
            std::lock_guard lock(g_audio_lock);
            g_packets.push_back({std::vector<uint8_t>(data + info.offset, data + info.offset + info.size),
                                 info.presentationTimeUs, info.flags & ~uint32_t(AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)});
        }
        AMediaCodec_releaseOutputBuffer(a, size_t(o), false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) done = true;
    }
    AMediaCodec_stop(a);
    AMediaCodec_delete(a);
    g_pcm.clear();
    g_pcm.shrink_to_fit();
    LOGR("video: %zu AAC packets of %.1f s of sound", g_packets.size(), double(total) / g_pcm_rate);
    return g_audio_format && !g_packets.empty();
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
            {
                std::unique_lock lock(g_audio_lock);
                g_audio_cv.wait(lock, [] { return g_audio_known; });
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
        uint64_t us = p.n * 1000000ull / g_fps;
        AMediaCodec_queueInputBuffer(g_codec, size_t(i), 0, size_t(stride) * slice * 3 / 2, us, 0);
        Drain(false);
        return;
    }
}

// the gallery told of the new file (MediaScannerConnection.scanFile)
void Scan(const std::string& path) {
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
        jobjectArray types = env->NewObjectArray(1, string, env->NewStringUTF("video/mp4"));
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
        if (g_audio_thread.joinable()) g_audio_thread.join();  // all the sound made before its last packets go in
        WriteAudio(g_last_us + int64_t(1000000 / g_fps));  // the sound to the end of the last frame, no further
        bool any = g_muxing && g_written;
        Close();
        std::string result;
        if (any) {
            Scan(g_path.string());
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
#endif

}  // namespace

#ifdef __ANDROID__
bool Available() { return true; }

bool Start(uint32_t w, uint32_t h, uint32_t fps, std::vector<int16_t> pcm, uint32_t pcm_rate) {
    if (g_state != 0) return false;
    g_pcm = std::move(pcm);
    g_pcm_rate = pcm_rate;
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
    g_w = w, g_h = h, g_fps = fps;
    std::filesystem::path dir = "/storage/emulated/0/Movies/MostWanted";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[64];
    std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof name, "Replay_%Y%m%d_%H%M%S.mp4", std::localtime(&now));
    g_path = dir / name;
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
    g_state = 1;
    g_audio_thread = std::thread(EncodeAudio);
    g_thread = std::thread(Run);
    LOGR("video: %ux%u at %u fps, colour %d, stride %d, slice %d, into %s", w, h, fps, g_color, g_stride, g_slice,
         g_path.c_str());
    return true;
}
#else
bool Available() { return false; }
bool Start(uint32_t, uint32_t, uint32_t, std::vector<int16_t>, uint32_t) { return false; }
#endif

bool Capturing() { return g_state == 1; }
bool Stopping() { return g_state == 2; }
bool Busy() { return g_state != 0; }

void Size(uint32_t& w, uint32_t& h) {
    w = g_w;
    h = g_h;
}

void Frame(const uint8_t* bgra, uint32_t w, uint32_t h, uint64_t n) {
    if (w != g_w || h != g_h || (g_state != 1 && g_state != 2)) return;
    std::unique_lock lock(g_lock);
    g_wake.wait(lock, [] { return g_queue.size() < kQueued; });  // the encoder's pace: the renderer waits for it
    g_queue.push_back({std::vector<uint8_t>(bgra, bgra + size_t(w) * h * 4), n});
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

}  // namespace video
