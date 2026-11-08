// "My Music", a list of its own beside the game's EA Trax, which it leaves as it is: every sound file in MostWanted/music, with a
// mode each and an order of the player's, kept in playlist.txt there. Songs are decoded by the phone's own codecs (NDK media, so mp3,
// aac, ogg, opus, flac and wav need nothing of ours), resampled to the mixer's rate and added to its mix. The game's music all goes
// through one 6 channel buffer with its other sounds, so it cannot be held back here: MusicFrame turns EA Trax off while this plays.
#include "common/music.h"

#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "audio/output.h"
#include "core/log.h"

namespace music {
namespace {

constexpr uint32_t kBufferFrames = audio::kRate * 4;
constexpr float kGainStep = 1.0f / (audio::kRate / 4);  // a quarter of a second from silence to full

struct Item {
    std::filesystem::path path;
    Entry entry;
};

std::filesystem::path g_dir;

// the list, what the panel changes and the decoder reads
std::mutex g_list_lock;
std::vector<Item> g_items;
int g_source = kMyMusic;
bool g_shuffle = false;
std::filesystem::path g_current;  // the song being decoded
std::filesystem::path g_asked_path;  // the song the panel asked for, to play next
bool g_started_new = false;
Song g_started;

std::atomic<bool> g_has_songs{false};
std::atomic<bool> g_playing{false};  // the game is past its boot
std::atomic<bool> g_racing{false};
std::atomic<int> g_source_now{kMyMusic};
std::atomic<float> g_gain{0.0f};
std::atomic<uint32_t> g_skips{0};  // each change that ends the song being decoded
float g_ramp = 0;                  // audio thread

// stereo frames decoded, not yet mixed
std::mutex g_lock;
std::condition_variable g_room;
std::vector<float> g_ring(size_t(kBufferFrames) * 2);
size_t g_read = 0, g_count = 0;  // in frames

bool IsSong(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* known : {".mp3", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".flac", ".wav"})
        if (e == known) return true;
    return false;
}

// "Artist - Title.mp3" in a folder named for its album; just "Title.mp3" at the top
Song Describe(const std::filesystem::path& path) {
    Song s;
    std::string stem = path.stem().string();
    std::replace(stem.begin(), stem.end(), '_', ' ');
    size_t dash = stem.find(" - ");
    if (dash != std::string::npos) {
        s.artist = stem.substr(0, dash);
        s.title = stem.substr(dash + 3);
    } else {
        s.title = stem;
    }
    std::error_code ec;
    if (!std::filesystem::equivalent(path.parent_path(), g_dir, ec)) s.album = path.parent_path().filename().string();
    return s;
}

std::vector<std::filesystem::path> FindSongs() {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(g_dir, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code type;
        if (it->is_regular_file(type) && IsSong(it->path())) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// playlist.txt: source=, shuffle=, then a line of mode, tab, the file's path under the folder for each song, in the list's order
void Save() {
    std::ofstream f(g_dir / "playlist.txt", std::ios::trunc);
    f << "source=" << g_source << "\nshuffle=" << (g_shuffle ? 1 : 0) << "\n";
    for (const Item& it : g_items) {
        std::error_code ec;
        f << int(it.entry.mode) << '\t' << std::filesystem::relative(it.path, g_dir, ec).string() << "\n";
    }
}

// what the file names and is still there, in its order, then what is new, playing everywhere
void Load() {
    std::vector<std::filesystem::path> found = FindSongs();
    std::ifstream f(g_dir / "playlist.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("source=", 0) == 0) {
            g_source = line[7] == '0' ? kEaTrax : kMyMusic;
        } else if (line.rfind("shuffle=", 0) == 0) {
            g_shuffle = line[8] == '1';
        } else if (line.size() > 2 && line[1] == '\t') {
            std::filesystem::path path = g_dir / line.substr(2);
            auto at = std::find(found.begin(), found.end(), path);
            if (at == found.end()) continue;
            found.erase(at);
            g_items.push_back({path, {Describe(path), uint8_t(std::clamp(line[0] - '0', 0, 3))}});
        }
    }
    for (const auto& path : found) g_items.push_back({path, {Describe(path), kModeAll}});
}

// waits for room: the decoder keeps a few seconds ahead of the mix. False once the song is to end
bool Write(const float* frames, size_t count, uint32_t skips) {
    std::unique_lock<std::mutex> lock(g_lock);
    while (count) {
        g_room.wait(lock, [&] { return g_count < kBufferFrames || g_skips != skips; });
        if (g_skips != skips) return false;
        size_t at = (g_read + g_count) % kBufferFrames;
        size_t n = std::min({count, size_t(kBufferFrames) - g_count, size_t(kBufferFrames) - at});
        std::memcpy(&g_ring[at * 2], frames, n * 2 * sizeof(float));
        g_count += n;
        frames += n * 2;
        count -= n;
    }
    return true;
}

// one song's samples to stereo at the mixer's rate, linearly
struct Resampler {
    double step = 1, pos = 0;
    float last[2] = {};
    bool have_last = false;
    std::vector<float> out;
    uint32_t skips = 0;

    bool Add(const float* stereo, size_t frames) {
        out.clear();
        for (size_t i = 0; i < frames; i++) {
            const float* now = stereo + i * 2;
            if (!have_last) {
                last[0] = now[0], last[1] = now[1];
                have_last = true;
                continue;
            }
            while (pos < 1.0) {
                float f = float(pos);
                out.push_back(last[0] + (now[0] - last[0]) * f);
                out.push_back(last[1] + (now[1] - last[1]) * f);
                pos += step;
            }
            pos -= 1.0;
            last[0] = now[0], last[1] = now[1];
        }
        return out.empty() || Write(out.data(), out.size() / 2, skips);
    }
};

// false if nothing of it could be played
bool PlaySong(const std::filesystem::path& path, uint32_t skips) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    off_t length = lseek(fd, 0, SEEK_END);
    AMediaExtractor* ex = AMediaExtractor_new();
    AMediaCodec* codec = nullptr;
    bool played = false;
    if (AMediaExtractor_setDataSourceFd(ex, fd, 0, length) == AMEDIA_OK) {
        for (size_t t = 0, n = AMediaExtractor_getTrackCount(ex); t < n && !codec; t++) {
            AMediaFormat* format = AMediaExtractor_getTrackFormat(ex, t);
            const char* mime = nullptr;
            if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) && mime && !std::strncmp(mime, "audio/", 6)) {
                codec = AMediaCodec_createDecoderByType(mime);
                if (codec && AMediaCodec_configure(codec, format, nullptr, nullptr, 0) != AMEDIA_OK) {
                    AMediaCodec_delete(codec);
                    codec = nullptr;
                }
                if (codec) AMediaExtractor_selectTrack(ex, t);
            }
            AMediaFormat_delete(format);
        }
    }
    if (codec && AMediaCodec_start(codec) == AMEDIA_OK) {
        int32_t channels = 2, rate = 44100, encoding = 2;  // 2: 16 bit PCM
        Resampler rs;
        rs.skips = skips;
        rs.step = double(rate) / audio::kRate;
        std::vector<float> stereo;
        bool input_done = false, output_done = false;
        while (!output_done && g_skips == skips) {
            if (!input_done) {
                ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 5000);
                if (in >= 0) {
                    size_t room = 0;
                    uint8_t* buf = AMediaCodec_getInputBuffer(codec, size_t(in), &room);
                    ssize_t got = buf ? AMediaExtractor_readSampleData(ex, buf, room) : -1;
                    if (got < 0) {
                        AMediaCodec_queueInputBuffer(codec, size_t(in), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                        input_done = true;
                    } else {
                        AMediaCodec_queueInputBuffer(codec, size_t(in), 0, size_t(got), AMediaExtractor_getSampleTime(ex), 0);
                        AMediaExtractor_advance(ex);
                    }
                }
            }
            AMediaCodecBufferInfo info;
            ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 5000);
            if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* f = AMediaCodec_getOutputFormat(codec);
                AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
                AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
                if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_PCM_ENCODING, &encoding)) encoding = 2;
                rs.step = double(rate) / audio::kRate;
                AMediaFormat_delete(f);
            } else if (out >= 0) {
                size_t size = 0;
                const uint8_t* buf = AMediaCodec_getOutputBuffer(codec, size_t(out), &size);
                if (buf && info.size > 0 && channels > 0) {
                    const uint8_t* data = buf + info.offset;
                    size_t sample = encoding == 4 ? sizeof(float) : sizeof(int16_t);
                    size_t frames = size_t(info.size) / (sample * size_t(channels));
                    stereo.resize(frames * 2);
                    for (size_t i = 0; i < frames; i++) {
                        const uint8_t* f = data + i * sample * size_t(channels);
                        float l, r;
                        if (encoding == 4) {
                            std::memcpy(&l, f, sizeof l);
                            r = l;
                            if (channels > 1) std::memcpy(&r, f + sizeof(float), sizeof r);
                        } else {
                            int16_t a, b;
                            std::memcpy(&a, f, sizeof a);
                            b = a;
                            if (channels > 1) std::memcpy(&b, f + sizeof(int16_t), sizeof b);
                            l = a / 32768.0f;
                            r = b / 32768.0f;
                        }
                        stereo[i * 2] = l;
                        stereo[i * 2 + 1] = r;
                    }
                    played = true;
                    if (!rs.Add(stereo.data(), frames)) output_done = true;
                }
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) output_done = true;
                AMediaCodec_releaseOutputBuffer(codec, size_t(out), false);
            } else if (out != AMEDIACODEC_INFO_TRY_AGAIN_LATER && out != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                break;
            }
        }
        AMediaCodec_stop(codec);
    }
    if (codec) AMediaCodec_delete(codec);
    AMediaExtractor_delete(ex);
    close(fd);
    return played;
}

