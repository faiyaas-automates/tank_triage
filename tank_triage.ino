// Tank Water Triage System — ESP32 WROOM-32 30-pin
// Arduino IDE 2.3 | Board: ESP32 Dev Module | Baud: 115200

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

// ── Pin map ───────────────────────────────────────────────────────────────────
#define PIN_POWER     26    // BC547 NPN power switch (HIGH=ON)
#define PIN_TDS       32    // TDS sensor (ADC)
#define PIN_TURB      33    // Turbidity sensor (ADC, 10kΩ÷10kΩ divider)
#define PIN_BOOT_BTN   0    // Built-in BOOT button, active LOW

// ── Timing ────────────────────────────────────────────────────────────────────
#define INTERVAL_MS    3000
#define WARMUP_MS       200
#define N_SAMPLES        10
#define BASELINE_N       30

// ── Anomaly thresholds ────────────────────────────────────────────────────────
#define THETA_TDS    80.0f
#define THETA_TURB    0.4f

// ── ADC reference ─────────────────────────────────────────────────────────────
#define ADC_COUNTS  4095.0f
#define VREF_V         3.3f

// ── Network ───────────────────────────────────────────────────────────────────
#define STA_TIMEOUT_MS  20000UL
#define AP_SSID         "TankTriage"

// ── Shared sensor state — written by loop(), read by server callbacks ─────────
portMUX_TYPE   g_mux          = portMUX_INITIALIZER_UNLOCKED;
volatile float g_tdsppm       = 0.0f;
volatile float g_turbv        = 0.0f;
volatile float g_deltatds     = 0.0f;
volatile float g_deltaturb    = 0.0f;
volatile bool  g_anomaly      = false;
volatile int   g_label_idx    = 0;      // 0=CLEAN 1=TURBID 2=HIGH-TDS 3=BOTH
volatile float g_baselinetds  = 0.0f;
volatile float g_baselineturb = 0.0f;

// ── Boot-gate flags ───────────────────────────────────────────────────────────
static bool          g_monitoring_active = false;
static volatile bool g_restart_pending   = false;

// ── NVS credential buffers ────────────────────────────────────────────────────
static char g_nvs_ssid[64]     = {0};
static char g_nvs_password[64] = {0};

static float baseline_tds  = 0.0f;
static float baseline_turb = 0.0f;

AsyncWebServer server(80);

// ─────────────────────────────────────────────────────────────────────────────
// NVS helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool nvsLoadCredentials(char ssid[64], char password[64]) {
  Preferences prefs;
  ssid[0]     = '\0';
  password[0] = '\0';
  prefs.begin("wifi-cfg", true);           // read-only
  prefs.getString("ssid",     ssid,     64);
  prefs.getString("password", password, 64);
  prefs.end();
  return (ssid[0] != '\0');
}

static void nvsSaveCredentials(const char* ssid, const char* password) {
  Preferences prefs;
  prefs.begin("wifi-cfg", false);          // read-write
  prefs.putString("ssid",     ssid);
  prefs.putString("password", password);
  prefs.end();
}

// ─────────────────────────────────────────────────────────────────────────────
// Boot-button detection — samples GPIO0 (BOOT, active LOW) for 2 s at start
// ─────────────────────────────────────────────────────────────────────────────

static bool bootButtonHeld() {
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  if (digitalRead(PIN_BOOT_BTN) == HIGH) return false;  // not pressed — fast path
  delay(2000);
  return (digitalRead(PIN_BOOT_BTN) == LOW);            // true if held full 2 s
}

// ─────────────────────────────────────────────────────────────────────────────
// STA connection — returns true on success, false on timeout
// ─────────────────────────────────────────────────────────────────────────────

static bool connectSTA(const char* ssid, const char* password) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("wifi: Connecting to STA");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start >= STA_TIMEOUT_MS) {
      Serial.println("\nwifi: STA connect timeout.");
      WiFi.disconnect(true);
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\nwifi: Connected to %s  IP: %s\n",
    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  MDNS.begin("tanktriage");
  Serial.println("mdns: Responding at tanktriage.local");
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Admin Gate HTTP routes (active only in SoftAP / provisioning mode)
// ─────────────────────────────────────────────────────────────────────────────

static void registerAdminRoutes() {
  server.serveStatic("/", LittleFS, "/").setDefaultFile("admin.html");

  // GET /api/status — current gate state
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    char buf[96];
    snprintf(buf, sizeof(buf),
      "{\"stage\":\"admin-gate\",\"ip\":\"192.168.4.1\",\"ssid\":\"\"}");
    request->send(200, "application/json", buf);
  });

  // POST /api/wifi — save credentials, trigger restart
  // Body: application/x-www-form-urlencoded   ssid=…&password=…
  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    // No Serial.print inside async callbacks (RTOS task constraint)
    if (!request->hasParam("ssid", true)) {
      request->send(400, "application/json",
        "{\"ok\":false,\"message\":\"ssid required\"}");
      return;
    }
    const AsyncWebParameter* p_ssid = request->getParam("ssid", true);
    if (!p_ssid || p_ssid->value().isEmpty()) {
      request->send(400, "application/json",
        "{\"ok\":false,\"message\":\"ssid required\"}");
      return;
    }

    // Copy to char arrays — no persistent String objects in callback
    char new_ssid[64] = {0};
    char new_pass[64] = {0};
    strncpy(new_ssid, p_ssid->value().c_str(), 63);

    const AsyncWebParameter* p_pass = request->getParam("password", true);
    if (p_pass) {
      strncpy(new_pass, p_pass->value().c_str(), 63);
    }

    nvsSaveCredentials(new_ssid, new_pass);
    g_restart_pending = true;
    request->send(200, "application/json",
      "{\"ok\":true,\"message\":\"Credentials saved. Restarting.\"}");
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Admin Gate — starts SoftAP, blocks until credentials saved, then restarts
// ─────────────────────────────────────────────────────────────────────────────

