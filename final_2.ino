#include <Arduino.h>
#include "HX711.h"
#include <U8g2lib.h>
#include <SD.h>
#include <SPI.h>
#include <math.h>

// ================= Pin Definitions =================
#define HX_DT      D2
#define HX_SCK     D3
#define OLED_CS    D10
#define OLED_DC    D9
#define OLED_RST   D8
#define SD_CS      SS
#define BTN_TARE   D4
#define BTN_NEXT   D5
#define BTN_PREV   D6
#define BTN_SAVE   D7

// ================= Config =================
#define STABLE_DIFF        0.6
#define STABLE_TIME        600
#define SAMPLE_COUNT       6
#define SAMPLE_INTERVAL    40
#define EMPTY_THRESHOLD    2.0
#define EMPTY_HOLD_TIME    800
#define SAVE_COOLDOWN      2000

// ================= Objects =================
HX711 scale;
U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI u8g2(U8G2_R0, OLED_CS, OLED_DC, OLED_RST);

// ================= Globals =================
float calibration_factor = -402.5;
bool sdReady = false;
bool isStarting = true; 

int fishIndex = 0;
float filteredWeight = 0;
float lastWeight = 0;
bool weightStable = false;
unsigned long stableStart = 0;

float sampleBuffer[SAMPLE_COUNT];
int sampleIndex = 0;
bool confirmed = false;
float confirmedWeight = 0;

unsigned long emptyStart = 0;
unsigned long lastRead = 0;
unsigned long lastDisplay = 0;

unsigned long lastSaveTime = 0 - SAVE_COOLDOWN;
String saveStatus = "";
unsigned long saveStatusTime = 0;

// ================= Fish Table =================
struct Fish { const char* name; float a, b; };
Fish fishes[] = {
  {"pla thu", 0.008, 3.1184},
  {"pla lung", 0.0047, 3.2894},
  {"pla kulaegluy", 0.0105, 2.949},
  {"pla lhungkheaw", 0.0084, 3.0388},
  {"pla sikunbang", 0.0094, 3.0839},
  {"pla thukaekrephuyaw", 0.0112, 2.9855},
  {"pla sikuntato", 0.0068, 3.2195},
  {"pla khanglhueng", 0.0139, 2.9368},
  {"pla kankai", 0.0186, 2.8928},
  {"pla olai", 0.019, 2.888},
  {"pla odum", 0.0304, 2.8693}
};
const int FISH_COUNT = sizeof(fishes) / sizeof(fishes[0]);

// ================= Time Logic =================
struct DateTime { int y, mo, d, h, mi, s; };
DateTime compileTime;

int monthFromStr(const char* m) {
  const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  return (strstr(months, m) - months) / 3 + 1;
}

void initCompileTime() {
  char m[4];
  sscanf(__DATE__, "%s %d %d", m, &compileTime.d, &compileTime.y);
  compileTime.mo = monthFromStr(m);
  sscanf(__TIME__, "%d:%d:%d", &compileTime.h, &compileTime.mi, &compileTime.s);
}

String nowTH() {
  unsigned long t = millis() / 1000;
  int s = compileTime.s + t;
  int mi = compileTime.mi + s / 60; s %= 60;
  int h = compileTime.h + mi / 60; mi %= 60;
  int d = compileTime.d + h / 24; h %= 24;
  char buf[24];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d", compileTime.y, compileTime.mo, d, h, mi, s);
  return String(buf);
}

// ================= Logic =================
float calcLength(float W, float a, float b) {
  return (W <= 0) ? 0 : pow(W / a, 1.0 / b);
}

void updateConfirm() {
  static unsigned long lastSample = 0;
  if (!weightStable || confirmed) {
    if (!weightStable) sampleIndex = 0;
    return;
  }
  if (millis() - lastSample < SAMPLE_INTERVAL) return;
  lastSample = millis();

  sampleBuffer[sampleIndex++] = filteredWeight;

  if (sampleIndex >= SAMPLE_COUNT) {
    float sum = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) sum += sampleBuffer[i];
    confirmedWeight = sum / SAMPLE_COUNT;
    confirmed = true;
    
    // --- Serial Output ---
    Serial.println("--- Weight Confirmed ---");
    Serial.print("Fish: "); Serial.println(fishes[fishIndex].name);
    Serial.print("Weight: "); Serial.print(confirmedWeight); Serial.println(" g");
    Serial.print("Calc Length: "); Serial.print(calcLength(confirmedWeight, fishes[fishIndex].a, fishes[fishIndex].b)); Serial.println(" cm");
    Serial.println("------------------------");
  }
}

void autoResetWhenEmpty() {
  if (filteredWeight < EMPTY_THRESHOLD) {
    if (emptyStart == 0) emptyStart = millis();
    if (millis() - emptyStart > EMPTY_HOLD_TIME) {
      if(confirmed) Serial.println("Scale Empty: System Ready for next fish.");
      confirmed = false;
      sampleIndex = 0;
      weightStable = false;
      stableStart = 0;
    }
  } else {
    emptyStart = 0;
  }
}

