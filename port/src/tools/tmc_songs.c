/*
 * tmc-songs: list the game's songs and render them to WAV with the native m4a
 * engine, without running the game.
 *
 * Each song starts on its own music player, as m4aSongNumStart would play it,
 * and the engine runs frame by frame until the song ends. A song that loops
 * (a track jumps back with GOTO) plays its loop the requested number of times
 * and then fades out the way the game fades music (m4aMPlayFadeOut).
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "global.h"
#include "gba/m4a.h"
#include "gba/m4a_internal.h"
#include "port/hw.h"
#include "port/platform.h"
#include "port/romdata.h"
#include "port/sound.h"
#include "../sound/sound_internal.h"
#include "../sound/wav.h"
#include "song_names.h"

#define FRAME_RATE 59.7275

void m4aMPlayFadeOut(MusicPlayerInfo* mplayInfo, u16 speed); // src/gba/m4a.c
#define NUM_BGM 99 // include/sound.h

// The platform hooks the game code links against; the exporter never runs the game.
uint64_t gPlatformFrame;

void PlatformVBlank(void) {
}

_Noreturn void PlatformSoftReset(void) {
    abort();
}

typedef struct Options {
    bool hq;
    int rate;
    int loops;
    double fade;
    double maxBgm;
    double maxSfx;
    const char* outDir;
    bool quiet;
} Options;

// ---- capture --------------------------------------------------------------

typedef struct Capture {
    int16_t* samples; // interleaved stereo
    size_t frames;
    size_t capacity;
    int rate;
    size_t lastLoud; // frames up to the last non-silent one
} Capture;

static void CaptureFrame(const float* stereo, int frames, int rate, void* context) {
    Capture* c = context;

    if (c->frames + frames > c->capacity) {
        c->capacity = (c->frames + frames) * 2;
        c->samples = realloc(c->samples, c->capacity * 2 * sizeof(int16_t));
    }
    c->rate = rate;
    for (int i = 0; i < 2 * frames; i++) {
        float v = stereo[i] * 32767.0f;
        int16_t s = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)(v + (v >= 0 ? 0.5f : -0.5f));
        c->samples[2 * c->frames + i] = s;
        if (s > 3 || s < -3) {
            c->lastLoud = c->frames + i / 2 + 1;
        }
    }
    c->frames += frames;
}

// ---- loop detection -----------------------------------------------------

// A backward GOTO ends one pass of a track's loop. Tracks can loop on their own
// (a drum pattern under a longer melody), so the song has looped as often as
// its least-looped running track.
static MusicPlayerInfo* sWatched;
static int sTrackLoops[16];

static void OnGoto(MusicPlayerInfo* player, MusicPlayerTrack* track, const uint8_t* from, const uint8_t* to) {
    long index = track - player->tracks;

    if (player == sWatched && to < from && index >= 0 && index < 16) {
        sTrackLoops[index]++;
    }
}

static int SongLoops(const MusicPlayerInfo* player) {
    int loops = -1;

    for (int i = 0; i < player->trackCount && i < 16; i++) {
        if ((player->tracks[i].flags & MPT_FLG_EXIST) && (loops < 0 || sTrackLoops[i] < loops)) {
            loops = sTrackLoops[i];
        }
    }
    return loops < 0 ? 0 : loops;
}

// ---- rendering ------------------------------------------------------------

static void ResetEngine(void) {
    memset(gHwIo + REG_OFFSET_NR10, 0, REG_OFFSET_WAVE_RAM0 + 16 - REG_OFFSET_NR10);
    m4aSoundInit();
    SoundMixerReset();
}

typedef struct Result {
    double seconds;
    int loops;
    bool capped;
    bool silent;
} Result;

static Result RenderSong(int n, const Options* opt, Capture* capture) {
    const Song* song = &gSongTable[n];
    MusicPlayerInfo* player = gMusicPlayers[song->musicPlayerIndex].info;
    double maxSeconds = n <= NUM_BGM ? opt->maxBgm : opt->maxSfx;
    long maxFrames = (long)(maxSeconds * FRAME_RATE);
    int fadeSpeed = (int)(opt->fade * FRAME_RATE / 16 + 0.5); // 16 fade steps
    bool fading = false;
    long tail = -1;
    long frame;
    Result r = { 0 };

    ResetEngine();
    capture->frames = capture->lastLoud = 0;
    sWatched = player;
    memset(sTrackLoops, 0, sizeof(sTrackLoops));

    m4aSongNumStart(n);
    for (frame = 0; frame < maxFrames; frame++) {
        bool playing;

        m4aSoundMain();
        m4aSoundVSync();

        if (!fading && opt->loops > 0 && SongLoops(player) >= opt->loops) {
            if (fadeSpeed > 0) {
                m4aMPlayFadeOut(player, fadeSpeed);
            } else {
                m4aMPlayAllStop();
            }
            fading = true;
        }
        playing = (player->status & MUSICPLAYER_STATUS_TRACK) && !(player->status & MUSICPLAYER_STATUS_PAUSE);
        if (tail < 0 && !playing && !SoundChannelsActive()) {
            tail = (long)(1.5 * FRAME_RATE); // reverb and release tails
        }
        if (tail >= 0 && tail-- == 0) {
            break;
        }
    }
    r.capped = frame >= maxFrames;
    r.loops = fading ? opt->loops : 0;
    r.silent = capture->lastLoud == 0;
    // Trailing silence, keeping a short fade of room tone.
    if (!r.silent) {
        size_t keep = capture->lastLoud + capture->rate / 10;
        capture->frames = keep < capture->frames ? keep : capture->frames;
    }
    r.seconds = (double)capture->frames / capture->rate;
    return r;
}

// ---- song selection -------------------------------------------------------

static bool Usable(int n) {
    return n > 0 && n < gSongNameCount && gSongNames[n].enumName[0] != '\0';
}

static int FindSong(const char* name) {
    char* end;
    long n = strtol(name, &end, 0);

    if (*end == '\0') {
        return n >= 0 && n < gSongNameCount ? (int)n : -1;
    }
    for (int i = 0; i < gSongNameCount; i++) {
        if (strcasecmp(name, gSongNames[i].enumName) == 0 || strcasecmp(name, gSongNames[i].label) == 0) {
            return i;
        }
    }
    return -1;
}

// Marks the songs a selector names: all, bgm, sfx, a number, a range a-b, or a name.
static bool Select(const char* arg, bool* selected) {
    const char* dash;

    if (strcasecmp(arg, "all") == 0 || strcasecmp(arg, "bgm") == 0 || strcasecmp(arg, "sfx") == 0) {
        for (int i = 1; i < gSongNameCount; i++) {
            bool bgm = i <= NUM_BGM;
            if (arg[0] == 'a' || arg[0] == 'A' || (bgm == (arg[0] == 'b' || arg[0] == 'B'))) {
                selected[i] |= Usable(i);
            }
        }
        return true;
    }
    dash = strchr(arg, '-');
    if (dash != NULL && dash != arg) {
        char first[32];
        int a, b;
        snprintf(first, sizeof(first), "%.*s", (int)(dash - arg), arg);
        a = FindSong(first);
        b = FindSong(dash + 1);
        if (a >= 0 && b >= a) {
            for (int i = a; i <= b; i++) {
                selected[i] |= Usable(i);
            }
            return true;
        }
    }
    int n = FindSong(arg);
    if (n < 0 || !Usable(n)) {
        fprintf(stderr, "tmc-songs: no song %s (see tmc-songs list)\n", arg);
        return false;
    }
    selected[n] = true;
    return true;
}

// ---- commands -------------------------------------------------------------

static void List(void) {
    printf("%-5s %-26s %-26s %-6s %s\n", "#", "constant", "label", "player", "title");
    for (int i = 1; i < gSongNameCount; i++) {
        if (!Usable(i)) {
            continue;
        }
        printf("%-5d %-26s %-26s %-6d %s\n", i, gSongNames[i].enumName, gSongNames[i].label,
               gSongTable[i].musicPlayerIndex, gSongNames[i].title);
    }
}

static int MakeDirs(const char* path) {
    char buffer[1024];

    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char* p = buffer + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buffer, 0755);
            *p = '/';
        }
    }
    if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
        perror(path);
        return -1;
    }
    return 0;
}

static int Render(const Options* opt, const bool* selected) {
    Capture capture = { 0 };
    char path[1200];
    char comment[256];
    int written = 0;

    if (MakeDirs(opt->outDir) != 0) {
        return 1;
    }
    SoundSetQuality(opt->hq ? SOUND_QUALITY_HQ : SOUND_QUALITY_AUTHENTIC, opt->rate);
    SoundSetSink(CaptureFrame, &capture);
    gM4aGotoHook = OnGoto;

    for (int n = 1; n < gSongNameCount; n++) {
        const SongName* name = &gSongNames[n];
        Result r;
        WavWriter* wav;
        WavInfo info;

        if (!selected[n]) {
            continue;
        }
        r = RenderSong(n, opt, &capture);
        if (r.silent) {
            if (!opt->quiet) {
                printf("%3d %-26s silent, skipped\n", n, name->enumName);
            }
            continue;
        }
        snprintf(path, sizeof(path), "%s/%03d-%s.wav", opt->outDir, n, name->enumName);
        snprintf(comment, sizeof(comment), "%s (%s), %s m4a engine, %d Hz", name->enumName, name->label,
                 opt->hq ? "HQ" : "authentic", capture.rate);
        info = (WavInfo){
            .title = name->title,
            .artist = "Nintendo / Capcom",
            .album = "The Legend of Zelda: The Minish Cap",
            .comment = comment,
            .genre = "Game",
            .track = n,
        };
        wav = WavOpen(path, capture.rate, &info);
        if (wav == NULL) {
            perror(path);
            return 1;
        }
        for (size_t i = 0; i < capture.frames;) {
            float block[2 * 4096];
            size_t count = capture.frames - i > 4096 ? 4096 : capture.frames - i;
            for (size_t k = 0; k < 2 * count; k++) {
                block[k] = capture.samples[2 * i + k] * (1.0f / 32767);
            }
            WavWrite(wav, block, (int)count);
            i += count;
        }
        WavClose(wav);
        written++;
        if (!opt->quiet) {
            printf("%3d %-26s %6.1f s%s%s  %s\n", n, name->enumName, r.seconds,
                   r.loops > 0 ? "  looped, faded" : "", r.capped ? "  (length cap)" : "",
                   path);
        }
    }
    free(capture.samples);
    printf("%d file%s in %s\n", written, written == 1 ? "" : "s", opt->outDir);
    return 0;
}

static void Usage(FILE* f) {
    fprintf(f,
            "usage: tmc-songs list\n"
            "       tmc-songs render [options] SONG...\n"
            "\n"
            "SONG is all, bgm, sfx, a song number, a range (1-20), a constant (BGM_TITLE_SCREEN)\n"
            "or a label (bgmTitleScreen).\n"
            "\n"
            "options:\n"
            "  --hq            high quality: float mixing at the output rate, cubic interpolation\n"
            "  --rate HZ       HQ output rate (default 48000)\n"
            "  --loops N       times to play a looping song's loop before fading (default 2)\n"
            "  --fade SECONDS  fade-out length (default 8)\n"
            "  --max SECONDS   length cap for BGM (default 600); SFX are capped at 60\n"
            "  --out DIR       output folder (default songs/authentic or songs/hq)\n"
            "  --quiet         only print the summary\n");
}

int main(int argc, char** argv) {
    static const struct option longOptions[] = {
        { "hq", no_argument, NULL, 'q' },     { "rate", required_argument, NULL, 'r' },
        { "loops", required_argument, NULL, 'l' }, { "fade", required_argument, NULL, 'f' },
        { "max", required_argument, NULL, 'm' }, { "out", required_argument, NULL, 'o' },
        { "quiet", no_argument, NULL, 's' },  { "help", no_argument, NULL, 'h' },
        { 0 },
    };
    Options opt = { .rate = 48000, .loops = 2, .fade = 8, .maxBgm = 600, .maxSfx = 60 };
    bool* selected;
    int c;

    if (argc < 2) {
        Usage(stderr);
        return 2;
    }
    RomDataPrepareOrExit("tmc-songs");
    if (strcmp(argv[1], "list") == 0) {
        List();
        return 0;
    }
    if (strcmp(argv[1], "render") != 0) {
        Usage(strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ? stdout : stderr);
        return strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ? 0 : 2;
    }
    optind = 2;
    while ((c = getopt_long(argc, argv, "h", longOptions, NULL)) != -1) {
        switch (c) {
            case 'q':
                opt.hq = true;
                break;
            case 'r':
                opt.rate = atoi(optarg);
                break;
            case 'l':
                opt.loops = atoi(optarg);
                break;
            case 'f':
                opt.fade = atof(optarg);
                break;
            case 'm':
                opt.maxBgm = atof(optarg);
                break;
            case 'o':
                opt.outDir = optarg;
                break;
            case 's':
                opt.quiet = true;
                break;
            case 'h':
                Usage(stdout);
                return 0;
            default:
                Usage(stderr);
                return 2;
        }
    }
    if (opt.rate < 8000 || opt.rate > 192000) {
        fprintf(stderr, "tmc-songs: --rate must be between 8000 and 192000\n");
        return 2;
    }
    if (opt.outDir == NULL) {
        opt.outDir = opt.hq ? "songs/hq" : "songs/authentic";
    }
    if (optind >= argc) {
        fprintf(stderr, "tmc-songs: name the songs to render (e.g. all, bgm, 3, BGM_TITLE_SCREEN)\n");
        return 2;
    }
    selected = calloc(gSongNameCount, sizeof(bool));
    for (int i = optind; i < argc; i++) {
        if (!Select(argv[i], selected)) {
            return 2;
        }
    }
    return Render(&opt, selected);
}
