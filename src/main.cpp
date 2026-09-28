#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <Wire.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>
#include "RhythmEngine.h"

#if !defined(__AVR_ATmega328P__) || F_CPU != 16000000UL
#error "MiRitmo requires a classic 16 MHz ATmega328P Nano."
#endif

namespace {
// Direct-port code below also depends on these assignments; see README.md.
constexpr uint8_t EncoderA = 2, EncoderB = 3, ClockIn = 4;
constexpr uint8_t ClockOut = 5, ClockLed = 6;
constexpr uint8_t SwitchLoad = 7, SwitchClock = 8, SwitchData = 9;
constexpr uint8_t GateLatch = 10; // 595 data = D11, clock = D13 (hardware SPI)
constexpr uint8_t EncoderButton = A0, GateEnable = A1, RunButton = A2;
constexpr uint8_t OledAddress = 0x3C;
constexpr int8_t EncoderDirection = 1; // Use -1 if rotation feels backwards.
constexpr int8_t EncoderTransitions = 4; // Some encoders need 2 per detent.
constexpr uint16_t DebounceMs = 20, LongPressMs = 600;

Adafruit_SSD1306 display(128, 64, &Wire, -1, 400000UL, 400000UL);
ritmo::Engine engine;
ritmo::Settings settings[ritmo::ChannelCount];
volatile int8_t encoderMovement = 0;
uint8_t encoderPrevious = 0;
uint8_t lastGates = 0xFF;
bool displayReady = false;

// Only call in an ISR or ATOMIC_BLOCK. SPI is reserved exclusively for the 595.
void flushOutputs() {
    const uint8_t gates = engine.gates;
    if (gates != lastGates) {
        PORTB &= ~_BV(PB2); // D10 latch low
        SPDR = gates;
        while (!(SPSR & _BV(SPIF))) {}
        (void)SPDR;
        PORTB |= _BV(PB2);
        lastGates = gates;
    }
    if (engine.clockHigh) PORTD |= _BV(PD5) | _BV(PD6);
    else PORTD &= ~(_BV(PD5) | _BV(PD6));
}

const int8_t EncoderTable[16] PROGMEM = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0
};

void encoderChanged() {
    const uint8_t state = (PIND >> 2) & 3;
    const int8_t delta = int8_t(pgm_read_byte(&EncoderTable[(encoderPrevious << 2) | state]));
    encoderPrevious = state;
    const int16_t next = int16_t(encoderMovement) + delta;
    if (next >= -120 && next <= 120) encoderMovement = next;
}

struct Button {
    uint8_t pin;
    bool candidate = false, down = false;
    uint32_t changed = 0;
    explicit Button(uint8_t p) : pin(p) {}
    // 1 = pressed, -1 = released, 0 = no debounced transition.
    int8_t poll(uint32_t now) {
        const bool sample = digitalRead(pin) == LOW;
        if (sample != candidate) { candidate = sample; changed = now; }
        if (candidate != down && uint32_t(now - changed) >= DebounceMs) {
            down = candidate;
            return down ? 1 : -1;
        }
        return 0;
    }
};
Button runButton(RunButton), encoderButton(EncoderButton);

enum class Page : uint8_t { Overview, Global, Channel };
Page page = Page::Overview;
uint8_t selection = 0; // 0 = global header, 1..8 = channels
uint8_t field = 0;
uint16_t bpm = 120;
uint8_t rateIndex = 2;
bool externalClock = false;
uint32_t encoderPressedAt = 0;
bool longPressHandled = false;

int16_t bounded(int16_t value, int16_t low, int16_t high) {
    return value < low ? low : (value > high ? high : value);
}

void configureChannel(uint8_t index) {
    const uint32_t bits = ritmo::pattern(settings[index]);
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        engine.configure(index, settings[index], bits);
        flushOutputs();
    }
}

