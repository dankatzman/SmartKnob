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

// pendingHz is written by the ISR — read atomically in main loop.
volatile long pendingHz = 0;


// ── Frequency tracking ────────────────────────────────────────────────────────

// After sending SET_FREQ, ignore LCD_FREQ updates from Python for this long.
// Keeps the LCD stable while the knob is being turned.
unsigned long freqTxIgnoreUntilMs = 0;
const unsigned long FREQ_TX_IGNORE_MS = 1500;

// (Frequency range enforcement removed)

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
//  Row0:│ │  FREQ A │  │(future)  │
//       ├─┼─────────┼──┼──────────┤
//  Row1:│ │  FREQ B │  │  STEP    │
//       └─┴─────────┴──┴──────────┘
//
//  FREQ field: col 1–9  (9 chars), rows 0 and 1.
//  STEP field: col 11–15 (5 chars), row 1 only — e.g. " 1KHZ" or ".5KHZ".
//  Col 10 is free on both rows.
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


// (FROM/UP field removed)

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

// Redraws only the rows whose frequency has changed.
void updateLcd() {
  // Always redraw both rows so the + sign moves instantly when the toggle changes
  lastLcdFreqA = lcdFreqA;
  lastLcdFreqB = lcdFreqB;
  writeFreqField(0, lcdFreqA);
  // FROM/UP field removed
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
      if (encAccum >= 2 || encAccum <= -2) {
        stepHz = (stepHz == 1000) ? 500 : 1000;
        stepChanged = true; // Signal main loop to update LCD/serial
        // Set snapPending for the currently tuned VFO so next knob turn snaps to new step boundary
        bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
        char targetVfo = knobControlsTx ? txVfo : (txVfo == 'A' ? 'B' : 'A');
        if (targetVfo == 'A') snapPendingA = true;
        else snapPendingB = true;
        // Do not touch LCD or Serial here!
        lastBlinkMs = millis();
        stepFieldVisible = true;
      }
      encAccum = 0; // Always reset in edit mode to prevent lockup
    } else {
      // Determine which VFO is being tuned
      bool knobControlsTx = (digitalRead(KNOB_TARGET_SW) == HIGH);
      char targetVfo = knobControlsTx ? txVfo : (txVfo == 'A' ? 'B' : 'A');
      long baseFreq = (targetVfo == 'A') ? lcdFreqA : lcdFreqB;
      bool *snapPending = (targetVfo == 'A') ? &snapPendingA : &snapPendingB;

      // Debug: print encoder action and variables
      Serial.print("[ENC] encAccum: "); Serial.print(encAccum);
      Serial.print(", stepHz: "); Serial.print(stepHz);
      Serial.print(", baseFreq: "); Serial.print(baseFreq);
      Serial.print(", snapPending: "); Serial.println(*snapPending);

      if (encAccum >= 2) { // CCW (down)
        if (*snapPending) {
          long snapped = (baseFreq / stepHz) * stepHz;
          Serial.print("[ENC] Snap down. snapped: "); Serial.println(snapped);
          pendingHz += (snapped - baseFreq);
          *snapPending = false;
        } else {
          Serial.println("[ENC] Step down.");
          pendingHz -= stepHz;
        }
      } else if (encAccum <= -2) { // CW (up)
        if (*snapPending) {
          long snapped = ((baseFreq + stepHz - 1) / stepHz) * stepHz;
          Serial.print("[ENC] Snap up. snapped: "); Serial.println(snapped);
          pendingHz += (snapped - baseFreq);
          *snapPending = false;
        } else {
          Serial.println("[ENC] Step up.");
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
    // Also print to Serial for debug monitor
    Serial.print("SET_FREQ:");
    Serial.print(newFreq);
    Serial.print(":");
    Serial.println(targetVfo);
    Serial.print("DEBUG: stepHz=");
    Serial.print(stepHz);
    Serial.print(", pk=");
    Serial.print(pk);
    Serial.print(", baseFreq=");
    Serial.print(baseFreq);
    Serial.print(", newFreq=");
    Serial.println(newFreq);
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

// Custom knob icon (single dot in center for reference)
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
  Serial.begin(115200); // Ensure Serial is initialized for debug output
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

  // (FROM/UP EEPROM and debug print removed)

  lcd.init();
  lcd.createChar(0, knobChar); // Ensure custom char is loaded after init
  lcd.backlight();

  // Initialise both frequency fields to "No signal" and show initial step.
  writeFreqField(0, 0);
  writeFreqField(1, 0);
  writeStepField(stepHz, false);

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

// ── State Handlers ──
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
  pollButton();
  pollFtdi();
  // If step value changed, update LCD and send serial
  if (stepChanged) {
    noInterrupts();
    stepChanged = false;
    interrupts();
    // Save stepHz to EEPROM
    EEPROM.put(EEPROM_STEP_ADDR, stepHz);
    writeStepField(stepHz, true, true, true);
    ftdiSerial.print("STEP_EDIT: ");
    ftdiSerial.println(stepHz);
  }
  // Blinking only the numbers in the step field in edit mode
  unsigned long now = millis();
  if (now - lastBlinkMs > 400) {
    lastBlinkMs = now;
    stepFieldVisible = !stepFieldVisible;
    writeStepField(stepHz, true, true, stepFieldVisible);
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
}

