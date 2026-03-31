#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <Adafruit_Fingerprint.h>
#include <time.h>

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* MQTT_HOST = "u660b616.ala.eu-central-1.emqxsl.com";
const int MQTT_PORT = 8883;
const char* MQTT_USERNAME = "helmet123";
const char* MQTT_PASSWORD = "helmet123";
const char* MQTT_CLIENT_ID = "sand76";

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

const char* TOPIC_KEY = "lock/status/key";
const char* TOPIC_NFC = "lock/status/nfc";
const char* TOPIC_FINGER = "lock/status/fingerprint";
const char* TOPIC_VEIN = "lock/status/vein";
const char* TOPIC_RELAY = "lock/status/relay";
const char* TOPIC_SYSTEM = "lock/status/system";
const char* TOPIC_ENROLL_PROGRESS = "lock/enroll/progress";
const char* TOPIC_LOG_ENTRY = "lock/log/entry";
const char* TOPIC_LOG_LAST = "lock/log/last";
const char* TOPIC_CMD_ENROLL = "lock/command/enroll";

#define KEY_PIN 27
#define LED_PIN 14
#define RELAY_PIN 26
#define BUTTON_PIN 33
#define VEIN_PIN 32
#define PN532_SDA 21
#define PN532_SCL 22

Adafruit_PN532 nfc(&Wire);
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

const uint8_t ALLOWED_UID[] = {0x53, 0x40, 0x72, 0x2D};
const uint8_t ALLOWED_UID_LENGTH = 4;

bool keyPassed = false;
bool nfcPassed = false;
bool fingerprintPassed = false;
bool veinPassed = false;
bool relayUnlocked = false;
bool previousRelayUnlocked = false;
bool enrollRequested = false;
int nextFingerprintId = 1;
unsigned long lastMqttRetry = 0;
unsigned long lastPublish = 0;
unsigned long lastBlink = 0;
bool blinkState = false;

void debugLog(const String& message) {
  Serial.println(message);
}

String isoTimeNow() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) {
    return "1970-01-01T00:00:00+0000";
  }
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S%z", &timeinfo);
  return String(buffer);
}

String makeStatus(const char* text, bool ok) {
  return "{\"text\":\"" + String(text) + "\",\"ok\":" + String(ok ? "true" : "false") + ",\"ts\":\"" + isoTimeNow() + "\"}";
}

void publishRetained(const char* topic, const String& payload) {
  if (mqttClient.connected()) {
    mqttClient.publish(topic, payload.c_str(), true);
  }
}

void publishLog(const char* title, const String& message) {
  debugLog("[LOG] " + String(title) + " - " + message);
  String payload = "{\"title\":\"" + String(title) + "\",\"message\":\"" + message + "\",\"ts\":\"" + isoTimeNow() + "\"}";
  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_LOG_ENTRY, payload.c_str(), false);
    mqttClient.publish(TOPIC_LOG_LAST, payload.c_str(), true);
  }
}

void publishEnroll(const char* text, const char* detail, bool ok) {
  debugLog("[ENROLL] " + String(text) + " - " + String(detail));
  String payload = "{\"text\":\"" + String(text) + "\",\"detail\":\"" + String(detail) + "\",\"ok\":" + String(ok ? "true" : "false") + ",\"ts\":\"" + isoTimeNow() + "\"}";
  publishRetained(TOPIC_ENROLL_PROGRESS, payload);
}

void publishSystem(const String& text, bool ok) {
  debugLog("[SYSTEM] " + text);
  publishRetained(TOPIC_SYSTEM, "{\"text\":\"" + text + "\",\"ok\":" + String(ok ? "true" : "false") + ",\"ts\":\"" + isoTimeNow() + "\"}");
}

void publishAllStates() {
  publishRetained(TOPIC_KEY, makeStatus(keyPassed ? "Key Unlocked" : "Key OFF / Locked", keyPassed));
  publishRetained(TOPIC_NFC, makeStatus(nfcPassed ? "Card Matched" : "Waiting / Not Matched", nfcPassed));
  publishRetained(TOPIC_FINGER, makeStatus(fingerprintPassed ? "Fingerprint Matched" : "Waiting / Not Matched", fingerprintPassed));
  publishRetained(TOPIC_VEIN, makeStatus(veinPassed ? "Vein Matched" : "Waiting / Not Matched", veinPassed));
  publishRetained(TOPIC_RELAY, makeStatus(relayUnlocked ? "Relay Active LOW - Unlocked" : "Relay HIGH - Locked", relayUnlocked));
}

void resetAuth() {
  nfcPassed = false;
  fingerprintPassed = false;
  veinPassed = false;
  relayUnlocked = false;
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(LED_PIN, LOW);
}