void turnEncoder(int8_t delta) {
    if (page == Page::Overview) {
        int16_t next = (int16_t(selection) + delta) % 9;
        if (next < 0) next += 9;
        selection = next;
    } else if (page == Page::Global) {
        if (field == 0) {
            const bool source = delta > 0;
            if (source != externalClock) {
                externalClock = source;
                ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                    engine.setSource(source);
                    // Match an already-high external input without inventing a step.
                    if (source) engine.clockHigh = (PIND & _BV(PD4)) != 0;
                    flushOutputs();
                }
            }
        } else if (field == 1) {
            bpm = bounded(int16_t(bpm) + delta, 20, 300);
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { engine.bpm = bpm; }
        } else {
            rateIndex = bounded(int16_t(rateIndex) + delta, 0, 3);
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { engine.stepsPerBeat = 1 << rateIndex; }
        }
    } else {
        const uint8_t ch = selection - 1;
        ritmo::Settings &s = settings[ch];
        switch (field) {
        case 0:
            s.steps = bounded(int16_t(s.steps) + delta, 1, ritmo::MaxSteps);
            if (s.hits > s.steps) s.hits = s.steps;
            s.rotation = bounded(s.rotation, 1 - int16_t(s.steps), s.steps - 1);
            break;
        case 1: s.hits = bounded(int16_t(s.hits) + delta, 0, s.steps); break;
        case 2: s.rotation = bounded(int16_t(s.rotation) + delta, 1 - int16_t(s.steps), s.steps - 1); break;
        case 3: s.duty = bounded(int16_t(s.duty) + delta, 1, 99); break;
        }
        configureChannel(ch);
    }
}

void clickEncoder() {
    if (page == Page::Overview) {
        page = selection ? Page::Channel : Page::Global;
        field = 0;
    } else {
        field = (field + 1) % (page == Page::Global ? 3 : 4);
    }
}

uint8_t readSwitches() {
    // QH holds H before the first clock: H->bit7, ..., A->bit0.
    uint8_t bits = 0;
    digitalWrite(SwitchClock, LOW);
    digitalWrite(SwitchLoad, LOW);
    delayMicroseconds(1);
    digitalWrite(SwitchLoad, HIGH);
    for (uint8_t i = 0; i < 8; ++i) {
        bits = (bits << 1) | (digitalRead(SwitchData) == HIGH);
        digitalWrite(SwitchClock, HIGH);
        digitalWrite(SwitchClock, LOW);
    }
    return bits; // Pull-up = enabled; switch closed to GND = muted.
}

void pollSwitches(uint32_t now) {
    static uint32_t lastPoll = 0;
    static uint8_t stable = 0, candidate = 0;
    static uint16_t changed[8] = {};
    if (uint32_t(now - lastPoll) < 5) return;
    lastPoll = now;
    const uint8_t sample = readSwitches();
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t bit = uint8_t(1) << i;
        if ((sample ^ candidate) & bit) {
            candidate ^= bit;
            changed[i] = uint16_t(now);
        }
        if (((stable ^ candidate) & bit) && uint16_t(uint16_t(now) - changed[i]) >= DebounceMs)
            stable ^= bit;
    }
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        engine.setEnabled(stable);
        flushOutputs();
    }
}

void pollControls(uint32_t now) {
    if (runButton.poll(now) == 1) {
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            engine.setRunning(!engine.running);
            flushOutputs();
        }
    }
    const int8_t event = encoderButton.poll(now);
    if (event == 1) { encoderPressedAt = now; longPressHandled = false; }
    if (encoderButton.down && !longPressHandled && uint32_t(now - encoderPressedAt) >= LongPressMs) {
        page = Page::Overview;
        longPressHandled = true;
    }
    if (event == -1 && !longPressHandled) clickEncoder();
    int8_t movement;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { movement = encoderMovement; encoderMovement = 0; }
    static int16_t remainder = 0;
    remainder += movement * EncoderDirection;
    const int8_t detents = remainder / EncoderTransitions;
    remainder %= EncoderTransitions;
    if (detents) turnEncoder(detents);
    pollSwitches(now);
}

struct Snapshot {
    uint8_t current[8];
    uint8_t gates, enabled;
    uint16_t period;
    bool running, locked;
};

