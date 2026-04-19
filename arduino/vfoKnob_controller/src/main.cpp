// ── Target Hardware: ESP32 DevKit1 ───────────────────────────────────────────
// Pin Assignment:
//   LCD SDA        → GPIO 21
//   LCD SCL        → GPIO 22
//   Encoder CLK    → GPIO 18
//   Encoder DT     → GPIO 19
//   Encoder SW     → GPIO 5
//   LED RX (LED1)  → GPIO 25
//   LED TX (LED2)  → GPIO 26
//   Switch         → GPIO 27
// ─────────────────────────────────────────────────────────────────────────────

#define LED_RX_PIN 25
#define LED_TX_PIN 26

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <string.h>
#include <EEPROM.h>

// ── UI State Machine ──
enum UiState {
  STATE_ONAIR,
  STATE_EDIT
};
volatile UiState uiState = STATE_ONAIR;

// ── Blinking for step edit mode ──
unsigned long lastBlinkMs = 0;
bool stepFieldVisible = true;

// Hardware serial (USB) — Python communication
const unsigned long PC_BAUD = 57600;

// I2C LCD 16×2 — address 0x27 (try 0x3F if display stays blank)
// Wiring: SDA → A4, SCL → A5, VCC → 5V, GND → GND
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Rotary encoder pins (CLK/DT must be interrupt-capable: pins 2 and 3 on Uno)
// Hardware debouncing: 0.1µF capacitor from each pin to GND
const int ENC_CLK = 18; // CLK (A)
const int ENC_DT  = 19; // DT  (B)

// Pause/resume encoder interrupts (INT0/INT1) during critical sections.
#define ENC_PAUSE()  noInterrupts()
#define ENC_RESUME() interrupts()
const int ENC_SW  = 5;  // Push button
const int KNOB_TARGET_SW = 27; // Toggle: HIGH = knob tunes TX, LOW = knob tunes RX

// Identification banner sent to Python over FTDI
const char *ARDUINO_BANNER = "HELLO_ARDUINO:VFOKNOB";

// Buffer for incoming FTDI data
char serialLine[48];
int  serialLineLen = 0;

// Timestamp of last banner sent
unsigned long lastHelloMs = 0;

// Timestamp of last valid command received from Python (for comm LED)
unsigned long lastPythonMsgMs = 0;
const unsigned long COMM_TIMEOUT_MS = 3000;
bool commConnected = false;   // true while Python is actively connected

// ── Rotary encoder — 4-state quadrature, CHANGE on CLK+DT ───────────────────
// Fires on every edge of CLK and DT. The lookup table filters invalid
// transitions (bounces). Steps accumulate ±1 per valid transition;
// one mechanical detent = 4 valid transitions → fires on reaching ±4.
static const int8_t ENC_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};
volatile uint8_t encState = 0;
volatile int8_t  encAccum = 0;

// pendingHz is written by the ISR — read atomically in main loop.
volatile long pendingHz = 0;




// ── Frequency tracking ────────────────────────────────────────────────────────

// Track if last freq change was manual (not from Arduino knob)

// After sending SET_FREQ, ignore LCD_FREQ updates from Python for this long.
// Keeps the LCD stable while the knob is being turned.
unsigned long freqTxIgnoreUntilMs = 0;
const unsigned long FREQ_TX_IGNORE_MS = 1500;

// ── Frequency Range ──────────────────────────────────────────────────────────
#define EEPROM_FROM_ADDR 4
#define EEPROM_UP_ADDR   8
long rangeFromKHz = 5;   // Default: 5 kHz offset
long rangeUpKHz   = 10;  // Default: 10 kHz offset

// ── Split range state ─────────────────────────────────────────────────────────
// Populated when split activates; cleared when split turns off.
bool  splitActive     = false;
long  splitRangeLow   = 0;    // baseFreq + rangeFromKHz * 1000
long  splitRangeHigh  = 0;    // baseFreq + rangeUpKHz  * 1000
char  splitTargetVfo  = 'A';  // which VFO the knob was tuning during split

// Gate: after a button-initiated split change, suppress LCD_FREQ split
// detection until the radio has caught up (prevents re-triggering).
unsigned long splitGateMs = 0;
const unsigned long SPLIT_GATE_MS = 3000;

// ── System ready flag ─────────────────────────────────────────────────────────
// Encoder output is suppressed until both the band table and a valid frequency
// have been received from Python. Prevents out-of-band jumps at startup.
bool systemReady = false;

// ── Legal band table ──────────────────────────────────────────────────────────
// Sent once by Python at startup from legalHFfreq.txt.
// Arduino looks up the current band in pollFreqSend and clamps accordingly.
#define MAX_BANDS 16
long  bandLow[MAX_BANDS];
long  bandHigh[MAX_BANDS];
int   bandCount = 0;

// Incoming band buffer — filled by BAND_ADD, compared on BAND_DONE.
long  incomingBandLow[MAX_BANDS];
long  incomingBandHigh[MAX_BANDS];
int   incomingCount = 0;
bool  incomingActive = false;  // true after first BAND_ADD in this session

// ── Tuning step ───────────────────────────────────────────────────────────────

volatile long stepHz = 1000;  // Hz per encoder click: 1000 = 1 kHz, 500 = 0.5 kHz
bool encReverse = false;      // true = flip CW/CCW direction
#define EEPROM_STEP_ADDR 0
#define EEPROM_ENC_REVERSE_ADDR 2   // 1 byte: 0x01 = reversed, else normal
#define EEPROM_BAND_MAGIC_ADDR 12   // 2 bytes: 0x5A, 0xA5
#define EEPROM_BAND_COUNT_ADDR 14   // 1 byte
#define EEPROM_BAND_DATA_ADDR  15   // MAX_BANDS * 8 bytes (4 low + 4 high)
#define EEPROM_BAND_MAGIC1 0x5A
#define EEPROM_BAND_MAGIC2 0xA5

// ── Step Edit Mode State ──
unsigned long swPressStart = 0;
bool swWasLongPressed = false;
volatile bool stepChanged = false; // Set by ISR, handled in main loop
bool editFirstFrame = true;        // Reset each time edit mode is entered

// ── Extra buttons (voice message triggers) ───────────────────────────────────
// GPIO 32, 33, 13, 14 — INPUT_PULLUP, fire BTN:n on press in STATE_ONAIR.
const int EXTRA_BTN_PINS[4] = {32, 33, 13, 14};
int    extraBtnLast[4]      = {HIGH, HIGH, HIGH, HIGH};
unsigned long extraBtnMs[4] = {0, 0, 0, 0};
bool   extraBtnArmed[4]     = {true, true, true, true};

