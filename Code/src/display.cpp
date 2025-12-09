#include "display.h"
#include "api.h"
#include <vector>
#include "icons.h"
#include "main.h"
#include <SPI.h>
#include "bambu.h"

// Instantiate the ST7789 display using Hardware SPI
ST7789 display(TFT_CS, TFT_DC, TFT_RST);

bool wifiOn = false;
bool iconToggle = false;

// Helpers for color conversion
uint16_t hexToRGB565(String hex) {
    if (hex.length() == 0) return ST7789_WHITE;
    if (hex.charAt(0) == '#') hex = hex.substring(1);

    long number = strtol(hex.c_str(), NULL, 16);
    long r = number >> 16;
    long g = (number >> 8) & 0xFF;
    long b = number & 0xFF;
    return display.color565(r, g, b);
}

void setupDisplay() {
    // Initialize SPI with specific pins for ESP32
    SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);

    // Set backlight pin
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, LOW); // Pull low to enable backlight

    display.begin();
    display.setRotation(3); // 1 or 3 for landscape (widescreen)
    display.fillScreen(ST7789_BLACK);
    display.setTextColor(ST7789_WHITE);

    oledShowTopRow();
    oledShowProgressBar(0, 7, DISPLAY_BOOT_TEXT, "Display init");
}

void oledclearline() {
    // Clear top status bar area
    display.fillRect(0, 0, SCREEN_WIDTH, 16, ST7789_BLACK);
}

void oledcleardata() {
    // Clear data area
    display.fillRect(0, OLED_DATA_START, SCREEN_WIDTH, SCREEN_HEIGHT - OLED_DATA_START, ST7789_BLACK);
}

int oled_center_h(const String &text) {
    // Simple estimation assuming standard font width (6px per char for size 1)
    int16_t w = text.length() * 6 * 1; // Assuming text size 1 (default)
    return (SCREEN_WIDTH - w) / 2;
}

int oled_center_v(const String &text) {
    int16_t h = 8 * 1; // Assuming text size 1
    return OLED_DATA_START + ((OLED_DATA_END - OLED_DATA_START - h) / 2);
}


void oledShowProgressBar(const uint8_t step, const uint8_t numSteps, const char* largeText, const char* statusMessage) {
    // Clear data area
    oledcleardata();
    
    display.setTextSize(2);
    display.setCursor(0, OLED_DATA_START + 5);
    display.setTextColor(ST7789_WHITE);
    display.print(largeText);
    
    display.setTextSize(1);
    display.setCursor(0, OLED_DATA_START + 25);
    display.print(statusMessage);

    int barHeight = 8;
    int barY = OLED_DATA_START + 40;
    int barWidth = SCREEN_WIDTH - 4;
    
    int progress = ((barWidth)*step)/numSteps;

    display.drawRect(2, barY, barWidth, barHeight, ST7789_WHITE);
    display.fillRect(4, barY + 2, progress, barHeight - 4, ST7789_WHITE);
}

void oledShowWeight(uint16_t weight) {
    oledcleardata();
    
    // Show Weight on the left side, big
    display.setTextSize(3);
    String weightStr = String(weight) + "g";
    
    display.setCursor(5, OLED_DATA_START + 15);
    display.setTextColor(ST7789_WHITE);
    display.print(weightStr);

    // Update Filament Display on the right
    updateFilamentDisplay();
}

void oledShowMessage(const String &message, uint8_t size) {
    oledcleardata();
    display.setTextSize(size);
    display.setTextColor(ST7789_WHITE);
    // Simple estimation
    int16_t charWidth = 6 * size;
    int16_t textWidth = message.length() * charWidth;
    int16_t x = (SCREEN_WIDTH - textWidth) / 2;
    if (x < 0) x = 0; // Left align if too long

    // Center vertically in data area
    int16_t charHeight = 8 * size;
    int16_t y = OLED_DATA_START + ((OLED_DATA_END - OLED_DATA_START - charHeight) / 2);

    display.setCursor(x, y);
    display.print(message);
}