// A quarter sine wave in flash avoids floating point and coordinate RAM tables.
const uint8_t SineQuarter[17] PROGMEM = {
    0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126, 127
};
int16_t sine(uint8_t angle) {
    angle &= 63;
    const uint8_t quadrant = angle >> 4;
    const uint8_t offset = angle & 15;
    const int16_t value = pgm_read_byte(&SineQuarter[(quadrant & 1) ? 16 - offset : offset]);
    return quadrant >= 2 ? -value : value;
}
void point(uint8_t slot, uint8_t steps, int8_t radius, int16_t &x, int16_t &y) {
    const uint8_t angle = uint16_t(slot) * 64 / steps;
    x = sine(angle) * radius / 127;
    y = -sine(angle + 16) * radius / 127;
}

void necklace(uint8_t ch, int16_t cx, int16_t cy, int8_t radius, uint8_t current, bool large) {
    const ritmo::Settings &s = settings[ch];
    const uint32_t bits = ritmo::pattern(s);
    if (large) display.drawCircle(cx, cy, radius, SSD1306_WHITE);
    for (uint8_t slot = 0; slot < s.steps; ++slot) {
        int16_t x, y;
        point(slot, s.steps, radius, x, y);
        if (bits & (uint32_t(1) << slot)) display.fillCircle(cx + x, cy + y, large ? 2 : 1, SSD1306_WHITE);
        else if (large) display.drawCircle(cx + x, cy + y, 1, SSD1306_WHITE);
        else display.drawPixel(cx + x, cy + y, SSD1306_WHITE);
    }
    if (current != ritmo::NoSlot) {
        int16_t x, y, x2, y2;
        point(current, s.steps, radius + (large ? 4 : 2), x, y);
        point(current, s.steps, large ? 5 : 3, x2, y2);
        display.drawLine(cx + x2, cy + y2, cx + x, cy + y, SSD1306_WHITE);
    }
}

void drawHeader(const Snapshot &s) {
    const bool selected = page == Page::Overview && selection == 0;
    if (selected) display.fillRect(0, 0, 128, 8, SSD1306_WHITE);
    display.setTextColor(selected ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print(s.running ? F("RUN ") : F("STOP "));
    if (externalClock) {
        display.print(F("EXT "));
        if (s.locked) { display.print(s.period); display.print(F("ms")); }
        else display.print(F("WAIT"));
    } else {
        display.print(F("INT "));
        display.print(bpm);
        display.print(F(" BPM"));
    }
    display.setTextColor(SSD1306_WHITE);
}

void drawField(uint8_t index, uint8_t x, uint8_t y, const __FlashStringHelper *label, int16_t value) {
    display.setCursor(x, y);
    display.print(field == index ? '>' : ' ');
    display.print(label);
    display.print(value);
}

void render() {
    Snapshot s;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        for (uint8_t i = 0; i < 8; ++i) s.current[i] = engine.channels[i].current;
        s.gates = engine.gates;
        s.enabled = engine.enabled;
        s.running = engine.running;
        s.locked = engine.externalSeen;
        s.period = engine.period;
    }
    display.clearDisplay();
    drawHeader(s);
    if (page == Page::Overview) {
        for (uint8_t i = 0; i < 8; ++i) {
            const uint8_t x = (i % 4) * 32, y = 9 + (i / 4) * 27;
            if (selection == i + 1) display.drawRect(x, y, 32, 27, SSD1306_WHITE);
            display.setCursor(x + 2, y + 2);
            display.print(i + 1);
            necklace(i, x + 17, y + 13, 9, s.current[i], false);
            if (!(s.enabled & (1 << i))) {
                display.drawLine(x + 9, y + 21, x + 25, y + 5, SSD1306_WHITE);
            }
            if (s.gates & (1 << i)) display.fillRect(x + 2, y + 21, 3, 3, SSD1306_WHITE);
        }
    } else if (page == Page::Global) {
        display.setCursor(0, 14);
        display.print(field == 0 ? F(">Clock: ") : F(" Clock: "));
        display.print(externalClock ? F("External") : F("Internal"));
        drawField(1, 0, 27, F("BPM: "), bpm);
        drawField(2, 0, 40, F("Steps/beat: "), 1 << rateIndex);
        display.setCursor(0, 56);
        display.print(F("Click:next Hold:back"));
    } else {
        const uint8_t ch = selection - 1;
        necklace(ch, 29, 37, 20, s.current[ch], true);
        display.setCursor(18, 34);
        display.print(F("CH")); display.print(ch + 1);
        if (!(s.enabled & (1 << ch))) {
            display.setCursor(18, 43); display.print(F("MUT"));
        }
        drawField(0, 62, 13, F("N:"), settings[ch].steps);
        drawField(1, 62, 24, F("K:"), settings[ch].hits);
        drawField(2, 62, 35, F("R:"), settings[ch].rotation);
        drawField(3, 62, 46, F("D:"), settings[ch].duty);
        display.print('%');
        display.setCursor(62, 57); display.print(F("Hold:back"));
    }
    display.display(); // Interrupts remain enabled throughout this I2C transfer.
    if (Wire.getWireTimeoutFlag()) {
        Wire.clearWireTimeoutFlag();
        displayReady = false; // Keep sequencing if the display bus fails.
    }
}
} // namespace

