// "My Music", a list of the player's own songs beside the game's EA Trax: native/music.cpp plays them, the panel (src/common/menu.cpp)
// edits the list, src/game/game.cpp's MusicFrame ties it to the game
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace music {

constexpr int kEaTrax = 0, kMyMusic = 1;  // what plays
constexpr uint8_t kModeOff = 0, kModeMenus = 1, kModeRacing = 2, kModeAll = 3;  // bits: where a song may play, as EA Trax's

struct Song {
    std::string title, artist, album;  // UTF-8
};
struct Entry {
    Song song;
    uint8_t mode = kModeAll;
};

void Start(const std::string& dir);  // finds the songs and starts the decoder, if there are any
bool HasSongs();
std::vector<Entry> List();           // in the list's order
int Current();                       // the song last started, in that order; -1 none
int Source();
bool Shuffle();
void SetSource(int source);
void SetShuffle(bool shuffle);
void SetMode(size_t song, uint8_t mode);
void Move(size_t song, int by);      // up or down the list
void PlayNow(size_t song);           // and My Music is what plays
void Next();
void SetContext(bool playing, bool racing, float gain);  // game thread, each frame
bool TakeStarted(Song* song);        // game thread: a song has begun since last asked, for the game's banner
void Mix(float* out, uint32_t frames);  // audio thread: adds to a mix of audio::kChannels channels

}  // namespace music
