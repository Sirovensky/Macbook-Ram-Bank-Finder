// SPDX-License-Identifier: GPL-2.0
//
// beeper — PC-speaker pass-end announcement.
//
// Drives the classic PIT channel-2 / port 0x61 speaker so the user
// hears an audible tone when a pass completes and the 30 s auto-
// reboot countdown begins.  T2 laptops may have the speaker path
// physically disconnected or firmware-filtered; in that case the
// port writes are silent no-ops and nothing bad happens.
//
// Safe to call from single-threaded pass-end context (no locks).

#include "stdint.h"

#include "io.h"
#include "unistd.h"

#define PIT_CH2_DATA    0x42
#define PIT_CMD         0x43
#define SPEAKER_CTRL    0x61
#define PIT_FREQ_HZ     1193180u

static void speaker_tone(unsigned freq_hz)
{
    if (freq_hz == 0) return;
    uint16_t div = (uint16_t)(PIT_FREQ_HZ / freq_hz);

    // Cmd: channel 2, access lobyte+hibyte, mode 3 (square wave), binary.
    outb(0xB6, PIT_CMD);
    outb((uint8_t)(div & 0xFF),        PIT_CH2_DATA);
    outb((uint8_t)((div >> 8) & 0xFF), PIT_CH2_DATA);

    // Enable speaker: gate2 + speaker_data_enable.
    uint8_t cur = inb(SPEAKER_CTRL);
    if ((cur & 0x03) != 0x03) {
        outb(cur | 0x03, SPEAKER_CTRL);
    }
}

static void speaker_off(void)
{
    uint8_t cur = inb(SPEAKER_CTRL);
    outb(cur & (uint8_t)~0x03, SPEAKER_CTRL);
}

// Play a short three-beep attention chirp: 800 Hz, 200 ms on / 100 ms
// off, ×3.  Total ~900 ms.  Called from board_decode_pass() right
// before the 30 s auto-reboot countdown so the user hears it whether
// they're at the laptop or across the room.  No-op if the hardware
// path is filtered (port writes silently ignored).
void board_beep_pass_end(void)
{
    for (int i = 0; i < 3; i++) {
        speaker_tone(800);
        usleep(200000);  // 200 ms on
        speaker_off();
        usleep(100000);  // 100 ms off
    }
    speaker_off();  // defensive
}
