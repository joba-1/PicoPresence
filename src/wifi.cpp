/* 
WiFi on core 0 of a Raspberry Pico W

TODO if connect fails open AP
TODO if connect fails provide web server form for ssid and pass to store in eeprom
*/

#include <Arduino.h>

#define HEALTH_LED_PIN 9
#define HTTP_PORT 80

#include <WiFi.h>
#include <WebServer.h>
#include <LEAmDNS.h>
#include <HTTPUpdateServer.h>

// Post to InfluxDB
#include <HTTPClient.h>

// Health LED
#include <Breathing.h>
const uint32_t health_ok_interval = 5000;
const uint32_t health_err_interval = 1000;
Breathing health_led(health_ok_interval, HEALTH_LED_PIN);
bool enabledBreathing = true;  // global flag to switch breathing animation on or off
extern bool sensor_health;  // from main.cpp

// Infrastructure

// Get wlan data from eeprom (written by PicoWlan firmware)
#include <EEPROM.h>
const uint32_t eeprom_magic = 0xeeda4aee;  // as written by PicoWlan
struct eeprom_data {
    uint32_t magic;
    char ssid[32];
    char pass[32];
    uint32_t sum;  // PicoWlan simply adds bytes of magic, ssid and pass 
} data;

static WebServer httpServer(HTTP_PORT);
static HTTPUpdateServer httpUpdater;

Stream &out = Serial1;    // Route messages through picoprobe (no delay at boot)

// Post to InfluxDB
int influx_status = 0;
time_t post_time = 0;

// publish to mqtt broker
#include <PubSubClient.h>

WiFiClient wifiMqtt;
PubSubClient mqtt(wifiMqtt);

// Syslog
#include <WiFiUdp.h>
#include <Syslog.h>
WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);
char msg[512];  // one buffer for all syslog and json messages

char lastBssid[] = "00:00:00:00:00:00";  // last known connected AP (for reporting) 
int8_t lastRssi = 0;                     // last RSSI (for reporting)

char start_time[30];


void slog(const char *message, uint16_t pri = LOG_INFO) {
    static bool log_infos = true;
    
    if (pri < LOG_INFO || log_infos) {
        out.println(message);
        syslog.log(pri, message);
    }

    if (log_infos && millis() > 10 * 60 * 1000) {
        log_infos = false;  // log infos only for first 10 minutes
        slog("Switch off info level messages", LOG_NOTICE);
    }
}


void publish( const char *topic, const char *payload ) {
    if (mqtt.connected() && !mqtt.publish(topic, payload)) {
        slog("Mqtt publish failed");
    }
}


// Post data to InfluxDB
bool postInflux(const char *line) {
    static const char uri[] = "/write?db=" INFLUX_DB "&precision=s";

    WiFiClient wifiHttp;
    HTTPClient http;

    http.begin(wifiHttp, INFLUX_SERVER, INFLUX_PORT, uri);
    http.setUserAgent(PROGNAME);
    int prev = influx_status;
    influx_status = http.POST(line);
    String payload;
    if (http.getSize() > 0) { // workaround for bug in getString()
        payload = http.getString();
    }
    http.end();

    if (influx_status != prev) {
        snprintf(msg, sizeof(msg), "%d", influx_status);
        publish(MQTT_TOPIC "/status/DBResponse", msg);
    }

    if (influx_status < 200 || influx_status >= 300) {
        snprintf(msg, sizeof(msg), "Post %s:%d%s status=%d line='%s' response='%s'",
            INFLUX_SERVER, INFLUX_PORT, uri, influx_status, line, payload.c_str());
        slog(msg, LOG_ERR);
        return false;
    }

    post_time = time(NULL);
    return true;
}


// Wifi status as JSON
bool json_Wifi(char *json, size_t maxlen, const char *bssid, int8_t rssi) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Hostname\":\"%s\",\"Wifi\":{"
        "\"BSSID\":\"%s\","
        "\"IP\":\"%s\","
        "\"RSSI\":%d}}";

    int len = snprintf(json, maxlen, jsonFmt, WiFi.getHostname(), bssid, WiFi.localIP().toString().c_str(), rssi);

    return len > 0 && (size_t)len < maxlen;
}


