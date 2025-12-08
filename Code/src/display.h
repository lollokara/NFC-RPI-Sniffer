#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include "TFTM2.25-1.h" // Include the new driver header
#include "config.h"


extern ST7789 display;
extern bool wifiOn;

void setupDisplay();
void oledclearline();
void oledcleardata();
int oled_center_h(const String &text);
int oled_center_v(const String &text);

void oledShowProgressBar(const uint8_t step, const uint8_t numSteps, const char* largeText, const char* statusMessage);

void oledShowWeight(uint16_t weight);
void oledShowMessage(const String &message, uint8_t size = 2);
void oledShowTopRow();
void oledShowIcon(const char* icon);

// New function for filament display
void updateFilamentDisplay();

#endif
