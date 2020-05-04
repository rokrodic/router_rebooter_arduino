/**
   @file ESP8266_Router.ino

   @author Rok Rodic (http://www.rodic.si/)
   @date 2017-08-30

   ROSA BootCon alias Router reseter
   Select: NODEMCU 1.0 4M(1M SPIFFS) 80MHz 
   Select: (for Sonoff S20) Generic ESP8266 Module, flashmode DOUT!!!!, freq 40MHz, CPU 80MHz, flash 1M(64k SPIFFS or none), 
    There is a button attached to GPIO0 and two LEDs, a green one connected to GPIO13 like in the Sonoff and a blue one to the GPIO12, like the relay, so whenever the relay is closed the LED will lit blue.
    Press button when connecting Vcc in order to program!!!
    GPIO0  button
    GPIO1  rxd
    GPIO3  txd
    GPIO12 blue led and Relay (0 = Off, 1 = On)
    GPIO13 green led (0 = On, 1 = Off)
    esptool.py --port COM3 write_flash 0x00000 firmware.bin
    esptool.py --port COM3 erase_flash

   Some info about variables (all times are in milliseconds):
   TargetHTTP - holds name of internet page you want to connect to.
   CheckPeriod - defined time between checks of internet page TargetHTTP.
   ResetPeriod - time to wait after the "no intrnet" = TargetHTTP-could-not-be-connected-to. Will start resetting after this time. Can be longer then CheckPeriod - the wifi might reappear in this time, and the S20 will not reboot as it is not needed anymore.
   TimeToWaitForRouterReset - time the S20 waits after it resets the router (router needs time to reboot) and tries with new checks of TargetHTTP.
   nextTime - time when next check of targetHTTP will be done.
   LastConnect - time of last connection to TargetHTTP.

   changelog:
    - added some more info above
    - SSDP for automatic detection
    - button reset
    - MDNS + http firmware upload
    - watchdog
    - basic logic / timings
*/
#define CheckPeriod 5*60*1000 //in milliseconds
#define ResetPeriod 15*60*1000 //in milliseconds
#define TimeToWaitForRouterReset 2*60*1000 //in milliseconds
static const char GSSID[] PROGMEM = "YourSSID"; //"YourSSID";
static const char GPASS[] PROGMEM = "YourPASSWORD"; //"YourPASSWORD";
const String TargetHTTP = "http://plain-text-ip.com/";
const char* firmware_update_path = "/firmware";
char firmware_update_username[] = "admin";
char firmware_update_password[] = "admin";
const char* host = "bootcon";

  #include <ESP8266WiFi.h>
  #include <WiFiClient.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266HTTPClient.h>
  #include <ESP8266SSDP.h>
  #include <ESP8266mDNS.h>
  #include <ESP8266HTTPUpdateServer.h>
  //#include <ESP8266NetBIOS.h>
  //#include <ESP8266LLMNR.h>
  //#include <Arduino.h>
  //#include <GDBStub.h>
  #include <ESP8266WiFiMulti.h>
  #include <ArduinoOTA.h>
  #include "Bounce2.h"
  extern "C" {
    #include "user_interface.h"  // Required for wifi_station_connect() to work
  }

IPAddress static_ip(192, 168, 1, 155); //Requested static IP address for the ESP
IPAddress static_gateway(192, 168, 1, 1); // IP address for the Wifi router
IPAddress static_netmask(255, 255, 255, 0);
const char* Intro_string = "ROSA RR 20170830.v004";
#define KeyPin 0
#define ResetPin 12
#define LEDPin 13
unsigned long nextTime = 0;
unsigned long LastConnect = 2*60*1000; //in milliseconds
#define DBG_PORT Serial
ESP8266WiFiMulti WiFiMulti;
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
HTTPClient http;
#define OFF 1 //for LED
#define ON  0 //for LED
#define ROFF 0 //for RELAY
#define RON  1 //for RELAY
unsigned long currentMillis;
unsigned long RestartTime = 0;
String g_ssid;
String g_pass;
const int PinR = ResetPin;
const int PinK = KeyPin;
const int PinLED = LEDPin;
Bounce debouncer = Bounce();
String IPString = "";
String StrA = "<!DOCTYPE html><html><body><h2>BootCon - A very cheap router resetter</h2><a href=\"/firmware\">Firmware update</a>.</br></br><a href=\"http://www.rodic.si/\">Author's homepage</a>.</body></html>";

