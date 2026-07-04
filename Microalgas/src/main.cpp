// ============================================================
//  MONITOR DE MICROALGAS — DS18B20 + BH1750 + LCD16x2 + MQTT
//  Plataforma : ESP32 / PlatformIO
//  Red        : WPA2-Enterprise (PEAP) — wTEC
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wpa2.h"
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <BH1750.h>
#include <LiquidCrystal_I2C.h>

// ============================================================
//  CONFIGURACIÓN
// ============================================================

// ── WiFi WPA2-Enterprise ─────────────────────────────────────
const char* WIFI_SSID     = "wTEC"; 
const char* EAP_IDENTITY  = "usuario_tec"; 
const char* EAP_PASSWORD  = "password_tec";             

// ── Broker MQTT ───────────────────────────────────────────────
const char* MQTT_BROKER    = "broker.hivemq.com";
const int   MQTT_PORT      = 1883;
const char* MQTT_CLIENT_ID = "microalgas_esp32_001";

// ── Topics MQTT ─────────────────────────────────────────────── //Temporalmente públicos
const char* TOPIC_TEMPERATURA = "microalgas/001/cib/tec/temperatura";
const char* TOPIC_LUZ         = "microalgas/001/cib/tec/luz";
const char* TOPIC_ESTADO      = "microalgas/001/cib/tec/estado";

// ── Pines ─────────────────────────────────────────────────────
#define PIN_DS18B20  4
#define PIN_SDA      21
#define PIN_SCL      22

// ── Intervalos y timeouts ─────────────────────────────────────
const unsigned long INTERVALO_MS     = 5000;
const unsigned long MQTT_RETRY_MS    = 3000;
const int           WIFI_TIMEOUT_SEG = 30; 
const int           MQTT_MAX_FALLOS  = 5;

// ============================================================
//  OBJETOS GLOBALES
// ============================================================
WiFiClient        wifiClient;
PubSubClient      mqtt(wifiClient);
OneWire           oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);
BH1750            bh1750;
LiquidCrystal_I2C lcd(0x27, 16, 2);

unsigned long ultimaPublicacion  = 0;
unsigned long ultimoReconectMQTT = 0;
int           fallosMQTT         = 0;

// ============================================================
//  LCD — helpers
// ============================================================
void lcdLinea(uint8_t fila, const char* texto) {
  lcd.setCursor(0, fila);
  lcd.print("                ");
  lcd.setCursor(0, fila);
  lcd.print(texto);
}

void lcdError(const char* detalle) {
  lcdLinea(0, "! ERROR");
  lcdLinea(1, detalle);
}

// ============================================================
//  WIFI — WPA2-Enterprise PEAP
// ============================================================
void conectarWiFi() {
  Serial.println("\n[WiFi] Conectando a wTEC (PEAP)...");
  lcdLinea(0, "Conectando WiFi");
  lcdLinea(1, "wTEC PEAP...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

  esp_wifi_sta_wpa2_ent_set_identity(
    (uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_wifi_sta_wpa2_ent_set_username(
    (uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_wifi_sta_wpa2_ent_set_password(
    (uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));

  esp_wifi_sta_wpa2_ent_enable();
  WiFi.begin(WIFI_SSID);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < WIFI_TIMEOUT_SEG) {
    delay(1000);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] ✓ IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
    lcdLinea(0, "WiFi OK");
    lcdLinea(1, WiFi.localIP().toString().c_str());
    delay(1500);
  } else {
    Serial.println("\n[WiFi] ✗ Sin conexion. Reiniciando...");
    Serial.println("[WiFi] Verificar: credenciales o registro MAC en TI");
    lcdError("Sin WiFi wTEC");
    delay(2000);
    ESP.restart();
  }
}

// ============================================================
//  MQTT
// ============================================================
bool conectarMQTT() {
  Serial.printf("[MQTT] Conectando a %s... ", MQTT_BROKER);
  lcdLinea(0, "Conectando MQTT");
  lcdLinea(1, MQTT_BROKER);

  bool ok = mqtt.connect(
    MQTT_CLIENT_ID,
    nullptr, nullptr,
    TOPIC_ESTADO, 1, true, "offline"
  );

  if (ok) {
    mqtt.publish(TOPIC_ESTADO, "online", true);
    Serial.println("✓");
    lcdLinea(0, "MQTT OK");
    lcdLinea(1, "");
    delay(1000);
    fallosMQTT = 0;
  } else {
    Serial.printf("✗ (codigo: %d)\n", mqtt.state());
    char buf[16];
    snprintf(buf, sizeof(buf), "Fallo cod:%d", mqtt.state());
    lcdError(buf);
  }
  return ok;
}

void verificarConexiones() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Conexion perdida. Reconectando...");
    lcdError("WiFi perdido");
    conectarWiFi();
  }

  if (!mqtt.connected()) {
    unsigned long ahora = millis();
    if (ahora - ultimoReconectMQTT >= MQTT_RETRY_MS) {
      ultimoReconectMQTT = ahora;

      if (conectarMQTT()) {
        fallosMQTT = 0;
      } else {
        fallosMQTT++;
        Serial.printf("[MQTT] Fallo %d/%d\n", fallosMQTT, MQTT_MAX_FALLOS);
        char buf[16];
        snprintf(buf, sizeof(buf), "MQTT %d/%d", fallosMQTT, MQTT_MAX_FALLOS);
        lcdError(buf);
        if (fallosMQTT >= MQTT_MAX_FALLOS) {
          Serial.println("[MQTT] Reiniciando ESP32...");
          lcdError("Reiniciando...");
          delay(1500);
          ESP.restart();
        }
      }
    }
  }
}

// ============================================================
//  SENSORES
// ============================================================
float leerTemperatura() {
  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) {
    Serial.println("[DS18B20] ✗ Sensor desconectado");
    return NAN;
  }
  return t;
}

