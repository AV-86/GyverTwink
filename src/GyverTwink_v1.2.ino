/*
  Скетч к проекту "GyverTwink"
  - Страница проекта (схемы, описания): https://alexgyver.ru/gyvertwink/
  - Исходники на GitHub: https://github.com/AlexGyver/GyverTwink
  Проблемы с загрузкой? Читай гайд для новичков: https://alexgyver.ru/arduino-first/
  AlexGyver, AlexGyver Technologies, 2021
*/

/*
  1.1 - исправлена калибровка больше 255 светодиодов
  1.2 - исправлена ошибка с калибровкой
*/

/*
  Мигает синим - открыт портал
  Мигает жёлтым - подключаемся к точке
  Мигнул зелёным - подключился к точке
  Мигнул красным - ошибка подключения к точке
  Мигнул розовым - создал точку
*/

// ================== LIBS ==================
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <SimplePortal.h>
#include <FastLED.h>
#include <EEManager.h>
#include <EncButton.h>
#include "palettes.h"
#include "Timer.h"
#include <AsyncMqttClient.h>
#include <Ticker.h>

// ================ НАСТРОЙКИ ================
#define BTN_PIN D3      // пин кнопки
#define BTN_TOUCH 0     // 1 - сенсорная кнопка, 0 - нет

#define LED_PIN D1      // пин ленты
#define LED_TYPE WS2812 // чип ленты
#define LED_ORDER GRB   // порядок цветов ленты
#define LED_MAX 200     // макс. светодиодов

// имя точки в режиме AP
#define GT_AP_SSID "GyverTwink"
#define GT_AP_PASS "12345678"
//#define DEBUG_SERIAL_GT   // раскомментируй, чтобы включить отладку

#define LED_BUILTINN D4

// ================== OBJECTS ==================
WiFiServer server(80);
WiFiUDP udp;
EEManager EEwifi(portalCfg);
CRGB leds[LED_MAX];
CLEDController *strip;
EncButton<EB_TICK, BTN_PIN> btn;
IPAddress myIP;

AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
Ticker wifiReconnectTimer;

// ================== EEPROM BLOCKS ==================
struct Cfg {
  uint16_t ledAm = 50;
  bool power = 1;
  byte bright = 100;
  bool autoCh = 0;
  bool rndCh = 0;
  byte prdCh = 1;
  bool turnOff = 0;
  byte offTmr = 60;
};
Cfg cfg;
EEManager EEcfg(cfg);

byte xy[LED_MAX][2];
EEManager EExy(xy);

struct MM {
  byte minY = 0;
  byte maxY = 255;
  byte minX = 0;
  byte maxX = 255;
  byte w = 255;
  byte h = 255;
};
MM mm;
EEManager EEmm(mm);

#define ACTIVE_PALETTES 11
struct Effects {
  bool fav = true;
  byte scale = 50;
  byte speed = 150;
};
Effects effs[ACTIVE_PALETTES * 2];
EEManager EEeff(effs);

// ================== MISC DATA ==================
Timer forceTmr(30000, false);
Timer switchTmr(0, false);
Timer offTmr(60000, false);
bool calibF = false;
byte curEff = 0;
byte forceEff = 0;

bool RemoteOnOffChanged = false;
bool RemoteOnOffState = false;

#ifdef DEBUG_SERIAL_GT
#define DEBUGLN(x) Serial.println(x)
#define DEBUG(x) Serial.print(x)
#else
#define DEBUGLN(x)
#define DEBUG(x)
#endif

// ================== SETUP ==================

void connectToMqtt() {
  Serial.println("Connecting to MQTT...");
  mqttClient.connect();
}

