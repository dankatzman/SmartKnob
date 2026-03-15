#include <Arduino.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <string.h>

// SoftwareSerial on pins 8/9 — FTDI232 USB adapter — Python communication
// Wiring: FTDI TX → pin 8, FTDI RX → pin 9, FTDI GND → GND
const unsigned long FTDI_BAUD = 57600;
SoftwareSerial ftdiSerial(8, 9);  // RX=pin8, TX=pin9

// I2C LCD 16×2 — address 0x27 (try 0x3F if display stays blank)
// Wiring: SDA → A4, SCL → A5, VCC → 5V, GND → GND
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Rotary encoder pins (CLK/DT must be interrupt-capable: pins 2 and 3 on Uno)
// Hardware debouncing: 0.1µF capacitor from each pin to GND
const int ENC_CLK = 2;  // CLK (A) — INT0
const int ENC_DT  = 3;  // DT  (B) — INT1
const int ENC_SW  = 6;  // Push button

// Identification banner sent to Python over FTDI
const char *ARDUINO_BANNER = "HELLO_ARDUINO:VFOKNOB";

// Buffer for incoming FTDI data
char serialLine[48];
int  serialLineLen = 0;

// Timestamp of last banner sent
unsigned long lastHelloMs = 0;

// ── Rotary encoder — interrupt-driven quadrature state machine ────────────────
// Quadrature lookup table.
// Index = (prevState << 2) | currState, where state = (CLK<<1)|DT.
// +1 = CW, -1 = CCW, 0 = bounce/invalid (ignored).
const int8_t ENC_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

volatile uint8_t encState = 3;   // last quadrature state
volatile int8_t  encAccum = 0;   // accumulator, reset at each detent

// pendingKhz is written by the ISR — read atomically in main loop.
volatile long pendingKhz = 0;

// ── Frequency tracking ────────────────────────────────────────────────────────

long baseTxFreqHz = 0;   // TX base freq for encoder delta math

// After sending SET_FREQ, ignore FREQ_TX from Python for this long.
// Prevents a stale FREQ_TX (in transit before Python processed SET_FREQ)
// from reverting the optimistic baseTxFreqHz update.
unsigned long freqTxIgnoreUntilMs = 0;
const unsigned long FREQ_TX_IGNORE_MS = 1500;

// ── Tuning step ───────────────────────────────────────────────────────────────

volatile long stepKhz = 1;   // kHz per encoder click — button will cycle this later

// ── Button ────────────────────────────────────────────────────────────────────

int lastSw = HIGH;

// ── LCD field layout ──────────────────────────────────────────────────────────
//
//  Col: 0 1         9 10          15
//       ┌─┬─────────┬────────────┐
//  Row0:│ │  FREQ A │  (future)  │
//       ├─┼─────────┼────────────┤
//  Row1:│ │  FREQ B │  (future)  │
//       └─┴─────────┴────────────┘
//
//  FREQ field: col 1–9 (9 chars), rows 0 and 1.
//  All other fields occupy col 9–15 and are added later.
//  Each field has exactly one write function — no other code touches its columns.

// Frequencies shown on the LCD — updated by LCD_FREQ and pollFreqSend.
long lcdFreqA = 0;
long lcdFreqB = 0;
char lcdActiveVfo = 'A';
char txVfo = 'A';    // which VFO is TX — carried in FREQ_TX from Python

long lastLcdFreqA = -1;
long lastLcdFreqB = -1;

// ── FREQ field (col 0–8, 9 chars) ────────────────────────────────────────────

// Format hz into exactly 9 chars, dot-grouped, right-aligned.
//   < 10 MHz : " 7.100.000"  → 10 chars, but we use 9 so trim leading space
//              "7.100.000"  ← 9 chars, 1 Hz resolution
//   10–99 MHz: "14.195.00"  ← 9 chars, 10 Hz resolution
//  ≥ 100 MHz : "144.000.0"  ← 9 chars, 100 Hz resolution
// "No signal" when hz ≤ 0 (also 9 chars).
// Format: "##.###.# " — MHz (2 digits), kHz (3 digits), 100Hz (1 digit), trailing space.
// Examples: "14.195.0 "  " 7.100.5 "  " 1.800.0 "
void formatFreqField(long hz, char *buf) {
  // buf must hold 10 bytes (9 chars + null terminator)
  if (hz <= 0) {
    memcpy(buf, "No signal", 9);
    buf[9] = '\0';
    return;
  }
  long mhz      = hz / 1000000L;
  long khz      = (hz % 1000000L) / 1000L;
  long hundreds = (hz % 1000L) / 100L;
  sprintf(buf, "%2ld.%03ld.%1ld ", mhz, khz, hundreds);
  buf[9] = '\0';
}

// Write the frequency field for one row. Only touches col 0–8.
void writeFreqField(int row, long hz) {
  char buf[10];
  formatFreqField(hz, buf);
  lcd.setCursor(1, row);
  lcd.print(buf);
}

// ── LCD update ────────────────────────────────────────────────────────────────

// Redraws only the rows whose frequency has changed.
void updateLcd() {
  if (lcdFreqA != lastLcdFreqA) {
    lastLcdFreqA = lcdFreqA;
    writeFreqField(0, lcdFreqA);
  }
  if (lcdFreqB != lastLcdFreqB) {
    lastLcdFreqB = lcdFreqB;
    writeFreqField(1, lcdFreqB);
  }
}

