// ============================================================
// Protótipo: monitoramento de pavimento (ESP32 + MPU + GPS + MQTT)
//
// - Lê o acelerômetro direto pelos registradores (aceita MPU6050 e
//   clones/MPU6500, WHO_AM_I 0x70)
// - Calcula o RMS da vibração em janelas de 1 s (100 Hz)
// - Une com posição/velocidade/hora do GPS
// - Núcleo 1 (loop): amostragem do sensor + GPS
// - Núcleo 0 (tarefa de rede): Wi-Fi + MQTT, com reconexão automática
// - Entre os dois existe uma FILA: se a internet cair, os registros
//   ficam guardados (até ~500, cerca de 8 min) e são enviados quando a
//   conexão voltar. Nada é perdido e a amostragem nunca é atrasada.
//
// Bibliotecas: TinyGPSPlus (Mikal Hart) e PubSubClient (Nick O'Leary)
//   Ferramentas > Gerenciar Bibliotecas
//
// LED único de status:
//   Aceso fixo             = aguardando GPS (sem fix, nada é gravado)
//   1 flash por segundo    = gravando E enviando para a internet
//   2 flashes por segundo  = gravando, mas SEM internet (guardando na fila)
//   Piscando rápido        = sensor MPU sem resposta (reconectando)
//
// Ligações (todas no lado do 3V3 do ESP32):
//   MPU:  SDA -> GPIO21, SCL -> GPIO22, VCC -> 3V3, GND -> GND
//   GPS:  TX do módulo -> GPIO19, VCC, GND  (RX do módulo: sem fio)
//   LED:  GPIO18 -> resistor -> LED -> GND
// ============================================================

#include <Wire.h>
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ---------------- REDE E MQTT (EDITE AQUI) ----------------
const char* WIFI_SSID  = "SEU_SSID_WIFI";
const char* WIFI_SENHA = "SUA_SENHA_WIFI";   // hotspot do celular em 2,4 GHz

const char* MQTT_HOST  = "://emqxsl.com";  // Cluster URL
const int   MQTT_PORT  = 8883;                      // TLS
const char* MQTT_USER  = "SEU_USUARIO_MQTT";
const char* MQTT_SENHA = "SUA_SENHA_MQTT";
const char* DEVICE_ID  = "esp32-01";
// Tópicos: pavimento/<DEVICE_ID>/leitura  e  pavimento/<DEVICE_ID>/status

// ---------------- PINOS ----------------
const int PIN_SDA       = 21;
const int PIN_SCL       = 22;
const int LED_STATUS    = 18;
const int PIN_GPS_RX    = 19;   // ESP32 RX <- TX do módulo GPS
const int PIN_GPS_TX    = -1;   // o ESP32 não envia nada ao GPS

// ---------------- CONFIGURAÇÃO ----------------
const float G_MS2              = 9.80665f;
const float ACEL_LSB_POR_G     = 8192.0f;       // faixa +-4 g
const int   AMOSTRAS_JANELA    = 100;           // 100 amostras = 1 s
const unsigned long PERIODO_US = 10000;         // 100 Hz
const int   FALHAS_PARA_PERDER = 20;
const int   TAM_FILA           = 500;           // registros guardados offline

// true  = MODO TESTE: bloco legível a cada segundo (com ou sem fix).
// false = MODO COLETA: só o CSV, e apenas quando há fix.
const bool MODO_TESTE = true;

// ---------------- ESTRUTURAS E GLOBAIS ----------------
struct Registro {
  uint32_t ms;
  char     ts[21];      // "2026-11-12T13:45:10Z" (hora UTC do GPS) ou vazio
  double   lat, lon;
  float    vel, rms;
  uint8_t  sats;
};

HardwareSerial SerialGPS(2);
TinyGPSPlus gps;

QueueHandle_t fila;
volatile bool wifiOk = false;
volatile bool mqttOk = false;
unsigned long descartados = 0;

WiFiClientSecure clienteTLS;
PubSubClient mqtt(clienteTLS);
char topicoLeitura[64];
char topicoStatus[64];

uint8_t enderecoMPU = 0x68;
bool sensorOk = false;
int  falhasSeguidas = 0;
bool cabecalhoImpresso = false;

