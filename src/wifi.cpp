/* 
WiFi on core 0 of a Raspberry Pico W

TODO if connect fails open AP
TODO if connect fails provide web server form for ssid and pass to store in eeprom
*/

#include <Arduino.h>

#define HTTP_PORT 80
#define PICO_HOST "pico"

#include <WiFi.h>
#include <WebServer.h>
#include <LEAmDNS.h>
#include <HTTPUpdateServer.h>

// Get wlan data from eeprom (written by PicoWlan firmware)
#include <EEPROM.h>
const uint32_t magic = 0xeeda4aee;
struct eeprom_data {
  uint32_t magic;
  char ssid[32];
  char pass[32];
  uint32_t sum;
} data;

static WebServer httpServer(HTTP_PORT);
static HTTPUpdateServer httpUpdater;

extern bool outStarted;
extern Stream &out;

void setup() {
  while( !outStarted );  // wait until other core initialized Serial
  out.println("Wifi core starting");

  EEPROM.begin(512);
  EEPROM.get(0, data);
  EEPROM.end();
  for (uint8_t *ptr=(uint8_t *)&data; ptr<(uint8_t *)&data.sum; ptr++) {
    data.sum -= *ptr;
  }

  if (data.magic != magic || data.sum) {
    out.println("EEPROM data invalid. Write with PicoWlan firmware");
  }

  out.printf("Connecting to %s\n", data.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(PICO_HOST);
  WiFi.begin(data.ssid, data.pass);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    out.println("Connection Failed! Rebooting...");
    delay(5000);
    rp2040.restart();
  }
  out.printf("Host %s IP %s\n", WiFi.getHostname(), WiFi.localIP().toString().c_str());

  MDNS.begin(WiFi.getHostname());

  httpUpdater.setup(&httpServer);
  httpServer.begin();

  MDNS.addService("http", "tcp", HTTP_PORT);

  out.printf("Firmware update on http://%s:%u/update\n", WiFi.getHostname(), HTTP_PORT);
}

void loop() {
  httpServer.handleClient();
  MDNS.update();
}