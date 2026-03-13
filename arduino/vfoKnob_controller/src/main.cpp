#include <Arduino.h>
#include <string.h>

// Serial baud rate for communication
const unsigned long SERIAL_BAUD = 115200;

// Identification banner sent to PC
const char *ARDUINO_BANNER = "HELLO_ARDUINO:VFOKNOB";

// Buffer for incoming serial data
char serialLine[48];
int serialLineLen = 0;

// Timestamp of last banner sent
unsigned long lastHelloMs = 0;

// Sends the Arduino identification banner over serial
void sendBanner() {
  Serial.println(ARDUINO_BANNER);
  Serial.flush();
  lastHelloMs = millis();
}

// Handles incoming serial commands: WHO and PING
void handleCommand(const char *line) {
  if (strcmp(line, "WHO") == 0) {
    sendBanner();
    return;
  }

  if (strcmp(line, "PING") == 0) {
    Serial.println("PONG_ARDUINO");
    return;
  }
}

// Reads serial input, builds lines, and dispatches commands
void pollSerial() {
  while (Serial.available() > 0) {
    char ch = static_cast<char>(Serial.read());
    if (ch == '\r') {
      continue;
    }
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

// Arduino setup: initialize serial and send initial banner
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1200);
  sendBanner();
}

// Main loop: process serial and periodically send ID banner
void loop() {
  pollSerial();

  // Repeat ID periodically so the PC app can detect the board even if opened later.
  if (millis() - lastHelloMs >= 1000) {
    sendBanner();
  }
}
