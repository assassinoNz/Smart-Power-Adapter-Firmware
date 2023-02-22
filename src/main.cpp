#define HARD_CODED_CREDENTIALS

#include <Arduino.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP8266httpUpdate.h>
#include <sensorRead.h>

namespace IO {
    const unsigned char PIN_RESET = D4;
}

namespace LFS {
    const char *SELF_AP_CREDENTIALS_PATH = "/HomeAPCredentials.txt";
    const char *MQTT_CLIENT_ID_PATH = "/MQTTClientId.txt";

    File file;
}

namespace WIFI {
    WiFiClientSecure client;
}

namespace WEB {
    const char *HOST_MAIN = "150.230.238.30";
    const short HOST_MAIN_PORT = 8080;
    const char *FIRMWARE_ROUTE = "/update/firmware.bin";
    const char SELF_AP_HTML[] PROGMEM = R"rawliteral(
        <!DOCTYPE html><html lang=en><head><meta charset=UTF-8><meta http-equiv=X-UA-Compatible content="IE=edge"><meta name=viewport content="width=device-width,initial-scale=1.0"><style>form{border:5px solid #000;padding:20px}form>div>label{display:block}input{padding:10px;margin:10px 0}</style></head><body><div style="display:flex;justify-content:center;align-items:center;height:100vh;"><form action=credentials><div><label for=ssid>SSID:</label><input type=text id=ssid name=ssid></div><div><label for=psk>PSK:</label><input type=psk id=psk name=psk></div><button type=submit>Save credentials and restart</button></form></div></body></html>
    )rawliteral";

    const char *HOST_ML = "wandering-water-6831.fly.dev";
    const short HOST_ML_PORT = 443;
    const char *PREDICT_ROUTE = "/predict";

    short responseCode = -1;

    AsyncWebServer server(80);
    HTTPClient client;

    String request(String body) {
        Serial.println("[WEB]: Attempting request");
        client.begin(WIFI::client, WEB::HOST_ML, WEB::HOST_ML_PORT, WEB::PREDICT_ROUTE, true);
        client.addHeader("Content-Type", "application/json");
        responseCode = client.POST(body);
        String response = client.getString();
        client.end();
        Serial.println("[WEB]: Request complete CODE: " + String(responseCode) + " RESPONSE: " + response);
        return response;
    }
}

namespace SIN {
    bool relayState = false; 
    const int nReads = 3;
    const unsigned long rDelay = 100;

    const float vSrg[3] =     {245,   250,    255};
    const float vSrgMin[3] =  {225,   230,    235};
    const float iSrg[3] =     {8,     9,      10};

    void surgeProtect(int m){
        const float v = getV();
        if((v> SIN::vSrg[0]) || (v<SIN::vSrgMin[0]) || (getI()> SIN::iSrg[m])) relayOn(false);
    }

    bool switchMain(){
        bool s = false;
        switch (switchAct()){
            case 1:
                s = relayOn(!getState());
                Serial.println("Toggle");
                break;

            case 2:
                LittleFS.remove(LFS::SELF_AP_CREDENTIALS_PATH);
                Serial.println("Full Reset");
                break;
        }
        return s;
    }
}

namespace MQTT {
    const char *HOST = "150.230.238.30";
    const short HOST_PORT = 1883;
    const char *HOST_USERNAME = "assassino";
    const char *HOST_PASSWORD = "assassino";

    String CLIENT_ID = "<CLIENT_ID>";
    String POWER_TOPIC = "<CLIENT_ID>/power";
    String READINGS_TOPIC = "<CLIENT_ID>/readings";

    PubSubClient client;

    void onMqttMessage(const char* topic, byte* payload, unsigned int length) {
        Serial.println("\n[MQTT]: Recieved TOPIC: " + String(topic) + " PAYLOAD: " + String((char*) payload));

        if (strcmp(topic, MQTT::POWER_TOPIC.c_str()) == 0) {
            DynamicJsonDocument message(1024);
            deserializeJson(message, payload);

            const bool state = message["state"];
            pinMode(LED_BUILTIN, OUTPUT);
            digitalWrite(LED_BUILTIN, !state); //WARNING: LED_BUILTIN seems to be active low
            relayOn(!state);
        }
    }
}

