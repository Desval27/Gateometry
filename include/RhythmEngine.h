#pragma once

#include <stdint.h>

namespace ritmo {
constexpr uint8_t ChannelCount = 8;
constexpr uint8_t MaxSteps = 32;
constexpr uint8_t NoSlot = 255;

struct Settings {
    uint8_t steps = 16;
    uint8_t hits = 4;
    int8_t rotation = 0;
    uint8_t duty = 50;
};

// Evenly spaced Euclidean onsets, anchored on slot zero before rotation.
// Positive rotation moves hits clockwise/later; no arrays or recursion needed.
inline uint32_t pattern(const Settings &s) {
    uint32_t result = 0;
    for (uint8_t slot = 0; slot < s.steps; ++slot) {
        if ((uint16_t(slot) * s.hits) % s.steps < s.hits) {
            int16_t rotated = (int16_t(slot) + s.rotation) % s.steps;
            if (rotated < 0) rotated += s.steps;
            result |= uint32_t(1) << rotated;
        }
    }
    return result;
}

inline uint16_t gateLength(uint16_t period, uint8_t duty) {
    uint16_t length = uint32_t(period) * duty / 100;
    if (length == 0) length = 1;
    // A low interval between adjacent hits is essential for retriggering.
    if (length >= period) length = period - 1;
    return length;
}

// Called from interrupts; foreground access must be inside ATOMIC_BLOCK.
// Volatile fields ensure foreground snapshots see interrupt updates on AVR.
class Engine {
public:
    struct Channel {
        volatile uint32_t bits = 0;
        volatile uint16_t remaining = 0;
        volatile uint8_t steps = 16;
        volatile uint8_t duty = 50;
        volatile uint8_t next = 0;
        volatile uint8_t current = NoSlot;
    } channels[ChannelCount];

    volatile uint32_t now = 0;
    volatile uint32_t lastExternal = 0;
    volatile uint32_t phase = 0;
    volatile uint16_t bpm = 120;
    volatile uint16_t period = 125;
    volatile uint16_t clockRemaining = 0;
    volatile uint8_t stepsPerBeat = 4;
    volatile uint8_t gates = 0;
    volatile uint8_t pending = 0;
    volatile uint8_t enabled = 0xFF;
    volatile bool running = false;
    volatile bool external = false;
    volatile bool externalSeen = false;
    volatile bool haveExternalEdge = false;
    volatile bool clockHigh = false;

    uint16_t internalPeriod() const {
        return 60000UL / (uint16_t(bpm) * stepsPerBeat);
    }

    void configure(uint8_t index, const Settings &s, uint32_t bits) {
        Channel &c = channels[index];
        c.bits = bits;
        c.steps = s.steps;
        c.duty = s.duty;
        c.remaining = 0;
        c.next = 0;
        c.current = NoSlot;
        gates &= ~(uint8_t(1) << index);
        pending &= ~(uint8_t(1) << index);
    }

    void setEnabled(uint8_t mask) {
        enabled = mask;
        gates &= mask;
        pending &= mask;
        for (uint8_t i = 0; i < ChannelCount; ++i)
            if (!(mask & (1 << i))) channels[i].remaining = 0;
    }

    void resetChannels() {
        gates = 0;
        pending = 0;
        for (uint8_t i = 0; i < ChannelCount; ++i) {
            channels[i].remaining = 0;
            channels[i].next = 0;
            channels[i].current = NoSlot;
        }
    }

    void setRunning(bool value) {
        running = value;
        resetChannels();
    }

    void setSource(bool value) {
        external = value;
        externalSeen = false;
        haveExternalEdge = false;
        phase = 0;
        clockHigh = false;
        clockRemaining = 0;
        period = internalPeriod();
        resetChannels();
    }

    void advance() {
        if (!running) return;
        for (uint8_t i = 0; i < ChannelCount; ++i) {
            Channel &c = channels[i];
            const uint8_t slot = c.next;
            c.current = slot;
            c.next = slot + 1 == c.steps ? 0 : slot + 1;
            const uint8_t bit = uint8_t(1) << i;
            if ((enabled & bit) && (c.bits & (uint32_t(1) << slot))) {
                c.remaining = gateLength(period, c.duty);
                if (gates & bit) {
                    // An unexpectedly early external edge must still retrigger.
                    // Force low now, then raise on the next timer tick.
                    gates &= ~bit;
                    pending |= bit;
                } else {
                    gates |= bit;
                    pending &= ~bit;
                }
            } else {
                c.remaining = 0;
                gates &= ~bit;
                pending &= ~bit;
            }
        }
    }

    // 1 kHz Timer1 service: OLED/Wire never runs in this context.
    void tick() {
        ++now;
        for (uint8_t i = 0; i < ChannelCount; ++i) {
            Channel &c = channels[i];
            const uint8_t bit = uint8_t(1) << i;
            if (pending & bit) {
                pending &= ~bit;
                gates |= bit;
            } else if (c.remaining && --c.remaining == 0) {
                gates &= ~bit;
            }
        }
        if (external) {
            uint32_t timeout = uint32_t(period) * 3;
            if (timeout < 1000) timeout = 1000;
            if (externalSeen && uint32_t(now - lastExternal) > timeout)
                externalSeen = false;
            return;
        }
        if (clockRemaining && --clockRemaining == 0) clockHigh = false;
        phase += uint16_t(bpm) * stepsPerBeat;
        if (phase >= 60000UL) {
            phase -= 60000UL;
            period = internalPeriod();
            clockHigh = true;
            clockRemaining = period / 2;
            advance();
        }
    }

    // Conditioned 0..5 V input. Every edge is copied; accepted rising edges
    // advance the sequencer. 20 ms minimum spacing limits steps to 50 Hz.
    void externalEdge(bool high) {
        if (!external) return;
        clockHigh = high;
        if (!high) return;
        const uint32_t elapsed = now - lastExternal;
        if (haveExternalEdge && elapsed < 20) return;
        period = haveExternalEdge && elapsed <= 10000 ? elapsed : internalPeriod();
        lastExternal = now;
        haveExternalEdge = true;
        externalSeen = true;
        advance();
    }
};
} // namespace ritmo
