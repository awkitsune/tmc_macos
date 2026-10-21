/*
 * WAV output: the song exporter's files and TMC_WAV recordings of headless runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/sound.h"
#include "wav.h"

struct WavWriter {
    FILE* file;
    long riffSizeAt;
    long dataSizeAt;
    uint64_t frames;
};

static void Put32(FILE* f, uint32_t v) {
    uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 };
    fwrite(b, 1, 4, f);
}

static void Put16(FILE* f, uint16_t v) {
    uint8_t b[2] = { v, v >> 8 };
    fwrite(b, 1, 2, f);
}

static uint32_t InfoFieldSize(const char* text) {
    uint32_t n = (uint32_t)strlen(text) + 1;
    return 8 + n + (n & 1);
}

static void PutInfoField(FILE* f, const char* id, const char* text) {
    uint32_t n = (uint32_t)strlen(text) + 1;

    fwrite(id, 1, 4, f);
    Put32(f, n);
    fwrite(text, 1, n, f);
    if (n & 1) {
        fputc(0, f);
    }
}

WavWriter* WavOpen(const char* path, int sampleRate, const WavInfo* info) {
    WavWriter* wav = calloc(1, sizeof(*wav));
    const char* ids[5] = { "INAM", "IART", "IPRD", "ICMT", "IGNR" };
    const char* texts[5] = { 0 };
    char track[16];
    uint32_t listSize = 4;

    wav->file = fopen(path, "wb");
    if (wav->file == NULL) {
        free(wav);
        return NULL;
    }
    if (info != NULL) {
        texts[0] = info->title;
        texts[1] = info->artist;
        texts[2] = info->album;
        texts[3] = info->comment;
        texts[4] = info->genre;
        for (int i = 0; i < 5; i++) {
            if (texts[i] != NULL && texts[i][0] != '\0') {
                listSize += InfoFieldSize(texts[i]);
            }
        }
        snprintf(track, sizeof(track), "%d", info->track);
        if (info->track > 0) {
            listSize += InfoFieldSize(track);
        }
    }

    fwrite("RIFF", 1, 4, wav->file);
    wav->riffSizeAt = ftell(wav->file);
    Put32(wav->file, 0);
    fwrite("WAVE", 1, 4, wav->file);
    fwrite("fmt ", 1, 4, wav->file);
    Put32(wav->file, 16);
    Put16(wav->file, 1); // PCM
    Put16(wav->file, 2);
    Put32(wav->file, sampleRate);
    Put32(wav->file, sampleRate * 4);
    Put16(wav->file, 4);
    Put16(wav->file, 16);
    if (listSize > 4) {
        fwrite("LIST", 1, 4, wav->file);
        Put32(wav->file, listSize);
        fwrite("INFO", 1, 4, wav->file);
        for (int i = 0; i < 5; i++) {
            if (texts[i] != NULL && texts[i][0] != '\0') {
                PutInfoField(wav->file, ids[i], texts[i]);
            }
        }
        if (info->track > 0) {
            PutInfoField(wav->file, "ITRK", track);
        }
    }
    fwrite("data", 1, 4, wav->file);
    wav->dataSizeAt = ftell(wav->file);
    Put32(wav->file, 0);
    return wav;
}

void WavWrite(WavWriter* wav, const float* stereo, int frames) {
    int16_t buffer[2 * 1024];

    while (frames > 0) {
        int n = frames > 1024 ? 1024 : frames;
        for (int i = 0; i < 2 * n; i++) {
            float v = stereo[i] * 32767.0f;
            buffer[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)(v + (v >= 0 ? 0.5f : -0.5f));
        }
        fwrite(buffer, sizeof(int16_t), 2 * n, wav->file); // little-endian host
        wav->frames += n;
        stereo += 2 * n;
        frames -= n;
    }
}

uint64_t WavFrames(const WavWriter* wav) {
    return wav->frames;
}

void WavClose(WavWriter* wav) {
    long end;

    if (wav == NULL) {
        return;
    }
    end = ftell(wav->file);
    fseek(wav->file, wav->riffSizeAt, SEEK_SET);
    Put32(wav->file, (uint32_t)(end - 8));
    fseek(wav->file, wav->dataSizeAt, SEEK_SET);
    Put32(wav->file, (uint32_t)(wav->frames * 4));
    fclose(wav->file);
    free(wav);
}

// ---- TMC_WAV --------------------------------------------------------------

static WavWriter* sRecording;
static const char* sRecordingPath;
static int sRecordingRate;

static void RecordFrame(const float* stereo, int frames, int sampleRate, void* context) {
    if (sRecording == NULL) {
        sRecording = WavOpen(sRecordingPath, sampleRate, NULL);
        sRecordingRate = sampleRate;
        if (sRecording == NULL) {
            fprintf(stderr, "TMC_WAV: cannot write %s\n", sRecordingPath);
            exit(1);
        }
    }
    if (sampleRate == sRecordingRate) {
        WavWrite(sRecording, stereo, frames);
    }
}

void SoundWavClose(void) {
    if (sRecording != NULL) {
        fprintf(stderr, "TMC_WAV: %.1f s at %d Hz written to %s\n", (double)WavFrames(sRecording) / sRecordingRate,
                sRecordingRate, sRecordingPath);
        WavClose(sRecording);
        sRecording = NULL;
    }
}

void SoundWavFromEnv(void) {
    const char* quality = getenv("TMC_SOUND_QUALITY");

    if (quality != NULL && strcmp(quality, "hq") == 0) {
        SoundSetQuality(SOUND_QUALITY_HQ, 48000);
    }
    sRecordingPath = getenv("TMC_WAV");
    if (sRecordingPath == NULL || sRecordingPath[0] == '\0') {
        return;
    }
    SoundSetSink(RecordFrame, NULL);
    atexit(SoundWavClose);
}
