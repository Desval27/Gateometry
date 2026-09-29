![Gateometry Header](assets/GATEOMETRY_header.png)

# Gateometry

Eight-channel Euclidean rhythm generator for a **classic Arduino Nano, ATmega328P,
16 MHz / 5 V**, using a **128×64 SSD1306 I2C OLED** and Adafruit GFX. This does not
target the Nano Every, Nano ESP32, or a SH1106 display.

## Resources

- [PCB](hardware/README.md)
- Panel coming soon.

## Build and upload

```sh
pio run
pio run -t upload
```

The existing `nanoatmega328` environment is retained. It uses the old Nano
bootloader setting. For a newer bootloader, change the `board` setting to
`nanoatmega328new`; the environment name can stay unchanged. If necessary set
`upload_port` in `platformio.ini`. PlatformIO installs the Adafruit dependencies.

The sketch starts **stopped**, using the internal clock at **120 BPM**, with
**4 steps per beat** (sixteenth notes). All channels start with **N=16, K=4,
rotation=0, duty=50%**. Settings are RAM-only and return to defaults at power-up.

## Controls and display

- **Run/Stop button:** stop immediately clears all channel gates after button
  debounce. Starting resets every channel and plays slot zero on the next clock
  rising edge. The clock itself keeps running and remains available at Clock Out.
- **Overview:** rotate the encoder to select the header or one of eight channels;
  click to open the selected editor. The header opens global clock settings.
- **Inside either editor:** turn to change the highlighted parameter; click to
  select the next parameter. Hold for 600 ms to return to the overview.
- **Global editor:** choose Internal/External, BPM (20–300), and steps per beat
  (1, 2, 4, or 8). For source selection, clockwise selects External and
  counterclockwise selects Internal. BPM and steps/beat apply to the internal
  clock and the initial external gate-length estimate.
- **Channel editor:** N=1–32, K=0–N, signed rotation=−(N−1)…+(N−1), duty=1–99%.
  Reducing N clamps K and rotation to valid values. Changing a channel parameter
  clears its gate and restarts that channel at slot zero on the next clock;
  the other channels retain their position.
- **Mute toggles:** closed to ground = muted; open = enabled. Muting clears the
  gate after switch debounce; the channel continues counting slots. Unmuting
  waits for the next eligible hit, so it does not create a partial pulse.

Each overview necklace shows every slot, filled hits, and a radial playhead.
Slot zero is at twelve o'clock; time and positive rotation run clockwise.
A diagonal slash marks a muted channel; a small square indicates its gate is
currently high. The selected channel gets a border. The detail page enlarges
its necklace. At 32 slots the small overview is necessarily dense; use the
larger editor to inspect individual slots. The OLED refreshes at about 15 fps,
so very short pulses may not appear in every frame; physical LEDs follow gates.

## Clock and gate behavior

One clock rising edge advances **one slot on every channel**. Each channel wraps
at its own N. For example, N=5 and N=7 realign after 35 clock edges. Euclidean
onsets are evenly distributed with the first unrotated hit at slot zero:
N=8/K=3 gives `10010010`. A positive rotation of one gives `01001001`.

Internal clock interval is `60000 / (BPM × stepsPerBeat)` milliseconds, with a
fractional accumulator to avoid cumulative integer-rounding drift. Clock Out
and the clock LED use approximately 50% duty, and operate even while stopped.
The selected source is explicit; inserting a cable does not automatically
change sources. Changing source clears gates and resets channel positions.

External clock input uses D4's pin-change interrupt. Clock Out and its LED
follow **both edges** of the conditioned input, including while stopped. This
preserves incoming pulse width, with interrupt latency; it is a logic-level
copy, not an analog voltage copy. There is no clock multiplication or division
in external mode: feed a sixteenth-note clock if that is the desired slot rate.

External rising edges closer than 20 ms to the last accepted edge are ignored
by the sequencer (maximum accepted step rate 50 Hz), although Clock Out still
copies them. Use clean, debounced clock edges; this interval guard is not a
replacement for input conditioning. Period measurement supports 20–10,000 ms.
The first pulse, and the first after a gap longer than 10 seconds, uses the
internal BPM/steps setting to estimate its gate length. Following pulses use
the latest measured rising-to-rising interval. The header displays that interval
in milliseconds; `WAIT` appears after the longer of one second or three estimated
periods without a new accepted edge. It does not switch to the internal clock.
If the input stays high, Clock Out stays high as well.

