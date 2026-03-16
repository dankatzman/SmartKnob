#include <Arduino.h>
#include <SoftwareSerial.h>

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
const int KNOB_TARGET_SW = 4;  // Toggle: HIGH = knob tunes TX, LOW = knob tunes RX
const int LED_COMM_PIN   = 10; // ON while Python is actively sending commands

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

// pendingHz is written by the ISR — read atomically in main loop.
volatile long pendingHz = 0;


// ── Frequency tracking ────────────────────────────────────────────────────────

// After sending SET_FREQ, ignore LCD_FREQ updates from Python for this long.
// Keeps the LCD stable while the knob is being turned.
unsigned long freqTxIgnoreUntilMs = 0;
const unsigned long FREQ_TX_IGNORE_MS = 1500;

// ── Frequency Range ──────────────────────────────────────────────────────────
#define EEPROM_FROM_ADDR 4
#define EEPROM_UP_ADDR   8
long rangeFromKHz = 5;   // Default: 5 kHz offset
long rangeUpKHz   = 10;  // Default: 10 kHz offset

// Clamp freq to the allowed range
long clampFreqToRange(long baseFreq, long freq) {
  long lower = baseFreq + rangeFromKHz * 1000L;
  long upper = baseFreq + rangeUpKHz * 1000L;
  if (freq < lower) return lower;
  if (freq > upper) return upper;
  return freq;
}

// ── Tuning step ───────────────────────────────────────────────────────────────

volatile long stepHz = 1000;  // Hz per encoder click: 1000 = 1 kHz, 500 = 0.5 kHz
#define EEPROM_STEP_ADDR 0

// ── Step Edit Mode State ──
unsigned long swPressStart = 0;
bool swWasLongPressed = false;
volatile bool stepChanged = false; // Set by ISR, handled in main loop

// ── Button ────────────────────────────────────────────────────────────────────

int lastSw = HIGH;

// ── LCD field layout ──────────────────────────────────────────────────────────
//
//  Col: 0 1         9 10 11       15
//       ┌─┬─────────┬──┬──────────┐
//  Row0:│ │  FREQ A │  │ FROM-UP  │
//       ├─┼─────────┼──┼──────────┤
//  Row1:│ │  FREQ B │  │  STEP    │
//       └─┴─────────┴──┴──────────┘
//
//  FREQ field: col 1–9  (9 chars), rows 0 and 1.
//  FROM/UP field: col 10–15, row 0 — e.g. " 5-10"
//  STEP field: col 11–15 (5 chars), row 1 only — e.g. " 1KHZ" or ".5KHZ".
//  Each field has exactly one write function — no other code touches its columns.

// Frequencies shown on the LCD — updated by LCD_FREQ and pollFreqSend.
long lcdFreqA = 0;
long lcdFreqB = 0;
char lcdActiveVfo = 'A';
char txVfo = 'A';    // which VFO is TX — updated from LCD_FREQ every cycle

long lastLcdFreqA = -1;
long lastLcdFreqB = -1;

bool snapPendingA = false;
bool snapPendingB = false;