void setup() {
  g_ssid = FPSTR(GSSID); //g_ssid = "";
  g_pass = FPSTR(GPASS); //g_pass = "";
  ESP.wdtEnable(8000);
  ESP.wdtFeed();
  pinMode(PinR, OUTPUT); digitalWrite(PinR, RON);
  pinMode(PinLED, OUTPUT); digitalWrite(PinLED, ON);
  pinMode(PinK, INPUT_PULLUP);
  debouncer.attach(PinK);
  debouncer.interval(25); // interval in ms
  DBG_PORT.begin(115200);
  DBG_PORT.println();
  //DBG_PORT.setDebugOutput(true);
  DBG_PORT.println(F("Booting"));
  DBG_PORT.printf("Sketch size: %u\n", ESP.getSketchSize());
  DBG_PORT.printf("Free size: %u\n", ESP.getFreeSketchSpace());
  DBG_PORT.printf("Heap: %u\n", ESP.getFreeHeap());
  DBG_PORT.printf("Boot Vers: %u\n", ESP.getBootVersion());
  DBG_PORT.printf("CPU: %uMHz\n", ESP.getCpuFreqMHz());
  DBG_PORT.printf("SDK: %s\n", ESP.getSdkVersion());
  DBG_PORT.printf("Chip ID: %u\n", ESP.getChipId());
  DBG_PORT.printf("Flash ID: %u\n", ESP.getFlashChipId());
  DBG_PORT.printf("Flash Size: %u\n", ESP.getFlashChipRealSize());
  DBG_PORT.printf("Vcc: %u\n", ESP.getVcc());
  DBG_PORT.println(Intro_string);
  DBG_PORT.print(F("Chip ID: 0x"));
  DBG_PORT.println(ESP.getChipId(), HEX);
  DBG_PORT.print(F("Reset reason: "));
  DBG_PORT.println(ESP.getResetReason());

  //DBG_PORT.print("WIFI connecting");
  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(g_ssid.c_str(), g_pass.c_str());
//  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  http.setReuse(true);
//  byte cnt = 0;
//  while ((WiFi.status() != WL_CONNECTED) && (cnt < 100)) {
//    delay(500);
//    DBG_PORT.print(".");
//    cnt++;
//    yield();
//    ESP.wdtFeed();
//  }
//  DBG_PORT.print(F("IP address: "));
//  IPString = WiFi.localIP().toString();
//  DBG_PORT.println(IPString);

  httpServer.on("/index.html", [](){
    httpServer.send(200, "text/html", StrA);
  });
  httpServer.on("/", [](){
    httpServer.send(200, "text/html", StrA);
  }); 
  httpServer.on("/description.xml", HTTP_GET, [](){SSDP.schema(httpServer.client());});
  MDNS.begin(host);
  httpUpdater.setup(&httpServer, firmware_update_path, firmware_update_username, firmware_update_password);
//  httpServer.begin();
  MDNS.addService("http", "tcp", 80);
  DBG_PORT.printf("HTTPUpdateServer ready! Open http://%s.local%s in your browser and login with username '%s' and password '%s'\n", host, firmware_update_path, firmware_update_username, firmware_update_password);

  httpServer.begin();

  Serial.printf("Starting SSDP...\n");
  SSDP.setSchemaURL("description.xml");
  SSDP.setHTTPPort(80);
  SSDP.setName("BootCon");
  SSDP.setSerialNumber("24586273");
  SSDP.setURL("index.html");
  SSDP.setModelName("ROSA Router reseter 2018");
  SSDP.setModelNumber("20180310007");
  SSDP.setModelURL("http://www.rodic.si");
  SSDP.setManufacturer("Rok Rodic");
  SSDP.setManufacturerURL("http://www.rodic.si");
  SSDP.setDeviceType("upnp:rootdevice");
  SSDP.begin();

  DBG_PORT.print(F("DONE\n"));
  digitalWrite(PinLED, OFF);
}

void WiFiOn() {
  wifi_fpm_do_wakeup();
  wifi_fpm_close();
  wifi_set_opmode(STATION_MODE);
  wifi_station_connect();
}

#define FPM_SLEEP_MAX_TIME 0xFFFFFFF

void WiFiOff() {
  wifi_station_disconnect();
  wifi_set_opmode(NULL_MODE);
  wifi_set_sleep_type(MODEM_SLEEP_T);
  wifi_fpm_open();
  wifi_fpm_do_sleep(FPM_SLEEP_MAX_TIME);
}

void loop() {
  currentMillis = millis();

  if (currentMillis > nextTime) {
    if((WiFiMulti.run() == WL_CONNECTED)) {
      digitalWrite(PinLED, ON);
      DBG_PORT.println("IP address: ");
      DBG_PORT.println(WiFi.localIP());
      DBG_PORT.print("[HTTP] begin...\n");
      http.begin(TargetHTTP);
      //DBG_PORT.print("[HTTP] GET...\n");
      int httpCode = http.GET(); // httpCode will be negative on error
      if (httpCode > 0) {
        if (currentMillis > (0xFFFFFFFF - CheckPeriod)) nextTime = 0; else nextTime = currentMillis + CheckPeriod; //To avoid overflow
        if (currentMillis > (0xFFFFFFFF - ResetPeriod)) LastConnect = 0; else LastConnect = currentMillis; //To avoid overflow
        //DBG_PORT.printf("[HTTP] GET... code: %d, got: ", httpCode);
        if (httpCode == HTTP_CODE_OK) {
          String payload = http.getString();
          //DBG_PORT.println(payload);
        }
        DBG_PORT.printf("[NEXT] Scan scheduled in %d seconds.", (CheckPeriod/1000));
      } else {
        DBG_PORT.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
        if (currentMillis > (0xFFFFFFFF - CheckPeriod)) nextTime = 0; else nextTime = currentMillis + (CheckPeriod / 100);
      }
      http.end();
      digitalWrite(PinLED, OFF);
    }
  }

  debouncer.update();

  // Trigger restart
  if ((currentMillis > (LastConnect + ResetPeriod)) || (debouncer.fell())) {
    digitalWrite(PinR, ROFF);
    delay(100);
    DBG_PORT.println(F("RESETTING!!!"));
    //WiFi.forceSleepBegin();
    //WiFiOff();
    unsigned int cnt = 0;
    unsigned long qwe = millis() + TimeToWaitForRouterReset;
    while (millis() < qwe) {
      //DBG_PORT.println(F("."));
      ESP.wdtFeed();
      yield();
      delay(500);
      cnt++;
      if ((cnt % 2) == 0) digitalWrite(PinLED, OFF); else digitalWrite(PinLED, ON);
      if (cnt > 10) digitalWrite(PinR, RON);
    }
    ESP.restart();
  }
    
  httpServer.handleClient();
  ESP.wdtFeed();
  yield();
}

