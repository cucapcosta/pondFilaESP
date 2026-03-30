// Firmware ESP32 Arduino — HC-SR04 (presença) + NTC (temperatura)
// WiFi com máquina de estados, fila circular, HTTP POST com retry, timer de hardware

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>

// Configuração
#define WIFI_SSID        "Wokwi-GUEST"
#define WIFI_PASS        ""
#define SERVER_URL       "http://192.168.1.100:3420/add"
#define SENSOR_ID        "ESP32-001"

#define TRIG_PIN         5
#define ECHO_PIN         18
#define NTC_PIN          34
#define NTC_BETA         3950.0f

#define DIST_LIMITE      30.0f
#define DEBOUNCE_COUNT   3
#define TRIGGER_INTERVAL 250
#define ADC_WINDOW       5
#define QUEUE_SIZE       10
#define HTTP_RETRIES     3
#define WIFI_MAX_RETRY   10

// Tipos
typedef struct {
  char  tipo[16];
  char  unidade[8];
  float valor;
  bool  discreto;
} leitura_t;

enum WifiState { WF_DISCONNECTED, WF_CONNECTING, WF_CONNECTED };

// ISR vars — HC-SR04
static volatile unsigned long echo_start = 0;
static volatile unsigned long echo_end   = 0;
static volatile bool          echo_done  = false;

// ISR var — Timer de hardware
static volatile bool timer_flag = false;
static hw_timer_t   *hw_tmr    = NULL;

// ISR: captura micros() nas bordas do ECHO
void IRAM_ATTR echo_isr() {
  if (digitalRead(ECHO_PIN) == HIGH) {
    echo_start = micros();
  } else {
    echo_end  = micros();
    echo_done = true;
  }
}

// ISR: seta flag para leitura de temperatura
void IRAM_ATTR timer_isr() {
  timer_flag = true;
}

// ---- Ring Buffer (fila circular) ----
static leitura_t rb_buf[QUEUE_SIZE];
static volatile int rb_head  = 0;
static volatile int rb_tail  = 0;
static volatile int rb_count = 0;

bool rb_push(const leitura_t *item) {
  if (rb_count >= QUEUE_SIZE) {
    Serial.println("[FILA] Buffer cheio, descartando mais antigo");
    rb_tail = (rb_tail + 1) % QUEUE_SIZE;
    rb_count--;
  }
  rb_buf[rb_head] = *item;
  rb_head = (rb_head + 1) % QUEUE_SIZE;
  rb_count++;
  return true;
}

bool rb_pop(leitura_t *item) {
  if (rb_count <= 0) return false;
  *item = rb_buf[rb_tail];
  rb_tail = (rb_tail + 1) % QUEUE_SIZE;
  rb_count--;
  return true;
}

// ---- Média Móvel (NTC) ----
static float mm_buf[ADC_WINDOW];
static int   mm_idx  = 0;
static bool  mm_full = false;

float media_movel(float nova) {
  mm_buf[mm_idx] = nova;
  mm_idx = (mm_idx + 1) % ADC_WINDOW;
  if (!mm_full && mm_idx == 0) mm_full = true;

  int n = mm_full ? ADC_WINDOW : mm_idx;
  if (n == 0) return nova;
  float soma = 0;
  for (int i = 0; i < n; i++) soma += mm_buf[i];
  return soma / (float)n;
}

// ---- WiFi — Máquina de Estados ----
static WifiState     wifi_state    = WF_DISCONNECTED;
static int           wifi_retries  = 0;
static unsigned long wifi_last_try = 0;

void wifi_state_machine() {
  switch (wifi_state) {
    case WF_DISCONNECTED:
      Serial.println("[WIFI] Iniciando conexao...");
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      wifi_state = WF_CONNECTING;
      wifi_retries = 0;
      wifi_last_try = millis();
      break;

    case WF_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        wifi_state = WF_CONNECTED;
        wifi_retries = 0;
        Serial.print("[WIFI] Conectado! IP: ");
        Serial.println(WiFi.localIP());
      } else {
        unsigned long backoff = 2000UL * (wifi_retries + 1);
        if (millis() - wifi_last_try > backoff) {
          wifi_retries++;
          if (wifi_retries >= WIFI_MAX_RETRY) {
            Serial.println("[WIFI] Max retries, reiniciando ciclo...");
            WiFi.disconnect();
            wifi_state = WF_DISCONNECTED;
          } else {
            Serial.printf("[WIFI] Tentativa %d/%d (backoff %lu ms)\n",
                          wifi_retries, WIFI_MAX_RETRY, backoff);
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            wifi_last_try = millis();
          }
        }
      }
      break;

    case WF_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Conexao perdida!");
        wifi_state = WF_DISCONNECTED;
      }
      break;
  }
}

// ---- NTP ----
void ntp_init() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[NTP] Sincronizando");
  int tries = 0;
  struct tm ti;
  while (!getLocalTime(&ti) && tries < 15) {
    Serial.print(".");
    delay(1000);
    tries++;
  }
  Serial.println(tries < 15 ? " OK!" : " falhou");
}