void blinkDenied() {
  if (millis() - lastBlink >= 200) {
    lastBlink = millis();
    blinkState = !blinkState;
    digitalWrite(LED_PIN, blinkState ? HIGH : LOW);
  }
}

bool uidMatched(uint8_t* uid, uint8_t uidLength) {
  if (uidLength != ALLOWED_UID_LENGTH) return false;
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] != ALLOWED_UID[i]) return false;
  }
  return true;
}

bool waitForAllowedCard(unsigned long timeoutMs) {
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  unsigned long started = millis();

  debugLog("[NFC] Waiting for authorized card");
  publishRetained(TOPIC_NFC, makeStatus("Tap NFC Card", false));
  publishSystem("Waiting for NFC card", false);

  while (millis() - started < timeoutMs) {
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
      debugLog("[NFC] Card detected");
      if (uidMatched(uid, uidLength)) {
        debugLog("[NFC] UID matched");
        publishRetained(TOPIC_NFC, makeStatus("Card Matched", true));
        publishLog("NFC", "Authorized UID matched");
        digitalWrite(LED_PIN, LOW);
        return true;
      }
      debugLog("[NFC] UID not matched");
      publishRetained(TOPIC_NFC, makeStatus("Card Not Matched", false));
      publishLog("NFC", "Unauthorized UID scanned");
      blinkDenied();
      delay(800);
    }
    mqttClient.loop();
    delay(20);
  }
  return false;
}

int getFingerprintID() {
  if (finger.getImage() != FINGERPRINT_OK) return -1;
  if (finger.image2Tz() != FINGERPRINT_OK) return -1;
  if (finger.fingerFastSearch() != FINGERPRINT_OK) return -1;
  return finger.fingerID;
}

bool waitForFingerprint(unsigned long timeoutMs) {
  unsigned long started = millis();
  debugLog("[FINGER] Waiting for fingerprint");
  publishRetained(TOPIC_FINGER, makeStatus("Place Finger", false));
  publishRetained(TOPIC_VEIN, makeStatus("Waiting for Finger/Vein", false));
  publishSystem("Waiting for fingerprint", false);

  while (millis() - started < timeoutMs) {
    int matchedId = getFingerprintID();
    if (matchedId != -1) {
      debugLog("[FINGER] Match found, ID = " + String(matchedId));
      fingerprintPassed = true;
      veinPassed = true;
      publishRetained(TOPIC_FINGER, makeStatus("Fingerprint Matched", true));
      publishRetained(TOPIC_VEIN, makeStatus("Vein Matched", true));
      publishLog("Fingerprint", "Fingerprint ID " + String(matchedId) + " matched");
      digitalWrite(LED_PIN, LOW);
      return true;
    }
    blinkDenied();
    mqttClient.loop();
    delay(60);
  }

  fingerprintPassed = false;
  veinPassed = false;
  debugLog("[FINGER] Match failed");
  publishRetained(TOPIC_FINGER, makeStatus("Fingerprint Not Matched", false));
  publishRetained(TOPIC_VEIN, makeStatus("Vein Not Matched", false));
  publishLog("Fingerprint", "Fingerprint authentication failed");
  return false;
}

