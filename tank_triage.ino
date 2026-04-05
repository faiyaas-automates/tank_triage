// Tank Water Triage System — ESP32 WROOM-32 30-pin
// Arduino IDE 2.3 | Board: ESP32 Dev Module | Baud: 115200

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

// ── Pin map ──────────────────────────────────────────────────────────────────
#define PIN_POWER  26
#define PIN_TDS    32
#define PIN_TURB   33

// ── Timing ───────────────────────────────────────────────────────────────────
#define INTERVAL_MS   3000
#define WARMUP_MS      200
#define N_SAMPLES       10
#define BASELINE_N      30

// ── Anomaly thresholds ───────────────────────────────────────────────────────
#define THETA_TDS   80.0f
#define THETA_TURB   0.4f

// ── ADC reference ────────────────────────────────────────────────────────────
#define ADC_COUNTS  4095.0f
#define VREF_V         3.3f

// ── Shared sensor state — written by loop(), read by server callback ─────────
portMUX_TYPE   g_mux          = portMUX_INITIALIZER_UNLOCKED;
volatile float g_tdsppm       = 0.0f;
volatile float g_turbv        = 0.0f;
volatile float g_deltatds     = 0.0f;
volatile float g_deltaturb    = 0.0f;
volatile bool  g_anomaly      = false;
volatile int   g_label_idx    = 0;    // 0=CLEAN 1=TURBID 2=HIGH-TDS 3=BOTH
volatile float g_baselinetds  = 0.0f;
volatile float g_baselineturb = 0.0f;

static float baseline_tds  = 0.0f;
static float baseline_turb = 0.0f;

WiFiMulti      wifiMulti;
AsyncWebServer server(80);

// ── Network registration ──────────────────────────────────────────────────────
static void registerNetworks() {
  wifiMulti.addAP("YOUR_SSID_1", "YOUR_PASSWORD_1");
  wifiMulti.addAP("YOUR_SSID_2", "YOUR_PASSWORD_2");
  wifiMulti.addAP("YOUR_SSID_3", "YOUR_PASSWORD_3");
}

