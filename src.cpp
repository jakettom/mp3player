#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <arduinoFFT.h>
#include "AudioFileSourceSD.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"

// ── SD Pins ──────────────────────────────────────────────────
#define PIN_SD_SCK    18
#define PIN_SD_MISO   19
#define PIN_SD_MOSI   23
#define PIN_SD_CS      5

// ── I2S Pins ─────────────────────────────────────────────────
#define PIN_I2S_BCLK  26
#define PIN_I2S_LRC   25
#define PIN_I2S_DOUT  22

// ── OLED ─────────────────────────────────────────────────────
#define PIN_I2C_SDA   21
#define PIN_I2C_SCL   27
#define OLED_ADDR     0x3C
#define OLED_W        128
#define OLED_H         64

// ── Buttons ──────────────────────────────────────────────────
#define BTN_PLAY_PAUSE  13
#define BTN_NEXT        14
#define BTN_PREV         4
#define BTN_VOL_UP      32
#define BTN_VOL_DOWN    33

#define IDX_PLAY_PAUSE  0
#define IDX_NEXT        1
#define IDX_PREV        2
#define IDX_VOL_UP      3
#define IDX_VOL_DOWN    4
#define NUM_BUTTONS     5
#define DEBOUNCE_MS   200UL

const uint8_t BTN_PINS[NUM_BUTTONS] = {
  BTN_PLAY_PAUSE, BTN_NEXT, BTN_PREV, BTN_VOL_UP, BTN_VOL_DOWN
};

// ── FFT ──────────────────────────────────────────────────────
#define FFT_SAMPLES   64
#define SAMPLE_RATE   44100
#define DISPLAY_BINS  (FFT_SAMPLES / 2)

// ── OLED Layout ──────────────────────────────────────────────
#define STATUS_H   10
#define FFT_TOP    STATUS_H
#define FFT_HEIGHT (OLED_H - STATUS_H)
#define BAR_COUNT  DISPLAY_BINS
#define BAR_SLOT   (OLED_W / BAR_COUNT)
#define BAR_W      (BAR_SLOT - 1)

// ── Display interval cap (20 FPS) ────────────────────────────
#define DISPLAY_INTERVAL_MS 50UL

Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

AudioOutputI2S    *out  = nullptr;
AudioFileSourceSD *file = nullptr;
AudioGeneratorWAV *wav  = nullptr;

float vReal[FFT_SAMPLES];
float vImag[FFT_SAMPLES];
ArduinoFFT<float> FFT(vReal, vImag, FFT_SAMPLES, (float)SAMPLE_RATE);

float peakHold[BAR_COUNT];
const float PEAK_GRAVITY = 0.90f;

#define MAX_TRACKS 50
String trackList[MAX_TRACKS];
int    trackCount   = 0;
int    currentTrack = 0;

enum PlayerState { PLAYING, PAUSED };
PlayerState playerState = PAUSED;

float       volume   = 0.020f;
const float VOL_STEP = 0.005f;
const float VOL_MIN  = 0.005f;
const float VOL_MAX  = 0.100f;

unsigned long lastPressTime[NUM_BUTTONS] = {};
unsigned long lastDisplayTime = 0;
bool displayDirty = true;


// ── AudioOutputTap: intercepts samples for FFT, forwards to I2S
class AudioOutputTap : public AudioOutput {
public:
  float captureBuffer[FFT_SAMPLES];
  int   writeIdx    = 0;
  bool  bufferReady = false;

  explicit AudioOutputTap(AudioOutputI2S* sink) : _sink(sink) {}

  bool begin() override { return _sink->begin(); }

  bool ConsumeSample(int16_t sample[2]) override {
    captureBuffer[writeIdx++] = (float)(sample[0] + sample[1]) * 0.5f;
    if (writeIdx >= FFT_SAMPLES) {
      writeIdx    = 0;
      bufferReady = true;
    }
    return _sink->ConsumeSample(sample);
  }

  bool SetRate(int hz)    override { return _sink->SetRate(hz);    }
  bool SetChannels(int c) override { return _sink->SetChannels(c); }
  bool stop()             override { return _sink->stop();         }

private:
  AudioOutputI2S* _sink;
};

AudioOutputTap* tap = nullptr;

void   scanSDForTracks();
void   startTrack(int index);
void   stopPlayback();
bool   isPressed(int idx);
void   handleButtons();
void   runFFTAndDraw();
void   drawPausedScreen();
String fitStr(const String& s, int maxChars);
void   logStatus(const char* event);


// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] ESP32 WAV Player + FFT Visualizer");

  for (int i = 0; i < NUM_BUTTONS; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[FATAL] OLED not found.");
    while (true) delay(1000);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(12, 20); display.print("WAV + FFT Analyzer");
  display.setCursor(28, 34); display.print("Loading SD...");
  display.display();

  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("[FATAL] SD mount failed.");
    while (true) delay(1000);
  }

  // 16 DMA buffers (~23ms headroom) covers ~20ms OLED I2C transfer
  out = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S, 16);
  out->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  out->SetGain(volume);

  tap = new AudioOutputTap(out);
  memset(peakHold, 0, sizeof(peakHold));

  scanSDForTracks();
  if (trackCount == 0) {
    Serial.println("[FATAL] No .wav files found in SD root.");
    while (true) delay(1000);
  }

  startTrack(0);
}


// ── Main Loop ────────────────────────────────────────────────
void loop() {
  handleButtons();

  if (playerState == PLAYING) {
    if (wav && wav->isRunning()) {
      if (!wav->loop()) {
        // Track ended — auto-advance
        stopPlayback();
        currentTrack = (currentTrack + 1) % trackCount;
        startTrack(currentTrack);
        return;
      }
    }
    if (tap->bufferReady && (millis() - lastDisplayTime >= DISPLAY_INTERVAL_MS)) {
      runFFTAndDraw();
      tap->bufferReady = false;
      lastDisplayTime  = millis();
    }
  }

  if (playerState == PAUSED && displayDirty) {
    drawPausedScreen();
    displayDirty = false;
  }
}


// ── Scan SD root for .wav files ───────────────────────────────
void scanSDForTracks() {
  File root = SD.open("/");
  if (!root || !root.isDirectory()) return;

  trackCount = 0;
  File entry = root.openNextFile();
  while (entry && trackCount < MAX_TRACKS) {
    if (!entry.isDirectory()) {
      String name  = String(entry.name());
      String lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".wav")) {
        trackList[trackCount++] = name;
        Serial.printf("  [%02d] %s\n", trackCount - 1, name.c_str());
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  Serial.printf("[SCAN] %d track(s) found.\n", trackCount);
}


// ── Start a track by index ────────────────────────────────────
void startTrack(int index) {
  if (index < 0 || index >= trackCount) return;

  stopPlayback();

  String path = "/" + trackList[index];
  file = new AudioFileSourceSD(path.c_str());
  if (!file->isOpen()) { delete file; file = nullptr; return; }

  wav = new AudioGeneratorWAV();
  if (!wav->begin(file, tap)) {
    delete wav;  wav  = nullptr;
    delete file; file = nullptr;
    return;
  }

  currentTrack = index;
  playerState  = PLAYING;
  memset(peakHold, 0, sizeof(peakHold));
  displayDirty = true;
  logStatus("PLAYING");
}


// ── Stop playback and free per-track objects ──────────────────
void stopPlayback() {
  if (wav)  { if (wav->isRunning()) wav->stop(); delete wav;  wav  = nullptr; }
  if (file) { delete file; file = nullptr; }
}


// ── FFT + OLED spectrum draw ──────────────────────────────────
void runFFTAndDraw() {
  // Snapshot capture buffer into FFT arrays
  for (int i = 0; i < FFT_SAMPLES; i++) {
    vReal[i] = tap->captureBuffer[i];
    vImag[i] = 0.0f;
  }

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  // Fast-attack / slow-decay auto-scaler
  float framePeak = 1.0f;
  for (int i = 1; i <= DISPLAY_BINS; i++)
    if (vReal[i] > framePeak) framePeak = vReal[i];

  static float scaleRef = 1.0f;
  scaleRef = (framePeak > scaleRef)
    ? framePeak
    : scaleRef * 0.97f + framePeak * 0.03f;

  display.clearDisplay();

  for (int b = 0; b < BAR_COUNT; b++) {
    float norm = vReal[b + 1] / scaleRef;
    int   barH = (int)constrain(norm * (float)FFT_HEIGHT, 0.0f, (float)FFT_HEIGHT);

    // Peak hold: snap up instantly, decay slowly
    peakHold[b] = ((float)barH > peakHold[b]) ? (float)barH : peakHold[b] * PEAK_GRAVITY;

    int xLeft = b * BAR_SLOT;

    if (barH > 0)
      display.fillRect(xLeft, OLED_H - barH, BAR_W, barH, SSD1306_WHITE);

    int ph      = (int)peakHold[b];
    int peakRow = OLED_H - ph - 1;
    if (ph > 0 && peakRow >= FFT_TOP)
      display.drawFastHLine(xLeft, peakRow, BAR_W, SSD1306_WHITE);
  }

  // Status bar overlay
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 1);
  char numBuf[8];
  snprintf(numBuf, sizeof(numBuf), "%02d/%02d", currentTrack + 1, trackCount);
  String statusLine = String("> ") + numBuf + " ";
  statusLine += fitStr(trackList[currentTrack], 21 - (int)statusLine.length());
  display.print(statusLine);
  display.drawFastHLine(0, STATUS_H - 1, OLED_W, SSD1306_WHITE);

  display.display();
}


