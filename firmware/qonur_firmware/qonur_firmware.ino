// Qonur robotic bear firmware (ESP32)
//
// Drives 10 servos through a PCA9685 PWM driver (I2C, address 0x40) and an
// 8-LED WS2812 ring. Accepts JSON commands over USB serial (115200) from
// main.py. Blynk app control (V1-V5) is optional: the firmware boots and runs
// the serial loop without WiFi, and attaches to Blynk when WiFi is available.
//
// PCA9685 channel map:
//   0 left_arm_shoulder   4 right_arm_wrist     7 eye_right
//   1 left_arm_elbow      5 right_arm_elbow     8 eye_left
//   2 left_arm_wrist      6 right_arm_shoulder  9 mouth
//   3 head
//
// Wiring: ESP32 SDA/SCL to PCA9685; WS2812 data on GPIO4. Servos and LEDs on
// a separate 5-6 V supply with common ground to the ESP32.
// See hardware/pca9685-connection.png.
//
// Serial JSON commands (one per line):
//   {"servo": 9, "angle": 70, "duration": 0.2}
//   {"listening": true}
//   {"state": "start_speaking"}   // also: recording_start/stop, end_speaking
//   {"reset": true}

#define BLYNK_TEMPLATE_ID   "ID"
#define BLYNK_TEMPLATE_NAME "NAME"
#define BLYNK_AUTH_TOKEN    "TOKEN"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>

char ssid[] = "SSID";
char pass[] = "PSWRD";

// LEDs
#define LED_PIN    4
#define LED_COUNT  8
Adafruit_NeoPixel pixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// Servos
Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);
#define SERVO_COUNT      10
#define SERVO_FREQ       50
// Pulse range calibrated for the MG90S/SG90 class servos used in the build.
#define SERVO_MIN_PULSE  150
#define SERVO_MAX_PULSE  600

const char* servoNames[SERVO_COUNT] = {
  "left_arm_shoulder",   // 0
  "left_arm_elbow",      // 1
  "left_arm_wrist",      // 2
  "head",                // 3
  "right_arm_wrist",     // 4
  "right_arm_elbow",     // 5
  "right_arm_shoulder",  // 6
  "eye_right",           // 7
  "eye_left",            // 8
  "mouth"                // 9
};

// Servo motion state
int   currentPos[SERVO_COUNT];
int   targetPos[SERVO_COUNT];
int   servoSpeed[SERVO_COUNT];
bool  servoMoving[SERVO_COUNT];
unsigned long lastServoUpdate[SERVO_COUNT];

// LED state
bool        isListening      = false;
uint8_t     breathBrightness = 0;
bool        breathDir        = true;
unsigned long lastBreath     = 0;
unsigned long lastRainbow    = 0;
uint16_t    rainbowHue       = 0;

// Mouth animation
bool        mouthAnimating   = false;
unsigned long mouthLast      = 0;
int         mouthStep        = 0;
const int   mouthPatternSize = 10;
int         mouthPattern[mouthPatternSize] = {70,85,75,90,80,75,85,70,80,90};
unsigned long mouthStepDur   = 200;

// Eye blinking
unsigned long lastBlink          = 0;
const unsigned long blinkInterval = 3000; // ms
const int    blinkClosedAngle     = 0;
const int    blinkOpenAngle       = 90;

// Blynk reconnect state. Connection attempts block for up to 2 s, so retry
// sparingly and only when WiFi is up; the serial loop stays responsive.
unsigned long lastBlynkAttempt = 0;
const unsigned long blynkRetryInterval = 30000; // ms

void setServoAngle(int ch, int angle);
void moveServo(int ch, int angle, float duration);
void updateServoMovements();
void updateLEDs();
void startMouthAnimation(int durationMs);
void stopMouthAnimation();
void updateMouthAnimation();
void resetToNeutral();
void handleCommand(const String &cmd);
void handleCustomState(const String &state);

// Two-parameter overload for the Blynk handlers.
void moveServo(uint8_t ch, int angle) {
  moveServo(ch, angle, 0);
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(SERVO_FREQ);

  for (int i = 0; i < SERVO_COUNT; i++) {
    currentPos[i]      = 90;
    targetPos[i]       = 90;
    servoSpeed[i]      = 5;
    servoMoving[i]     = false;
    lastServoUpdate[i] = 0;
    setServoAngle(i, 90);
  }

  pixels.begin();
  pixels.show();

  // Non-blocking Blynk setup: configure only, connect from loop() when WiFi
  // is available. Blynk.begin() would block boot forever without WiFi.
  WiFi.begin(ssid, pass);
  Blynk.config(BLYNK_AUTH_TOKEN);
  Serial.println("ESP32 robot ready");
}

void loop() {
  // 1) Serial JSON commands from main.py
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length()) handleCommand(cmd);
  }

  // 2) Blynk, only when WiFi is up
  if (WiFi.status() == WL_CONNECTED) {
    if (Blynk.connected()) {
      Blynk.run();
    } else if (millis() - lastBlynkAttempt >= blynkRetryInterval) {
      lastBlynkAttempt = millis();
      Blynk.connect(2000);
    }
  }

  // 3) LEDs and mouth
  updateLEDs();
  updateMouthAnimation();

  // 4) Eye blink every 3 seconds
  unsigned long now = millis();
  if (now - lastBlink >= blinkInterval) {
    lastBlink = now;
    moveServo(7, blinkClosedAngle, 0.1);
    moveServo(8, blinkClosedAngle, 0.1);
    delay(100);
    moveServo(7, blinkOpenAngle, 0.1);
    moveServo(8, blinkOpenAngle, 0.1);
  }

  // 5) Smooth servo interpolation
  updateServoMovements();
}