unsigned long proximaAmostraUs = 0;
unsigned long ultimaTentativaMs = 0;
unsigned long ultimoStatusMs = 0;
unsigned long ultimoEnvioMs = 0;
unsigned long leituraNum = 0;

int    nJanela = 0;
double sx = 0, sy = 0, sz = 0, sxx = 0, syy = 0, szz = 0;
float  minx = 1e9, miny = 1e9, minz = 1e9, maxx = -1e9, maxy = -1e9, maxz = -1e9;

// ---------------- I2C ----------------
bool lerReg(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool escreverReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

void iniciarI2C() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);   // devagar = mais tolerante a mau contato
  Wire.setTimeOut(50);     // nunca trava esperando o sensor
}

bool iniciarSensor() {
  const uint8_t candidatos[] = {0x68, 0x69};
  for (uint8_t addr : candidatos) {
    uint8_t id;
    if (!lerReg(addr, 0x75, &id, 1)) continue;      // WHO_AM_I
    enderecoMPU = addr;
    Serial.printf("# MPU respondeu em 0x%02X, WHO_AM_I=0x%02X\n", addr, id);
    if (!escreverReg(addr, 0x6B, 0x00)) return false;  // acorda o chip
    delay(100);
    escreverReg(addr, 0x1C, 0x08);                   // acelerômetro +-4 g
    return true;
  }
  return false;
}

void zerarJanela() {
  nJanela = 0;
  sx = sy = sz = sxx = syy = szz = 0;
  minx = miny = minz = 1e9;
  maxx = maxy = maxz = -1e9;
}

// ---------------- REDE (roda no núcleo 0) ----------------
bool publicarRegistro(const Registro &r) {
  char json[256];
  int n = snprintf(json, sizeof(json),
    "{\"device\":\"%s\",\"ts\":\"%s\",\"ms\":%lu,\"lat\":%.6f,\"lon\":%.6f,"
    "\"speed_kmh\":%.1f,\"sats\":%d,\"rms\":%.3f}",
    DEVICE_ID, r.ts, (unsigned long)r.ms, r.lat, r.lon,
    r.vel, (int)r.sats, r.rms);
  if (n <= 0 || n >= (int)sizeof(json)) return true;   // malformado: descarta
  return mqtt.publish(topicoLeitura, json);
}