// ── Edit Mode Parameter Selection ────────────────────────────────────────────
enum EditParam { EDIT_STEP = 0, EDIT_FROM = 1, EDIT_UP = 2 };
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
  // Only the numbers blink, 'KHZ' is always visible
  lcd.setCursor(11, 1);
  // Always clear the field first
  lcd.print("     ");
  lcd.setCursor(11, 1);
  if (hz >= 1000) {
    // " 1KHz"
    if (editing && blinkNumbers && !numbersVisible) {
      lcd.print("  K");
    } else {
      lcd.print(" 1K");
    }
    lcd.print("Hz");
  } else {
    // ".5KHz"
    if (editing && blinkNumbers && !numbersVisible) {
      lcd.print("  K");
    } else {
      lcd.print(".5K");
    }
    lcd.print("Hz");
  }
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
  // Always redraw both rows so the knob symbol moves instantly when the toggle changes
  lastLcdFreqA = lcdFreqA;
  lastLcdFreqB = lcdFreqB;
  writeFreqField(0, lcdFreqA);
  writeFromUpField(rangeFromKHz, rangeUpKHz); // row 0, col 10-15
  writeFreqField(1, lcdFreqB);
  writeStepField(stepHz, uiState == STATE_EDIT);
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
    if (uiState == STATE_EDIT) {
      if (encAccum >= 2) {
        editDirection = -1; // CCW = down
        stepChanged = true;
      } else if (encAccum <= -2) {
        editDirection = 1;  // CW = up
        stepChanged = true;
      }
      encAccum = 0;
    } else {
      // Determine which VFO is being tuned
      bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
      char targetVfo = knobControlsTx ? txVfo : (txVfo == 'A' ? 'B' : 'A');
      long baseFreq = (targetVfo == 'A') ? lcdFreqA : lcdFreqB;
      bool *snapPending = (targetVfo == 'A') ? &snapPendingA : &snapPendingB;

      if (encAccum >= 2) { // CCW (down)
        if (*snapPending) {
          long snapped = (baseFreq / stepHz) * stepHz;
          pendingHz += (snapped - baseFreq);
          *snapPending = false;
        } else {
          pendingHz -= stepHz;
        }
      } else if (encAccum <= -2) { // CW (up)
        if (*snapPending) {
          long snapped = ((baseFreq + stepHz - 1) / stepHz) * stepHz;
          pendingHz += (snapped - baseFreq);
          *snapPending = false;
        } else {
          pendingHz += stepHz;
        }
      }
      encAccum = 0;
    }
  }
}

// ── Banner ────────────────────────────────────────────────────────────────────

void sendBanner() {
  ftdiSerial.println(ARDUINO_BANNER);
  lastHelloMs = millis();
}

// ── Command handler ───────────────────────────────────────────────────────────