void handleCommand(const String &jsonCmd) {
  StaticJsonDocument<256> doc;
  auto err = deserializeJson(doc, jsonCmd);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return;
  }
  if (doc.containsKey("servo")) {
    int ch    = doc["servo"];
    int angle = doc["angle"];
    float dur = doc["duration"] | 0.5;
    if (ch >= 0 && ch < SERVO_COUNT) {
      moveServo(ch, angle, dur);
      Serial.printf("%s (ch %d) -> %d deg in %.1f s\n",
        servoNames[ch], ch, angle, dur);
    }
  }
  else if (doc.containsKey("listening")) {
    isListening = doc["listening"];
  }
  else if (doc.containsKey("state")) {
    handleCustomState(doc["state"].as<String>());
  }
  else if (doc.containsKey("reset")) {
    resetToNeutral();
  }
}

void handleCustomState(const String &state) {
  if (state == "recording_start") {
    isListening = true;
  }
  else if (state == "recording_stop") {
    isListening = false;
  }
  else if (state == "start_speaking") {
    startMouthAnimation(3000);
  }
  else if (state == "end_speaking") {
    stopMouthAnimation();
  }
}

// Blynk virtual pins: V1 wave, V2 dance, V3 head shake, V4/V5 wrist sliders
BLYNK_WRITE(V1) { if (param.asInt()) { moveServo(0,120); moveServo(1,60); delay(500); moveServo(0,90); moveServo(1,90);} }
BLYNK_WRITE(V2) { if (param.asInt()) { for (int i=0;i<3;i++){ moveServo(0,60); moveServo(1,120); moveServo(3,60); delay(300); moveServo(0,120); moveServo(1,60); moveServo(3,120); delay(300);} moveServo(0,90); moveServo(1,90); moveServo(3,90);} }
BLYNK_WRITE(V3) { if (param.asInt()) { moveServo(3,45); delay(300); moveServo(3,135); delay(300); moveServo(3,90);} }
BLYNK_WRITE(V4) { moveServo(4, param.asInt()); }
BLYNK_WRITE(V5) { moveServo(5, param.asInt()); }

void moveServo(int ch, int angle, float duration) {
  targetPos[ch] = constrain(angle, 0, 180);
  int diff = abs(targetPos[ch] - currentPos[ch]);
  // Step size per 20 ms update so the move takes roughly `duration` seconds.
  if (duration > 0 && diff > 0) servoSpeed[ch] = max(1, diff / int(duration * 50)); else servoSpeed[ch] = 10;
  servoMoving[ch] = (currentPos[ch] != targetPos[ch]);
  if (duration < 0.1) { currentPos[ch] = targetPos[ch]; setServoAngle(ch, targetPos[ch]); servoMoving[ch] = false; }
}

void updateServoMovements() {
  unsigned long now = millis();
  for (int i = 0; i < SERVO_COUNT; i++) {
    if (servoMoving[i] && now - lastServoUpdate[i] >= 20) {
      lastServoUpdate[i] = now;
      int diff = targetPos[i] - currentPos[i];
      if (abs(diff) <= servoSpeed[i]) { currentPos[i] = targetPos[i]; servoMoving[i] = false; }
      else currentPos[i] += (diff > 0 ? servoSpeed[i] : -servoSpeed[i]);
      setServoAngle(i, currentPos[i]);
    }
  }
}

void setServoAngle(int ch, int angle) {
  int pulse = map(constrain(angle, 0, 180), 0, 180, SERVO_MIN_PULSE, SERVO_MAX_PULSE);
  pwm.setPWM(ch, 0, pulse);
}

void updateLEDs() {
  unsigned long now = millis();
  if (isListening) {
    // Green breathing while recording
    if (now - lastBreath >= 30) {
      lastBreath = now;
      breathBrightness += breathDir ? 8 : -8;
      if (breathBrightness >= 200) { breathBrightness = 200; breathDir = false; }
      else if (breathBrightness <= 20) { breathBrightness = 20; breathDir = true; }
      for (int i = 0; i < LED_COUNT; i++) pixels.setPixelColor(i, pixels.Color(0, breathBrightness, 0));
      pixels.show();
    }
  } else {
    // Slow rainbow when idle
    if (now - lastRainbow >= 50) {
      lastRainbow = now;
      rainbowHue += 1024;
      for (int i = 0; i < LED_COUNT; i++) pixels.setPixelColor(i, pixels.gamma32(pixels.ColorHSV(rainbowHue + (i * 65536L / LED_COUNT))));
      pixels.show();
    }
  }
}

void startMouthAnimation(int durationMs) {
  mouthAnimating = true;
  mouthStep = 0;
  mouthLast = millis();
  mouthStepDur = max(50, durationMs / mouthPatternSize);
}

void stopMouthAnimation() {
  mouthAnimating = false;
  moveServo(9, 90, 0.3);
}

void updateMouthAnimation() {
  if (!mouthAnimating) return;
  unsigned long now = millis();
  if (now - mouthLast >= mouthStepDur) {
    mouthLast = now;
    if (mouthStep < mouthPatternSize) {
      setServoAngle(9, mouthPattern[mouthStep]);
      currentPos[9] = mouthPattern[mouthStep++];
    } else {
      stopMouthAnimation();
    }
  }
}

void resetToNeutral() {
  for (int i = 0; i < SERVO_COUNT; i++) moveServo(i, 90, 1.0);
  stopMouthAnimation();
  isListening = false;
}
