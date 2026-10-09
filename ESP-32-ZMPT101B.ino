// ESP32 + ZMPT101B: VRMS measurement
const int ADC_PIN = 34;              // ADC1 pin (works with Wi-Fi)
const int SAMPLE_US = 250;           // ~4 kHz sampling
const int WINDOW_MS = 100;           // 5 cycles @ 50 Hz
float CAL_FACTOR = 0.414;            // calibrate against a trusted multimeter
// การ Calibrate CAL_FACTOR = (VRMSจริง/VRMSที่ESP32อ่าน)*CAL_FACTOR

float readVrms() {
  double sum = 0, sumSq = 0;
  int n = 0;
  uint32_t start = millis();
  uint32_t next = micros();

  while (millis() - start < WINDOW_MS) {
    while ((int32_t)(micros() - next) < 0) {}   // wait for next sample slot
    next += SAMPLE_US;
    float mv = analogReadMilliVolts(ADC_PIN);   // uses factory calibration
    sum += mv;
    sumSq += mv * mv;
    n++;
  }

  double mean = sum / n;                        // DC offset
  double variance = sumSq / n - mean * mean;    // AC power
  if (variance < 0) variance = 0;
  return sqrt(variance) * CAL_FACTOR;           // mV (sensor side) * factor -> volts
}

void setup() {
  Serial.begin(115200);
  analogSetPinAttenuation(ADC_PIN, ADC_11db);
}

void loop() {
  float vrms = readVrms();
  Serial.printf("VRMS: %.1f V\n", vrms);
}