void pollExtraButtons() {
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    int raw = digitalRead(EXTRA_BTN_PINS[i]);
    if (raw != extraBtnLast[i]) extraBtnMs[i] = now;
    if ((now - extraBtnMs[i]) >= 50) {
      if (raw == LOW && extraBtnArmed[i]) {
        Serial.print("BTN:");
        Serial.println(i + 1);
        extraBtnArmed[i] = false;
      }
      if (raw == HIGH) extraBtnArmed[i] = true;
    }
    extraBtnLast[i] = raw;
  }
}

// ── Button ────────────────────────────────────────────────────────────────────

int lastSw = HIGH;
//
//  Col: 0 1         9 10 11       15
//       ┌─┬─────────┬──┬──────────┐
//  Row0:│ │  FREQ A │  │ FROM-UP  │
//       ├─┼─────────┼──┼──────────┤
//  Row1:│ │  FREQ B │  │  STEP    │
//       └─┴─────────┴──┴──────────┘
//
//  FREQ field: col 1–9  (9 chars), rows 0 and 1.

//  Each field has exactly one write function — no other code touches its columns.

// Frequencies shown on the LCD — updated by LCD_FREQ and pollFreqSend.
long lcdFreqA = 0;
long lcdFreqB = 0;
char lcdActiveVfo = 'A';
char txVfo = 'A';       // which VFO is TX — updated from LCD_FREQ every cycle
bool pythonSplit = false; // explicit split flag from Python — more reliable than VFO inference

// Track if last freq change was external (manual)
bool externalFreqA = false;
bool externalFreqB = false;

long lastLcdFreqA = -1;
long lastLcdFreqB = -1;
long lastRangeFromKHz = -1;
long lastRangeUpKHz   = -1;
long lastStepHz       = -1;
bool lastEditing      = false;
bool lastSplitActive  = false;

bool snapPendingA = false;
bool snapPendingB = false;

// Pending band-change confirmation — cross-band LCD_FREQ updates are held
// until a second consecutive message confirms the new band (corruption guard).
long pendingFreqA = 0;
unsigned long pendingFreqATimeMs = 0;
long pendingFreqB = 0;
unsigned long pendingFreqBTimeMs = 0;
#define FREQ_CONFIRM_MS 1500UL

// ── Edit Mode Parameter Selection ────────────────────────────────────────────
enum EditParam { EDIT_STEP = 0, EDIT_FROM = 1, EDIT_UP = 2, EDIT_DIR = 3 };
EditParam editParam = EDIT_STEP;
volatile int8_t editDirection = 0; // set by ISR in edit mode: 1=up, -1=down

// ── FROM/UP field (col 10–15, row 0) ─────────────────────────────────────────
// Shows the range as ' 5- 8', '12-15', etc., dash always at col 13
void writeFromUpField(long fromKHz, long upKHz) {
  // Always clear the field first (col 10-15)
  lcd.setCursor(10, 0);
  lcd.print("      ");

  // Enforce 1-99 only
  if (fromKHz < 1) fromKHz = 1;
  if (fromKHz > 99) fromKHz = 99;
  if (upKHz < 1) upKHz = 1;
  if (upKHz > 99) upKHz = 99;

  // FROM: col 11/12
  if (fromKHz < 10) {
    lcd.setCursor(12, 0);
    lcd.print(fromKHz);
  } else {
    lcd.setCursor(11, 0);
    char buf[3];
    snprintf(buf, sizeof(buf), "%02ld", fromKHz);
    lcd.print(buf);
  }

  // Dash at col 13
  lcd.setCursor(13, 0);
  lcd.print('-');

  // UP: col 14/15
  if (upKHz < 10) {
    lcd.setCursor(14, 0);
    lcd.print(upKHz);
  } else {
    lcd.setCursor(14, 0);
    char buf[3];
    snprintf(buf, sizeof(buf), "%02ld", upKHz);
    lcd.print(buf);
  }
}

// Like writeFromUpField() but one number can blink independently.
// blinkFrom/blinkUp: true = this number is the blinking one.
// numbersVisible: false = hide the blinking number.
void writeFromUpFieldBlink(long fromKHz, long upKHz, bool blinkFrom, bool blinkUp, bool numbersVisible) {
  lcd.setCursor(10, 0);
  lcd.print("      ");

  if (fromKHz < 1) fromKHz = 1;
  if (fromKHz > 99) fromKHz = 99;
  if (upKHz < 1) upKHz = 1;
  if (upKHz > 99) upKHz = 99;

  // FROM: col 11/12
  if (!blinkFrom || numbersVisible) {
    if (fromKHz < 10) {
      lcd.setCursor(12, 0);
      lcd.print(fromKHz);
    } else {
      lcd.setCursor(11, 0);
      char buf[3];
      snprintf(buf, sizeof(buf), "%02ld", fromKHz);
      lcd.print(buf);
    }
  }

  // Dash always visible at col 13
  lcd.setCursor(13, 0);
  lcd.print('-');

  // UP: col 14/15
  if (!blinkUp || numbersVisible) {
    if (upKHz < 10) {
      lcd.setCursor(14, 0);
      lcd.print(upKHz);
    } else {
      lcd.setCursor(14, 0);
      char buf[3];
      snprintf(buf, sizeof(buf), "%02ld", upKHz);
      lcd.print(buf);
    }
  }
}

// Only writeStepField() may write to col 11–15 on row 1.
void writeStepField(long hz, bool editing = false, bool blinkNumbers = false, bool numbersVisible = true) {
  // Only the numbers blink, 'K' is always visible and right-justified
  // Always clear the field first (6 chars — includes col 10 spacer)
  lcd.setCursor(10, 1);
  lcd.print("      ");
  if (hz >= 1000) {
    // 1K, right-justified: "   1K"
    if (editing && blinkNumbers && !numbersVisible) {
      lcd.setCursor(11, 1);
      lcd.print("    "); // 4 spaces
      lcd.setCursor(15, 1);
      lcd.print("K");
    } else {
      lcd.setCursor(11, 1);
      lcd.print("   1");
      lcd.setCursor(15, 1);
      lcd.print("K");
    }
  } else {
    // .5K, right-justified: "  .5K"
    if (editing && blinkNumbers && !numbersVisible) {
      lcd.setCursor(11, 1);
      lcd.print("    "); // 4 spaces
      lcd.setCursor(15, 1);
      lcd.print("K");
    } else {
      lcd.setCursor(11, 1);
      lcd.print("  .5");
      lcd.setCursor(15, 1);
      lcd.print("K");
    }
  }
}

// ── DIR field (col 10–15, row 1) — shown only in edit mode when EDIT_DIR active ──
void writeDirField(bool reversed, bool visible = true) {
  lcd.setCursor(10, 1);
  if (!visible) {
    lcd.print("      ");
  } else {
    lcd.print(reversed ? " DIR:-" : " DIR:+");
  }
}

