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
    long number = strtol(&hex[1], NULL, 16);
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
    display.setRotation(0); // Adjust as needed
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
    display.setCursor(0, OLED_DATA_START + 10);
    display.setTextColor(ST7789_WHITE);
    display.print(largeText);
    
    display.setTextSize(1);
    display.setCursor(0, OLED_DATA_START + 40);
    display.print(statusMessage);

    int barHeight = 10;
    int barY = OLED_DATA_START + 60;
    int barWidth = SCREEN_WIDTH - 4;
    
    int progress = ((barWidth)*step)/numSteps;

    display.drawRect(2, barY, barWidth, barHeight, ST7789_WHITE);
    display.fillRect(4, barY + 2, progress, barHeight - 4, ST7789_WHITE);
}

void oledShowWeight(uint16_t weight) {
    oledcleardata();
    
    // Show Weight
    display.setTextSize(3);
    String weightStr = String(weight) + "g";
    
    // Center text roughly
    int16_t x = (SCREEN_WIDTH - (weightStr.length() * 18)) / 2;
    if (x < 0) x = 0;
    
    display.setCursor(x, OLED_DATA_START + 20);
    display.setTextColor(ST7789_WHITE);
    display.print(weightStr);

    // Update Filament Display below weight
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
    // int iconSize = 16;

    // Icons position (adjusted for 76px width)
    // 76px is narrow. Let's stack them or put them tight.
    // Bambu: 30, Spoolman: 48, Wifi: 60

    if(!booting){
        if(bambuDisabled == false) {
             if (bambu_connected == 1) {
                display.drawBitmap(30, iconY, bitmap_bambu_on , 16, 16, ST7789_WHITE);
            } else {
                if(iconToggle){
                     display.drawBitmap(30, iconY, bitmap_bambu_on , 16, 16, ST7789_WHITE);
                     display.drawLine(30, iconY+15, 46, iconY, ST7789_WHITE);
                }
            }
        }

        if (spoolmanConnected) {
             display.drawBitmap(48, iconY, bitmap_spoolman_on , 16, 16, ST7789_WHITE);
        } else {
             if(iconToggle){
                 display.drawBitmap(48, iconY, bitmap_spoolman_on , 16, 16, ST7789_WHITE);
                 display.drawLine(48, iconY+15, 64, iconY, ST7789_WHITE);
             }
        }

        if (wifiOn == 1) {
             display.drawBitmap(64, iconY, wifi_on , 16, 16, ST7789_WHITE);
        } else {
             if(iconToggle){
                 display.drawBitmap(64, iconY, wifi_on , 16, 16, ST7789_WHITE);
                 display.drawLine(64, iconY+15, 76, iconY+5, ST7789_WHITE); // Clip line
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
    int startY = OLED_DATA_START + 60;
    int lineHeight = 20;
    
    display.setTextSize(1);

    // AMS Data
    int count = 0;
    // Iterate through AMS units (simplified to show active ones or just list)
    // Since screen is tall (284px), we can list filaments

    for(int i=0; i < ams_count; i++) {
        for(int j=0; j<4; j++) {
            // Check if tray exists (simplified check)
            if(ams_data[i].trays[j].id != 255) { // 255 usually means empty
                 String colorHex = ams_data[i].trays[j].tray_color;
                 // If colorHex starts with #, skip it? usually it comes as FF0000 etc from API, but bambu.h says String.
                 if(!colorHex.startsWith("#")) colorHex = "#" + colorHex;

                 uint16_t color = hexToRGB565(colorHex);

                 // Draw Color Box
                 display.fillRect(0, startY + (count * lineHeight), 10, 10, color);
                 display.drawRect(0, startY + (count * lineHeight), 10, 10, ST7789_WHITE);

                 // Draw Type
                 display.setCursor(15, startY + (count * lineHeight));
                 display.setTextColor(ST7789_WHITE);
                 String type = ams_data[i].trays[j].tray_type;
                 if(type.length() > 6) type = type.substring(0, 6);
                 display.print(type);

                 count++;
                 if(startY + (count * lineHeight) > SCREEN_HEIGHT) return; // Stop if screen full
            }
        }
    }
    // Also show external spool if exists
}