// Report a change of RSSI or BSSID
void report_wifi( int8_t rssi, const byte *bssid ) {
    static const char digits[] = "0123456789abcdef";
    static const char lineFmt[] =
        "Wifi,Host=%s,Version=" VERSION " "
        "BSSID=\"%s\","
        "IP=\"%s\","
        "RSSI=%d";
    char line[sizeof(lineFmt)+100];
    static const uint32_t interval = 60000;
    static const int8_t min_diff = 5;
    static uint32_t prev = 0;
    static int8_t reportedRssi = 0;

    // Update for reporting
    lastRssi = rssi;
    for (size_t i=0; i<sizeof(lastBssid); i+=3) {
        lastBssid[i] = digits[bssid[i/3] >> 4];
        lastBssid[i+1] = digits[bssid[i/3] & 0xf];
    }

    // RSSI rate limit for log and db
    int8_t diff = reportedRssi - lastRssi;
    if (diff < 0 ) {
        diff = -diff;
    }
    uint32_t now = millis();
    if (diff >= min_diff || (now - prev > interval) ) {
        json_Wifi(msg, sizeof(msg), lastBssid, lastRssi);
        slog(msg);
        publish(MQTT_TOPIC "/json/Wifi", msg);

        snprintf(line, sizeof(line), lineFmt, WiFi.getHostname(), lastBssid, WiFi.localIP().toString().c_str(), lastRssi);
        postInflux(line);

        reportedRssi = lastRssi;
        prev = now;
    }
}


// check and report RSSI and BSSID changes
bool handle_wifi() {
    static byte prevBssid[6] = {0};
    static int8_t prevRssi = 0;
    static bool prevConnected = false;

    static const uint32_t reconnectInterval = 10000;  // try reconnect every 10s
    static const uint32_t reconnectLimit = 60;        // try restart after 10min
    static uint32_t reconnectPrev = 0;
    static uint32_t reconnectCount = 0;

    bool currConnected = WiFi.isConnected();
    int8_t currRssi = 0;
    byte *currBssid = prevBssid;

    if (currConnected) {
        currRssi = WiFi.RSSI();
        currBssid = WiFi.BSSID(currBssid);

        if (!prevConnected) {
            report_wifi(prevRssi, prevBssid);
        }

        if (currRssi != prevRssi || memcmp(currBssid, prevBssid, sizeof(prevBssid))) {
            report_wifi(currRssi, currBssid);
        }

        memcpy(prevBssid, currBssid, sizeof(prevBssid));
        reconnectCount = 0;
    }
    else {
        uint32_t now = millis();
        if (reconnectCount == 0 || now - reconnectPrev > reconnectInterval) {
            // WiFi.reconnect();  // currently not supported :(
            reconnectCount++;
            if (reconnectCount > reconnectLimit) {
                out.println("Failed to reconnect WLAN, about to reset");
                for (int i = 0; i < 20; i++) {
                    digitalWrite(HEALTH_LED_PIN, (i & 1) ? LOW : HIGH);
                    delay(100);
                }
                rp2040.restart();
                while (true)
                    ;
            }
            reconnectPrev = now;
        }
    }

    prevRssi = currRssi;
    prevConnected = currConnected;

    return currConnected;
}


// check ntp status
// return true if time is valid
bool check_ntptime() {
    static bool have_time = false;
    static bool ntp_stopped = true;

    if (!have_time) {
        time_t now = time(NULL);
        if (now > 50 * 366 * 24 * 60 * 60) {  // > 2020-01-01
            have_time = true;
            strftime(start_time, sizeof(start_time), "%FT%T", localtime(&now));
            snprintf(msg, sizeof(msg), "Got valid time at %s", start_time);
            slog(msg, LOG_NOTICE);
            if (mqtt.connected()) {
                publish(MQTT_TOPIC "/status/StartTime", start_time);
            }
        }
        else if (ntp_stopped && now > 1) {
            ntp_stopped = false;
            NTP.begin(NTP_SERVER);  // start with a delay
        }
    }

    return have_time;
}


void do_reset(const char *reason) {
    out.print(reason);
    out.println(": rebooting...");
    delay(100);
    rp2040.restart();
}


void handle_reset() {
    if (BOOTSEL) {
        while(BOOTSEL);  // wait until released to avoid flash mode
        do_reset("BOOTSEL pressed");
    }
}

// Called on incoming mqtt messages
void mqtt_callback(char* topic, byte* payload, unsigned int length) {

    typedef struct cmd { const char *name; void (*action)(void); } cmd_t;
    
    static cmd_t cmds[] = { 
        { "reset", [](){ do_reset("MQTT reset"); } }
    };

    // char *data = (char *)payload;

    if (strcasecmp(MQTT_TOPIC "/cmd", topic) == 0) {
        for (auto &cmd: cmds) {
            if (strncasecmp(cmd.name, (char *)payload, length) == 0) {
                snprintf(msg, sizeof(msg), "Execute mqtt command '%s'", cmd.name);
                slog(msg, LOG_INFO);
                (*cmd.action)();
                return;
            }
        }
    }

    snprintf(msg, sizeof(msg), "Ignore mqtt %s: '%.*s'", topic, length, (char *)payload);
    slog(msg, LOG_WARNING);
}