// ── Split offset field (col 10–15, row 1) — shown only in split mode ─────────
// Shows offset between controlled and frozen VFO, e.g. " +2.0K", "+12.5K".
void writeSplitOffsetField(long offsetHz) {
  char sign = (offsetHz >= 0) ? '+' : '-';
  long absHz  = (offsetHz >= 0) ? offsetHz : -offsetHz;
  long intKhz = absHz / 1000L;
  int  frac   = (int)((absHz % 1000L) / 100L); // 0 or 5
  char inner[7];
  snprintf(inner, sizeof(inner), "%c%ld.%dK", sign, intKhz, frac);
  char buf[7];
  snprintf(buf, sizeof(buf), "%6s", inner); // right-justify in 6 chars
  lcd.setCursor(10, 1);
  lcd.print(buf);
}

// ── FREQ field (col 1–9, 9 chars) ────────────────────────────────────────────

// Format hz into exactly 9 chars, dot-grouped, right-aligned.
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

// Write the frequency field for one row. Only touches col 0–9.
void writeFreqField(int row, long hz) {
  char buf[10];
  formatFreqField(hz, buf);
  // In edit mode, do not show knob symbol
  if (uiState == STATE_EDIT) {
    lcd.setCursor(0, row);
    lcd.print(' ');
    lcd.setCursor(1, row);
    lcd.print(buf);
    return;
  }
  // Print knob symbol (custom char 0) at col 0 if this row is being modified by the knob
  bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
  char targetVfo = knobControlsTx ? txVfo : (txVfo == 'A' ? 'B' : 'A');
  bool isTargetRow = (row == 1 && targetVfo == 'B') || (row == 0 && targetVfo == 'A');
  lcd.setCursor(0, row);
  if (isTargetRow) {
    lcd.write(byte(0)); // knob symbol
  } else {
    lcd.print(' ');
  }
  lcd.setCursor(1, row);
  lcd.print(buf);
}

// ── LCD update ────────────────────────────────────────────────────────────────

void updateLcd() {
  // Always redraw freq fields — knob symbol must move instantly when toggle changes
  lastLcdFreqA = lcdFreqA;
  lastLcdFreqB = lcdFreqB;
  writeFreqField(0, lcdFreqA);
  // Only redraw from/up field when values change (avoids flicker)
  if (rangeFromKHz != lastRangeFromKHz || rangeUpKHz != lastRangeUpKHz) {
    lastRangeFromKHz = rangeFromKHz;
    lastRangeUpKHz   = rangeUpKHz;
    writeFromUpField(rangeFromKHz, rangeUpKHz);
  }
  writeFreqField(1, lcdFreqB);
  if (splitActive) {
    bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
    char targetVfo = knobControlsTx ? txVfo : (txVfo == 'A' ? 'B' : 'A');
    long controlledHz = (targetVfo == 'A') ? lcdFreqA : lcdFreqB;
    long frozenHz     = (targetVfo == 'A') ? lcdFreqB : lcdFreqA;
    writeSplitOffsetField(controlledHz - frozenHz);
    lastSplitActive = true;
  } else {
    // Only redraw step field when values change, or when just leaving split mode
    bool editing = (uiState == STATE_EDIT);
    if (stepHz != lastStepHz || editing != lastEditing || lastSplitActive) {
      lastStepHz      = stepHz;
      lastEditing     = editing;
      lastSplitActive = false;
      writeStepField(stepHz, editing);
    }
  }
}

// ── Encoder ISR ───────────────────────────────────────────────────────────────
// Fires on CHANGE of CLK or DT. Lookup table filters invalid transitions.
// Accumulates ±1 per valid transition; fires a step on ±4 (= 1 detent).
void IRAM_ATTR encoderISR() {
  uint8_t newState = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  int8_t delta = ENC_TABLE[(encState << 2) | newState];
  encState = newState;
  if (delta == 0) return;  // invalid/bounce transition — ignore
  encAccum += delta;
  int8_t dir = 0;
  if (encAccum <= -4) { dir =  1; encAccum = 0; }  // CW
  if (encAccum >=  4) { dir = -1; encAccum = 0; }  // CCW
  if (dir == 0) return;   // not yet a full detent
  if (encReverse) dir = -dir;

  if (uiState == STATE_EDIT) {
    editDirection = dir;
    stepChanged = true;
    return;
  }

  bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
  char targetVfo = knobControlsTx ? txVfo : (txVfo == 'A' ? 'B' : 'A');
  long baseFreq = (targetVfo == 'A') ? lcdFreqA : lcdFreqB;
  bool *snapPending = (targetVfo == 'A') ? &snapPendingA : &snapPendingB;

  if (dir > 0) {
    if (*snapPending) {
      long snapped = ((baseFreq + stepHz - 1) / stepHz) * stepHz;
      long delta = snapped - baseFreq;
      if (delta == 0) pendingHz += stepHz;
      else            pendingHz += delta;
      *snapPending = false;
    } else {
      pendingHz += stepHz;
    }
  } else {
    if (*snapPending) {
      long snapped = (baseFreq / stepHz) * stepHz;
      long delta = snapped - baseFreq;
      if (delta == 0) pendingHz -= stepHz;
      else            pendingHz += delta;
      *snapPending = false;
    } else {
      pendingHz -= stepHz;
    }
  }
}

void pollEncoder() {} // encoder is ISR-driven; kept to avoid removing call sites

// ── Banner ────────────────────────────────────────────────────────────────────

void sendBanner() {
  Serial.println(ARDUINO_BANNER);
  lastHelloMs = millis();
}

// Returns band index if freq is within a known legal band, else -1.
int bandOf(long freq) {
  for (int i = 0; i < bandCount; i++) {
    if (freq >= bandLow[i] && freq <= bandHigh[i]) return i;
  }
  return -1;
}

// Compute split range from baseFreq, clamped to the legal band.
// splitRangeLow  = baseFreq + FROM, clamped to [bandLow, bandHigh]
// splitRangeHigh = baseFreq + UP,   clamped to [splitRangeLow, bandHigh]
void calcSplitRange(long baseFreq, long &outLow, long &outHigh) {
  long rawLow  = baseFreq + rangeFromKHz * 1000L;
  long rawHigh = baseFreq + rangeUpKHz   * 1000L;
  int  band    = bandOf(baseFreq);
  if (band >= 0) {
    if (rawLow  > bandHigh[band]) rawLow  = bandHigh[band];
    if (rawLow  < bandLow[band])  rawLow  = bandLow[band];
    if (rawHigh > bandHigh[band]) rawHigh = bandHigh[band];
    if (rawHigh < rawLow)         rawHigh = rawLow;
  }
  outLow  = ((rawLow + stepHz / 2) / stepHz) * stepHz; // snap to nearest step
  outHigh = rawHigh;
}

// ── Command handler ───────────────────────────────────────────────────────────

