#include <TFT_eSPI.h>
#include "esp_adc/adc_continuous.h"

TFT_eSPI tft = TFT_eSPI();

// ---------- Pins ----------
#define PULSE_PIN   18   // pulse out, 50% duty (wire to GPIO34 for the comparison)
#define MODE_PIN    19   // HIGH = pot2 sets pulse freq, LOW = pot2 sets time base
const adc_channel_t CH_SIG  = ADC_CHANNEL_6;   // GPIO34 signal
const adc_channel_t CH_TRIG = ADC_CHANNEL_7;   // GPIO35 pot1: trigger level / edge
const adc_channel_t CH_FREQ = ADC_CHANNEL_0;   // GPIO36 pot2: pulse freq / time base

// ---------- Screen ----------
#define SCREEN_W     320
#define SCREEN_H     240
#define HEADER_H     20
#define PLOT_TOP     HEADER_H
#define PLOT_H       (SCREEN_H - HEADER_H)
#define PLOT_BOTTOM  (SCREEN_H - 1)
#define GRID_COLOR   0x39E7
#define TRACE_COLOR  TFT_GREEN   // ADC reading
#define REF_COLOR    TFT_CYAN    // commanded pulse

// ---------- Calibration (tune to your board) ----------
#define MV_PER_LSB   0.877f
#define REF_HI_RAW   ((uint16_t)(3300.0f / MV_PER_LSB))   // 3.3 V in ADC counts
#define POT_MAX      3700
#define EDGE_LO_RAW  150
#define EDGE_HI_RAW  3600

// ---------- Pulse ----------
#define PULSE_FREQ_MIN  100.0f
#define PULSE_FREQ_MAX  500000.0f
uint32_t pulseFreq   = 1000;   // requested
uint32_t pulseActual = 1000;   // what LEDC really configured

// ---------- Time base ----------
struct TB { uint32_t fs; uint16_t n; };   // n = samples shown across the screen
const TB tbs[] = {
  {1000000, 40}, {1000000, 80}, {1000000, 160}, {1000000, 320},
  {500000, 320}, {250000, 320}, {100000, 320}, {50000, 320},
  {20000, 200}
};
#define NTB      9
#define SEARCH   2048
int tbIdx = 6;

// ---------- Trigger ----------
bool     triggerRising = true;
uint16_t triggerLevel  = 2048;   // raw ADC counts
uint16_t triggerHyst   = 60;

// ---------- Buffers / state ----------
uint16_t cap[SEARCH + SCREEN_W + 1];
uint16_t samples[SCREEN_W];
int16_t  yNew[SCREEN_W], yOld[SCREEN_W];
int16_t  yRef[SCREEN_W], yRefOld[SCREEN_W];
bool     firstFrame = true;
bool     triggered = false, refOnOld = false;
float    trigOffset = 0;         // samples from trigger crossing to first plotted sample
int      markerYOld = -1;
uint16_t potT = 2048, potF = 0;

static uint8_t dmaBuf[1024];

int adcCapture(const adc_channel_t *chs, int nch, uint32_t fs, int want,
               uint32_t frame, uint16_t *vals, uint8_t *cids) {
  adc_continuous_handle_t h = NULL;
  adc_continuous_handle_cfg_t hc = {};
  hc.max_store_buf_size = 16384;
  hc.conv_frame_size = frame;
  if (adc_continuous_new_handle(&hc, &h) != ESP_OK) return 0;

  adc_digi_pattern_config_t pat[3] = {};
  for (int i = 0; i < nch; i++) {
    pat[i].atten = ADC_ATTEN_DB_12;
    pat[i].channel = chs[i];
    pat[i].unit = ADC_UNIT_1;
    pat[i].bit_width = ADC_BITWIDTH_12;
  }
  adc_continuous_config_t dc = {};
  dc.pattern_num = nch;
  dc.adc_pattern = pat;
  dc.sample_freq_hz = fs;
  dc.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  dc.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;

  if (adc_continuous_config(h, &dc) != ESP_OK || adc_continuous_start(h) != ESP_OK) {
    adc_continuous_deinit(h);
    return 0;
  }

  int got = 0, skip = 2, timeouts = 0;
  while (got < want && timeouts < 10) {
    uint32_t len = 0;
    if (adc_continuous_read(h, dmaBuf, frame, &len, 100) != ESP_OK) { timeouts++; continue; }
    if (skip > 0) { skip--; continue; }
    for (uint32_t i = 0; i + sizeof(adc_digi_output_data_t) <= len && got < want;
         i += sizeof(adc_digi_output_data_t)) {
      adc_digi_output_data_t *p = (adc_digi_output_data_t *)&dmaBuf[i];
      vals[got] = p->type1.data;
      if (cids) cids[got] = p->type1.channel;
      got++;
    }
  }
  adc_continuous_stop(h);
  adc_continuous_deinit(h);
  return got;
}

