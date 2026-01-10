#include "config.h"

// ################### Config Bereich Start
// ***** PN532 (RFID)
//#define PN532_SCK   18
//#define PN532_MOSI  23
//#define PN532_SS    5
//#define PN532_MISO  19
const uint8_t PN532_IRQ = 20;
const uint8_t PN532_RESET = 21;
// ***** PN532

// ***** HX711 (Waage)
// HX711 circuit wiring
const uint8_t LOADCELL_DOUT_PIN = 0;
const uint8_t LOADCELL_SCK_PIN = 1;
const uint8_t calVal_eepromAdress = 0;
const uint16_t SCALE_LEVEL_WEIGHT = 500;
// ***** HX711

// ***** TTP223 (Touch Sensor)
// TTP223 circuit wiring
const uint8_t TTP223_PIN = 5;
// ***** TTP223


// ***** Display
const uint8_t OLED_TOP_START = 0;
const uint8_t OLED_TOP_END = 16;
const uint8_t OLED_DATA_START = 17;
const uint8_t OLED_DATA_END = SCREEN_HEIGHT;

const uint8_t TFT_CS = 7;
const uint8_t TFT_DC = 3;
const uint8_t TFT_RST = 2;
const uint8_t TFT_SCK = 4;
const uint8_t TFT_MOSI = 6;
const uint8_t TFT_BLK = 10;
// ***** Display

// ***** Webserver
const uint8_t webserverPort = 80;
// ***** Webserver

// ***** API
const char* apiUrl = "/api/v1";
// ***** API

// ***** Bambu Auto Set Spool

// ***** Task Prios
uint8_t rfidTaskCore = 1;
uint8_t rfidTaskPrio = 1;

uint8_t rfidWriteTaskPrio = 1;

uint8_t mqttTaskCore = 1;
uint8_t mqttTaskPrio = 1;

uint8_t scaleTaskCore = 0;
uint8_t scaleTaskPrio = 2;
// ***** Task Prios