// what the panel asked for, or the song after the last among those the list lets play here
bool NextSong(std::mt19937& random, std::filesystem::path* path) {
    std::lock_guard<std::mutex> lock(g_list_lock);
    if (g_source != kMyMusic || !g_playing) return false;
    const Item* pick = nullptr;
    if (!g_asked_path.empty()) {
        for (const Item& it : g_items)
            if (it.path == g_asked_path) pick = &it;
        g_asked_path.clear();
    }
    if (!pick) {
        uint8_t wanted = g_racing ? kModeRacing : kModeMenus;
        std::vector<const Item*> allowed;
        for (const Item& it : g_items)
            if (it.entry.mode & wanted) allowed.push_back(&it);
        if (allowed.empty()) return false;
        if (g_shuffle && allowed.size() > 1) {
            do {
                pick = allowed[random() % allowed.size()];
            } while (pick->path == g_current);
        } else {
            // in the whole list's order, from where the last song is now
            size_t from = 0;
            for (size_t i = 0; i < g_items.size(); i++)
                if (g_items[i].path == g_current) from = i + 1;
            for (size_t i = 0; i < g_items.size() && !pick; i++) {
                const Item& it = g_items[(from + i) % g_items.size()];
                if (it.entry.mode & wanted) pick = &it;
            }
        }
    }
    if (!pick) return false;
    *path = pick->path;
    g_current = pick->path;
    g_started = pick->entry.song;
    g_started_new = true;
    return true;
}