void tarefaRede(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  Serial.printf("# Rede: conectando em '%s'...\n", WIFI_SSID);

  clienteTLS.setInsecure();          // criptografa, mas não valida o certificado
  clienteTLS.setHandshakeTimeout(8);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
  mqtt.setBufferSize(384);

  unsigned long ultWifi = millis();
  unsigned long ultMqtt = millis() - 5000;   // 1ª tentativa imediata

  for (;;) {
    // ----- Wi-Fi -----
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiOk) Serial.println("# Rede: Wi-Fi caiu, tentando reconectar...");
      wifiOk = false;
      mqttOk = false;
      if (millis() - ultWifi >= 15000) {
        ultWifi = millis();
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_SENHA);
        Serial.println("# Rede: nova tentativa de Wi-Fi...");
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!wifiOk) {
      wifiOk = true;
      Serial.printf("# Rede: Wi-Fi OK, IP %s\n", WiFi.localIP().toString().c_str());
    }

    // ----- MQTT -----
    if (!mqtt.connected()) {
      if (mqttOk) Serial.println("# MQTT: desconectado");
      mqttOk = false;
      if (millis() - ultMqtt >= 5000) {
        ultMqtt = millis();
        char clientId[40];
        snprintf(clientId, sizeof(clientId), "pavimento-%s", DEVICE_ID);
        // Last Will: se o ESP32 sumir, o broker publica "offline" no status
        if (mqtt.connect(clientId, MQTT_USER, MQTT_SENHA,
                         topicoStatus, 0, true, "offline")) {
          mqtt.publish(topicoStatus, "online", true);
          mqttOk = true;
          Serial.println("# MQTT: conectado");
        } else {
          Serial.printf("# MQTT: falhou (codigo %d), nova tentativa em 5 s\n", mqtt.state());
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    mqttOk = true;
    mqtt.loop();

    // ----- Envia a fila (só retira depois de publicar) -----
    Registro r;
    int enviados = 0;
    while (enviados < 10 && xQueuePeek(fila, &r, 0) == pdTRUE) {
      if (!publicarRegistro(r)) break;
      xQueueReceive(fila, &r, 0);
      enviados++;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void enfileirar(const Registro &r) {
  if (xQueueSend(fila, &r, 0) != pdTRUE) descartados++;   // fila cheia
}

// ---------------- GPS E LED ----------------
bool temFix() {
  return gps.location.isValid() && gps.location.age() < 3000;
}

void atualizarLeds() {
  bool ligado;
  if (!sensorOk) {
    ligado = (millis() / 150) % 2;                        // pisca rápido
  } else if (!temFix()) {
    ligado = true;                                        // fixo: sem fix
  } else if (ultimoEnvioMs == 0) {
    ligado = false;
  } else {
    unsigned long dt = millis() - ultimoEnvioMs;
    if (mqttOk) ligado = dt < 150;                        // 1 flash
    else        ligado = dt < 100 || (dt > 250 && dt < 350);   // 2 flashes
  }
  digitalWrite(LED_STATUS, ligado ? HIGH : LOW);
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n# === Prototipo de monitoramento de pavimento ===");

  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, HIGH);
  delay(800);
  digitalWrite(LED_STATUS, LOW);

  snprintf(topicoLeitura, sizeof(topicoLeitura), "pavimento/%s/leitura", DEVICE_ID);
  snprintf(topicoStatus,  sizeof(topicoStatus),  "pavimento/%s/status",  DEVICE_ID);

  SerialGPS.setRxBufferSize(1024);   // evita perder frases do GPS
  SerialGPS.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  iniciarI2C();
  sensorOk = iniciarSensor();
  Serial.println(sensorOk ? "# Sensor OK" : "# Sensor SEM resposta (vai tentar reconectar)");

  fila = xQueueCreate(TAM_FILA, sizeof(Registro));
  // Tarefa de rede no núcleo 0; o loop() do Arduino roda no núcleo 1
  xTaskCreatePinnedToCore(tarefaRede, "rede", 12288, NULL, 1, NULL, 0);

  proximaAmostraUs = micros() + PERIODO_US;
}

// ---------------- LOOP (núcleo 1) ----------------
void loop() {
  while (SerialGPS.available()) gps.encode(SerialGPS.read());

  atualizarLeds();

  if ((!MODO_TESTE || !sensorOk) && millis() - ultimoStatusMs > 5000) {
    ultimoStatusMs = millis();
    Serial.printf("# status: sensor=%s fix=%s sats=%d wifi=%s mqtt=%s fila=%u\n",
                  sensorOk ? "OK" : "FALHA",
                  temFix() ? "SIM" : "NAO",
                  gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                  wifiOk ? "OK" : "NAO", mqttOk ? "OK" : "NAO",
                  (unsigned)uxQueueMessagesWaiting(fila));
  }

  // Sensor perdido: tenta reconectar a cada 1 s
  if (!sensorOk) {
    if (millis() - ultimaTentativaMs > 1000) {
      ultimaTentativaMs = millis();
      Wire.end();
      delay(20);
      iniciarI2C();
      if (iniciarSensor()) {
        sensorOk = true;
        falhasSeguidas = 0;
        zerarJanela();
        proximaAmostraUs = micros() + PERIODO_US;
        Serial.println("# Sensor reconectado");
      }
    }
    return;
  }

  // Amostragem a 100 Hz
  unsigned long agora = micros();
  if ((long)(agora - proximaAmostraUs) < 0) return;
  proximaAmostraUs += PERIODO_US;
  if ((long)(agora - proximaAmostraUs) > 100000) {
    proximaAmostraUs = agora + PERIODO_US;   // ressincroniza se atrasou muito
  }

  uint8_t b[6];
  if (!lerReg(enderecoMPU, 0x3B, b, 6)) {
    falhasSeguidas++;
    if (falhasSeguidas >= FALHAS_PARA_PERDER) {
      sensorOk = false;
      zerarJanela();
      Serial.println("# Sensor perdeu contato");
    }
    return;
  }
  falhasSeguidas = 0;

  float ax = (int16_t)((b[0] << 8) | b[1]) / ACEL_LSB_POR_G;   // em g
  float ay = (int16_t)((b[2] << 8) | b[3]) / ACEL_LSB_POR_G;
  float az = (int16_t)((b[4] << 8) | b[5]) / ACEL_LSB_POR_G;

  sx += ax;  sy += ay;  sz += az;
  sxx += ax * ax;  syy += ay * ay;  szz += az * az;
  if (ax < minx) minx = ax;  if (ax > maxx) maxx = ax;
  if (ay < miny) miny = ay;  if (ay > maxy) maxy = ay;
  if (az < minz) minz = az;  if (az > maxz) maxz = az;
  nJanela++;

  if (nJanela >= AMOSTRAS_JANELA) {
    // RMS sem a gravidade (subtrai a média da janela = desvio padrão por
    // eixo). Somar os três eixos evita depender da orientação da placa.
    double n = nJanela;
    double vx = sxx / n - (sx / n) * (sx / n);
    double vy = syy / n - (sy / n) * (sy / n);
    double vz = szz / n - (sz / n) * (sz / n);
    if (vx < 0) vx = 0;
    if (vy < 0) vy = 0;
    if (vz < 0) vz = 0;
    float rmsMs2 = sqrt(vx + vy + vz) * G_MS2;
    bool fix = temFix();
    int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
    float vel = gps.speed.isValid() ? gps.speed.kmph() : 0.0;

    if (MODO_TESTE) {
      float pp = fmaxf(fmaxf(maxx - minx, maxy - miny), maxz - minz) * G_MS2;
      leituraNum++;
      Serial.printf("===== Leitura #%lu | %.1f s =====\n", leituraNum, millis() / 1000.0);
      Serial.println("MPU:");
      Serial.printf("  Aceleracao media (g)  : X=%.2f  Y=%.2f  Z=%.2f\n", sx / n, sy / n, sz / n);
      Serial.printf("  RMS por eixo (m/s2)   : X=%.3f  Y=%.3f  Z=%.3f\n",
                    sqrt(vx) * G_MS2, sqrt(vy) * G_MS2, sqrt(vz) * G_MS2);
      Serial.printf("  RMS total (m/s2)      : %.3f\n", rmsMs2);
      Serial.printf("  Pico a pico (m/s2)    : %.3f\n", pp);
      if (fix) {
        Serial.printf("GPS: fix=SIM  sats=%d  lat=%.6f  lon=%.6f  vel=%.1f km/h\n",
                      sats, gps.location.lat(), gps.location.lng(), vel);
      } else {
        Serial.printf("GPS: fix=NAO  sats=%d  (sem posicao)\n", sats);
      }
      Serial.printf("Rede: wifi=%s  mqtt=%s  fila=%u  descartados=%lu\n",
                    wifiOk ? "OK" : "NAO", mqttOk ? "OK" : "NAO",
                    (unsigned)uxQueueMessagesWaiting(fila), descartados);
      Serial.printf("Gravando: %s\n", fix ? "SIM" : "NAO");
    }

    if (fix) {
      Registro reg;
      reg.ms   = millis();
      reg.lat  = gps.location.lat();
      reg.lon  = gps.location.lng();
      reg.vel  = vel;
      reg.rms  = rmsMs2;
      reg.sats = (uint8_t)sats;
      if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024) {
        snprintf(reg.ts, sizeof(reg.ts), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 gps.date.year(), gps.date.month(), gps.date.day(),
                 gps.time.hour(), gps.time.minute(), gps.time.second());
      } else {
        reg.ts[0] = '\0';
      }
      enfileirar(reg);   // a tarefa de rede envia (ou guarda até a internet voltar)

      // Cópia local em CSV pelo Serial (backup)
      if (MODO_TESTE) {
        Serial.printf("CSV (ms,lat,lon,vel_kmh,sats,rms_ms2): %lu,%.6f,%.6f,%.1f,%d,%.3f\n",
                      (unsigned long)reg.ms, reg.lat, reg.lon, vel, sats, rmsMs2);
      } else {
        if (!cabecalhoImpresso) {
          Serial.println("ms,lat,lon,vel_kmh,sats,rms_ms2");
          cabecalhoImpresso = true;
        }
        Serial.printf("%lu,%.6f,%.6f,%.1f,%d,%.3f\n",
                      (unsigned long)reg.ms, reg.lat, reg.lon, vel, sats, rmsMs2);
      }
      ultimoEnvioMs = millis();   // dispara o flash do LED
    }

    if (MODO_TESTE) Serial.println();   // linha em branco entre os blocos
    zerarJanela();
  }
}
