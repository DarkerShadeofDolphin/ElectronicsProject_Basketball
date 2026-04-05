#include "WiFiS3.h"
#include "WiFiUdp.h"
#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_ILI9341.h"
#include "TouchScreen.h"

// TFT & Touch Pins
#define TFT_DC 7
#define TFT_CS 10
#define TFT_RST 6
#define YP A2  
#define XM A3  
#define YM 8   
#define XP 9   

// Calibration
#define TS_MINX 150
#define TS_MINY 120
#define TS_MAXX 920
#define TS_MAXY 940
#define MINPRESSURE 10
#define MAXPRESSURE 1000

const int buttonPin = 2;

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
TouchScreen ts = TouchScreen(XP, YP, XM, YM, 300);
WiFiUDP Udp;

char ssid[] = "Group9";
char pass[] = "12345678";
unsigned int localPort = 2390;
char packetBuffer[255];
IPAddress hoopIp(192, 168, 4, 1);

// Game Variables
int score_total = 0;
int balls_scored = 0;
int level = 1;
int lastLevel = 1;
bool playerInPosition = false;
unsigned long lastTapTime = 0;
unsigned long stateTimer = 0;
int countdownStep = 0;

// Timer Variables (From Updated Version)
unsigned long gameStartTime = 0;
unsigned long pauseStartTime = 0;
const unsigned long gameTimerLimit = 20000; // 20 seconds
int lastDisplayedSeconds = -1;

enum GameState { START_MENU, COUNTDOWN, GAME_RUNNING, PAUSED, GAME_OVER };
GameState currentState = START_MENU;

void setup() {
  Serial.begin(115200);
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);
  
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Connecting to WiFi...");

  WiFi.begin(ssid, pass);
  Udp.begin(localPort);
  pinMode(buttonPin, INPUT_PULLUP);

  showInstructions();
}

void loop() {
  pinMode(YP, INPUT);
  pinMode(XM, INPUT);
  pinMode(YM, INPUT);
  pinMode(XP, INPUT);
  TSPoint p = ts.getPoint();
  pinMode(XM, OUTPUT);
  pinMode(YP, OUTPUT);

  bool touched = (p.z > MINPRESSURE && p.z < MAXPRESSURE);
  unsigned long now = millis();

  int tx = 0, ty = 0;
  if (touched) {
    tx = map(p.y, TS_MINY, TS_MAXY, 0, 320);
    ty = map(p.x, TS_MAXX, TS_MINX, 0, 240);
  }

  int buttonState = digitalRead(buttonPin);
  
  // Physical Button Logic: Pauses Game
  if (buttonState == LOW && currentState == GAME_RUNNING) {
    currentState = PAUSED;
    pauseStartTime = millis(); // Track when we paused
    drawPauseMenu(); 
    lastTapTime = now;
  }

  switch (currentState) {
    case START_MENU:
      handleData(); 
      if (touched && (now - lastTapTime > 500)) {
        startCountdown();
      }
      break;

    case COUNTDOWN:
      handleCountdown(now);
      break;

    case GAME_RUNNING:
      handleData();
      updateDisplay(); // Now handles timer logic
      
      if (touched && ty > 200 && (now - lastTapTime > 500)) {
        currentState = PAUSED;
        pauseStartTime = millis(); 
        drawPauseMenu();
        lastTapTime = now;
      }
      break;

    case PAUSED:
      handlePauseMenu(touched, tx, ty, now);
      break;

    case GAME_OVER:
      if (now - stateTimer > 5000) {
        sendControlPacket("UNMUTE");
        score_total = 0;
        balls_scored = 0;
        level = 1;
        lastLevel = 1;
        lastDisplayedSeconds = -1;
        currentState = START_MENU;
        showInstructions();
      }
      break;
  }
}

void sendControlPacket(String msg) {
  Udp.beginPacket(hoopIp, localPort);
  Udp.print(msg);
  Udp.endPacket();
}

void handleData() {
  int packetSize = Udp.parsePacket();
  if (packetSize) {
    int len = Udp.read(packetBuffer, 255);
    if (len > 0) packetBuffer[len] = 0;
    String rawData = String(packetBuffer);

    if (rawData == "CMD:READY") {
      playerInPosition = true;
      if (currentState == START_MENU) startCountdown();
    }
    else if (rawData == "CMD:NOTREADY") {
      playerInPosition = false;
    }
    else if (rawData.startsWith("B:")) {
      float diam = rawData.substring(2).toFloat();
      balls_scored++;
      if (diam < 100.0) score_total += 2;
      else if (diam >= 100.0 && diam <= 150.0) score_total += 3;
      else if (diam > 150.0) score_total += 4;

      level = (balls_scored / 5) + 1;
    }
    
    // Logic to update Motor Arduino when level increases
    if (level != lastLevel) {
      sendControlPacket("LVL:" + String(level));
      lastLevel = level;
    }
  }
}

void startCountdown() {
  sendControlPacket("MUTE");
  currentState = COUNTDOWN;
  countdownStep = 3;
  stateTimer = millis();
  lastTapTime = millis();
  tft.fillScreen(ILI9341_BLACK);
}