void updateRelay() {
  relayUnlocked = keyPassed && nfcPassed && fingerprintPassed && veinPassed;
  if (relayUnlocked) {
    debugLog("[RELAY] Unlocking relay");
    digitalWrite(RELAY_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    publishRetained(TOPIC_RELAY, makeStatus("Relay Active LOW - Unlocked", true));
    publishSystem("All checks passed, door unlocked", true);
    if (!previousRelayUnlocked) {
      publishLog("Access", "Door unlocked");
    }
  } else {
    debugLog("[RELAY] Keeping relay locked");
    digitalWrite(RELAY_PIN, HIGH);
    publishRetained(TOPIC_RELAY, makeStatus("Relay HIGH - Locked", false));
    publishSystem("One or more checks are red", false);
    if (previousRelayUnlocked) {
      publishLog("Access", "Door locked");
    }
  }
  previousRelayUnlocked = relayUnlocked;
}

void enrollFingerprint(int id) {
  publishLog("Enrollment", "Enrollment started for ID " + String(id));
  publishEnroll("Started", "Key is OFF. Enrollment started.", true);

  publishEnroll("Place Finger", "Place the finger on the sensor.", false);
  while (finger.getImage() != FINGERPRINT_OK) {
    mqttClient.loop();
    delay(50);
  }
  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    publishEnroll("Failed", "First image conversion failed.", false);
    return;
  }

  publishEnroll("Remove Finger", "Remove the finger from the sensor.", false);
  delay(2000);
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    mqttClient.loop();
    delay(50);
  }

  publishEnroll("Place Again", "Place the same finger again.", false);
  while (finger.getImage() != FINGERPRINT_OK) {
    mqttClient.loop();
    delay(50);
  }
  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    publishEnroll("Failed", "Second image conversion failed.", false);
    return;
  }

  if (finger.createModel() != FINGERPRINT_OK) {
    publishEnroll("Failed", "Fingerprints did not match.", false);
    return;
  }

  if (finger.storeModel(id) != FINGERPRINT_OK) {
    publishEnroll("Failed", "Could not store fingerprint template.", false);
    return;
  }

  publishEnroll("Success", "Successfully enrolled fingerprint.", true);
  publishLog("Enrollment", "Fingerprint stored with ID " + String(id));
  nextFingerprintId++;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String incoming;
  for (unsigned int i = 0; i < length; i++) {
    incoming += (char)payload[i];
  }

  debugLog("[MQTT] Topic: " + String(topic) + " Payload: " + incoming);

  if (String(topic) == TOPIC_CMD_ENROLL) {
    if (!keyPassed) {
      enrollRequested = true;
      publishLog("Command", "Enrollment command accepted");
    } else {
      publishEnroll("Rejected", "Turn key OFF before enrollment.", false);
      publishLog("Command", "Enrollment rejected because key is ON");
    }
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  debugLog("[WIFI] Connecting...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  debugLog("[WIFI] Connected");
  Serial.print("[WIFI] ");
  Serial.println(WiFi.localIP());
}

bool connectMqtt() {
  if (mqttClient.connected()) return true;
  debugLog("[MQTT] Connecting...");
  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
    debugLog("[MQTT] Connected");
    mqttClient.subscribe(TOPIC_CMD_ENROLL, 1);
    publishSystem("ESP32 connected to EMQX", true);
    publishLog("System", "MQTT connected");
    publishAllStates();
    return true;
  }
  debugLog("[MQTT] Connection failed");
  return false;
}

void setup() {
  Serial.begin(9600);
  debugLog("=== SMART LOCK BOOT ===");
  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(VEIN_PIN, INPUT_PULLUP);
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(PN532_SDA, PN532_SCL);
  nfc.begin();
  if (nfc.getFirmwareVersion()) {
    debugLog("[PN532] Detected and ready");
    nfc.SAMConfig();
  } else {
    debugLog("[PN532] Not detected");
  }

  fingerSerial.begin(57600, SERIAL_8N1, 16, 17);
  finger.begin(57600);
  if (finger.verifyPassword()) {
    debugLog("[FINGER] Sensor ready");
    finger.getTemplateCount();
    nextFingerprintId = finger.templateCount + 1;
    debugLog("[FINGER] Next template ID: " + String(nextFingerprintId));
  } else {
    debugLog("[FINGER] Sensor not detected");
  }

  connectWiFi();
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  secureClient.setInsecure();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  resetAuth();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqttClient.connected()) {
    if (millis() - lastMqttRetry > 5000) {
      lastMqttRetry = millis();
      connectMqtt();
    }
  } else {
    mqttClient.loop();
  }

  if (digitalRead(BUTTON_PIN) == LOW && !keyPassed) {
    debugLog("[BUTTON] Enrollment button pressed");
    enrollRequested = true;
    delay(300);
  }

  keyPassed = digitalRead(KEY_PIN) == LOW;
  debugLog(String("[KEY] State: ") + (keyPassed ? "ON / Unlocked" : "OFF / Locked"));
  publishRetained(TOPIC_KEY, makeStatus(keyPassed ? "Key Unlocked" : "Key OFF / Locked", keyPassed));

  if (!keyPassed) {
    resetAuth();
    updateRelay();
    if (enrollRequested) {
      enrollRequested = false;
      enrollFingerprint(nextFingerprintId);
    } else {
      publishEnroll("Idle", "Waiting for enrollment command while key is OFF.", false);
    }
  } else {
    if (!nfcPassed) {
      nfcPassed = waitForAllowedCard(7000);
      if (!nfcPassed) {
        publishSystem("NFC check failed", false);
      }
    }
    if (nfcPassed && !fingerprintPassed) {
      waitForFingerprint(7000);
      if (!fingerprintPassed) {
        publishSystem("Fingerprint/vein check failed", false);
      }
    }
    updateRelay();
  }

  if (mqttClient.connected() && millis() - lastPublish > 2000) {
    lastPublish = millis();
    publishAllStates();
  }

  delay(40);
}