Channel duty is a percentage of **one slot interval**, not the time between
filled slots. Width is quantized to milliseconds, with a minimum of one timer
tick and a maximum one tick shorter than the estimated interval. This leaves a
low gap between adjacent hits. When an external clock suddenly accelerates
before a previous gate has finished, that gate is lowered and retriggered on
the next 1 kHz timer tick. External widths depend on the previous interval,
so abrupt tempo changes cannot have a perfectly predicted duty cycle. If the
external clock disappears, active channel gates still expire normally.

## Nano pin allocation

The pin assignments are fixed in `src/main.cpp`. D2/D3, D4, D5/D6, and D10 use
AVR port/interrupt registers as well as Arduino pin constants; changing those
constants alone is insufficient to move these signals.

| Nano pin | Connection |
| --- | --- |
| D2 / INT0 | Encoder A, contact to ground, internal pull-up |
| D3 / INT1 | Encoder B, contact to ground, internal pull-up |
| D4 / PCINT20 | Conditioned external clock, active high, 0–5 V |
| D5 | Clock Out logic, through output buffer to jack |
| D6 | Clock LED anode through 2.2–4.7 kΩ; cathode to ground |
| D7 | 74HC165 SH/LD (active-low parallel load) |
| D8 | 74HC165 CLK |
| D9 | 74HC165 QH (non-inverted serial output) |
| D10 | 74HC595 RCLK (output latch) |
| D11 / MOSI | 74HC595 SER (serial data) |
| D13 / SCK | 74HC595 SRCLK (shift clock) |
| A0 | Encoder push switch to ground, internal pull-up |
| A1 | 74HC595 OE, with external 10 kΩ pull-up to +5 V |
| A2 | Run/Stop push switch to ground, internal pull-up |
| A4 / SDA | OLED SDA |
| A5 / SCL | OLED SCL |
| D0/D1, D12, A3 | Spare |
| A6/A7 | Unused; analog input only on this Nano |

The encoder common terminal goes to ground. No encoder or button pin is shared
with a jack. Quadrature decoding runs on both encoder interrupts. Set
`EncoderDirection` to −1 if needed; set `EncoderTransitions` to 2 for an encoder
that produces two transitions per detent instead of four.

OLED address defaults to `0x3C`; change `OledAddress` to `0x3D` if necessary.
Use a module compatible with the Nano's 5 V I2C levels, or add appropriate
level translation. A bare SSD1306 panel is not a drop-in 5 V module. The sketch
assumes the module supplies its own reset circuit (no dedicated OLED reset pin).
Use suitable I2C pull-ups and short wiring for the 400 kHz bus.

## Shift registers and output stages

Use **one 74HC595** for the eight gate logic signals, and **one 74HC165** for
the eight mute switches. The 165 uses a separate clock so switch scanning cannot
interfere with interrupt-driven gate updates. Gate LEDs can follow the buffered
gate signals, so no additional LED shift register is necessary.

For the usual 16-pin packages, verify the chosen manufacturer's pinout:

| 74HC595 signal | IC pin | Wire to |
| --- | --- | --- |
| VCC / GND | 16 / 8 | +5 V / ground |
| SER / SRCLK / RCLK | 14 / 11 / 12 | Nano D11 / D13 / D10 |
| OE | 13 | Nano A1 and 10 kΩ pull-up to +5 V |
| SRCLR | 10 | +5 V |
| QA | 15 | Channel 1 buffer input |
| QB…QH | 1…7 | Channels 2…8 buffer inputs |
| QH' | 9 | Unconnected |

Add a 100 kΩ pull-down at each buffer input driven by QA…QH. OE is held high
through reset and bootloader activity; these pull-downs define a low while the
595 outputs are high impedance. The sketch latches zeros before enabling OE.

| 74HC165 signal | IC pin | Wire to |
| --- | --- | --- |
| VCC / GND | 16 / 8 | +5 V / ground |
| SH/LD / CLK / QH | 1 / 2 / 9 | Nano D7 / D8 / D9 |
| CLK INH | 15 | Ground |
| SER | 10 | Ground |
| A / B / C / D | 11 / 12 / 13 / 14 | Channel 1 / 2 / 3 / 4 mute switch |
| E / F / G / H | 3 / 4 / 5 / 6 | Channel 5 / 6 / 7 / 8 mute switch |
| Inverted QH | 7 | Unconnected |

