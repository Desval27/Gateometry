#include "RhythmEngine.h"
#include <initializer_list>
#include <assert.h>
#include <stdio.h>
#include <limits.h>
using namespace ritmo;

static void ticks(Engine &e, uint32_t count) {
    while (count--) e.tick();
}
static void configure(Engine &e, uint8_t n = 16, uint8_t k = 4, uint8_t duty = 50) {
    Settings s;
    s.steps = n; s.hits = k; s.duty = duty;
    for (uint8_t i = 0; i < ChannelCount; ++i) e.configure(i, s, pattern(s));
}
static uint8_t popcount(uint32_t bits) {
    uint8_t count = 0;
    while (bits) { count += bits & 1; bits >>= 1; }
    return count;
}
static void patterns() {
    Settings s;
    s.steps = 8; s.hits = 3;
    assert(pattern(s) == 0x49); // Slots 0, 3, 6.
    s.rotation = 1; assert(pattern(s) == 0x92);
    s.rotation = -1; assert(pattern(s) == 0xA4);
    for (uint8_t n = 1; n <= MaxSteps; ++n) {
        for (uint8_t k = 0; k <= n; ++k) {
            s.steps = n; s.hits = k; s.rotation = 0;
            const uint32_t base = pattern(s);
            assert(popcount(base) == k);
            if (n < 32) assert((base >> n) == 0);
            if (k) {
                int first = -1, previous = -1, minGap = 33, maxGap = 0;
                for (uint8_t slot = 0; slot <= n; ++slot) {
                    if (slot == n || (base & (uint32_t(1) << slot))) {
                        if (first < 0) first = slot;
                        else {
                            const int gap = slot == n ? n + first - previous : slot - previous;
                            if (gap < minGap) minGap = gap;
                            if (gap > maxGap) maxGap = gap;
                        }
                        previous = slot;
                    }
                }
                assert(maxGap - minGap <= 1);
            }
            for (int r = 1 - n; r < n; ++r) {
                s.rotation = r;
                const uint32_t rotated = pattern(s);
                assert(popcount(rotated) == k);
                for (uint8_t slot = 0; slot < n; ++slot) {
                    const uint8_t target = (int(slot) + r + n) % n;
                    assert(bool(base & (uint32_t(1) << slot)) == bool(rotated & (uint32_t(1) << target)));
                }
            }
        }
    }
}
static void internalTiming() {
    Engine e; configure(e); e.setRunning(true);
    ticks(e, 124); assert(e.gates == 0);
    e.tick(); assert(e.gates == 255 && e.clockHigh && e.channels[0].current == 0);
    ticks(e, 61); assert(e.gates == 255);
    e.tick(); assert(e.gates == 0 && !e.clockHigh);
    ticks(e, 63); assert(e.channels[0].current == 1 && e.gates == 0);
    e.setRunning(false); ticks(e, 375);
    assert(e.gates == 0 && e.clockHigh && e.channels[0].current == NoSlot);
    e.setRunning(true); ticks(e, 125);
    assert(e.channels[0].current == 0 && e.gates == 255);
    e.setEnabled(0xFE); assert(e.gates == 0xFE);
    e.setEnabled(0xFF); assert(e.gates == 0xFE); // No mid-pulse unmute.
    e.setRunning(false); assert(e.gates == 0);
    for (uint16_t bpm : {uint16_t(20), uint16_t(137), uint16_t(300)}) {
        for (uint8_t rate : {uint8_t(1), uint8_t(2), uint8_t(4), uint8_t(8)}) {
            Engine clock; clock.bpm = bpm; clock.stepsPerBeat = rate;
            uint16_t count = 0;
            bool high = false;
            for (uint32_t ms = 0; ms < 60000; ++ms) {
                clock.tick();
                if (clock.clockHigh && !high) ++count;
                high = clock.clockHigh;
            }
            assert(count == bpm * rate);
        }
    }
    for (uint16_t period = 20; period <= 10000; ++period)
        for (uint8_t duty = 1; duty <= 99; ++duty)
            assert(gateLength(period, duty) >= 1 && gateLength(period, duty) < period);
}
static void externalTiming() {
    Engine e; configure(e, 4, 4, 99); e.setSource(true);
    e.externalEdge(true); assert(e.clockHigh && e.gates == 0);
    e.externalEdge(false); assert(!e.clockHigh);
    e.setRunning(true); ticks(e, 500); e.externalEdge(true);
    assert(e.period == 500 && e.gates == 255 && e.channels[0].current == 0);
    e.externalEdge(false); ticks(e, 10); e.externalEdge(true);
    assert(e.channels[0].current == 0); // Reject too-close rising edge.
    e.externalEdge(false); ticks(e, 90); e.externalEdge(true);
    assert(e.period == 100 && e.channels[0].current == 1);
    assert(e.gates == 0 && e.pending == 255); // Tempo jump gets a low gap.
    e.tick(); assert(e.gates == 255 && e.pending == 0);
    ticks(e, 99); assert(e.gates == 0);
    ticks(e, 1000); assert(!e.externalSeen && e.gates == 0);
    e.externalEdge(false); assert(!e.clockHigh);
    e.setSource(false); assert(e.gates == 0 && e.phase == 0 && !e.clockHigh);

    Engine slow; configure(slow); slow.setSource(true); slow.setRunning(true);
    slow.externalEdge(true); slow.externalEdge(false); ticks(slow, 2000);
    assert(!slow.externalSeen);
    slow.externalEdge(true); assert(slow.period == 2000); // Learn slow clocks.
    slow.externalEdge(false); ticks(slow, 2000); slow.externalEdge(true);
    assert(slow.period == 2000 && slow.channels[0].current == 2);

    Engine rollover; configure(rollover); rollover.setSource(true);
    rollover.now = UINT32_MAX - 50; rollover.externalEdge(true);
    rollover.externalEdge(false); ticks(rollover, 100); rollover.externalEdge(true);
    assert(rollover.period == 100);
}
static void independence() {
    Engine e; configure(e); e.setRunning(true);
    Settings a; a.steps = 3; a.hits = 3; a.duty = 10;
    e.configure(0, a, pattern(a));
    Settings b; b.steps = 5; b.hits = 0;
    e.configure(1, b, pattern(b));
    ticks(e, 125); assert((e.gates & 3) == 1);
    ticks(e, 12); assert(!(e.gates & 1) && (e.gates & 4));
    ticks(e, 125 * 5 - 12);
    assert(e.channels[0].current == 2 && e.channels[1].current == 0);
    const uint8_t otherSlot = e.channels[2].current;
    e.configure(0, a, pattern(a));
    assert(e.channels[0].current == NoSlot && e.channels[2].current == otherSlot);
    e.setEnabled(0xFE); ticks(e, 125);
    assert(e.channels[0].current == 0 && !(e.gates & 1)); // Muted still advances.
}
int main() {
    patterns(); internalTiming(); externalTiming(); independence();
    puts("PASS: patterns, rotations, clocks, duty, retrigger, mute, independence, rollover");
}
