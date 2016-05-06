#include <ESP8266WiFi.h>

// Define these in the config.h file
//#define WIFI_SSID "yourwifi"
//#define WIFI_PASSWORD "yourpassword"
//#define INFLUX_HOSTNAME "data.example.com"
//#define INFLUX_PORT 8086
//#define INFLUX_PATH "/write?db=<database>&u=<user>&p=<pass>"
//#define WEBSERVER_USERNAME "something"
//#define WEBSERVER_PASSWORD "something"
#include "config.h"

#define DEVICE_NAME "fridge"

#define ONE_WIRE_PIN 2
#define RELAY_PIN 12
#define RED_LED_PIN 13

#define N_SENSORS 3
#define COMPRESSOR 0
#define EVAPORATOR 1
#define AMBIANT 2
byte sensorAddr[N_SENSORS][8] = {
  {0x28, 0x00, 0x03, 0x80, 0x06, 0x00, 0x00, 0x58}, // (compressorTemp)
  {0x28, 0xAE, 0x41, 0x80, 0x06, 0x00, 0x00, 0x7D}, // (evaporatorTemp)
  {0x28, 0x17, 0xA7, 0x7F, 0x06, 0x00, 0x00, 0x41}, // (internalAmbiant)
};
char * sensorNames[N_SENSORS] = {
  "compressorTemp",
  "evaporatorTemp",
  "internalAmbiant"
};


// The minimum time after shutting off the compressor
// before it is allowed to be turned on again (ms)
#define MIN_RESTART_TIME 300000

#define SETTINGS_VERSION "Kf01"
struct Settings {
  float lowPoint;
  float highPoint;
  float lowComp;
  float highComp;
} settings = {
  10., 11., 55., 50.
};


#include "libdcc/webserver.h"
#include "libdcc/onewire.h"
#include "libdcc/settings.h"
#include "libdcc/influx.h"


// Flag to indicate that a settings report should be sent to InfluxDB
// during the next loop()
bool doPostSettings = false;

bool relayState = LOW;

// Time of the last relayState change
unsigned long lastStateChange;


String formatSettings() {
  return \
    String("lowPoint=") + String(settings.lowPoint, 3) + \
    String(",highPoint=") + String(settings.highPoint, 3) + \
    String(",lowComp=") + String(settings.lowComp, 3) + \
    String(",highComp=") + String(settings.highComp, 3);
}

void handleSettings() {
  REQUIRE_AUTH;

  for (int i=0; i<server.args(); i++) {
    if (server.argName(i).equals("lowPoint")) {
      settings.lowPoint = server.arg(i).toFloat();
    } else if (server.argName(i).equals("highPoint")) {
      settings.highPoint = server.arg(i).toFloat();
    } else if (server.argName(i).equals("lowComp")) {
      settings.lowComp = server.arg(i).toFloat();
    } else if (server.argName(i).equals("highComp")) {
      settings.highComp = server.arg(i).toFloat();
    } else {
      Serial.println("Unknown argument: " + server.argName(i) + ": " + server.arg(i));
    }
  }

  saveSettings();

  String msg = String("Settings saved: ") + formatSettings();
  Serial.println(msg);
  server.send(200, "text/plain", msg);

  doPostSettings = true;
}


void setup() {
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);

  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(RELAY_PIN, LOW);
  lastStateChange = millis();

  Serial.begin(115200);

  // FIXME: This chip crashes when on STA but works with AP_STA
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Internet Fridge", WEBSERVER_PASSWORD);
  //WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  server.on("/settings", handleSettings);
  server.on("/restart", handleRestart);
  server.on("/status", handleStatus);
  server.on("/sensors", handleSensors);
  server.onNotFound(handleNotFound);
  server.begin();

  loadSettings();
  Serial.println(formatSettings());
}


unsigned long lastIteration;
void loop() {
  server.handleClient();
  delay(100);

  if (millis() < lastIteration + 10000) return;
  lastIteration = millis();

  float accum = 0.0;
  int numAccum = 0;
  String sensorBody = String(DEVICE_NAME) + " uptime=" + String(millis()) + "i";

  digitalWrite(RED_LED_PIN, HIGH);

  takeAllMeasurements();

  // Read each ds18b20 device individually
  float temp[N_SENSORS];
  for (int i=0; i<N_SENSORS; i++) {
    Serial.print("Temperature sensor ");
    Serial.print(i);
    Serial.print(": ");
    if (readTemperature(sensorAddr[i], &temp[i])) {
      Serial.print(temp[i]);
      Serial.println();
      if (i == AMBIANT) {
        accum += temp[i];
        numAccum++;
      }
      sensorBody += String(",") + sensorNames[i] + "=" + String(temp[i], 3);
    }
    delay(100);
  }
  Serial.println(sensorBody);


  if (numAccum) {
    float avgTemp = accum/numAccum;
    bool newRelayState = relayState;

    Serial.print("Average Temp: ");
    Serial.println(avgTemp);
    if (relayState) {
      if ((avgTemp < settings.lowPoint) || (temp[COMPRESSOR] > settings.highComp)) {
        newRelayState = LOW;
      }
    } else {
      if ((avgTemp > settings.highPoint) && (temp[COMPRESSOR] < settings.lowComp) && (millis() - lastStateChange) > MIN_RESTART_TIME) {
        newRelayState = HIGH;
      }
    }

    if (newRelayState != relayState) {
      relayState = newRelayState;
      digitalWrite(RELAY_PIN, relayState);
      lastStateChange = millis();
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(RED_LED_PIN, LOW);
    delay(100);
    digitalWrite(RED_LED_PIN, HIGH);
    delay(100);
    digitalWrite(RED_LED_PIN, LOW);
    delay(100);
    digitalWrite(RED_LED_PIN, HIGH);

    delay(1000);
    Serial.println("Connecting to wifi...");
    return;
  }
  Serial.println("Wifi connected to " + WiFi.SSID() + " IP:" + WiFi.localIP().toString());

  WiFiClient client;
  if (client.connect(INFLUX_HOSTNAME, INFLUX_PORT)) {
    Serial.println(String("Connected to ") + INFLUX_HOSTNAME + ":" + INFLUX_PORT);
    delay(50);

    sensorBody += ",compressorRelay=" + String(relayState);

    postRequest(sensorBody, client);

    if (doPostSettings) {
      postRequest(String(DEVICE_NAME) + " " + formatSettings(), client);
      doPostSettings = false;
    }

    client.stop();
  } else {
    digitalWrite(RED_LED_PIN, LOW);
    delay(100);
    digitalWrite(RED_LED_PIN, HIGH);
    delay(100);
  }
  digitalWrite(RED_LED_PIN, LOW);
  delay(100);
}