// ── Encoder ISR ───────────────────────────────────────────────────────────────
// Fires on every CHANGE of CLK or DT.
// Direct port read (PIND) instead of digitalRead() for speed.
// ENC_CLK = pin 2 = PD2, ENC_DT = pin 3 = PD3.

void encoderISR() {
  uint8_t pins  = PIND;
  uint8_t state = (((pins >> 2) & 1) << 1) | ((pins >> 3) & 1);
  if (state == encState) return;

  encAccum += ENC_TABLE[(encState << 2) | state];
  encState = state;

  // Only count when encoder returns to its detent (both pins HIGH).
  // This gives exactly one count per physical click regardless of bounce.
  if (state == 3) {
    if (encAccum >= 2) {
      pendingKhz -= stepKhz;
    } else if (encAccum <= -2) {
      pendingKhz += stepKhz;
    }
    encAccum = 0;
  }
}

// ── Banner ────────────────────────────────────────────────────────────────────

void sendBanner() {
  ftdiSerial.println(ARDUINO_BANNER);
  lastHelloMs = millis();
}

// ── Command handler ───────────────────────────────────────────────────────────

void handleCommand(const char *line) {
  if (strcmp(line, "WHO") == 0) {
    sendBanner();
    return;
  }

  if (strcmp(line, "PING") == 0) {
    ftdiSerial.println("PONG_ARDUINO");
    return;
  }

  // FREQ_TX:<hz>:<vfo> — TX frequency and VFO letter from Python.
  if (strncmp(line, "FREQ_TX:", 8) == 0) {
    char *p = (char *)line + 8;
    long freq = atol(p);
    char *vp = strchr(p, ':');
    if (vp) txVfo = *(++vp);  // 'A' or 'B'
    noInterrupts();
    long pk = pendingKhz;
    interrupts();
    if (pk == 0 && millis() >= freqTxIgnoreUntilMs) {
      baseTxFreqHz = freq;
    }
    return;
  }

  // LCD_FREQ:<freqA>:<freqB>:<activeVfo> — display frequencies from Python.
  // Python sends this every refresh cycle with the actual confirmed radio values.
  if (strncmp(line, "LCD_FREQ:", 9) == 0) {
    char *p = (char *)line + 9;
    if (millis() >= freqTxIgnoreUntilMs) {
      long newA = atol(p);
      if (newA > 0) lcdFreqA = newA;
      p = strchr(p, ':');
      if (p) {
        long newB = atol(++p);
        if (newB > 0) lcdFreqB = newB;
        p = strchr(p, ':');
        if (p) {
          lcdActiveVfo = *(++p);  // 'A' or 'B'
        }
      }
    }
    updateLcd();
    return;
  }
}

// ── FTDI serial reader ────────────────────────────────────────────────────────

void pollFtdi() {
  while (ftdiSerial.available() > 0) {
    char ch = static_cast<char>(ftdiSerial.read());
    if (ch == '\r') continue;
    if (ch == '\n') {
      serialLine[serialLineLen] = '\0';
      if (serialLineLen > 0) {
        handleCommand(serialLine);
      }
      serialLineLen = 0;
      continue;
    }
    if (serialLineLen < static_cast<int>(sizeof(serialLine) - 1)) {
      serialLine[serialLineLen++] = ch;
    } else {
      serialLineLen = 0;
    }
  }
}

// ── Deferred frequency send ───────────────────────────────────────────────────

void pollFreqSend() {
  // Atomic snapshot of ISR-written pendingKhz.
  noInterrupts();
  long pk = pendingKhz;
  interrupts();

  if (pk == 0) return;

  if (baseTxFreqHz > 0) {
    long newFreq = baseTxFreqHz + (pk * 1000L);
    ftdiSerial.print("SET_FREQ:");
    ftdiSerial.println(newFreq);
    baseTxFreqHz = newFreq;          // optimistic update
    freqTxIgnoreUntilMs = millis() + FREQ_TX_IGNORE_MS;
    // Immediately reflect the new frequency on the TX row only.
    // Do NOT call updateLcd() — that would touch the non-TX row which may
    // still be 0 (not yet received from Python) and write "No signal" there.
    if (txVfo == 'B') {
      lcdFreqB = newFreq;
      lastLcdFreqB = newFreq;   // keep cache in sync
      writeFreqField(1, newFreq);
    } else {
      lcdFreqA = newFreq;
      lastLcdFreqA = newFreq;   // keep cache in sync
      writeFreqField(0, newFreq);
    }
  } else {
    ftdiSerial.println("NO_BASE_FREQ");
  }

  noInterrupts();
  pendingKhz = 0;
  interrupts();
}

// ── Button ────────────────────────────────────────────────────────────────────

void pollButton() {
  int sw = digitalRead(ENC_SW);
  if (sw != lastSw) {
    if (sw == LOW) {
      ftdiSerial.println("BTN:PRESS");
    }
    lastSw = sw;
  }
}

// ── Setup & loop ──────────────────────────────────────────────────────────────

void setup() {
  ftdiSerial.begin(FTDI_BAUD);

  lcd.init();
  lcd.backlight();

  // Initialise both frequency fields to "No signal".
  writeFreqField(0, 0);
  writeFreqField(1, 0);

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);

  encState = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  lastSw   = digitalRead(ENC_SW);

  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT),  encoderISR, CHANGE);

  delay(1200);
  sendBanner();
}

void loop() {
  pollFreqSend();
  pollButton();
  pollFtdi();

  if (millis() - lastHelloMs >= 1000) {
    sendBanner();
  }
}