void handleCommand(const char *line) {
  lastPythonMsgMs = millis(); // any valid command = Python is alive

  if (strcmp(line, "WHO") == 0) {
    sendBanner();
    return;
  }

  if (strcmp(line, "PING") == 0) {
    Serial.println("PONG_ARDUINO");
    return;
  }

  // BAND_ADD:<low_hz>:<high_hz>*CS — collect into incoming buffer
  if (strncmp(line, "BAND_ADD:", 9) == 0) {
    char *star = strchr(line, '*');
    if (star != NULL) {
      uint8_t expected = (uint8_t)strtol(star + 1, NULL, 16);
      uint8_t computed = 0;
      for (char *c = (char *)line; c < star; c++) computed ^= (uint8_t)*c;
      if (computed != expected) return;  // corrupted — discard
      *star = '\0';
    }
    if (!incomingActive) {
      incomingCount = 0;  // reset buffer on first BAND_ADD of this session
      incomingActive = true;
    }
    if (incomingCount < MAX_BANDS) {
      char *p = (char *)line + 9;
      incomingBandLow[incomingCount]  = atol(p);
      p = strchr(p, ':');
      if (p) incomingBandHigh[incomingCount] = atol(++p);
      incomingCount++;
    }
    return;
  }

  // BAND_DONE:<count> — verify count, compare incoming buffer with EEPROM; update if different
  if (strncmp(line, "BAND_DONE:", 10) == 0) {
    incomingActive = false;
    int expectedCount = atoi(line + 10);
    if (incomingCount != expectedCount) {
      Serial.print("DBG:BAND_DONE_MISMATCH:got="); Serial.print(incomingCount);
      Serial.print(":expected="); Serial.println(expectedCount);
      incomingCount = 0;
      return;
    }
    bool different = (incomingCount != bandCount);
    if (!different) {
      for (int i = 0; i < incomingCount; i++) {
        if (incomingBandLow[i] != bandLow[i] || incomingBandHigh[i] != bandHigh[i]) {
          different = true;
          break;
        }
      }
    }
    if (different) {
      // Update active table
      bandCount = incomingCount;
      for (int i = 0; i < bandCount; i++) {
        bandLow[i]  = incomingBandLow[i];
        bandHigh[i] = incomingBandHigh[i];
      }
      // Write to EEPROM
      EEPROM.write(EEPROM_BAND_COUNT_ADDR, (uint8_t)bandCount);
      for (int i = 0; i < bandCount; i++) {
        int addr = EEPROM_BAND_DATA_ADDR + i * 8;
        EEPROM.put(addr,     bandLow[i]);
        EEPROM.put(addr + 4, bandHigh[i]);
      }
      EEPROM.write(EEPROM_BAND_MAGIC_ADDR,     EEPROM_BAND_MAGIC1);
      EEPROM.write(EEPROM_BAND_MAGIC_ADDR + 1, EEPROM_BAND_MAGIC2);
      EEPROM.commit();
      //Serial.println("DBG:EEPROM_UPDATED");
    } else {
      //Serial.println("DBG:EEPROM_SAME");
    }
    //Serial.print("DBG:EEPROM_BANDS_AFTER:"); Serial.println(bandCount);
    //for (int i = 0; i < bandCount; i++) {
    //  Serial.print("DBG:EEPROM_BAND_AFTER:"); Serial.print(i);
    //  Serial.print(":"); Serial.print(bandLow[i]);
    //  Serial.print(":"); Serial.println(bandHigh[i]);
    //}
    if (!systemReady && bandCount > 0) systemReady = true;
    return;
  }

  // LCD_FREQ:<freqA>:<freqB>:<activeVfo>:<txVfo>:<splitFlag>
  // Python sends this every refresh cycle with confirmed radio values.
  // Freq values are gated (ignored while knob is active).
  // txVfo and splitFlag are always updated so split mode changes are instant.
  // splitFlag: 'S' = split ON, 'N' = split OFF (explicit — do not infer from VFOs).
  if (strncmp(line, "LCD_FREQ:", 9) == 0) {
    // Verify checksum if present (*XX at end of line)
    char *star = strchr(line, '*');
    if (star != NULL) {
      uint8_t expected = (uint8_t)strtol(star + 1, NULL, 16);
      uint8_t computed = 0;
      for (const char *c = line; c < star; c++) computed ^= (uint8_t)*c;
      if (computed != expected) return;  // corrupted — discard
      *star = '\0';  // strip checksum before parsing
    }
    // Save state before update so we can detect split transitions
    long oldFreqA     = lcdFreqA;
    long oldFreqB     = lcdFreqB;
    char oldActiveVfo = lcdActiveVfo;
    char oldTxVfo     = txVfo;

    char *p = (char *)line + 9;
    bool gateOpen = (millis() >= freqTxIgnoreUntilMs);
    long newA = atol(p);
    // Validate newA: skip update entirely if band table not loaded yet (BAND_CLEAR window).
    if (bandCount == 0) newA = 0;
    // Validate newA: require confirmation if crossing into a different legal band.
    // Out-of-band values are accepted for display — knob snap handles them in pollFreqSend.
    if (bandCount > 0 && newA > 0) {
      int newBand = bandOf(newA);
      if (newBand < 0) {
        // Out of band — require two consecutive agreeing messages
        if (pendingFreqA > 0 && bandOf(pendingFreqA) < 0 && abs(pendingFreqA - newA) < 5000L) {
          pendingFreqA = 0;  // confirmed — accept
        } else {
          pendingFreqA = newA;
          pendingFreqATimeMs = millis();
          newA = 0;  // hold until confirmed
        }
      } else {
        int curBand = bandOf(lcdFreqA);
        if (lcdFreqA > 0 && curBand >= 0 && curBand != newBand) {
          // Different band — require one confirming message
          if (pendingFreqA > 0 && bandOf(pendingFreqA) == newBand) {
            pendingFreqA = 0;  // confirmed — accept
          } else {
            pendingFreqA = newA;
            pendingFreqATimeMs = millis();
            newA = 0;  // not yet confirmed — skip update
          }
        } else {
          pendingFreqA = 0;  // same band or uninitialized — accept immediately
        }
      }
    }
    // Band change: bypass gate — a >1 MHz jump is never a tuning step, always a band switch.
    // Also clear any accumulated encoder delta so the knob starts fresh on the new band.
    bool isBandChange = (lcdFreqA > 0 && newA > 0 && abs(newA - lcdFreqA) > 1000000L);
    if (isBandChange) {
      freqTxIgnoreUntilMs = 0;  // open gate immediately
      ENC_PAUSE();
      pendingHz = 0;            // discard stale delta from old band
      ENC_RESUME();
      gateOpen = true;
    }
    if ((gateOpen) && newA > 0) {
      if (lcdFreqA != newA) {
        // Mark as external/manual change — snap on next encoder click
        externalFreqA = true;
        snapPendingA = true;
      }
      ENC_PAUSE(); lcdFreqA = newA; ENC_RESUME();
      // Both band table and a valid freq are now in — enable encoder output.
      // Clear any accumulated pendingHz so startup turns don't fire at once.
      if (!systemReady && bandCount > 0) {
        systemReady = true;
        ENC_PAUSE(); pendingHz = 0; ENC_RESUME();
      }
    }
    p = strchr(p, ':');
    if (p) {
      long newB = atol(++p);
      // Validate newB: same rules as newA.
      if (bandCount == 0) newB = 0;
      if (bandCount > 0 && newB > 0) {
        int newBand = bandOf(newB);
        if (newBand < 0) {
          // Out of band — require two consecutive agreeing messages
          if (pendingFreqB > 0 && bandOf(pendingFreqB) < 0 && abs(pendingFreqB - newB) < 5000L) {
            pendingFreqB = 0;  // confirmed — accept
          } else {
            pendingFreqB = newB;
            pendingFreqBTimeMs = millis();
            newB = 0;  // hold until confirmed
          }
        } else {
          int curBand = bandOf(lcdFreqB);
          if (lcdFreqB > 0 && curBand >= 0 && curBand != newBand) {
            if (pendingFreqB > 0 && bandOf(pendingFreqB) == newBand) {
              pendingFreqB = 0;  // confirmed — accept
            } else {
              pendingFreqB = newB;
              pendingFreqBTimeMs = millis();
              newB = 0;  // not yet confirmed — skip update
            }
          } else {
            pendingFreqB = 0;
          }
        }
      }
      if (gateOpen && newB > 0) {
        if (lcdFreqB != newB) {
          // Mark as external/manual change — snap on next encoder click
          externalFreqB = true;
          snapPendingB = true;
        }
        ENC_PAUSE(); lcdFreqB = newB; ENC_RESUME();
      }
      p = strchr(p, ':');
      if (p) {
        if (gateOpen) lcdActiveVfo = *(++p);
        else ++p;
        p = strchr(p, ':');
        if (p) {
          txVfo = *(++p);  // always update — no gate
          p = strchr(p, ':');
          if (p) pythonSplit = (*(++p) == 'S');  // always update — explicit split flag
        }
      }
    }

    // ── Split transition detection ──────────────────────────────────────────
    // splitOffCount: debounce split-OFF — require consecutive non-split frames
    // before acting, to ignore brief radio glitches.
    static int splitOffCount = 0;

    bool isSplit = pythonSplit;  // use explicit flag — VFO inference fails on some radios

    // After a button-initiated split change, suppress detection until the
    // radio has caught up — prevents the stale LCD_FREQ from re-triggering.
    if (millis() < splitGateMs) {
      splitOffCount = 0;
    } else if (!splitActive && isSplit) {
      splitOffCount = 0; // entering split — reset debounce
      // Split just turned ON — set the target VFO to baseFreq + FROM
      long baseFreq = (oldActiveVfo == 'A') ? oldFreqA : oldFreqB;
      if (baseFreq > 0) {
        calcSplitRange(baseFreq, splitRangeLow, splitRangeHigh);
        splitActive    = true;
        // Hunter: KNOB_TARGET_SW HIGH → tune TX; Fox: LOW → tune RX
        bool isHunter  = (digitalRead(KNOB_TARGET_SW) == HIGH);
        char targetVfo = isHunter ? txVfo : lcdActiveVfo;
        splitTargetVfo = targetVfo;
        Serial.print("SNAP_FREQ:");
        Serial.print(splitRangeLow);
        Serial.print(":");
        Serial.println(targetVfo);
        freqTxIgnoreUntilMs = millis() + FREQ_TX_IGNORE_MS;
        // Update local LCD freq so display is consistent
        if (targetVfo == 'A') { lcdFreqA = splitRangeLow; lastLcdFreqA = splitRangeLow; }
        else                  { lcdFreqB = splitRangeLow; lastLcdFreqB = splitRangeLow; }
      }
    } else if (splitActive && !isSplit) {
      // Debounce: only fire split OFF after 2 consecutive non-split frames
      splitOffCount++;
      if (splitOffCount >= 6) {
        splitOffCount = 0;
        // Split turned OFF — snap tuned VFO to nearest step boundary
        long curFreq = (splitTargetVfo == 'A') ? lcdFreqA : lcdFreqB;
        if (curFreq > 0 && stepHz > 0) {
          long snapped = ((curFreq + stepHz / 2) / stepHz) * stepHz;
          Serial.print("SNAP_FREQ:");
          Serial.print(snapped);
          Serial.print(":");
          Serial.println(splitTargetVfo);
          freqTxIgnoreUntilMs = millis() + FREQ_TX_IGNORE_MS;
          if (splitTargetVfo == 'A') { lcdFreqA = snapped; lastLcdFreqA = snapped; }
          else                       { lcdFreqB = snapped; lastLcdFreqB = snapped; }
        }
        splitActive    = false;
        splitRangeLow  = 0;
        splitRangeHigh = 0;
        lcd.setCursor(10, 1); lcd.print(' '); // clear leftover offset char at col 10
      }
    } else {
      splitOffCount = 0; // stable state — reset debounce
    }

    if (uiState != STATE_EDIT) {
      updateLcd();
    }
    return;
  }
}