void handleCountdown(unsigned long now) {
  if (now - stateTimer > 1000) {
    tft.fillScreen(ILI9341_BLACK);
    tft.setCursor(100, 100);
    tft.setTextSize(5);
    tft.setTextColor(ILI9341_WHITE);

    if (countdownStep == 3) tft.print("READY!");
    else if (countdownStep == 2) tft.print("SET!");
    else if (countdownStep == 1) tft.print("GO!");
    
    countdownStep--;
    stateTimer = now;

    if (countdownStep < 0) {
      tft.fillScreen(ILI9341_BLACK);
      currentState = GAME_RUNNING;
      gameStartTime = millis(); 
      updateDisplay();
    }
  }
}

void updateDisplay() {
  unsigned long now = millis();
  long timeLeftMillis = gameTimerLimit - (now - gameStartTime);
  
  // Check for Time Out
  if (timeLeftMillis < 1000) {
    currentState = GAME_OVER;
    stateTimer = now;
    showFinalScore();
    return;
  }

  int timeLeftSeconds = timeLeftMillis / 1000;

  // Only update display if a second has passed or score changed
  if (timeLeftSeconds != lastDisplayedSeconds) {
    
    // Clear screen for dramatic Big Numbers (3, 2, 1)
    if ((timeLeftSeconds == 3 && lastDisplayedSeconds > 3)) {
      tft.fillScreen(ILI9341_BLACK);
    }

    lastDisplayedSeconds = timeLeftSeconds;

    if (timeLeftSeconds <= 3) {
      tft.fillRect(140, 100, 100, 100, ILI9341_BLACK);
      tft.setCursor(140, 100);
      tft.setTextSize(8);
      tft.setTextColor(ILI9341_WHITE);
      tft.print(timeLeftSeconds);
    }
    else {
      // Timer HUD (Flicker Fix: setTextColor with Black background)
      tft.setCursor(10, 10);
      tft.setTextSize(3);
      tft.setTextColor(ILI9341_CYAN, ILI9341_BLACK);
      tft.print("TIME: "); tft.print(timeLeftSeconds); tft.print("s  ");

      // Score
      tft.setCursor(10, 50);
      tft.setTextSize(5);
      tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
      tft.print("SCORE: "); tft.println(score_total);

      // Stats
      tft.setCursor(10, 100);
      tft.setTextSize(3);
      tft.setTextColor(ILI9341_GREEN, ILI9341_BLACK);
      tft.print("Balls: "); tft.print(balls_scored); tft.print("  ");
      
      tft.setCursor(10, 130);
      tft.setTextColor(ILI9341_YELLOW, ILI9341_BLACK);
      tft.print("Level: "); tft.print(level); tft.print("  ");

      // Ready Status
      tft.setCursor(220, 5);
      if (playerInPosition || currentState == GAME_RUNNING) {
        tft.setTextColor(ILI9341_GREEN, ILI9341_BLACK);
        tft.print("READY");
      } else {
        tft.setTextColor(ILI9341_RED, ILI9341_BLACK);
        tft.print("WAIT ");
      }

      tft.drawFastHLine(0, 210, 320, ILI9341_DARKGREY);
      tft.setCursor(85, 220);
      tft.setTextSize(2);
      tft.setTextColor(ILI9341_RED, ILI9341_BLACK);
      tft.print("TAP TO PAUSE");
    }
  }  
}

void showInstructions() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(10, 40);
  tft.setTextSize(4);
  tft.println("WELCOME TO");
  tft.setCursor(10, 80);
  tft.println("HoopMaster!");
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_BLUE);
  tft.println("\nInstructions:");
  tft.println("- Small Ball: 2pts");
  tft.println("- Med Ball: 3pts");
  tft.println("- Large Ball: 4pts");
  tft.setTextColor(ILI9341_WHITE);
  tft.println("\nStand in range to start!");
}

void drawPauseMenu() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(110, 40);
  tft.print("PAUSED");
  tft.drawRect(10, 100, 140, 80, ILI9341_GREEN);
  tft.setTextSize(2);
  tft.setCursor(35, 130); tft.print("RESUME");
  tft.drawRect(170, 100, 140, 80, ILI9341_RED);
  tft.setTextSize(3);
  tft.setCursor(210, 130); tft.print("END");
}

void handlePauseMenu(bool touched, int tx, int ty, unsigned long now) {
  if (touched && (now - lastTapTime > 500)) {
    if (tx >= 10 && tx <= 150 && ty >= 100 && ty <= 180) { 
      // Resume Logic: Calculate Pause Duration
      unsigned long pauseDuration = millis() - pauseStartTime;
      gameStartTime += pauseDuration; // Shift start time so timer doesn't lose time

      tft.fillScreen(ILI9341_BLACK);
      currentState = GAME_RUNNING;
      lastDisplayedSeconds = -1;
      updateDisplay();
    }
    else if (tx >= 170 && tx <= 310 && ty >= 100 && ty <= 180) {
      currentState = GAME_OVER;
      stateTimer = now;
      showFinalScore();
    }
    lastTapTime = now;
  }
}

void showFinalScore() {
  sendControlPacket("LVL:1");
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_RED);
  tft.setTextSize(4);
  tft.setCursor(60, 40);
  tft.println("GAME OVER");
  tft.drawFastHLine(0, 80, 320, ILI9341_WHITE);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(30, 100);
  tft.print("Final Score:   "); tft.println(score_total);
  tft.setCursor(30, 130);
455  tft.print("Total Balls: "); tft.println(balls_scored);
  tft.setCursor(30, 160);
  tft.print("Final Level:   "); tft.println(level);
  tft.setCursor(40, 210);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.print("Returning to menu in 5 seconds...");
}