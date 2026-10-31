/*
 * The bottom screen and the boot options (port/dsi/README.md, "On the DSi").
 *
 * The bottom screen shows the title, the version and how to open the options:
 * holding Start in the first half second of the boot. The options menu lists
 * sOptions (Up and Down pick one, A or Left and Right change it, Start plays);
 * the choices are kept in tmc/settings.txt next to the save, one "key=value"
 * line each. The boot log goes to the screen only in DSI_CHECKS builds or when
 * the port stops (DsiShowLog).
 */

#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "port/view.h"

#include "dsi.h"

#ifndef TMC_VERSION
#error TMC_VERSION must be the version: project VERSION in port/CMakeLists.txt
#endif

#define OPTION_ROW 5   // the first option's row in the menu
#define HINT_ROW 9     // "hold [Start] ..." under the options' room
#define BOOT_FRAMES 30 // how long a held Start is looked for

typedef struct {
    const char* title;
    const char* key; // in tmc/settings.txt
    const char* values[2];
    const char* names[2];
    int value;
} Option;

// New options go here; the first value is index 0.
enum { OPTION_SCREEN };
static Option sOptions[] = {
    [OPTION_SCREEN] = { "Screen", "screen", { "bordered", "fullscreen" }, { "Bordered", "Fullscreen" }, 1 },
};
#define OPTION_COUNT (int)(sizeof(sOptions) / sizeof(sOptions[0]))

static bool sStartHeld;

static void Header(void) {
    consoleClear();
    printf("The Legend of Zelda:\nThe Minish Cap\nDSi version\nv%s\n", TMC_VERSION);
}

// What the bottom screen shows while the game runs.
static void Title(void) {
    Header();
    printf("\x1b[%d;0Hhold [Start] to change options", HINT_ROW);
}

void DsiScreenBoot(void) {
    if (DSI_CHECKS) {
        return; // the console shows the log
    }
    Title();
    for (int i = 0; i < BOOT_FRAMES && !sStartHeld; i++) {
        swiWaitForVBlank();
        scanKeys();
        sStartHeld = (keysHeld() & KEY_START) != 0;
    }
}

void DsiShowLog(void) {
    const char* tail = gDsiLog + strlen(gDsiLog);
    int lines = 0;

    // The last 23 lines that fit the console.
    while (tail > gDsiLog && lines < 23) {
        if (*--tail == '\n' && tail[1] != 0) {
            lines++;
        }
    }
    if (*tail == '\n') {
        tail++;
    }
    consoleClear();
    fputs(tail, stdout);
}

void DsiSettingsLoad(const char* path) {
    char line[128];
    FILE* file = fopen(path, "rb");

    while (file != NULL && fgets(line, sizeof(line), file) != NULL) {
        for (int i = 0; i < OPTION_COUNT; i++) {
            size_t length = strlen(sOptions[i].key);
            if (strncmp(line, sOptions[i].key, length) != 0 || line[length] != '=') {
                continue;
            }
            for (int v = 0; v < 2; v++) {
                if (strncmp(line + length + 1, sOptions[i].values[v], strlen(sOptions[i].values[v])) == 0) {
                    sOptions[i].value = v;
                }
            }
        }
    }
    if (file != NULL) {
        fclose(file);
    }
}

static void Save(const char* path) {
    FILE* file = fopen(path, "wb");
    bool ok = file != NULL;

    for (int i = 0; ok && i < OPTION_COUNT; i++) {
        ok = fprintf(file, "%s=%s\n", sOptions[i].key, sOptions[i].values[sOptions[i].value]) > 0;
    }
    if (file != NULL) {
        ok &= fclose(file) == 0;
    }
    DsiLog(ok ? "saved %s\n" : "cannot write %s\n", path);
}

static void DrawMenu(int cursor) {
    Header();
    printf("\x1b[%d;0HOptions", OPTION_ROW - 1);
    for (int i = 0; i < OPTION_COUNT; i++) {
        const Option* o = &sOptions[i];
        printf("\x1b[%d;0H%c %s: %s%s%s / %s%s%s", OPTION_ROW + i, i == cursor ? '>' : ' ', o->title,
               o->value == 0 ? "[" : "", o->names[0], o->value == 0 ? "]" : "", o->value == 1 ? "[" : "",
               o->names[1], o->value == 1 ? "]" : "");
    }
    printf("\x1b[%d;0HA: change   [Start]: play", OPTION_ROW + OPTION_COUNT + 2);
}

/* The menu, if Start was held at boot; the choices saved to path if changed. */
void DsiSettingsMenu(const char* path) {
    int cursor = 0;
    bool changed = false, released = false;

    if (!sStartHeld) {
        return;
    }
    DsiLog("options menu\n");
    DrawMenu(cursor);
    while (true) {
        uint32_t down;

        swiWaitForVBlank();
        scanKeys();
        down = keysDown();
        released |= !(keysHeld() & KEY_START); // the Start that opened the menu does not close it
        if (released && (down & KEY_START)) {
            break;
        }
        if (down & KEY_UP) {
            cursor = (cursor + OPTION_COUNT - 1) % OPTION_COUNT;
        } else if (down & KEY_DOWN) {
            cursor = (cursor + 1) % OPTION_COUNT;
        } else if (down & (KEY_A | KEY_LEFT | KEY_RIGHT)) {
            sOptions[cursor].value ^= 1;
            changed = true;
        } else {
            continue;
        }
        DrawMenu(cursor);
    }
    if (changed && path != NULL) {
        Save(path);
    }
    Title();
}

void DsiSettingsApply(void) {
    bool fullscreen = sOptions[OPTION_SCREEN].value == 1;

    if (fullscreen) {
        PortViewSet(256, 192);
    }
    DsiLog("screen: %s\n", fullscreen ? "fullscreen" : "bordered");
}