float leerLuz() {
  float lux = bh1750.readLightLevel();
  if (lux < 0) {
    Serial.println("[BH1750] ✗ Error de lectura");
    return NAN;
  }
  return lux;
}

// ============================================================
//  LCD — pantalla principal
// ============================================================
void mostrarMediciones(float temp, float luz) {
  if (isnan(temp)) {
    lcdLinea(0, "Temp: ERROR");
  } else {
    char buf[16];
    snprintf(buf, sizeof(buf), "Temp: %.1f C", temp);
    lcdLinea(0, buf);
  }

  if (isnan(luz)) {
    lcdLinea(1, "Luz: ERROR");
  } else {
    char buf[16];
    if (luz >= 10000)
      snprintf(buf, sizeof(buf), "Luz:%.1f klux", luz / 1000.0);
    else
      snprintf(buf, sizeof(buf), "Luz: %.0f lux", luz);
    lcdLinea(1, buf);
  }
}

// ============================================================
//  PUBLICACIÓN MQTT
// ============================================================
void publicarDatos(float temperatura, float luz) {
  char payload[16];

  if (!isnan(temperatura)) {
    dtostrf(temperatura, 5, 2, payload);
    mqtt.publish(TOPIC_TEMPERATURA, payload);
    Serial.printf("[MQTT] Temperatura: %s C\n", payload);
  } else {
    mqtt.publish(TOPIC_TEMPERATURA, "error");
  }

  if (!isnan(luz)) {
    dtostrf(luz, 7, 2, payload);
    mqtt.publish(TOPIC_LUZ, payload);
    Serial.printf("[MQTT] Luz: %s lux\n", payload);
  } else {
    mqtt.publish(TOPIC_LUZ, "error");
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  lcd.backlight();
  lcdLinea(0, "Iniciando...");
  lcdLinea(1, "Monitor Algas");
  delay(1500);

  if (bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("[BH1750]  ✓ Listo");
    lcdLinea(1, "BH1750 OK");
  } else {
    Serial.println("[BH1750]  ✗ Error");
    lcdError("BH1750 fallo");
    delay(2000);
  }

  ds18b20.begin();
  int sensores = ds18b20.getDeviceCount();
  Serial.printf("[DS18B20] ✓ %d sensor(es)\n", sensores);
  if (sensores == 0) {
    lcdError("DS18B20 fallo");
    delay(2000);
  } else {
    lcdLinea(1, "DS18B20 OK");
    delay(1000);
  }

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setKeepAlive(60);
  conectarWiFi();
  conectarMQTT();

  Serial.println("=== Sistema listo ===\n");
  lcdLinea(0, "Sistema listo");
  lcdLinea(1, "");
  delay(1500);
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  verificarConexiones();
  mqtt.loop();

  unsigned long ahora = millis();
  if (ahora - ultimaPublicacion >= INTERVALO_MS) {
    ultimaPublicacion = ahora;

    float temperatura = leerTemperatura();
    float luz         = leerLuz();

    mostrarMediciones(temperatura, luz);
    publicarDatos(temperatura, luz);
  }
}