// a folder that reads empty is read again: once it did at a boot with its songs in it, and a song added now needs no new boot
void WaitForSongs() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_list_lock);
            Load();
            if (!g_items.empty()) {
                LOGR("music: %zu songs in %s", g_items.size(), g_dir.c_str());
                g_source_now = g_source;
                g_has_songs = true;
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void Decoder() {
    WaitForSongs();
    std::mt19937 random{std::random_device{}()};
    for (;;) {
        std::filesystem::path path;
        if (!NextSong(random, &path)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (!PlaySong(path, g_skips)) {
            LOGR("music: cannot play %s", path.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

// the song being decoded ends now, and what it had decoded with it
void Skip() {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_read = g_count = 0;
        g_skips++;
    }
    g_room.notify_all();
}

}  // namespace

void Start(const std::string& dir) {
    g_dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    std::thread(Decoder).detach();
}

bool HasSongs() { return g_has_songs; }

std::vector<Entry> List() {
    std::lock_guard<std::mutex> lock(g_list_lock);
    std::vector<Entry> out;
    out.reserve(g_items.size());
    for (const Item& it : g_items) out.push_back(it.entry);
    return out;
}

int Current() {
    std::lock_guard<std::mutex> lock(g_list_lock);
    for (size_t i = 0; i < g_items.size(); i++)
        if (g_items[i].path == g_current) return int(i);
    return -1;
}

int Source() { return g_source_now; }

bool Shuffle() {
    std::lock_guard<std::mutex> lock(g_list_lock);
    return g_shuffle;
}

void SetSource(int source) {
    {
        std::lock_guard<std::mutex> lock(g_list_lock);
        if (g_source == source) return;
        g_source = source;
        g_source_now = source;
        Save();
    }
    Skip();
}

void SetShuffle(bool shuffle) {
    std::lock_guard<std::mutex> lock(g_list_lock);
    g_shuffle = shuffle;
    Save();
}

void SetMode(size_t song, uint8_t mode) {
    std::lock_guard<std::mutex> lock(g_list_lock);
    if (song >= g_items.size()) return;
    g_items[song].entry.mode = mode;
    Save();
}

void Move(size_t song, int by) {
    std::lock_guard<std::mutex> lock(g_list_lock);
    long to = long(song) + by;
    if (song >= g_items.size() || to < 0 || to >= long(g_items.size())) return;
    std::swap(g_items[song], g_items[size_t(to)]);
    Save();
}

void PlayNow(size_t song) {
    {
        std::lock_guard<std::mutex> lock(g_list_lock);
        if (song >= g_items.size()) return;
        g_asked_path = g_items[song].path;
    }
    SetSource(kMyMusic);
    Skip();
}

void Next() { Skip(); }

void SetContext(bool playing, bool racing, float gain) {
    g_playing = playing;
    g_racing = racing;
    g_gain = gain;
}

bool TakeStarted(Song* song) {
    std::lock_guard<std::mutex> lock(g_list_lock);
    if (!g_started_new) return false;
    g_started_new = false;
    *song = g_started;
    return true;
}

void Mix(float* out, uint32_t frames) {
    if (!g_has_songs || !g_playing || g_source_now != kMyMusic) return;
    std::unique_lock<std::mutex> lock(g_lock, std::try_to_lock);
    if (!lock.owns_lock()) return;
    float target = g_gain;
    size_t n = std::min<size_t>(frames, g_count);
    for (size_t i = 0; i < n; i++) {
        size_t at = (g_read + i) % kBufferFrames;
        g_ramp += std::clamp(target - g_ramp, -kGainStep, kGainStep);
        float* f = out + i * audio::kChannels;
        f[audio::FrontLeft] += g_ring[at * 2] * g_ramp;
        f[audio::FrontRight] += g_ring[at * 2 + 1] * g_ramp;
    }
    g_read = (g_read + n) % kBufferFrames;
    g_count -= n;
    lock.unlock();
    if (n) g_room.notify_all();
}

}  // namespace music