static void connectWiFi() {
  registerNetworks();
  Serial.print("wifi: Scanning known networks");

  unsigned long startMs = millis();
  while (wifiMulti.run(10000) != WL_CONNECTED) {
    if (millis() - startMs >= 30000) {
      Serial.println("\nwifi: No known network found in 30s. Halting.");
      while (true) delay(1000);
    }
    Serial.print(".");
  }

  Serial.printf("\nwifi: Connected to %s  IP: %s\n",
    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  MDNS.begin("tanktriage");
  Serial.println("mdns: Responding at tanktriage.local");
}

// ── Math & Reading ────────────────────────────────────────────────────────────
static float adcToPpm(float counts) {
  float v   = (counts / ADC_COUNTS) * VREF_V;
  float ppm = (133.42f * v * v * v) - (255.86f * v * v) + (857.39f * v);
  ppm *= 0.5f;
  if (ppm < 0.0f) ppm = 0.0f;
  return ppm;
}

static float adcToTurbV(float counts) {
  float v_half = (counts / ADC_COUNTS) * VREF_V;
  float v_full = v_half * 2.0f;
  return v_full;
}

static void readSensors(float &tds_val, float &turb_val) {
  digitalWrite(PIN_POWER, HIGH);
  delay(WARMUP_MS);

  long sum_tds = 0, sum_turb = 0;
  for (int i = 0; i < N_SAMPLES; i++) {
    sum_tds  += analogRead(PIN_TDS);
    sum_turb += analogRead(PIN_TURB);
    delay(10);
  }
  digitalWrite(PIN_POWER, LOW);

  tds_val  = adcToPpm((float)sum_tds  / N_SAMPLES);
  turb_val = adcToTurbV((float)sum_turb / N_SAMPLES);
}

// ── Baseline computation ──────────────────────────────────────────────────────
static void computeBaseline(float &baseTds, float &baseTurb) {
  double sum_tds  = 0.0;
  double sum_turb = 0.0;

  for (int i = 0; i < BASELINE_N; i++) {
    float t, u;
    readSensors(t, u);
    sum_tds  += t;
    sum_turb += u;
    delay(500);
  }

  baseTds  = (float)(sum_tds  / BASELINE_N);
  baseTurb = (float)(sum_turb / BASELINE_N);

  if (baseTds  < 0.01f) baseTds  = 0.01f;
  if (baseTurb < 0.01f) baseTurb = 0.01f;
}

static const char* triageLabel(bool tdsflag, bool turbflag) {
  if (!tdsflag && !turbflag) return "CLEAN";
  if ( turbflag && !tdsflag) return "TURBID";
  if ( tdsflag  && !turbflag) return "HIGH-TDS";
  return "BOTH";
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(PIN_POWER, OUTPUT);
  digitalWrite(PIN_POWER, LOW);

  analogSetAttenuation(ADC_11db);

  connectWiFi();

  if (!LittleFS.begin(true)) {
    Serial.println("lfs: Mount failed — halting.");
    while (true) delay(1000);
  }
  Serial.println("lfs: Mounted OK");

  // Baseline computed before server starts — no stale zero reads served
  Serial.println("[boot] Computing baseline ...");
  computeBaseline(baseline_tds, baseline_turb);
  Serial.printf("[boot] baseline_tds_ppm=%.1f baseline_turb_v=%.3f\n",
    baseline_tds, baseline_turb);

  // Push baseline into shared globals before first loop() cycle
  taskENTER_CRITICAL(&g_mux);
  g_baselinetds  = baseline_tds;
  g_baselineturb = baseline_turb;
  taskEXIT_CRITICAL(&g_mux);

  // Static file serving — registered before /api/data
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // JSON data endpoint — no Serial.print, no String, snapshot under critical section
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    taskENTER_CRITICAL(&g_mux);
    float snap_tds     = g_tdsppm;
    float snap_turbv   = g_turbv;
    float snap_dtds    = g_deltatds;
    float snap_dturb   = g_deltaturb;
    bool  snap_anomaly = g_anomaly;
    int   snap_idx     = g_label_idx;
    float snap_btds    = g_baselinetds;
    float snap_bturb   = g_baselineturb;
    taskEXIT_CRITICAL(&g_mux);

    const char* labels[] = {"CLEAN", "TURBID", "HIGH-TDS", "BOTH"};
    char buf[256];
    snprintf(buf, sizeof(buf),
      "{\"tdsppm\":%.1f,\"turbv\":%.3f,"
      "\"deltatds\":%.1f,\"deltaturb\":%.3f,"
      "\"anomaly\":%s,\"label\":\"%s\","
      "\"baselinetds\":%.1f,\"baselineturb\":%.3f}",
      snap_tds,   snap_turbv,
      snap_dtds,  snap_dturb,
      snap_anomaly ? "true" : "false",
      labels[snap_idx],
      snap_btds,  snap_bturb);

    request->send(200, "application/json", buf);
  });

  // Server starts only after baseline is ready and globals are initialized
  server.begin();
  Serial.printf("http: Server at http://%s/\n", WiFi.localIP().toString().c_str());
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  static unsigned long last_emit = 0;
  unsigned long now = millis();
  if (now - last_emit < INTERVAL_MS) return;
  last_emit = now;

  float tdsppm = 0.0f, turbv = 0.0f;
  readSensors(tdsppm, turbv);

  float deltatds  = tdsppm - baseline_tds;
  float deltaturb = turbv  - baseline_turb;

  bool tdsflag  = (deltatds  >  THETA_TDS  || deltatds  < -THETA_TDS);
  bool turbflag = (deltaturb >  THETA_TURB || deltaturb < -THETA_TURB);
  bool anomaly  = tdsflag || turbflag;

  const char *label = triageLabel(tdsflag, turbflag);

  taskENTER_CRITICAL(&g_mux);
  g_tdsppm       = tdsppm;
  g_turbv        = turbv;
  g_deltatds     = deltatds;
  g_deltaturb    = deltaturb;
  g_anomaly      = anomaly;
  g_label_idx    = (strcmp(label, "CLEAN")    == 0) ? 0 :
                   (strcmp(label, "TURBID")   == 0) ? 1 :
                   (strcmp(label, "HIGH-TDS") == 0) ? 2 : 3;
  g_baselinetds  = baseline_tds;
  g_baselineturb = baseline_turb;
  taskEXIT_CRITICAL(&g_mux);
}