ISR(TIMER1_COMPA_vect) {
    engine.tick();
    flushOutputs();
}

ISR(PCINT2_vect) {
    engine.externalEdge((PIND & _BV(PD4)) != 0);
    flushOutputs();
}

void setup() {
    // External 10k OE pull-up keeps output buffers disabled during reset/boot.
    digitalWrite(GateEnable, HIGH);
    pinMode(GateEnable, OUTPUT);
    pinMode(GateLatch, OUTPUT);
    pinMode(ClockOut, OUTPUT);
    pinMode(ClockLed, OUTPUT);
    pinMode(ClockIn, INPUT); // External conditioner must drive a defined level.
    pinMode(EncoderA, INPUT_PULLUP);
    pinMode(EncoderB, INPUT_PULLUP);
    pinMode(EncoderButton, INPUT_PULLUP);
    pinMode(RunButton, INPUT_PULLUP);
    pinMode(SwitchLoad, OUTPUT);
    digitalWrite(SwitchLoad, HIGH);
    pinMode(SwitchClock, OUTPUT);
    pinMode(SwitchData, INPUT);
    SPI.begin();
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    // No other SPI device is used: the interrupt-driven 595 owns this bus.
    engine.enabled = readSwitches();
    for (uint8_t i = 0; i < 8; ++i) configureChannel(i);
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { flushOutputs(); }
    digitalWrite(GateEnable, LOW);

    Wire.begin();
    Wire.setWireTimeout(3000, true);
    Wire.beginTransmission(OledAddress);
    const bool oledPresent = Wire.endTransmission() == 0;
    displayReady = oledPresent && display.begin(SSD1306_SWITCHCAPVCC, OledAddress, true, false);
    if (displayReady) {
        display.setTextSize(1);
        display.setTextWrap(false);
        display.setTextColor(SSD1306_WHITE);
    }

    encoderPrevious = (PIND >> 2) & 3;
    attachInterrupt(digitalPinToInterrupt(EncoderA), encoderChanged, CHANGE);
    attachInterrupt(digitalPinToInterrupt(EncoderB), encoderChanged, CHANGE);
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        PCMSK2 = _BV(PCINT20); // D4 pin-change interrupt; D2/D3 use INT0/INT1.
        PCIFR = _BV(PCIF2);
        PCICR |= _BV(PCIE2);
        TCCR1A = 0;
        TCCR1B = 0;
        TCNT1 = 0;
        OCR1A = F_CPU / 64 / 1000 - 1;
        TIFR1 = _BV(OCF1A);
        TCCR1B = _BV(WGM12) | _BV(CS11) | _BV(CS10);
        TIMSK1 = _BV(OCIE1A);
    }
}

void loop() {
    const uint32_t now = millis();
    pollControls(now);
    static uint32_t lastFrame = 0;
    if (displayReady && uint32_t(now - lastFrame) >= 67) {
        lastFrame = now;
        render(); // ~15 fps; clock and gates continue in interrupts.
    }
}