Each A…H input needs its **own 10 kΩ pull-up to +5 V** and a toggle switch to
ground. There are no internal pull-ups in the 165. Place 100 nF decoupling
capacitors at both registers and every buffer IC.

A practical prototype output arrangement is two **74AHCT541 non-inverting
octal buffers**, powered from +5 V. One serves the eight gate outputs; one
channel of the second serves Clock Out. Tie both active-low enables on each
buffer to ground, tie unused inputs to ground, and leave unused outputs open.
Add a 100 kΩ pull-down on the Clock Out buffer input as well.

```text
595 QA..QH ----+--> 74AHCT541 input
              |
             100k
              |
             GND

buffer output ---- 1k ---- gate jack tip
       |
       +---- 4.7k ---- LED anode; LED cathode to GND

Nano D5 --> buffered identically --> clock jack tip
jack sleeves ---------------------> common GND
```

This provides nominal 0/+5 V gates for modules that accept 5 V logic gates.
The series resistor limits short-circuit current and isolates cable loading;
keep per-pin and total package current within the buffer datasheet ratings.
Take the LED branch before the jack's series resistor. The clock LED already
has its own Nano D6 drive. The 595 only drives buffer inputs in this arrangement.
This is a prototype interface suggestion, not a fully protected Eurorack output
circuit: patching an output against an external voltage requires suitable
additional protection, and modules requiring higher gates need another driver.

**Do not connect a raw Eurorack clock jack directly to D4.** A practical
through-hole prototype input stage uses two gates of a **74HCT14 Schmitt-trigger
inverter**, powered from +5 V, and two **Vishay BAT85S Schottky clamp diodes**:

```text
clock jack tip ----+---- 22k ----+---- 74HCT14 ---- 74HCT14 ---- Nano D4
                  |            |      gate 1      gate 2
                 100k          +---- upper clamp to +5 V
                  |            +---- lower clamp to GND
                 GND

jack sleeve ------------------------------------------------ common GND
```

Connect both clamps at the protected node **after the 22 kΩ resistor**, close
to the first gate's input:

| Clamp diode | Anode | Cathode (banded end) |
| --- | --- | --- |
| Upper BAT85S | Protected input node | +5 V |
| Lower BAT85S | GND | Protected input node |

The 100 kΩ jack-to-ground pull-down defines the unplugged state. The 22 kΩ
series resistor limits current when an input goes below ground or above +5 V;
the diodes clamp the protected node near those rails. With approximately 0.3 V
diode drops, a +12 V input produces about 0.30 mA through the upper clamp, and
a −12 V input produces about 0.53 mA through the lower clamp.