void readPots() {
  static uint16_t v[200];
  static uint8_t  c[200];
  const adc_channel_t chs[2] = {CH_TRIG, CH_FREQ};
  int n = adcCapture(chs, 2, 100000, 200, 256, v, c);
  uint32_t s0 = 0, s1 = 0, n0 = 0, n1 = 0;
  for (int i = 0; i < n; i++) {
    if (c[i] == CH_TRIG) { s0 += v[i]; n0++; }
    else if (c[i] == CH_FREQ) { s1 += v[i]; n1++; }
  }
  if (n0) potT = s0 / n0;
  if (n1) potF = s1 / n1;
}

int valueToY(uint16_t v) {
  if (v > 4095) v = 4095;
  return PLOT_BOTTOM - (int)((uint32_t)v * (PLOT_H - 1) / 4095);
}

void setPulseFreq(uint32_t f) {
  int bits = 10;
  while (bits > 2 && (80000000UL >> bits) < f) bits--;
  uint32_t a = ledcChangeFrequency(PULSE_PIN, f, bits);
  pulseActual = a ? a : f;
  ledcWrite(PULSE_PIN, 1 << (bits - 1));           // exactly 50%
}

void updateControls() {
  readPots();

  if (potT <= EDGE_LO_RAW)      triggerRising = false;
  else if (potT >= EDGE_HI_RAW) triggerRising = true;
  triggerLevel = constrain(potT, triggerHyst + 1, 4095 - triggerHyst - 1);

  float x = constrain(potF / (float)POT_MAX, 0.0f, 1.0f);
  if (digitalRead(MODE_PIN)) {
    float f = PULSE_FREQ_MIN * powf(PULSE_FREQ_MAX / PULSE_FREQ_MIN, x);
    if (fabsf(f - (float)pulseFreq) > pulseFreq * 0.02f) {
      pulseFreq = (uint32_t)f;
      setPulseFreq(pulseFreq);
    }
  } else {
    float pos = x * NTB;
    int idx = constrain((int)pos, 0, NTB - 1);
    if (fabsf(pos - (tbIdx + 0.5f)) > 0.65f) tbIdx = idx;
  }
}

void captureFrame() {
  const TB &tb = tbs[tbIdx];
  int need = tb.n + 1;
  triggered = false;
  trigOffset = 0;
  int got = adcCapture(&CH_SIG, 1, tb.fs, SEARCH + need, 1024, cap, nullptr);
  if (got < need) {
    for (int i = 0; i < SCREEN_W; i++) samples[i] = 0;
    return;
  }

  int st = 0;
  int limit = min(got - need, SEARCH);
  uint16_t lo = (triggerLevel > triggerHyst) ? triggerLevel - triggerHyst : 0;
  uint16_t hi = (triggerLevel + triggerHyst < 4095) ? triggerLevel + triggerHyst : 4095;
  bool armed = false;
  for (int i = 0; i < limit; i++) {
    uint16_t v = cap[i];
    if (triggerRising) {
      if (!armed) { if (v < lo) armed = true; }
      else if (v >= triggerLevel) { st = i; triggered = true; break; }
    } else {
      if (!armed) { if (v > hi) armed = true; }
      else if (v <= triggerLevel) { st = i; triggered = true; break; }
    }
  }

  // sub-sample position of the crossing (between st-1 and st)
  if (triggered) {
    float a = cap[st - 1], b = cap[st];
    float frac = (b != a) ? ((float)triggerLevel - a) / (b - a) : 1.0f;
    frac = constrain(frac, 0.0f, 1.0f);
    trigOffset = 1.0f - frac;
  }

  // stretch tb.n samples over the 320 columns (linear interpolation)
  for (int x = 0; x < SCREEN_W; x++) {
    float pos = x * (tb.n - 1) / (float)(SCREEN_W - 1);
    int i0 = (int)pos;
    float fr = pos - i0;
    float a = cap[st + i0], b = cap[st + i0 + 1];
    samples[x] = (uint16_t)(a + (b - a) * fr + 0.5f);
  }
}