void onWifiConnect(const WiFiEventStationModeGotIP& event) {
  Serial.println("Connected to Wi-Fi.");
  connectToMqtt();
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  Serial.println("Disconnected from Wi-Fi.");
  mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  wifiReconnectTimer.once(2, connectToWifi);
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);
  uint16_t packetIdSub = mqttClient.subscribe("yolka/onoff", 2);
  Serial.print("Subscribing at QoS 2, packetId: ");
  Serial.println(packetIdSub);


  mqttClient.publish("test/lol", 0, true, "test 1");
  Serial.println("Publishing at QoS 0");
  
  uint16_t packetIdPub1 = mqttClient.publish("test/lol", 1, true, "test 2");
  Serial.print("Publishing at QoS 1, packetId: ");
  Serial.println(packetIdPub1);

  uint16_t packetIdPub2 = mqttClient.publish("test/lol", 2, true, "test 3");
  Serial.print("Publishing at QoS 2, packetId: ");
  Serial.println(packetIdPub2);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");

  if (WiFi.isConnected()) {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  Serial.println("Subscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
  Serial.print("  qos: ");
  Serial.println(qos);
}

void onMqttUnsubscribe(uint16_t packetId) {
  Serial.println("Unsubscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  Serial.println("Publish received.");
  Serial.print("  topic: ");
  Serial.println(topic);
  Serial.print("  qos: ");
  Serial.println(properties.qos);
  Serial.print("  dup: ");
  Serial.println(properties.dup);
  Serial.print("  retain: ");
  Serial.println(properties.retain);
  Serial.print("  len: ");
  Serial.println(len);
  Serial.print("  index: ");
  Serial.println(index);
  Serial.print("  total: ");
  Serial.println(total);

  Serial.print("  payload: ");
  Serial.println(payload);

  RemoteOnOffChanged = true;  
  if('0' == payload[0]){RemoteOnOffState = false;}
  if('1' == payload[0]){RemoteOnOffState = true;}
 

}

void onMqttPublish(uint16_t packetId) {
  Serial.println("Publish acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}



void setup() {
#ifdef DEBUG_SERIAL_GT
  Serial.begin(115200);
  DEBUGLN();
#endif
  delay(200);
  if (BTN_TOUCH) btn.setButtonLevel(HIGH);
  startStrip();
  EEPROM.begin(2048); // с запасом!

  Serial.begin(115200);

  // если это первый запуск или щелчок по кнопке, открываем портал
  if (EEwifi.begin(0, 'a') || checkButton()) portalRoutine();


  wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
  wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onSubscribe(onMqttSubscribe);
  mqttClient.onUnsubscribe(onMqttUnsubscribe);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onPublish(onMqttPublish);
  
  mqttClient.setCredentials("u_P0LBPU", "k2wBcYrb");
  mqttClient.setServer("m5.wqtt.ru", 8920);


  // создаём точку или подключаемся к AP
  if (portalCfg.mode == WIFI_AP || (portalCfg.mode == WIFI_STA && portalCfg.SSID[0] == '\0')){
     setupAP();
  }else{
     connectToWifi();
  }
  
  
  DEBUGLN(myIP);

  // setupMQTT("m5.wqtt.ru", 8920, "u_P0LBPU", "k2wBcYrb")



  EEcfg.begin(EEwifi.nextAddr(), 'a');
  EEeff.begin(EEcfg.nextAddr(), 'a');
  EEmm.begin(EEeff.nextAddr(), (uint8_t)LED_MAX);
  EExy.begin(EEmm.nextAddr(), (uint8_t)LED_MAX);

  switchTmr.setPrd(cfg.prdCh * 60000ul);
  if (cfg.autoCh) switchTmr.restart();
  switchEff();
  cfg.turnOff = false;
  strip->setLeds(leds, cfg.ledAm);
  udp.begin(8888);
}

// ================== LOOP ==================
void loop() {
  button();   // опрос кнопки

  // менеджер епром
  EEcfg.tick();
  EEeff.tick();

  parsing();  // парсим udp

  // таймер принудительного показа эффектов
  if (forceTmr.ready()) {
    forceTmr.stop();
    switchEff();
  }

  // форс выключен и настало время менять эффект
  if (!forceTmr.state() && switchTmr.ready()) switchEff();

  // таймер выключения
  if (offTmr.ready()) {
    offTmr.stop();
    cfg.turnOff = false;
    cfg.power = false;
    strip->showLeds(0);
    EEcfg.update();
    DEBUGLN("Off tmr");
  }

  if(RemoteOnOffChanged){
    RemoteOnOffChanged = false;

    cfg.power = RemoteOnOffState;
    if (!cfg.power) strip->showLeds(0);
    EEcfg.update();

  }

  // показываем эффект, если включены
  if (!calibF && cfg.power) effects();

 // Wait for two seconds (to demonstrate the active low LED)
}