The first gate's hysteresis cleans up slow or noisy transitions. The second
gate restores polarity, giving D4 clean, non-inverted 0–5 V logic that idles
low when unplugged. This matters because Clock Out follows both input edges.
The **74HCT14** has lower, TTL-compatible input thresholds than the 74HC14,
giving more margin for 3.3 V clocks while accepting 5 V clocks through the same
network. See the [74HC14/74HCT14 datasheet](https://assets.nexperia.com/documents/data-sheet/74HC_HCT14.pdf).
Place a **100 nF bypass capacitor** directly between the IC's supply pins;
tie unused gate inputs to ground and leave unused outputs unconnected.

The [Vishay BAT85S](https://www.vishay.com/docs/85513/bat85s.pdf) is an axial
**DO-35 through-hole** diode despite its “S” suffix; **BAT85S-TAP** is one
ordering code. It is rated for 30 V reverse voltage and 200 mA forward current,
with a maximum forward drop of 0.32 V at 1 mA at 25°C.

This is a starting circuit for a **powered prototype**, not a fully qualified
Eurorack input. Verify thresholds, clamp voltages and clean edges over the
intended input-voltage and temperature range. The upper clamp injects current
into the +5 V rail: the powered circuit must absorb that current without raising
the rail, and an external clock can partially power the module when it is off.
Powered-off patch protection needs additional circuitry. This analog front end
is separate from the sketch.

Supply the Nano, registers and buffers from a regulated +5 V rail with common
signal ground. A Eurorack +12 V rail must be regulated for these logic parts;
use proper reverse-polarity protection and decoupling at the module power input.
Plan USB/power isolation if the Nano will remain connected to USB while rack
power is present.

## Implementation and validation

- `include/RhythmEngine.h`: portable Euclidean pattern and clock/gate engine.
- `src/main.cpp`: Nano interrupts, shift registers, debounced controls, OLED UI.
- `test/test_engine.cpp`: host-side behavioral regression tests.

Timer1 runs at 1 kHz, owns gate expiration and internal clock generation, and
uses hardware SPI to latch the gate byte. External edges advance directly from
a pin-change ISR. Foreground changes and snapshots use short atomic blocks;
Wire/display operations never occur in interrupts. Timer0 remains available
for Arduino timekeeping. Do not add Servo/Timer1 code or another SPI owner
without redesigning this allocation. The onboard D13 LED will flicker with SPI
traffic; use the separate D6 clock LED on the panel.

An absent OLED leaves clock/gates and physical controls operational with the
default settings. A Wire timeout disables further display refreshes until
reset. Clock/gate interrupts continue during display transfers and timeouts.

Verified with PlatformIO Atmel AVR 5.3.0, Adafruit GFX 1.12.6, and Adafruit
SSD1306 2.5.17: **19,004 bytes flash; 546 bytes static RAM**. SSD1306 allocates
another **1,024 bytes at runtime**, which PlatformIO's RAM summary does not
include. That leaves about **478 bytes before heap bookkeeping and stack**.
Avoid adding `String`, large local arrays, or large libraries without reviewing
memory usage. Stack margin and ISR latency still need measurement on hardware.

Run the portable tests on a host with GCC:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -Iinclude test/test_engine.cpp \
    -o /tmp/gateometry-test
/tmp/gateometry-test
```

The tests check every supported N/K/rotation combination, equal-spacing gaps,
clock counts at several BPM/subdivision extremes, gate-width bounds, run/stop,
independent channel lengths/duties, mute/unmute, external period estimation,
sudden tempo changes/retriggering, slow clocks, clock loss, and timer rollover.
The firmware builds and these tests pass. **Physical hardware has not been
exercised**. Before connecting other modules, verify output polarity/levels,
channel ordering, boot-time silence, encoder detents, input conditioning, and
clock/gate timing on a scope, including during OLED updates.

## References

- [Arduino Nano specifications](https://store.arduino.cc/arduino-nano)
- [ATmega328P datasheet: interrupt and timer registers](https://docs.arduino.cc/resources/datasheets/ATmega328P-datasheet.pdf)
- [PlatformIO Nano configuration](https://docs.platformio.org/en/latest/boards/atmelavr/nanoatmega328.html)
- [Adafruit SSD1306 implementation and framebuffer allocation](https://github.com/adafruit/Adafruit_SSD1306/blob/master/Adafruit_SSD1306.cpp)
- [TI 74HC595 datasheet](https://www.ti.com/lit/ds/symlink/sn74hc595.pdf)
- [TI 74HC165 datasheet and pinout](https://www.ti.com/product/SN74HC165)
- [TI 74AHCT541 buffer specifications](https://www.ti.com/product/SN74AHCT541)

## ⚠️ Disclaimer⚠️

This project is provided **"as is"**, without warranty of any kind, express or implied,
including but not limited to the warranties of merchantability, fitness for a
particular purpose, and noninfringement.

This is a DIY electronics project intended for educational and experimental use.
Use at your own risk.

The author assumes **no responsibility or liability** for any damage, injury, or loss
resulting from the use or misuse of this design, including but not limited to:

* Damage to connected equipment (Eurorack modules, power supplies, computers, etc.)
* Incorrect assembly or wiring
* Use with incompatible voltages or signal levels
* Personal injury

### Electrical Safety

This project may interface with:

* ±12V & +5V Power rails
* AC Mains Power (Lethal Risk)
* External control voltages (CV)
* Digital and analog circuitry

Improper handling may result in damage or unsafe conditions.

You are responsible for:

* Verifying all connections before powering the system
* Ensuring correct polarity of power connections
* Confirming voltage ranges and signal compatibility
* Using appropriate protection circuits where necessary

### No Guarantees

Schematics, PCB layouts, and code are provided for reference only.
They may contain errors, omissions, or design flaws.

**Always review and validate the design before building or using it.**