static void runAdminGate() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  Serial.printf("ap: SoftAP \"%s\" started  IP: %s\n",
    AP_SSID, WiFi.softAPIP().toString().c_str());

  registerAdminRoutes();
  server.begin();
  Serial.println("http: Admin portal live at 192.168.4.1");

  while (!g_restart_pending) {
    delay(10);
  }

  delay(300);      // allow HTTP response to flush before restart
  ESP.restart();   // never returns
}

// ─────────────────────────────────────────────────────────────────────────────
// Dashboard routes (active only in MONITORING state after baseline)
// ─────────────────────────────────────────────────────────────────────────────

static void registerDashboardRoutes() {
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // GET /api/data — eight-field JSON snapshot; no Serial.print, no String
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
}

// ─────────────────────────────────────────────────────────────────────────────
// Math & sensor reading  (unchanged)
// ─────────────────────────────────────────────────────────────────────────────

static float adcToPpm(float counts) {
  float v   = (counts / ADC_COUNTS) * VREF_V;
  float ppm = (133.42f * v * v * v) - (255.86f * v * v) + (857.39f * v);
  ppm *= 0.5f;
  if (ppm < 0.0f) ppm = 0.0f;
  return ppm;
}

static float adcToTurbV(float counts) {
  float v_half = (counts / ADC_COUNTS) * VREF_V;
  return v_half * 2.0f;
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

// ─────────────────────────────────────────────────────────────────────────────
// Baseline computation  (unchanged)
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// Setup — 6-stage boot sequence
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  // ── STATE 0: POST ─────────────────────────────────────────────────────────
  Serial.begin(115200);
  Serial.println("\n[boot] POST");

  pinMode(PIN_POWER, OUTPUT);
  digitalWrite(PIN_POWER, LOW);
  analogSetAttenuation(ADC_11db);

  if (!LittleFS.begin(true)) {
    Serial.println("[post] lfs: Mount failed — halting.");
    while (true) delay(1000);
  }
  Serial.println("[post] lfs: Mounted OK");

  // ── STATE 1: GEAR UP ──────────────────────────────────────────────────────
  Serial.println("[boot] Gear Up");
  bool has_creds = nvsLoadCredentials(g_nvs_ssid, g_nvs_password);
  bool force_ap  = bootButtonHeld();

  if (!has_creds || force_ap) {
    if (!has_creds) Serial.println("[boot] No NVS credentials — Admin Gate");
    if ( force_ap)  Serial.println("[boot] Boot button held  — Admin Gate");
    // ── STATE 2: ADMIN GATE ───────────────────────────────────────────────
    runAdminGate();    // does not return — calls ESP.restart()
  }

  // ── STATE 3: STA CONNECT ─────────────────────────────────────────────────
  Serial.println("[boot] STA Connect");
  if (!connectSTA(g_nvs_ssid, g_nvs_password)) {
    Serial.println("[boot] STA failed — Admin Gate fallback");
    runAdminGate();    // does not return
  }

  // ── STATE 4: BASELINE ─────────────────────────────────────────────────────
  Serial.println("[boot] Baseline — computing 30 samples ...");
  computeBaseline(baseline_tds, baseline_turb);
  Serial.printf("[boot] baseline_tds_ppm=%.1f  baseline_turb_v=%.3f\n",
    baseline_tds, baseline_turb);

  taskENTER_CRITICAL(&g_mux);
  g_baselinetds  = baseline_tds;
  g_baselineturb = baseline_turb;
  taskEXIT_CRITICAL(&g_mux);

  // ── STATE 5: MONITORING ───────────────────────────────────────────────────
  registerDashboardRoutes();
  server.begin();
  Serial.printf("[boot] http: Dashboard at http://%s/\n",
    WiFi.localIP().toString().c_str());
  g_monitoring_active = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop — sensor read + triage (no-op until MONITORING state is active)
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
  if (!g_monitoring_active) return;

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
