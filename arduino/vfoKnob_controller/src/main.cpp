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
const int ENC_CLK = 2;  // CLK (A) — INT0selection vie
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

volatile uint8_t encState  = 3;   // last quadrature state
volatile int8_t  encAccum  = 0;   // accumulator, reset at each detent

// ── Frequency tracking and deferred send ─────────────────────────────────────

// pendingKhz and lastStepMs are written by the ISR and read by the main loop.
// Access them with noInterrupts()/interrupts() for atomic reads on AVR.
volatile long          pendingKhz = 0;
volatile unsigned long lastStepMs = 0;

long          baseTxFreqHz = 0;
const unsigned long IDLE_MS = 300;

// Tuning step per encoder click (kHz) — button will cycle this later
volatile long stepKhz = 1;

// After sending SET_FREQ, ignore FREQ_TX from Python for this long.
// Prevents a stale FREQ_TX (in transit before Python processed SET_FREQ)
// from reverting the optimistic baseTxFreqHz update.
unsigned long freqTxIgnoreUntilMs = 0;
const unsigned long FREQ_TX_IGNORE_MS = 1500;

// ── Button ────────────────────────────────────────────────────────────────────

int lastSw = HIGH;

// ── LCD ───────────────────────────────────────────────────────────────────────

long lastLcdFreq = -1;

// Format hz as "14.195.000 Hz" padded to exactly 16 chars.
void formatFreqLine(long hz, char *buf) {
  char tmp[17];
  if (hz <= 0) {
    strcpy(tmp, "No signal");
  } else {
    long mhz = hz / 1000000L;
    long khz = (hz % 1000000L) / 1000L;
    long rem = hz % 1000L;
    // Only show .0 or .5 (one decimal place for kHz)
    int half = (rem >= 500) ? 5 : 0;
    sprintf(tmp, "%ld.%03ld.%1d", mhz, khz, half);
  }
  int len = strlen(tmp);
  memcpy(buf, tmp, len);
  for (int i = len; i < 16; i++) buf[i] = ' ';
  buf[16] = '\0';
}

// Format "Step: N kHz    " padded to exactly 16 chars.
void formatStepLine(long step, char *buf) {
  // Always show step as "1.0"
  strcpy(buf, "1.0");
}

void lcdWriteLine(int row, const char *text) {
  lcd.setCursor(0, row);
  lcd.print(text);
}

// Called only when baseTxFreqHz changes.
void updateLcdFreq() {
  if (baseTxFreqHz == lastLcdFreq) return;
  lastLcdFreq = baseTxFreqHz;
  char buf[17];
  formatFreqLine(baseTxFreqHz, buf);
  lcdWriteLine(0, buf);
  // Update step at (9,1) and 'KHz' at (13,1) with one space between value and K
  formatStepLine(stepKhz, buf);
  lcd.setCursor(9, 1);
  lcd.print(buf);
  lcd.print(" KHz"); // ensures one space between digit and K
}

// ── Encoder ISR ───────────────────────────────────────────────────────────────
// Fires on every CHANGE of CLK or DT.
// Direct port read (PIND) is used instead of digitalRead() for speed.
// ENC_CLK = pin 2 = PD2, ENC_DT = pin 3 = PD3.

void encoderISR() {
  uint8_t pins  = PIND;
  uint8_t state = (((pins >> 2) & 1) << 1) | ((pins >> 3) & 1);
  if (state == encState) return;

  encAccum += ENC_TABLE[(encState << 2) | state];
  encState = state;

  // Only count when encoder returns to its detent (both pins HIGH).
  if (state == 3) {
    if (encAccum >= 2) {
      pendingKhz -= stepKhz;
      lastStepMs  = millis();
    } else if (encAccum <= -2) {
      pendingKhz += stepKhz;
      lastStepMs  = millis();
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

  if (strncmp(line, "FREQ_TX:", 8) == 0) {
    long freq = atol(line + 8);
    noInterrupts();
    long pk = pendingKhz;
    interrupts();
    if (pk == 0 && millis() >= freqTxIgnoreUntilMs) {
      baseTxFreqHz = freq;
      updateLcdFreq();
    }
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
  // Atomic snapshot of ISR-written variables.
  noInterrupts();
  long          pk  = pendingKhz;
  // ...existing code...
  interrupts();

  if (pk == 0) return;
  // Instantly send frequency update after rotary detent, no IDLE_MS delay

  if (baseTxFreqHz > 0) {
    long newFreq = baseTxFreqHz + (pk * 1000L);
    ftdiSerial.print("SET_FREQ:");
    ftdiSerial.println(newFreq);
    baseTxFreqHz = newFreq;          // optimistic update
    updateLcdFreq();                 // update LCD instantly
    freqTxIgnoreUntilMs = millis() + FREQ_TX_IGNORE_MS;
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

  char buf[17];
  formatFreqLine(0, buf);
  lcdWriteLine(0, buf);
  // Show step at (9,1) and 'Khz' at (14,1)
  formatStepLine(stepKhz, buf);
  lcd.setCursor(9, 1);
  lcd.print(buf);
  lcd.setCursor(13, 1);
  lcd.print("KHz");

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