bool handle_mqtt( bool time_valid ) {
    static const int32_t interval = 5000;  // if disconnected try reconnect this often in ms
    static uint32_t prev = -interval;      // first connect attempt without delay

    if (mqtt.connected()) {
        mqtt.loop();
        return true;
    }

    uint32_t now = millis();
    if (now - prev > interval) {
        prev = now;

        if (mqtt.connect(HOSTNAME, MQTT_TOPIC "/status/LWT", 0, true, "Offline")
            && mqtt.publish(MQTT_TOPIC "/status/LWT", "Online", true)
            && mqtt.publish(MQTT_TOPIC "/status/Hostname", HOSTNAME)
            && mqtt.publish(MQTT_TOPIC "/status/DBServer", INFLUX_SERVER)
            && mqtt.publish(MQTT_TOPIC "/status/DBPort", itoa(INFLUX_PORT, msg, 10))
            && mqtt.publish(MQTT_TOPIC "/status/DBName", INFLUX_DB)
            && mqtt.publish(MQTT_TOPIC "/status/Version", VERSION)
            && (!time_valid || mqtt.publish(MQTT_TOPIC "/status/StartTime", start_time))
            && mqtt.subscribe(MQTT_TOPIC "/cmd")) {
            snprintf(msg, sizeof(msg), "Connected to MQTT broker %s:%d using topic %s", MQTT_SERVER, MQTT_PORT, MQTT_TOPIC);
            slog(msg, LOG_NOTICE);
            return true;
        }

        int error = mqtt.state();
        mqtt.disconnect();
        snprintf(msg, sizeof(msg), "Connect to MQTT broker %s:%d failed with code %d", MQTT_SERVER, MQTT_PORT, error);
        slog(msg, LOG_ERR);
    }

    return false;
}


void setup() {
    pinMode(HEALTH_LED_PIN, OUTPUT);
    digitalWrite(HEALTH_LED_PIN, HIGH);

    delay(2000);
    Serial1.begin(BAUDRATE);
    out.println("\nStarting " PROGNAME " v" VERSION " " __DATE__ " " __TIME__);

    EEPROM.begin(512);
    EEPROM.get(0, data);
    EEPROM.end();

    for (uint8_t *ptr=(uint8_t *)&data; ptr<(uint8_t *)&data.sum; ptr++) {
        data.sum -= *ptr;
    }

    if (data.magic != eeprom_magic || data.sum) {
        out.println("EEPROM data invalid. Write with PicoWlan firmware");
    }

    out.printf("Connecting to '%s'\n", data.ssid);
    String host(HOSTNAME);
    host.toLowerCase();
    WiFi.setHostname(host.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(data.ssid, data.pass);
    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
        out.println("Connection Failed! Rebooting...");
        delay(5000);
        rp2040.restart();
    }
    // WiFi.setDNS(IPAddress(192, 168, 1, 221));

    out.printf("Host '%s' IP: %s\n", WiFi.getHostname(), WiFi.localIP().toString().c_str());

    MDNS.begin(WiFi.getHostname());

    // Syslog setup
    syslog.server(SYSLOG_SERVER, SYSLOG_PORT);
    syslog.deviceHostname(WiFi.getHostname());
    syslog.appName("Joba1");
    syslog.defaultPriority(LOG_KERN);

    httpUpdater.setup(&httpServer);
    httpServer.begin();

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqtt_callback);

    MDNS.addService("http", "tcp", HTTP_PORT);

    digitalWrite(HEALTH_LED_PIN, LOW);

    health_led.limits(1, health_led.range() / 2);  // only barely off to 50% brightness
    health_led.begin();

    out.printf("Firmware update on http://%s:%u/update\n", WiFi.getHostname(), HTTP_PORT);
}

#include <queue>

std::queue<String> msgs;

extern "C" {
    void push(const char *msg) {
        msgs.push(msg);
    }
}

void loop() {
    bool health = true;

    bool have_time = check_ntptime();

    health &= handle_mqtt(have_time);
    health &= handle_wifi();
    health &= (influx_status >= 200 && influx_status < 300);

    if (have_time && enabledBreathing) {
        health_led.interval(health ? health_ok_interval : health_err_interval);
        health_led.handle();
    }

    httpServer.handleClient();
    MDNS.update();
    handle_reset();

    while(!msgs.empty()) {
        Serial1.print(msgs.front());
        msgs.pop();
    }
}