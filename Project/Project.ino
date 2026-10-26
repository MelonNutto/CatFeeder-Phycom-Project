/*
 * Arduino UNO R4 WiFi - IoT Cat Feeder
 * Features:
 *  - Hardware Real-Time Clock (RTC.h) for precise timekeeping
 *  - Native WiFiUDP NTP Client (supports pool.ntp.org, time.google.com, time.cloudflare.com)
 *  - Web/MQTT Time Sync Fallback (syncs via MQTT if UDP 123 is blocked by mobile hotspot)
 *  - MQTT Subscribe (Topic: testtopic/iot23) for "FEED" command
 *  - Automatic Interval Timer for periodic feeding (e.g. every 4 hours)
 *  - I2C LCD (16x2) displaying current time (NTP UTC+7) and elapsed time since last feed
 *  - Servo control on pin 2 (0 deg = Open, 90 deg = Closed)
 */

#include <Wire.h>
#include <Servo.h>
#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <MQTTClient.h>
#include <LiquidCrystal_I2C.h>
#include "RTC.h"

// I2C LCD 16x2 at address 0x27 (Change to 0x3F if your module uses that address)
LiquidCrystal_I2C lcd(0x27, 16, 2);

const char WIFI_SSID[] = "iPhone";     // CHANGE TO YOUR WIFI SSID
const char WIFI_PASSWORD[] = "12345678";  // CHANGE TO YOUR WIFI PASSWORD

const char MQTT_BROKER_ADRRESS[] = "mqtt-dashboard.com";
const int MQTT_PORT = 1883;
const char MQTT_CLIENT_ID[] = "arduino-uno-r4-clienssjjajt";
const char MQTT_USERNAME[] = "";
const char MQTT_PASSWORD[] = "";

// The MQTT topic that Arduino should subscribe
const char SUBSCRIBE_TOPIC[] = "testtopic/iot23";

Servo myPointer;
const int servoControlPin = 2;

WiFiClient network;
MQTTClient mqtt = MQTTClient(256);

// Feeding timer configuration (Default: 4 hours)
unsigned long FEED_INTERVAL_MS = 4UL * 60UL * 60UL * 1000UL; 
unsigned long lastFeedTime = 0;

// NTP & Time Tracking
WiFiUDP Udp;
const unsigned int localPort = 2390;
const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];
bool timeSynced = false;
unsigned long lastLcdUpdate = 0;

// NTP servers to query (with fallbacks)
const char* ntpServers[] = {
  "time.google.com",
  "pool.ntp.org",
  "time.cloudflare.com"
};

void feedPet();
void connectToMQTT();
void messageReceived(String &topic, String &payload);
void updateLCD();
bool syncNTP();

void setup() {
  Serial.begin(9600);
  
  // Initialize hardware RTC on UNO R4
  RTC.begin();

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("IoT Cat Feeder");
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi.");

  myPointer.attach(servoControlPin);
  myPointer.write(90); // Start at closed gate position (90 deg)

  int status = WL_IDLE_STATUS;
  while (status != WL_CONNECTED) {
    Serial.print("Arduino UNO R4 - Attempting to connect to SSID: ");
    Serial.println(WIFI_SSID);
    status = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(4000);
  }

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  lcd.setCursor(0, 1);
  lcd.print("WiFi Connected! ");
  delay(1000);

  // Sync time via NTP
  lcd.setCursor(0, 1);
  lcd.print("Syncing NTP Time");
  if (syncNTP()) {
    lcd.setCursor(0, 1);
    lcd.print("NTP Synced! OK  ");
  } else {
    lcd.setCursor(0, 1);
    lcd.print("NTP Wait WebSync");
    Serial.println("NTP failed (port 123 may be blocked). Awaiting Web Dashboard sync...");
  }
  delay(1000);

  // Connect to MQTT broker
  connectToMQTT();

  lcd.clear();
}

void loop() {
  mqtt.loop();

  // Reconnect MQTT if disconnected
  if (!mqtt.connected()) {
    connectToMQTT();
  }

  // Automatic interval feeding timer (e.g. every 4 hours)
  if (lastFeedTime > 0 && (millis() - lastFeedTime >= FEED_INTERVAL_MS)) {
    Serial.println("Scheduled feeding timer triggered!");
    feedPet();
  }

  // Update LCD display every second
  updateLCD();
}

void feedPet() {
  Serial.println("Dispensing food...");

  // Show feeding notification on LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("** FEEDING... **");
  lcd.setCursor(0, 1);
  lcd.print("Enjoy Meal! =^.^=");

  myPointer.write(0);    // Open food gate (0 deg)
  delay(600);          // Wait for food to drop (1.5 seconds)
  myPointer.write(90);   // Close food gate (90 deg)

  lastFeedTime = millis();
  Serial.println("Feeding complete.");

  delay(1000);
  lcd.clear();
}

