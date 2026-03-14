#include <Arduino.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <string.h>

// SoftwareSerial on pins 8/9 — FTDI232 USB adapter — Python communication
const unsigned long FTDI_BAUD = 57600;
SoftwareSerial ftdiSerial(8, 9);  // RX=pin8, TX=pin9

// I2C LCD 16×2 — address 0x27 (try 0x3F if display stays blank)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Custom knob icon (circle with dot)
byte knobChar[8] = {
  0b01110,
  0b10001,
  0b10101,
  0b10001,
  0b01110,
  0b00000,
  0b00000,
  0b00000
};

// RX/TX frequency and active marker
long rxFreqHz = 0;
long txFreqHz = 0;
char activeVfo = 'T'; // 'R' for RX, 'T' for TX

// Rotary encoder pins
const int ENC_CLK = 2;
const int ENC_DT  = 3;
const int ENC_SW  = 6;

const char *ARDUINO_BANNER = "HELLO_ARDUINO:VFOKNOB";

char serialLine[48];
int  serialLineLen = 0;
unsigned long lastHelloMs = 0;

// Quadrature lookup table
const int8_t ENC_TABLE[16] = {
  0, -1,  1,  0,
  1,  0,  0, -1,
  -1,  0,  0,  1,
  0,  1, -1,  0
};

volatile uint8_t encState  = 3;
volatile int8_t  encAccum  = 0;

volatile long          pendingKhz = 0;
volatile unsigned long lastStepMs = 0;

long baseTxFreqHz = 0;
const unsigned long IDLE_MS = 300;
volatile long stepKhz = 1;
unsigned long freqTxIgnoreUntilMs = 0;
const unsigned long FREQ_TX_IGNORE_MS = 1500;

int lastSw = HIGH;
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
  strcpy(buf, "1.0");
}

void lcdWriteLine(int row, const char *text) {
  lcd.setCursor(0, row);
  lcd.print(text);
}

void updateLcdFreq() {
  // Print RX frequency in row 0, columns 0-8
  char buf[17];
  formatFreqLine(rxFreqHz, buf);
  lcd.setCursor(0, 0);
  for (int i = 0; i < 9; ++i) lcd.print(buf[i]);

  // Print TX frequency in row 1, columns 0-8
  formatFreqLine(txFreqHz, buf);
  lcd.setCursor(0, 1);
  for (int i = 0; i < 9; ++i) lcd.print(buf[i]);
  // No printing outside columns 0-8 in either row
}


void encoderISR() {
  uint8_t pins  = PIND;
  uint8_t state = (((pins >> 2) & 1) << 1) | ((pins >> 3) & 1);
  if (state == encState) return;
  encAccum += ENC_TABLE[(encState << 2) | state];
  encState = state;
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

void sendBanner() {
  ftdiSerial.println(ARDUINO_BANNER);
  lastHelloMs = millis();
}

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
  if (strncmp(line, "LCD_FREQ:", 9) == 0) {
    char *p = (char*)line + 9;
    rxFreqHz = atol(p);
    p = strchr(p, ':'); if (p) { txFreqHz = atol(++p); }
    p = strchr(p, ':'); if (p) { activeVfo = (*(++p) == 'R') ? 'R' : 'T'; }
    updateLcdFreq();
    return;
  }
}

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

void pollFreqSend() {
  noInterrupts();
  long pk = pendingKhz;
  interrupts();
  if (pk == 0) return;
  if (baseTxFreqHz > 0) {
    long newFreq = baseTxFreqHz + (pk * 1000L);
    baseTxFreqHz = newFreq;
    rxFreqHz = newFreq; // Immediately update RX frequency for LCD
    updateLcdFreq(); // Update LCD immediately after frequency changes
    ftdiSerial.print("SET_FREQ:");
    ftdiSerial.println(newFreq);
    freqTxIgnoreUntilMs = millis() + FREQ_TX_IGNORE_MS;
  } else {
    ftdiSerial.println("NO_BASE_FREQ");
  }
  noInterrupts();
  pendingKhz = 0;
  interrupts();
}

void pollButton() {
  int sw = digitalRead(ENC_SW);
  if (sw != lastSw) {
    if (sw == LOW) {
      ftdiSerial.println("BTN:PRESS");
    }
    lastSw = sw;
  }
}

void setup() {
  ftdiSerial.begin(FTDI_BAUD);
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, knobChar);
  char buf[17];
  formatFreqLine(0, buf);
  lcdWriteLine(0, buf);
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