// ── FTDI serial reader ────────────────────────────────────────────────────────

void pollFtdi() {
  while (Serial.available() > 0) {
    char ch = static_cast<char>(Serial.read());
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
  // Atomic snapshot of ISR-written pendingHz.
  ENC_PAUSE();
  long pk = pendingHz;
  ENC_RESUME();

  if (pk == 0 || uiState == STATE_EDIT || !systemReady) {
    if (pk != 0 && !systemReady) { ENC_PAUSE(); pendingHz = 0; ENC_RESUME(); }
    return;
  }

  bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
  char targetVfo      = knobControlsTx ? txVfo : (txVfo == 'A' ? 'B' : 'A');
  long baseFreq       = (targetVfo == 'A') ? lcdFreqA : lcdFreqB;

  if (baseFreq > 0) {
    if (bandCount == 0) { ENC_PAUSE(); pendingHz = 0; ENC_RESUME(); return; }
    int baseBand = bandOf(baseFreq);
    if (baseBand < 0) {
      // ── VIP snap ─────────────────────────────────────────────────────────────
      // baseFreq is outside all legal bands. First knob click snaps to the
      // nearest band edge in the direction of turn.
      long snapFreq = 0;
      if (pk > 0) {
        // Going up: lower edge of the nearest band above baseFreq
        long bestDist = -1;
        for (int i = 0; i < bandCount; i++) {
          if (bandLow[i] > baseFreq) {
            long dist = bandLow[i] - baseFreq;
            if (bestDist < 0 || dist < bestDist) { bestDist = dist; snapFreq = bandLow[i]; }
          }
        }
      } else {
        // Going down: upper edge of the nearest band below baseFreq
        long bestDist = -1;
        for (int i = 0; i < bandCount; i++) {
          if (bandHigh[i] < baseFreq) {
            long dist = baseFreq - bandHigh[i];
            if (bestDist < 0 || dist < bestDist) { bestDist = dist; snapFreq = bandHigh[i]; }
          }
        }
      }
      if (snapFreq > 0) {
        Serial.print("SET_FREQ:"); Serial.print(snapFreq);
        Serial.print(":"); Serial.println(targetVfo);
        freqTxIgnoreUntilMs = millis() + FREQ_TX_IGNORE_MS;
        if (targetVfo == 'B') {
          ENC_PAUSE(); lcdFreqB = snapFreq; lastLcdFreqB = snapFreq; ENC_RESUME();
          writeFreqField(1, snapFreq);
        } else {
          ENC_PAUSE(); lcdFreqA = snapFreq; lastLcdFreqA = snapFreq; ENC_RESUME();
          writeFreqField(0, snapFreq);
        }
      }
      ENC_PAUSE(); pendingHz = 0; ENC_RESUME();
      return;
    }
    long newFreq = baseFreq + pk;
    // Discard if step is physically impossible (corruption guard: max 200 clicks per poll)
    if (abs(newFreq - baseFreq) > 15 * stepHz) {
      ENC_PAUSE(); pendingHz = 0; ENC_RESUME();
      return;
    }
    // Clamp to split range when active
    if (splitActive && splitRangeLow > 0 && splitRangeHigh > 0) {
      if (newFreq < splitRangeLow)  newFreq = splitRangeLow;
      if (newFreq > splitRangeHigh) newFreq = splitRangeHigh;
    }
    // Clamp newFreq to the band that baseFreq is in.
    // When clamped, clear any same-direction pendingHz and set pk=0 so the
    // end-of-function pendingHz -= pk leaves opposite-direction ISR clicks intact.
    if (baseBand >= 0) {
      if (newFreq < bandLow[baseBand]) {
        newFreq = bandLow[baseBand];
        ENC_PAUSE(); if (pendingHz < 0) pendingHz = 0; ENC_RESUME();
        pk = 0; // mark consumed — don't subtract again at end
      }
      if (newFreq > bandHigh[baseBand]) {
        newFreq = bandHigh[baseBand];
        ENC_PAUSE(); if (pendingHz > 0) pendingHz = 0; ENC_RESUME();
        pk = 0; // mark consumed — don't subtract again at end
      }
    }
    //Serial.print(baseFreq); Serial.print('>'); Serial.println(newFreq);
    Serial.print("SET_FREQ:");
    Serial.print(newFreq);
    Serial.print(":");
    Serial.println(targetVfo);
    freqTxIgnoreUntilMs = millis() + FREQ_TX_IGNORE_MS;
    // Update only the target VFO row on the LCD immediately.
    if (targetVfo == 'B') {
      ENC_PAUSE(); lcdFreqB = newFreq; lastLcdFreqB = newFreq; ENC_RESUME();
      writeFreqField(1, newFreq);
    } else {
      ENC_PAUSE(); lcdFreqA = newFreq; lastLcdFreqA = newFreq; ENC_RESUME();
      writeFreqField(0, newFreq);
    }
    if (splitActive) {
      long frozenHz = (targetVfo == 'A') ? lcdFreqB : lcdFreqA;
      writeSplitOffsetField(newFreq - frozenHz);
    }
  } else {
    Serial.println("NO_BASE_FREQ");
  }

  // Subtract only what was processed.  Any ISR clicks that arrived during the
  // serial print (interrupts enabled) are preserved for the next poll cycle.
  ENC_PAUSE();
  pendingHz -= pk;
  ENC_RESUME();
}

// ── Button ────────────────────────────────────────────────────────────────────

void pollButton() {
  int sw = digitalRead(ENC_SW);
  unsigned long now = millis();
  if (sw == LOW && lastSw == HIGH) {
    swPressStart = now;
    swWasLongPressed = false;
  }
  if (sw == LOW && !swWasLongPressed && swPressStart && (now - swPressStart > 1000)) {
    // Long press detected
    swWasLongPressed = true;
    if (uiState == STATE_EDIT) {
      // Exiting edit mode: save and restore normal display
      long stepHzCopy2 = stepHz;
      EEPROM.put(EEPROM_STEP_ADDR, stepHzCopy2);
      EEPROM.put(EEPROM_FROM_ADDR, rangeFromKHz);
      EEPROM.put(EEPROM_UP_ADDR, rangeUpKHz);
      EEPROM.write(EEPROM_ENC_REVERSE_ADDR, encReverse ? 0x01 : 0x00);
      EEPROM.commit();
      uiState = STATE_ONAIR;
      lastBlinkMs = now;
      stepFieldVisible = true;
      lastRangeFromKHz = -1; // force FROM/UP field redraw
      lastRangeUpKHz   = -1;
      lastStepHz       = -1; // force step field redraw
      lastEditing      = false;
      writeFromUpField(rangeFromKHz, rangeUpKHz);
      writeStepField(stepHz, false);
      updateLcd();
      swWasLongPressed = true; // prevent release being treated as short press
      lastSw = LOW;            // sync pollButton()'s lastSw so release is handled correctly
    } else if (uiState == STATE_ONAIR && !splitActive) {
      // Entering edit mode
      uiState = STATE_EDIT;
      editFirstFrame = true;
    }
  }
  if (sw == HIGH && lastSw == LOW) {
    if (!swWasLongPressed) {
      if (splitActive) {
        // ── Button-initiated split OFF ──────────────────────────────────────
        splitActive    = false;
        splitRangeLow  = 0;
        splitRangeHigh = 0;
        lcd.setCursor(10, 1); lcd.print(' ');
        splitGateMs = millis() + SPLIT_GATE_MS;
        Serial.println("SET_SPLIT:OFF");
        updateLcd();
      } else {
        // ── Button-initiated split ON ───────────────────────────────────────
        char newTxVfo  = (lcdActiveVfo == 'A') ? 'B' : 'A';
        long baseFreq  = (lcdActiveVfo == 'A') ? lcdFreqA : lcdFreqB;
        if (baseFreq > 0) {
          calcSplitRange(baseFreq, splitRangeLow, splitRangeHigh);
          splitActive    = true;
          bool isHunter  = (digitalRead(KNOB_TARGET_SW) == HIGH);
          char targetVfo = isHunter ? newTxVfo : lcdActiveVfo;
          splitTargetVfo = targetVfo;
          splitGateMs    = millis() + SPLIT_GATE_MS;
          Serial.println("SET_SPLIT:ON");
          Serial.print("SNAP_FREQ:");
          Serial.print(splitRangeLow);
          Serial.print(":");
          Serial.println(targetVfo);
          freqTxIgnoreUntilMs = millis() + FREQ_TX_IGNORE_MS;
          if (targetVfo == 'A') { lcdFreqA = splitRangeLow; lastLcdFreqA = splitRangeLow; }
          else                  { lcdFreqB = splitRangeLow; lastLcdFreqB = splitRangeLow; }
          updateLcd();
        }
      }
    }
    swPressStart = 0;
    swWasLongPressed = false;
  }
  lastSw = sw;
}

// Custom knob icon (circle with center dot)
byte knobChar[8] = {
  0b00000,
  0b01110,
  0b10001,
  0b10101,
  0b10101,
  0b10001,
  0b01110,
  0b00000
};

// ── Setup & loop ──────────────────────────────────────────────────────────────

void setup() {
    pinMode(LED_RX_PIN, OUTPUT);
    pinMode(LED_TX_PIN, OUTPUT);
    // Set initial state
    bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
    digitalWrite(LED_RX_PIN, knobControlsTx ? LOW : HIGH);
    digitalWrite(LED_TX_PIN, knobControlsTx ? HIGH : LOW);
  Serial.begin(PC_BAUD);
  EEPROM.begin(256);

  // Load stepHz from EEPROM (default to 1000 if invalid)
  long eepromStep = 0;
  EEPROM.get(EEPROM_STEP_ADDR, eepromStep);
  if (eepromStep == 500 || eepromStep == 1000) {
    stepHz = eepromStep;
  } else {
    stepHz = 1000;
  }

  // Load encReverse from EEPROM
  encReverse = (EEPROM.read(EEPROM_ENC_REVERSE_ADDR) == 0x01);

  // Load band table from EEPROM if valid
  if (EEPROM.read(EEPROM_BAND_MAGIC_ADDR) == EEPROM_BAND_MAGIC1 &&
      EEPROM.read(EEPROM_BAND_MAGIC_ADDR + 1) == EEPROM_BAND_MAGIC2) {
    uint8_t storedCount = EEPROM.read(EEPROM_BAND_COUNT_ADDR);
    if (storedCount > 0 && storedCount <= MAX_BANDS) {
      bandCount = storedCount;
      int addr = EEPROM_BAND_DATA_ADDR;
      for (int i = 0; i < bandCount; i++) {
        EEPROM.get(addr, bandLow[i]);  addr += 4;
        EEPROM.get(addr, bandHigh[i]); addr += 4;
      }
      systemReady = true;
      //Serial.print("DBG:EEPROM_BANDS:"); Serial.println(bandCount);
      //for (int i = 0; i < bandCount; i++) {
      //  Serial.print("DBG:EEPROM_BAND:"); Serial.print(i);
      //  Serial.print(":"); Serial.print(bandLow[i]);
      //  Serial.print(":"); Serial.println(bandHigh[i]);
      //}
    }
  } else {
    //Serial.println("DBG:EEPROM_BANDS:virgin");
  }

  // Load rangeFromKHz and rangeUpKHz from EEPROM (default to 5 and 10 if invalid)
  long eepromFrom = 0, eepromUp = 0;
  EEPROM.get(EEPROM_FROM_ADDR, eepromFrom);
  EEPROM.get(EEPROM_UP_ADDR, eepromUp);
  if (eepromFrom > 0 && eepromFrom < 100) {
    rangeFromKHz = eepromFrom;
  } else {
    rangeFromKHz = 5;
  }
  if (eepromUp > 0 && eepromUp < 100) {
    rangeUpKHz = eepromUp;
  } else {
    rangeUpKHz = 10;
  }

  Wire.begin(21, 22);  // ESP32: SDA=GPIO21, SCL=GPIO22
  lcd.init();
  lcd.createChar(0, knobChar); // Ensure custom char is loaded after init
  lcd.backlight();

  // Initialise both frequency fields to "No signal" and show initial step.
  writeFreqField(0, 0);
  writeFromUpField(rangeFromKHz, rangeUpKHz);
  writeFreqField(1, 0);
  writeStepField(stepHz, false);

  pinMode(ENC_CLK,        INPUT_PULLUP);
  pinMode(ENC_DT,         INPUT_PULLUP);
  pinMode(ENC_SW,         INPUT_PULLUP);
  pinMode(KNOB_TARGET_SW, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) {
    pinMode(EXTRA_BTN_PINS[i], INPUT_PULLUP);
    extraBtnLast[i] = digitalRead(EXTRA_BTN_PINS[i]);
  }

  encState = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  lastSw   = digitalRead(ENC_SW);

  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT),  encoderISR, CHANGE);

  delay(1200);
  sendBanner();
}