void get_timestamp(char *buf, int len) {
  struct tm ti;
  if (getLocalTime(&ti))
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &ti);
  else
    snprintf(buf, len, "1970-01-01T00:00:00Z");
}

// ---- NTC — Temperatura via ADC + fórmula Beta ----
float ler_temperatura() {
  int raw = analogRead(NTC_PIN);
  if (raw <= 0) raw = 1;

  float r_ntc = 10000.0f * raw / (4095.0f - raw);
  float temp = 1.0f / (logf(r_ntc / 10000.0f) / NTC_BETA + 1.0f / 298.15f) - 273.15f;
  return media_movel(temp);
}

// ---- HTTP POST com retry ----
void enviar_http(const leitura_t *item) {
  if (wifi_state != WF_CONNECTED) {
    Serial.println("[HTTP] Sem WiFi, mantendo no buffer");
    return;
  }

  char ts[30];
  get_timestamp(ts, sizeof(ts));

  JsonDocument doc;
  doc["SensorID"]   = SENSOR_ID;
  doc["Timestamp"]  = ts;
  doc["Type"]       = item->tipo;
  doc["Unit"]       = item->unidade;
  doc["IsDiscrete"] = item->discreto;
  doc["Value"]      = item->valor;

  String body;
  serializeJson(doc, body);
  Serial.printf("[HTTP] POST -> %s\n", body.c_str());

  for (int i = 0; i < HTTP_RETRIES; i++) {
    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    int code = http.POST(body);
    http.end();

    if (code > 0) {
      Serial.printf("[HTTP] Resposta: %d\n", code);
      return;
    }
    Serial.printf("[HTTP] Falha %d/%d (erro %d)\n", i + 1, HTTP_RETRIES, code);
    delay(1000);
  }
  Serial.println("[HTTP] Todas as tentativas falharam");
}

// ---- Setup ----
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 Sensor Firmware ===");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Conectando");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifi_state = WF_CONNECTED;
    Serial.printf("\n[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
    ntp_init();
  } else {
    wifi_state = WF_CONNECTING;
    Serial.println("\n[WIFI] Timeout, reconexao no loop");
  }

  // HC-SR04
  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(ECHO_PIN), echo_isr, CHANGE);

  // ADC 12-bit para NTC
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // Timer de hardware — 5s para leitura de temperatura
  hw_tmr = timerBegin(1000000);
  timerAttachInterrupt(hw_tmr, timer_isr);
  timerAlarm(hw_tmr, 5000000, true, 0);

  memset(mm_buf, 0, sizeof(mm_buf));
  memset(rb_buf, 0, sizeof(rb_buf));
  Serial.println("[SYS] Pronto!\n");
}

// ---- Loop ----
static bool          ultimo_estado = false;
static int           debounce_cnt  = 0;
static unsigned long last_trig     = 0;
static bool          ntp_synced    = false;

void loop() {
  // 1. WiFi state machine
  wifi_state_machine();

  if (wifi_state == WF_CONNECTED && !ntp_synced) {
    ntp_init();
    ntp_synced = true;
  }

  // 2. HC-SR04: trigger a cada 250ms, ISR mede echo
  if (millis() - last_trig >= TRIGGER_INTERVAL) {
    last_trig = millis();
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
  }

  if (echo_done) {
    echo_done = false;
    unsigned long dur = echo_end - echo_start;
    float dist = dur / 58.0f;
    bool presenca = (dist > 0 && dist < DIST_LIMITE);

    // Debounce: 3 leituras consecutivas iguais para confirmar mudança
    if (presenca != ultimo_estado) {
      debounce_cnt++;
      if (debounce_cnt >= DEBOUNCE_COUNT) {
        ultimo_estado = presenca;
        debounce_cnt = 0;
        Serial.printf("[SR04] Presenca: %s (%.1f cm)\n",
                      presenca ? "DETECTADA" : "AUSENTE", dist);
        leitura_t item;
        strncpy(item.tipo, "presence", sizeof(item.tipo));
        strncpy(item.unidade, "bool", sizeof(item.unidade));
        item.valor = presenca ? 1.0f : 0.0f;
        item.discreto = true;
        rb_push(&item);
      }
    } else {
      debounce_cnt = 0;
    }
  }

  // 3. Timer flag → ler NTC com média móvel → push no ring buffer
  if (timer_flag) {
    timer_flag = false;
    float temp = ler_temperatura();
    Serial.printf("[NTC] Temperatura: %.1f C\n", temp);
    leitura_t item;
    strncpy(item.tipo, "temperature", sizeof(item.tipo));
    strncpy(item.unidade, "C", sizeof(item.unidade));
    item.valor = temp;
    item.discreto = false;
    rb_push(&item);
  }

  // 4. Consumir ring buffer → enviar HTTP POST
  leitura_t pending;
  if (rb_count > 0 && rb_pop(&pending)) {
    enviar_http(&pending);
  }

  delay(10);
}