void oledShowTopRow() {
    oledclearline();

    display.setTextSize(1);
    display.setTextColor(ST7789_WHITE);
    display.setCursor(0, 4);
    display.print("v");
    display.print(VERSION);

    iconToggle = !iconToggle;

    int iconY = 0;
    int spacing = 20;
    int startX = 60; // Start icons after version text

    if(!booting){
        if(bambuDisabled == false) {
             if (bambu_connected == 1) {
                // Green for connected
                display.drawBitmap(startX, iconY, bitmap_bambu_on , 16, 16, ST7789_GREEN);
            } else {
                if(iconToggle){
                     display.drawBitmap(startX, iconY, bitmap_bambu_on , 16, 16, ST7789_RED);
                     // display.drawLine(startX, iconY+15, startX+16, iconY, ST7789_RED);
                }
            }
        }
        startX += spacing;

        if (spoolmanConnected) {
             display.drawBitmap(startX, iconY, bitmap_spoolman_on , 16, 16, ST7789_BLUE);
        } else {
             if(iconToggle){
                 display.drawBitmap(startX, iconY, bitmap_spoolman_on , 16, 16, ST7789_RED);
                 // display.drawLine(startX, iconY+15, startX+16, iconY, ST7789_RED);
             }
        }
        startX += spacing;

        if (wifiOn == 1) {
             display.drawBitmap(startX, iconY, wifi_on , 16, 16, ST7789_GREEN);
        } else {
             if(iconToggle){
                 display.drawBitmap(startX, iconY, wifi_on , 16, 16, ST7789_RED);
                 // display.drawLine(startX, iconY+15, startX+16, iconY+5, ST7789_RED);
             }
        }
    }
}

void oledShowIcon(const char* icon) {
    oledcleardata();
    // For now just print text as bitmaps are for SSD1306 and might need conversion/handling for 76x284
    display.setTextSize(2);
    display.setTextColor(ST7789_WHITE);
    display.setCursor(5, OLED_DATA_START + 20);
    if (strcmp(icon, "failed") == 0) display.print("Failed");
    else if (strcmp(icon, "success") == 0) display.print("Success");
    else if (strcmp(icon, "transfer") == 0) display.print("Transfer");
    else if (strcmp(icon, "loading") == 0) display.print("Loading");
}

void updateFilamentDisplay() {
    // Start drawing to the right of weight display (roughly x=100)
    int startX = 120;
    int startY = OLED_DATA_START + 5;
    int boxSize = 12;
    int gap = 4;
    int count = 0;
    int maxCols = (SCREEN_WIDTH - startX) / (boxSize + gap);

    // Iterate through AMS units
    for(int i=0; i < ams_count; i++) {
        for(int j=0; j<4; j++) {
            // Check if tray exists
            // Usually tray_type or tray_color is empty if no filament
            if(ams_data[i].trays[j].id != 255 && ams_data[i].trays[j].tray_type != "") {
                 String colorHex = ams_data[i].trays[j].tray_color;
                 if(!colorHex.startsWith("#")) colorHex = "#" + colorHex;

                 uint16_t color = hexToRGB565(colorHex);

                 int x = startX + (count * (boxSize + gap));
                 if (x + boxSize > SCREEN_WIDTH) break; // Stop if screen full horizontally

                 // Draw Color Box
                 display.fillRect(x, startY, boxSize, boxSize, color);
                 display.drawRect(x, startY, boxSize, boxSize, ST7789_WHITE);

                 // Draw Type below box (very small text? or just first letter)
                 display.setTextSize(1);
                 display.setTextColor(ST7789_WHITE);
                 String type = ams_data[i].trays[j].tray_type;
                 if(type.length() > 0) {
                    display.setCursor(x, startY + boxSize + 2);
                    display.print(type.substring(0, 1)); // Just first char
                 }

                 count++;
            }
        }
    }
}