// ── State Handlers ────────────────────────────────────────────────────────────

void handleOnAir() {
  pollEncoder();
  pollFreqSend();
  pollButton();
  pollExtraButtons();
  pollFtdi();
  // Always show step field in normal mode
  if (!stepFieldVisible) {
    writeStepField(stepHz, false);
    stepFieldVisible = true;
  }
}

void handleEdit() {
  pollEncoder();
  pollFtdi();

  int sw = digitalRead(ENC_SW);
  unsigned long now = millis();
  static int lastSw = HIGH;
  static unsigned long swPressStart = 0;
  static bool swWasLongPressed = false;
  bool &firstEditFrame = editFirstFrame;
  static bool waitForRelease = false;
  static bool blinkOn = true;
  const unsigned long BLINK_ON_MS  = 500;
  const unsigned long BLINK_OFF_MS = 600;

  // ── First frame: reset state, wait for button release ──
  if (firstEditFrame) {
    editParam = EDIT_STEP;
    blinkOn = true;
    lastBlinkMs = now;
    waitForRelease = true; // button still held from long press — ignore until released
    updateLcd();           // hide wheel icon immediately
    firstEditFrame = false;
  }

  // ── Wait for button to be released before accepting any input ──
  if (waitForRelease) {
    if (sw == HIGH) {
      waitForRelease = false;
      lastSw = HIGH;
      swPressStart = 0;
      swWasLongPressed = false;
    } else {
      lastSw = sw;
      // fall through to blink logic below
    }
  }

  if (!waitForRelease) {
    // ── Button logic ──
    if (sw == LOW && lastSw == HIGH) {
      swPressStart = now;
      swWasLongPressed = false;
    }
    if (sw == LOW && !swWasLongPressed && swPressStart && (now - swPressStart > 1000)) {
      // Long press: save all and exit edit mode
      swWasLongPressed = true;
      EEPROM.put(EEPROM_STEP_ADDR, stepHz);
      EEPROM.put(EEPROM_FROM_ADDR, rangeFromKHz);
      EEPROM.put(EEPROM_UP_ADDR, rangeUpKHz);
      EEPROM.write(EEPROM_ENC_REVERSE_ADDR, encReverse ? 0x01 : 0x00);
      EEPROM.commit();
      uiState = STATE_ONAIR;
      lastBlinkMs = now;
      stepFieldVisible = true;
      lastRangeFromKHz = -1;
      lastRangeUpKHz   = -1;
      lastStepHz       = -1;
      lastEditing      = false;
      writeFromUpField(rangeFromKHz, rangeUpKHz);
      writeStepField(stepHz, false);
      updateLcd();
      firstEditFrame = true;
      return;
    }
    if (sw == HIGH && lastSw == LOW) {
      unsigned long pressDuration = now - swPressStart;
      if (!swWasLongPressed && pressDuration >= 50 && pressDuration < 1000) {
        // Restore current field to steady before switching
        switch (editParam) {
          case EDIT_STEP: writeStepField(stepHz, true); break;
          case EDIT_FROM:
          case EDIT_UP:   writeFromUpField(rangeFromKHz, rangeUpKHz); break;
          case EDIT_DIR:  writeDirField(encReverse); break;
        }
        editParam = (EditParam)(((int)editParam + 1) % 4);
        blinkOn = true;
        lastBlinkMs = now;
      }
      swPressStart = 0;
      swWasLongPressed = false;
    }
    lastSw = sw;

    // ── Encoder: adjust active parameter ──
    if (stepChanged) {
      ENC_PAUSE();
      stepChanged = false;
      int8_t dir = editDirection;
      editDirection = 0;
      ENC_RESUME();

      switch (editParam) {
        case EDIT_STEP:
          stepHz = (stepHz == 1000) ? 500 : 1000; // toggle on any turn
          snapPendingA = true; // next tune will snap to new step boundary
          snapPendingB = true;
          writeStepField(stepHz, true, true, true); // show new value immediately
          break;
        case EDIT_FROM:
          rangeFromKHz += dir; // CW=+1, CCW=-1
          if (rangeFromKHz < 1)  rangeFromKHz = 99;
          if (rangeFromKHz > 99) rangeFromKHz = 1;
          writeFromUpFieldBlink(rangeFromKHz, rangeUpKHz, true, false, true); // FROM only
          break;
        case EDIT_UP:
          rangeUpKHz += dir;
          if (rangeUpKHz < 1)  rangeUpKHz = 99;
          if (rangeUpKHz > 99) rangeUpKHz = 1;
          writeFromUpFieldBlink(rangeFromKHz, rangeUpKHz, false, true, true); // UP only
          break;
        case EDIT_DIR:
          encReverse = !encReverse; // any turn toggles direction
          writeDirField(encReverse, true);
          break;
      }
      blinkOn = true; // restart blink cycle so value is visible right after change
      lastBlinkMs = now;
    }
  }

  // ── Blink active parameter: 500ms on, 600ms off ──
  if (blinkOn && (now - lastBlinkMs >= BLINK_ON_MS)) {
    blinkOn = false;
    lastBlinkMs = now;
    switch (editParam) {
      case EDIT_STEP:
        writeStepField(stepHz, true, true, false); // numbers hidden
        writeFromUpField(rangeFromKHz, rangeUpKHz);
        break;
      case EDIT_FROM:
        writeStepField(stepHz, true);
        writeFromUpFieldBlink(rangeFromKHz, rangeUpKHz, true, false, false); // FROM hidden
        break;
      case EDIT_UP:
        writeStepField(stepHz, true);
        writeFromUpFieldBlink(rangeFromKHz, rangeUpKHz, false, true, false); // UP hidden
        break;
      case EDIT_DIR:
        writeStepField(stepHz, true);
        writeDirField(encReverse, false); // hidden
        break;
    }
  } else if (!blinkOn && (now - lastBlinkMs >= BLINK_OFF_MS)) {
    blinkOn = true;
    lastBlinkMs = now;
    switch (editParam) {
      case EDIT_STEP:
        writeStepField(stepHz, true, true, true); // numbers visible
        writeFromUpField(rangeFromKHz, rangeUpKHz);
        break;
      case EDIT_FROM:
        writeStepField(stepHz, true);
        writeFromUpFieldBlink(rangeFromKHz, rangeUpKHz, true, false, true); // FROM visible
        break;
      case EDIT_UP:
        writeStepField(stepHz, true);
        writeFromUpFieldBlink(rangeFromKHz, rangeUpKHz, false, true, true); // UP visible
        break;
      case EDIT_DIR:
        writeStepField(stepHz, true);
        writeDirField(encReverse, true); // visible
        break;
    }
  }
}