// ── Paused screen ─────────────────────────────────────────────
void drawPausedScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("|| PAUSED");
  display.drawFastHLine(0, 9, OLED_W, SSD1306_WHITE);

  display.setCursor(0, 12);
  char tBuf[22];
  snprintf(tBuf, sizeof(tBuf), "Track %02d / %02d", currentTrack + 1, trackCount);
  display.print(tBuf);

  display.setCursor(0, 22);
  display.print(fitStr(trackList[currentTrack], 21));

  display.drawFastHLine(0, 31, OLED_W, SSD1306_WHITE);
  display.setCursor(0, 34);
  int filled = constrain((int)((volume / VOL_MAX) * 10.0f + 0.5f), 0, 10);
  char seg[11];
  for (int i = 0; i < 10; i++) seg[i] = (i < filled) ? '#' : '-';
  seg[10] = '\0';
  char vBuf[20];
  snprintf(vBuf, sizeof(vBuf), "Vol [%s]", seg);
  display.print(vBuf);

  display.drawFastHLine(0, 44, OLED_W, SSD1306_WHITE);
  display.setCursor(4, 47);  display.print("<PRV  PLAY  NXT>");
  display.setCursor(0, 57);  display.print("VOL-");
  display.setCursor(98, 57); display.print("VOL+");

  display.display();
}


// ── Button debounce check ─────────────────────────────────────
bool isPressed(int idx) {
  if (digitalRead(BTN_PINS[idx]) == LOW) {
    unsigned long now = millis();
    if (now - lastPressTime[idx] >= DEBOUNCE_MS) {
      lastPressTime[idx] = now;
      return true;
    }
  }
  return false;
}


// ── Button state machine ──────────────────────────────────────
void handleButtons() {
  if (isPressed(IDX_PLAY_PAUSE)) {
    if (playerState == PLAYING) {
      stopPlayback();
      playerState  = PAUSED;
      displayDirty = true;
      logStatus("PAUSED");
    } else {
      startTrack(currentTrack);
    }
  }

  if (playerState == PAUSED && isPressed(IDX_NEXT)) {
    currentTrack = (currentTrack + 1) % trackCount;
    displayDirty = true;
    logStatus("SKIP >");
  }

  if (playerState == PAUSED && isPressed(IDX_PREV)) {
    currentTrack = (currentTrack - 1 + trackCount) % trackCount;
    displayDirty = true;
    logStatus("SKIP <");
  }

  if (isPressed(IDX_VOL_UP)) {
    volume = constrain(volume + VOL_STEP, VOL_MIN, VOL_MAX);
    out->SetGain(volume);
    if (playerState == PAUSED) displayDirty = true;
    Serial.printf("[VOL+] %.3f\n", volume);
  }

  if (isPressed(IDX_VOL_DOWN)) {
    volume = constrain(volume - VOL_STEP, VOL_MIN, VOL_MAX);
    out->SetGain(volume);
    if (playerState == PAUSED) displayDirty = true;
    Serial.printf("[VOL-] %.3f\n", volume);
  }
}


// ── Truncate string to maxChars, append ".." if cut ──────────
String fitStr(const String& s, int maxChars) {
  if (maxChars <= 0)               return "";
  if ((int)s.length() <= maxChars) return s;
  if (maxChars <= 2)               return s.substring(0, maxChars);
  return s.substring(0, maxChars - 2) + "..";
}


// ── Serial status log ─────────────────────────────────────────
void logStatus(const char* event) {
  Serial.printf("[%-10s] Track %02d/%02d → %s\n",
    event, currentTrack + 1, trackCount,
    trackList[currentTrack].c_str());
}