void saveToSD() {
  if (!sdReady || !confirmed || (millis() - lastSaveTime < SAVE_COOLDOWN)) {
    if(!sdReady) Serial.println("Save Failed: SD not ready");
    if(!confirmed) Serial.println("Save Failed: Weight not confirmed");
    return;
  }

  digitalWrite(OLED_CS, HIGH);
  File f = SD.open("/fish.csv", FILE_APPEND);
  if (!f) {
    saveStatus = "SD ERROR";
    saveStatusTime = millis();
    Serial.println("Error opening fish.csv");
    return;
  }

  float finalL = calcLength(confirmedWeight, fishes[fishIndex].a, fishes[fishIndex].b);
  String timestamp = nowTH();

  f.printf("%s,%s,%.2f,%.2f\n", timestamp.c_str(), fishes[fishIndex].name, confirmedWeight, finalL);
  f.close();

  saveStatus = "SAVED";
  saveStatusTime = millis();
  lastSaveTime = millis();

  // --- Serial Output ---
  Serial.print(">>> DATA SAVED TO SD: ");
  Serial.print(timestamp); Serial.print(" | ");
  Serial.print(fishes[fishIndex].name); Serial.print(" | ");
  Serial.print(confirmedWeight); Serial.print("g | ");
  Serial.print(finalL); Serial.println("cm");
}

void drawOLED() {
  u8g2.firstPage();
  do {
    if (isStarting) {
      u8g2.setFont(u8g2_font_6x13_tr);
      u8g2.setCursor(0, 12); u8g2.print("Select Fish Species:");
      u8g2.drawHLine(0, 14, 128);
      u8g2.setFont(u8g2_font_7x14_tr); 
      u8g2.setCursor(0, 34); u8g2.print("> "); u8g2.print(fishes[fishIndex].name);
      int nextIdx = (fishIndex + 1) % FISH_COUNT;
      u8g2.setFont(u8g2_font_6x12_tr);
      u8g2.setCursor(0, 52); u8g2.print("  "); u8g2.print(fishes[nextIdx].name); u8g2.print("...");
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.setCursor(0, 64); u8g2.print("NEXT/PREV: Scroll | SAVE: Select");
    } else {
      float showW = confirmed ? confirmedWeight : filteredWeight;
      u8g2.setFont(u8g2_font_6x13_tr);
      u8g2.setCursor(0, 14); u8g2.print("Fish: "); u8g2.print(fishes[fishIndex].name);
      u8g2.setCursor(0, 30); u8g2.print("W: "); u8g2.print(showW, 2); u8g2.print(" g");
      u8g2.setCursor(0, 46); u8g2.print("L: ");
      u8g2.print(calcLength(showW, fishes[fishIndex].a, fishes[fishIndex].b), 2); u8g2.print(" cm");
      u8g2.setCursor(0, 62);
      if (saveStatus != "" && millis() - saveStatusTime < 1500) u8g2.print(saveStatus);
      else if (confirmed) u8g2.print("[CONFIRMED]");
      else if (weightStable) u8g2.print("[SAMPLING]");
      else u8g2.print("[WAITING]");
    }
  } while (u8g2.nextPage());
}

void handleButtons() {
  static unsigned long lastPress = 0;
  if (millis() - lastPress < 200) return;

  if (digitalRead(BTN_NEXT) == LOW) {
    fishIndex = (fishIndex + 1) % FISH_COUNT;
    Serial.print("Menu Select: "); Serial.println(fishes[fishIndex].name);
    lastPress = millis();
  }
  else if (digitalRead(BTN_PREV) == LOW) {
    fishIndex = (fishIndex - 1 + FISH_COUNT) % FISH_COUNT;
    Serial.print("Menu Select: "); Serial.println(fishes[fishIndex].name);
    lastPress = millis();
  }
  else if (digitalRead(BTN_SAVE) == LOW) {
    if (isStarting) {
      isStarting = false;
      scale.tare();
      Serial.print("System Started with Fish: "); Serial.println(fishes[fishIndex].name);
    } else {
      saveToSD();
    }
    lastPress = millis();
  }
  else if (digitalRead(BTN_TARE) == LOW && !isStarting) {
    scale.tare();
    confirmed = false;
    Serial.println("Manual Tare Executed");
    lastPress = millis();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- AquaScale Pro System Initialize ---");
  initCompileTime();

  pinMode(BTN_TARE, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_SAVE, INPUT_PULLUP);
  pinMode(OLED_CS, OUTPUT);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(OLED_CS, HIGH);
  digitalWrite(SD_CS, HIGH);

  u8g2.begin();
  Serial.println("OLED Display: Ready");

  scale.begin(HX_DT, HX_SCK);
  scale.set_scale(calibration_factor);
  scale.tare();
  Serial.println("Load Cell: Ready");

  if (SD.begin(SD_CS)) {
    sdReady = true;
    Serial.println("SD Card: Ready");
    if (!SD.exists("/fish.csv")) {
      File f = SD.open("/fish.csv", FILE_WRITE);
      if (f) {
        f.println("DateTime_TH,Fish,Weight_g,Length_cm");
        f.close();
        Serial.println("Created new fish.csv file.");
      }
    }
  } else {
    Serial.println("SD Card: Failed!");
  }
  Serial.println("System Ready. Please select fish on device.");
}

void loop() {
  if (millis() - lastRead >= 50) {
    lastRead = millis();
    float raw = scale.get_units(1);
    if (isnan(raw)) raw = 0;
    filteredWeight = (raw * 0.4f) + (filteredWeight * 0.6f);
    if (filteredWeight < 0.8f) filteredWeight = 0;

    if (abs(filteredWeight - lastWeight) < STABLE_DIFF) {
      if (stableStart == 0) stableStart = millis();
      if (millis() - stableStart > STABLE_TIME) weightStable = true;
    } else {
      stableStart = 0;
      weightStable = false;
    }
    lastWeight = filteredWeight;
  }

  autoResetWhenEmpty();
  updateConfirm();
  if (millis() - lastDisplay >= 250) {
    lastDisplay = millis();
    drawOLED();
  }
  handleButtons();
}