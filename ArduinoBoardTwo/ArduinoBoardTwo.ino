#include "WiFiS3.h"
#include "WiFiUdp.h"
#include <Wire.h>
#include <Motoron.h>

MotoronI2C mc;
WiFiUDP Udp;

// Network Settings
char ssid[] = "Group9";
char pass[] = "12345678";
IPAddress remoteIp(192, 168, 4, 2);
unsigned int localPort = 2390;

// Pins
const int trigPin = 9, echo1Pin = 10, echo2Pin = 11, ledPin = 8;
const uint8_t LEFT_ENC_A = 2, LEFT_ENC_B = 4;
const uint8_t RIGHT_ENC_A = 3, RIGHT_ENC_B = 5;

// Motoron Constants
const uint8_t LEFT_MOTOR = 1;
const uint8_t RIGHT_MOTOR = 2;
const int16_t DRIVE_SPEED = 500;
const int8_t LEFT_DIR_SIGN = 1;
const int8_t RIGHT_DIR_SIGN = -1;

// Global State
int currentLevel = 1;
typedef struct {
  int distance;
  boolean receiveComplete;
} TF;
TF Lidar = { 0, false };
const float TOTAL_DIST = 25;
float maxDiameterFound = 0;
unsigned long ballDetectedTime = 0;
bool currentlyDetecting = false;
bool wasReady = false;
bool lidarEnabled = true;
bool movingForward = true;
char packetBuffer[255];

volatile long leftCount = 0;
volatile long rightCount = 0;

//Function Prototypes for Interrupts
void leftEncoderISR();
void rightEncoderISR();

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);

  WiFi.beginAP(ssid, pass);
  Udp.begin(localPort);

  pinMode(trigPin, OUTPUT);
  pinMode(echo1Pin, INPUT);
  pinMode(echo2Pin, INPUT);
  pinMode(ledPin, OUTPUT);

  Wire.begin();
  mc.reinitialize();
  mc.disableCrc();
  mc.clearResetFlag();

  mc.setMaxAcceleration(LEFT_MOTOR, 200);
  mc.setMaxDeceleration(LEFT_MOTOR, 300);
  mc.setMaxAcceleration(RIGHT_MOTOR, 200);
  mc.setMaxDeceleration(RIGHT_MOTOR, 300);

  pinMode(LEFT_ENC_A, INPUT_PULLUP);
  pinMode(LEFT_ENC_B, INPUT_PULLUP);
  pinMode(RIGHT_ENC_A, INPUT_PULLUP);
  pinMode(RIGHT_ENC_B, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(LEFT_ENC_A), leftEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(RIGHT_ENC_A), rightEncoderISR, CHANGE);

  Serial.println("System Ready");
}

void loop() {
  // Check for incoming UDP commands
  int packetSize = Udp.parsePacket();
  if (packetSize) {
    int len = Udp.read(packetBuffer, 255);
    if (len > 0) packetBuffer[len] = 0;
    String cmd = String(packetBuffer);

    if (cmd.startsWith("LVL:")) {
      currentLevel = cmd.substring(4).toInt();
      Serial.print("Level changed to: ");
      Serial.println(currentLevel);
    } else if (cmd == "MUTE") {
      lidarEnabled = false;
      Serial.println("WiFi Reporting: MUTED");
    } else if (cmd == "UNMUTE") {
      lidarEnabled = true;
      Serial.println("WiFi Reporting: UNMUTED");
    }
  }

  // Handle Lidar and Motor Logic
  getLidarData(&Lidar);

  if (Lidar.receiveComplete) {
    Lidar.receiveComplete = false;

    if (currentLevel >= 2) {
      if (movingForward) {
        goBackward();
        if (Lidar.distance <= 70) movingForward = false;
      } else {
        goForward();
        if (Lidar.distance >= 100) movingForward = true;
      }
    } else {
      stopMoving();
    }

    // WiFi Reporting
    if (lidarEnabled && currentLevel == 1) {
      bool isReady = (Lidar.distance >= 90 && Lidar.distance <= 100);
      if (isReady && !wasReady) {
        sendUDP("CMD:READY");
        wasReady = true;
      } else if (!isReady && wasReady) {
        sendUDP("CMD:NOTREADY");
        wasReady = false;
      }
    }
  }

  // Ball Detection
  runSonars();
}

// Helper Functions

void getLidarData(TF* lidar) {
  static int i = 0;
  static int rx[9];
  while (Serial1.available()) {
    rx[i] = Serial1.read();
    if (rx[0] != 0x59) i = 0;
    else if (i == 1 && rx[1] != 0x59) i = 0;
    else if (i == 8) {
      int checksum = 0;
      for (int j = 0; j < 8; j++) checksum += rx[j];
      if (rx[8] == (checksum % 256)) {
        lidar->distance = rx[2] + rx[3] * 256;
        lidar->receiveComplete = true;
      }
      i = 0;
    } else i++;
  }
}

void runSonars() {
  float d1 = readDistance(echo1Pin), d2 = readDistance(echo2Pin);
  float currentDiameter = TOTAL_DIST - (d1 + d2);

  if (currentDiameter > 5.0) {
    currentlyDetecting = true;
    ballDetectedTime = millis();
    digitalWrite(ledPin, HIGH);
    if (currentDiameter > maxDiameterFound) maxDiameterFound = currentDiameter;
  } else if (currentlyDetecting && (millis() - ballDetectedTime > 100)) {
    sendUDP("B:" + String(maxDiameterFound));
    digitalWrite(ledPin, LOW);
    maxDiameterFound = 0;
    currentlyDetecting = false;
  }
}

float readDistance(int pin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(pin, HIGH, 2000);
  return (duration == 0) ? TOTAL_DIST : (duration * 0.034) / 2;
}

void sendUDP(String message) {
  Udp.beginPacket(remoteIp, localPort);
  Udp.print(message);
  Udp.endPacket();
  Serial.println("Sent: " + message);
}

void goForward() {
  mc.setSpeed(LEFT_MOTOR, LEFT_DIR_SIGN * DRIVE_SPEED);
  mc.setSpeed(RIGHT_MOTOR, RIGHT_DIR_SIGN * DRIVE_SPEED);
}

void goBackward() {
  mc.setSpeed(LEFT_MOTOR, -LEFT_DIR_SIGN * DRIVE_SPEED);
  mc.setSpeed(RIGHT_MOTOR, -RIGHT_DIR_SIGN * DRIVE_SPEED);
}

void stopMoving() {
  mc.setSpeed(LEFT_MOTOR, 0);
  mc.setSpeed(RIGHT_MOTOR, 0);
}

// Encoder ISRs
void leftEncoderISR() {
  bool a = digitalRead(LEFT_ENC_A);
  bool b = digitalRead(LEFT_ENC_B);
  (a == b) ? leftCount++ : leftCount--;
}

void rightEncoderISR() {
  bool a = digitalRead(RIGHT_ENC_A);
  bool b = digitalRead(RIGHT_ENC_B);
  (a == b) ? rightCount-- : rightCount++;
}