bool isAtBandEdge(long freq) {
  if (bandCount == 0 || freq == 0) return false;
  for (int i = 0; i < bandCount; i++) {
    if (freq >= bandLow[i] && freq <= bandHigh[i]) {
      return (freq == bandLow[i] || freq == bandHigh[i]);
    }
  }
  return false;
}

void loop() {
    // Update LEDs: blink active LED at band edge, normal RX/TX indication otherwise
    bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
    char targetVfo = knobControlsTx ? txVfo : (txVfo == 'A' ? 'B' : 'A');
    long checkFreq  = (targetVfo == 'A') ? lcdFreqA : lcdFreqB;
    if (isAtBandEdge(checkFreq)) {
      bool blinkOn = (millis() % 500) < 250;
      digitalWrite(LED_RX_PIN, (!knobControlsTx && blinkOn) ? HIGH : LOW);
      digitalWrite(LED_TX_PIN, ( knobControlsTx && blinkOn) ? HIGH : LOW);
    } else {
      digitalWrite(LED_RX_PIN, knobControlsTx ? LOW : HIGH);
      digitalWrite(LED_TX_PIN, knobControlsTx ? HIGH : LOW);
    }
  switch (uiState) {
    case STATE_ONAIR:
      handleOnAir();
      break;
    case STATE_EDIT:
      handleEdit();
      break;
  }
  // Expire unconfirmed pending band changes after timeout.
  if (pendingFreqA > 0 && millis() - pendingFreqATimeMs >= FREQ_CONFIRM_MS) pendingFreqA = 0;
  if (pendingFreqB > 0 && millis() - pendingFreqBTimeMs >= FREQ_CONFIRM_MS) pendingFreqB = 0;

  bool pythonAlive = (millis() - lastPythonMsgMs < COMM_TIMEOUT_MS);
  if (!pythonAlive && commConnected) {
    // Python just disconnected — show "no signal" and reset state
    commConnected    = false;
    systemReady      = false;
    lcdFreqA         = 0;
    lcdFreqB         = 0;
    // Force updateLcd() to redraw all fields on reconnect
    lastRangeFromKHz = -1;
    lastRangeUpKHz   = -1;
    lastStepHz       = -1;
    lastSplitActive  = false;
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("-- NO SIGNAL -- ");
  }
  if (!pythonAlive && millis() - lastHelloMs >= 1000) {
    sendBanner();
  }
  if (pythonAlive && !commConnected) {
    commConnected = true;  // Python reconnected; normal LCD_FREQ messages will restore display
  }
  // If band table not yet received, keep requesting it every second regardless of Python activity
  if (bandCount == 0 && millis() - lastHelloMs >= 1000) {
    sendBanner();
  }
}