void setup() {
    //Serial communication
    Serial.begin(115200);
    Serial.println("\n==================================================================");
    Serial.println("[SETUP]: Configured serial communication AT: 115200");

    //Sensor Init
    setup_Analog();

    //LittleFS
    LittleFS.begin();

    #ifdef HARD_CODED_CREDENTIALS
        LFS::file = LittleFS.open(LFS::MQTT_CLIENT_ID_PATH, "w");
        LFS::file.print("abcdefghi");
        LFS::file.close();

        LFS::file = LittleFS.open(LFS::SELF_AP_CREDENTIALS_PATH, "w");
        LFS::file.print("Scorpius");
        LFS::file.print(',');
        LFS::file.print("LogIn.WiFi.NSMTFS");
        LFS::file.print(',');
        LFS::file.close();
    #endif

    Serial.println("\n[SETUP]: Retrieving MQTT client ID");
    if (LittleFS.exists(LFS::MQTT_CLIENT_ID_PATH)) {
        LFS::file = LittleFS.open(LFS::MQTT_CLIENT_ID_PATH, "r");
        MQTT::CLIENT_ID = LFS::file.readString();
        LFS::file.close();

        MQTT::POWER_TOPIC = MQTT::CLIENT_ID + "/power";
        MQTT::READINGS_TOPIC = MQTT::CLIENT_ID + "/readings";

        Serial.println("\n[LFS]: Retrieved MQTT client ID: " + MQTT::CLIENT_ID);
    } else {
        Serial.println("\n[LFS]: No device ID found. Please contact support");
        while (true);
    }

    Serial.println("\n[SETUP]: Searching for stored credentials for your home AP");
    if (LittleFS.exists(LFS::SELF_AP_CREDENTIALS_PATH)) {
        //CASE: Credentials for home AP is available
        LFS::file = LittleFS.open(LFS::SELF_AP_CREDENTIALS_PATH, "r");
        String homeApSSID = LFS::file.readStringUntil(',');
        String homeApPSK = LFS::file.readStringUntil(',');
        LFS::file.close();
        Serial.println("[SETUP]: Stored credentials found SSID: " + homeApSSID + " PSK: " + homeApPSK);

        //WiFi
        Serial.println("[SETUP]: Connecting to WiFi SSID: " + homeApSSID);
        WIFI::client.setInsecure();
        WiFi.begin(homeApSSID, homeApPSK);
        while (WiFi.status() != WL_CONNECTED) {
            Serial.print(".");
            delay(500);
        }
        Serial.println("\n[WIFI]: Connected to WiFi SSID: " + homeApSSID);

        //OTA
        Serial.println("[SETUP]: Checking for OTA updates");
        switch(ESPhttpUpdate.update(WIFI::client, WEB::HOST_MAIN, WEB::HOST_MAIN_PORT, WEB::FIRMWARE_ROUTE)) {
            case HTTP_UPDATE_FAILED: {
                Serial.println("[OTA] Failed");
                break;
            }
            case HTTP_UPDATE_NO_UPDATES: {
                Serial.println("[OTA] No updates available");
                break;
            }
            case HTTP_UPDATE_OK: {
                Serial.println("[OTA] Successful");
                break;
            }
        }

        //Time
        Serial.println("[SETUP]: Configuring system time");
        if (time(NULL) < 1663485411) {
            //CASE: System time is not configured
            configTime(19800, 0, "pool.ntp.org");
            Serial.println("[TIME]: Configured system time HOST: pool.ntp.org");
        }

        Serial.println("[SETUP]: Configuring MQTT");
        MQTT::client.setClient(WIFI::client);
        MQTT::client.setServer(MQTT::HOST, MQTT::HOST_PORT);
        MQTT::client.setCallback(MQTT::onMqttMessage);

        Serial.println("[MQTT]: Connecting to broker");
        while (!MQTT::client.connected()) {
            if (MQTT::client.connect(MQTT::CLIENT_ID.c_str(), MQTT::HOST_USERNAME, MQTT::HOST_PASSWORD)) {
                Serial.println("[MQTT]: Connected BROKER: " + String(MQTT::HOST) + ":" + String(MQTT::HOST_PORT));
                MQTT::client.subscribe(MQTT::POWER_TOPIC.c_str());
            } else {
                Serial.print(".");
                delay(500);
            }
        }

        while (true) {
            MQTT::client.loop();

            // int sw = 0;
            // // int sw = switchAct();
            // if (sw == 1) {
            //     if (getState()) relayOn(false);
            //     else relayOn(true);
            // } else if (sw == 2){
            //     LittleFS.remove(LFS::SELF_AP_CREDENTIALS_PATH);
            //     Serial.println("[LFS]: Removed all home AP credentials");
            // }

            // DynamicJsonDocument doc(1024);
            // DynamicJsonDocument doc2(1024);
            
            // for (int i = 0; i < SIN::nReads; i++){
            //     doc["v"] = getV();
            //     doc["i"] = getI();
            //     doc["time"] = time(NULL);

            //     doc2[i] = doc;
            //     // delay(SIN::rDelay);

            //     // long t0 = millis();
            //     // while ((millis()<t0+SIN::rDelay)&&(millis()-t0 >= 0)){
            //     //     SIN::surgeProtect(0);
            //     // }
            // } 

            // char buffer[256];
            // serializeJson(doc2, buffer);
            // Serial.println(buffer);

            // MQTT::client.publish(MQTT::READINGS_TOPIC.c_str(), buffer);


            WEB::request("{\"device_id\":\"QEIZrUmZGUuzBqRnw0jZ\",\"data_reading\":{\"i\":0.9900436818128375,\"time\":1676362048419,\"v\":0.5681495890359043}}");
            
            
            delay(1000);
        }
    } else {
        //CASE: Create a softAP to change the authentication details
        Serial.println("\n[SETUP]: No stored credentials found. Configuring self AP");

        WiFi.softAP("Smart Power Adapter", "123456789");
        Serial.print("\n[SELF_AP]: Configured self AP SSID: Smart Power Adapter PSK: 123456789");
        
        WEB::server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->send_P(200, "text/html", WEB::SELF_AP_HTML);
        });
        WEB::server.on("/credentials", HTTP_GET, [](AsyncWebServerRequest *request) {
            File file = LittleFS.open(LFS::SELF_AP_CREDENTIALS_PATH, "w");
            String ssid = request->arg("ssid");
            String psk = request->arg("psk");
            file.print(ssid);
            file.print(',');
            file.print(psk);
            file.print(',');
            file.close();
            Serial.println("[SELF_AP]: Stored credentials for your home AP SSID: " + ssid + " PSK: " + psk);
            request->send_P(200, "text/plain", ("[SELF_AP]: Stored credentials for your home AP SSID: " + ssid + " PSK: " + psk).c_str());
            delay(2000);
            ESP.restart();
        });
        WEB::server.begin();
        Serial.println("\n[SELF_AP]: Configured server PORT: 80");
    }
}

void loop() {
    Serial.println("[ERR]: Loop reached");
    delay(2000);
}