// Ideal 50% square wave of the commanded pulse, phase-locked to the trigger crossing
void computeRef() {
  const TB &tb = tbs[tbIdx];
  float cyclesPerSample = (float)pulseActual / (float)tb.fs;
  for (int x = 0; x < SCREEN_W; x++) {
    float pos = x * (tb.n - 1) / (float)(SCREEN_W - 1);
    float ph = fmodf((pos + trigOffset) * cyclesPerSample, 1.0f);
    bool high = triggerRising ? (ph < 0.5f) : (ph >= 0.5f);
    yRef[x] = valueToY(high ? REF_HI_RAW : 0);
  }
}

void drawGrid() {
  for (int i = 0; i <= 10; i++) {
    int x = i * (SCREEN_W - 1) / 10;
    tft.drawFastVLine(x, PLOT_TOP, PLOT_H, GRID_COLOR);
  }
  for (int j = 0; j <= 8; j++) {
    int y = PLOT_TOP + j * (PLOT_H - 1) / 8;
    tft.drawFastHLine(0, y, SCREEN_W, GRID_COLOR);
  }
}

void eraseTriggerMarker() {
  if (markerYOld < 0) return;
  tft.drawFastHLine(0, markerYOld, SCREEN_W, TFT_BLACK);
  int y0 = max(markerYOld - 7, PLOT_TOP);
  int y1 = min(markerYOld + 7, PLOT_BOTTOM);
  tft.fillRect(0, y0, 10, y1 - y0 + 1, TFT_BLACK);
}

void drawTriggerMarker() {
  int y = valueToY(triggerLevel);
  tft.drawFastHLine(0, y, SCREEN_W, TFT_RED);
  if (triggerRising) tft.drawLine(2, y + 6, 8, y - 6, TFT_RED);
  else               tft.drawLine(2, y - 6, 8, y + 6, TFT_RED);
  markerYOld = y;
}

void drawHeader(uint16_t vmin, uint16_t vmax) {
  const TB &tb = tbs[tbIdx];
  float winUs = 1e6f * tb.n / tb.fs;
  char w[16];
  if (winUs < 1000) snprintf(w, sizeof(w), "%.0fus", winUs);
  else              snprintf(w, sizeof(w), "%.1fms", winUs / 1000.0f);

  char buf[80];
  snprintf(buf, sizeof(buf), "Vpp%.2f Trig%.2f%c %luHz %s [%c]%s",
           (vmax - vmin) * MV_PER_LSB / 1000.0f,
           triggerLevel * MV_PER_LSB / 1000.0f,
           triggerRising ? 'R' : 'F',
           (unsigned long)pulseActual, w,
           digitalRead(MODE_PIN) ? 'P' : 'T',
           triggered ? "" : " NT");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextPadding(SCREEN_W);
  tft.drawString(buf, 4, 2, 2);
}

void setup() {
  Serial.begin(115200);
  pinMode(MODE_PIN, INPUT_PULLDOWN);

  ledcAttach(PULSE_PIN, pulseFreq, 10);
  setPulseFreq(pulseFreq);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  drawGrid();
}

void loop() {
  updateControls();
  captureFrame();

  bool refOn = triggered;           // reference only means something when triggered
  if (refOn) computeRef();

  uint16_t vmin = 4095, vmax = 0;
  for (int i = 0; i < SCREEN_W; i++) {
    yNew[i] = valueToY(samples[i]);
    if (samples[i] < vmin) vmin = samples[i];
    if (samples[i] > vmax) vmax = samples[i];
  }

  if (!firstFrame) {
    for (int x = 1; x < SCREEN_W; x++)
      tft.drawLine(x - 1, yOld[x - 1], x, yOld[x], TFT_BLACK);
    if (refOnOld)
      for (int x = 1; x < SCREEN_W; x++)
        tft.drawLine(x - 1, yRefOld[x - 1], x, yRefOld[x], TFT_BLACK);
  }
  eraseTriggerMarker();
  drawGrid();
  drawTriggerMarker();

  if (refOn)
    for (int x = 1; x < SCREEN_W; x++)
      tft.drawLine(x - 1, yRef[x - 1], x, yRef[x], REF_COLOR);
  for (int x = 1; x < SCREEN_W; x++)
    tft.drawLine(x - 1, yNew[x - 1], x, yNew[x], TRACE_COLOR);

  memcpy(yOld, yNew, sizeof(yNew));
  memcpy(yRefOld, yRef, sizeof(yRef));
  refOnOld = refOn;
  firstFrame = false;
  drawHeader(vmin, vmax);
}