bool syncNTP() {
  Udp.begin(localPort);

  for (int s = 0; s < 3; s++) {
    Serial.print("Querying NTP server: ");
    Serial.println(ntpServers[s]);

    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;             // Stratum
    packetBuffer[2] = 6;             // Polling Interval
    packetBuffer[3] = 0xEC;          // Peer Clock Precision
    packetBuffer[12] = 49;
    packetBuffer[13] = 0x4E;
    packetBuffer[14] = 49;
    packetBuffer[15] = 52;

    Udp.beginPacket(ntpServers[s], 123);
    Udp.write(packetBuffer, NTP_PACKET_SIZE);
    Udp.endPacket();

    unsigned long startWait = millis();
    while (millis() - startWait < 1500) {
      int size = Udp.parsePacket();
      if (size >= NTP_PACKET_SIZE) {
        Udp.read(packetBuffer, NTP_PACKET_SIZE);
        unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
        unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
        unsigned long secsSince1900 = highWord << 16 | lowWord;

        // Unix time starts on Jan 1 1970 (2208988800 seconds after 1900)
        const unsigned long seventyYears = 2208988800UL;
        unsigned long epochUTC = secsSince1900 - seventyYears;

        // Thailand Timezone: UTC+7 = 25200 seconds
        unsigned long epochThailand = epochUTC + (7 * 3600);

        RTCTime timeToSet = RTCTime(epochThailand);
        RTC.setTime(timeToSet);
        timeSynced = true;

        Serial.print("NTP Sync Success! Time (UTC+7): ");
        Serial.println(epochThailand);
        Udp.stop();
        return true;
      }
      delay(50);
    }
  }

  Udp.stop();
  return false;
}

void updateLCD() {
  if (millis() - lastLcdUpdate < 1000) {
    return;
  }
  lastLcdUpdate = millis();

  char line1[17];
  if (timeSynced) {
    RTCTime currentTime;
    RTC.getTime(currentTime);
    snprintf(line1, sizeof(line1), "Time:   %02d:%02d:%02d",
             currentTime.getHour(), currentTime.getMinutes(), currentTime.getSeconds());
  } else {
    // If not yet synced, show running uptime
    unsigned long sec = millis() / 1000;
    int h = (sec / 3600) % 24;
    int m = (sec % 3600) / 60;
    int s = sec % 60;
    snprintf(line1, sizeof(line1), "Up:     %02d:%02d:%02d", h, m, s);
  }

  // Calculate Time Elapsed Since Last Feed
  char line2[17];
  if (lastFeedTime == 0) {
    snprintf(line2, sizeof(line2), "Last: Never     ");
  } else {
    unsigned long elapsed = (millis() - lastFeedTime) / 1000;
    unsigned long eh = elapsed / 3600;
    unsigned long em = (elapsed % 3600) / 60;
    unsigned long es = elapsed % 60;
    if (eh > 0) {
      snprintf(line2, sizeof(line2), "Last: %02luh %02lum ago", eh, em);
    } else {
      snprintf(line2, sizeof(line2), "Last: %02lum %02lus ago", em, es);
    }
  }

  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void connectToMQTT() {
  mqtt.begin(MQTT_BROKER_ADRRESS, MQTT_PORT, network);
  mqtt.onMessage(messageReceived);

  Serial.print("Arduino UNO R4 - Connecting to MQTT broker");

  int retries = 0;
  while (!mqtt.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD) && retries < 20) {
    Serial.print(".");
    delay(100);
    retries++;
  }
  Serial.println();

  if (!mqtt.connected()) {
    Serial.println("Arduino UNO R4 - MQTT broker Timeout!");
    return;
  }

  if (mqtt.subscribe(SUBSCRIBE_TOPIC)) {
    Serial.print("Arduino UNO R4 - Subscribed to topic: ");
  } else {
    Serial.print("Arduino UNO R4 - Failed to subscribe to topic: ");
  }
  Serial.println(SUBSCRIBE_TOPIC);
  Serial.println("Arduino UNO R4 - MQTT Connected!");
}

void messageReceived(String &topic, String &payload) {
  Serial.println("Arduino UNO R4 - received from MQTT:");
  Serial.println("- topic: " + topic);
  Serial.println("- payload: " + payload);

  // 1. Manual feed trigger
  if (payload == "FEED") {
    Serial.println("Feed command received via MQTT!");
    feedPet();
  }
  // 2. Web/MQTT Time Sync Fallback (e.g. "TIME:1729953600")
  else if (payload.startsWith("TIME:")) {
    unsigned long webEpoch = payload.substring(5).toInt();
    if (webEpoch > 1000000000UL) {
      // Apply Thailand timezone offset (+7 hours)
      unsigned long epochThailand = webEpoch + (7 * 3600);
      RTCTime timeToSet = RTCTime(epochThailand);
      RTC.setTime(timeToSet);
      timeSynced = true;
      Serial.print("RTC Time synchronized from Web Dashboard: ");
      Serial.println(epochThailand);

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Time Synced!");
      lcd.setCursor(0, 1);
      lcd.print("From Web App OK ");
      delay(800);
      lcd.clear();
    }
  }
  // 3. Change auto-feed interval (e.g. payload "TIMER:3600" for 1 hour)
  else if (payload.startsWith("TIMER:")) {
    long sec = payload.substring(6).toInt();
    if (sec > 0) {
      FEED_INTERVAL_MS = sec * 1000UL;
      Serial.print("Updated FEED_INTERVAL_MS to: ");
      Serial.println(FEED_INTERVAL_MS);
    }
  }
}
