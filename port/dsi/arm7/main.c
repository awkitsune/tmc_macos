/*
 * The DSi port's ARM7 program: BlocksDS's default ARM7 (sys/arm7/main_core,
 * the same calls in the same order: sound, system and storage FIFOs, the
 * camera on a DSi, the power button, the clock, the keys and touch screen each
 * VBlank, Start+Select+L+R to quit, held for half a second here) without
 * maxmod, which the port does not
 * use, plus the m4a voices on the sound channels: each frame the ARM9 sends the
 * address of a register image (port/dsi/src/sound_image.h), and its channels
 * are written here, in the FIFO interrupt.
 */

#include <nds.h>

#include "../src/sound_image.h"

#define SOUND_CHANNEL(ch) (0x04000400 + 16 * (ch))

static volatile int sQuit; // DSI_QUIT_*
static volatile bool sQuitAnswered;

static void PowerButton(void) {
    sQuit = DSI_QUIT_POWER;
}

static void QuitAnswer(u32 value, void* userdata) {
    (void)value;
    (void)userdata;
    sQuitAnswered = true;
}

static void VBlank(void) {
    inputGetAndSend();
}

static void WriteChannels(u32 address, void* userdata) {
    const volatile DsiSoundImage* image = (const volatile DsiSoundImage*)address;
    uint16_t write = image->write, start = image->start;

    (void)userdata;
    for (int ch = 0; ch < 16; ch++) {
        const volatile DsiSoundChannel* c = &image->ch[ch];
        uint32_t base = SOUND_CHANNEL(ch);
        uint32_t cnt = c->cnt;

        if (!(write >> ch & 1)) {
            continue;
        }
        if (!(cnt & SCHANNEL_ENABLE)) {
            *(vu32*)base = 0;
        } else if (start >> ch & 1) {
            *(vu32*)base = 0;
            *(vu32*)(base + 4) = c->src;
            *(vu16*)(base + 8) = c->timer;
            *(vu16*)(base + 10) = c->loop;
            *(vu32*)(base + 12) = c->length;
            *(vu32*)base = cnt;
        } else {
            // Volume, divider, pan and pitch; the duty byte (with the start
            // bit) only while the channel plays, so a sample that ended is
            // not started again.
            *(vu16*)(base + 8) = c->timer;
            *(vu16*)base = (u16)cnt;
            *(vu8*)(base + 2) = (u8)(cnt >> 16);
            if (*(vu32*)base & SCHANNEL_ENABLE) {
                *(vu8*)(base + 3) = (u8)(cnt >> 24);
            }
        }
    }
}

int main(void) {
    enableSound();
    readUserSettings();
    ledBlink(0);
    touchInit();
    irqInit();
    fifoInit();
    installSoundFIFO();
    installSystemFIFO();
    if (isDSiMode()) {
        installCameraFIFO();
    }
    fifoSetValue32Handler(DSI_SOUND_FIFO, WriteChannels, NULL);
    fifoSetValue32Handler(DSI_QUIT_FIFO, QuitAnswer, NULL);
    setPowerButtonCB(PowerButton);
    initClockIRQTimer(LIBNDS_DEFAULT_TIMER_RTC);
    irqSet(IRQ_VBLANK, VBlank);
    irqEnable(IRQ_VBLANK);

    // A keypad read just after a wake can show every key held, so the keys
    // must stay held for 30 frames.
    for (int held = 0; sQuit == 0;) {
        held = (REG_KEYINPUT & (KEY_SELECT | KEY_START | KEY_L | KEY_R)) == 0 ? held + 1 : 0;
        if (held >= 30) {
            sQuit = DSI_QUIT_KEYS;
        }
        swiWaitForVBlank();
    }
    fifoSendValue32(DSI_QUIT_FIFO, sQuit);
    for (int i = 0; i < 120 && !sQuitAnswered; i++) {
        swiWaitForVBlank();
    }
    return 0;
}