void handleCommand(const char *line) {
  lastPythonMsgMs = millis(); // any valid command = Python is alive

  if (strcmp(line, "WHO") == 0) {
    sendBanner();
    return;
  }

  if (strcmp(line, "PING") == 0) {
    ftdiSerial.println("PONG_ARDUINO");
    return;
  }

  // LCD_FREQ:<freqA>:<freqB>:<activeVfo>:<txVfo>
  // Python sends this every refresh cycle with confirmed radio values.
  // Freq values are gated (ignored while knob is active).
  // txVfo is always updated so split mode changes are instant.
  if (strncmp(line, "LCD_FREQ:", 9) == 0) {
    char *p = (char *)line + 9;
    bool gateOpen = (millis() >= freqTxIgnoreUntilMs);
    long newA = atol(p);
    if (gateOpen && newA > 0) {
      if (lcdFreqA != newA) snapPendingA = true;
      lcdFreqA = newA;
    }
    p = strchr(p, ':');
    if (p) {
      long newB = atol(++p);
      if (gateOpen && newB > 0) {
        if (lcdFreqB != newB) snapPendingB = true;
        lcdFreqB = newB;
      }
      p = strchr(p, ':');
      if (p) {
        if (gateOpen) lcdActiveVfo = *(++p);
        else ++p;
        p = strchr(p, ':');
        if (p) txVfo = *(++p);  // always update — no gate
      }
    }
    if (uiState != STATE_EDIT) {
      updateLcd();
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
  // Atomic snapshot of ISR-written pendingHz.
  noInterrupts();
  long pk = pendingHz;
  interrupts();

  if (pk == 0 || uiState == STATE_EDIT) return;

  bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
  char targetVfo      = knobControlsTx ? txVfo : (txVfo == 'A' ? 'B' : 'A');
  long baseFreq       = (targetVfo == 'A') ? lcdFreqA : lcdFreqB;

  if (baseFreq > 0) {
    long newFreq = baseFreq + pk;
    ftdiSerial.print("SET_FREQ:");
    ftdiSerial.print(newFreq);
    ftdiSerial.print(":");
    ftdiSerial.println(targetVfo);
    freqTxIgnoreUntilMs = millis() + FREQ_TX_IGNORE_MS;
    // Update only the target VFO row on the LCD immediately.
    if (targetVfo == 'B') {
      lcdFreqB = newFreq;
      lastLcdFreqB = newFreq;
      writeFreqField(1, newFreq);
    } else {
      lcdFreqA = newFreq;
      lastLcdFreqA = newFreq;
      writeFreqField(0, newFreq);
    }
  } else {
    ftdiSerial.println("NO_BASE_FREQ");
  }

  noInterrupts();
  pendingHz = 0;
  interrupts();
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
      // Exiting edit mode: restore normal display
      uiState = STATE_ONAIR;
      lastBlinkMs = now;
      stepFieldVisible = true;
      writeStepField(stepHz, false);
    } else {
      // Entering edit mode
      uiState = STATE_EDIT;
      lastBlinkMs = now;
      stepFieldVisible = true;
      writeStepField(stepHz, true);
    }
  }
  if (sw == HIGH && lastSw == LOW) {
    if (!swWasLongPressed) {
      ftdiSerial.println("BTN:PRESS");
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
  Serial.begin(115200);
  // DO NOT REMOVE OR MODIFY THE LINE BELOW!
  // This message is required for verifying serial monitor operation.
  Serial.println("Arduino started. Serial is working!");
  ftdiSerial.begin(FTDI_BAUD);

  // Load stepHz from EEPROM (default to 1000 if invalid)
  long eepromStep = 0;
  EEPROM.get(EEPROM_STEP_ADDR, eepromStep);
  if (eepromStep == 500 || eepromStep == 1000) {
    stepHz = eepromStep;
  } else {
    stepHz = 1000;
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

  lcd.init();
  lcd.createChar(0, knobChar); // Ensure custom char is loaded after init
  lcd.backlight();

  // Initialise both frequency fields to "No signal" and show initial step.
  writeFreqField(0, 0);
  writeFromUpField(rangeFromKHz, rangeUpKHz);
  writeFreqField(1, 0);
  writeStepField(stepHz, false);

  pinMode(LED_COMM_PIN,   OUTPUT);
  digitalWrite(LED_COMM_PIN, LOW);

  pinMode(ENC_CLK,        INPUT_PULLUP);
  pinMode(ENC_DT,         INPUT_PULLUP);
  pinMode(ENC_SW,         INPUT_PULLUP);
  pinMode(KNOB_TARGET_SW, INPUT_PULLUP);

  encState = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  lastSw   = digitalRead(ENC_SW);

  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT),  encoderISR, CHANGE);

  delay(1200);
  sendBanner();
}

// ── State Handlers ────────────────────────────────────────────────────────────

void handleOnAir() {
  pollFreqSend();
  pollButton();
  pollFtdi();
  // Always show step field in normal mode
  if (!stepFieldVisible) {
    writeStepField(stepHz, false);
    stepFieldVisible = true;
  }
}

void handleEdit() {
  pollFtdi();

  int sw = digitalRead(ENC_SW);
  unsigned long now = millis();
  static int lastSw = HIGH;
  static unsigned long swPressStart = 0;
  static bool swWasLongPressed = false;
  static bool firstEditFrame = true;
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
      uiState = STATE_ONAIR;
      lastBlinkMs = now;
      stepFieldVisible = true;
      writeStepField(stepHz, false);
      updateLcd(); // restore wheel icon
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
        }
        editParam = (EditParam)(((int)editParam + 1) % 3);
        blinkOn = true;
        lastBlinkMs = now;
      }
      swPressStart = 0;
      swWasLongPressed = false;
    }
    lastSw = sw;

    // ── Encoder: adjust active parameter ──
    if (stepChanged) {
      noInterrupts();
      stepChanged = false;
      int8_t dir = editDirection;
      editDirection = 0;
      interrupts();

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
    }
  }
}

void loop() {
  switch (uiState) {
    case STATE_ONAIR:
      handleOnAir();
      break;
    case STATE_EDIT:
      handleEdit();
      break;
  }
  if (millis() - lastHelloMs >= 1000) {
    sendBanner();
  }
  digitalWrite(LED_COMM_PIN, (millis() - lastPythonMsgMs < COMM_TIMEOUT_MS) ? HIGH : LOW);
}
