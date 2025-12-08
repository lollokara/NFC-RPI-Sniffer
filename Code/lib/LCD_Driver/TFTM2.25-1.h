
#ifndef _ST7789H_
#define _ST7789H_

#if ARDUINO >= 100
 #include "Arduino.h"
 #include "Print.h"
#else
 #include "WProgram.h"
#endif
#include "ERGFX.h"

#ifdef __AVR__
 #include <avr/pgmspace.h>
#endif


#define ST7789_TFTWIDTH  76
#define ST7789_TFTHEIGHT 284

#define ST7789_CASET   0x2A
#define ST7789_PASET   0x2B
#define ST7789_RAMWR   0x2C


// Color definitions
#define	ST7789_BLACK   0x0000
#define	ST7789_BLUE    0x001F
#define	ST7789_RED     0xF800
#define	ST7789_GREEN   0x07E0
#define ST7789_CYAN    0x07FF
#define ST7789_MAGENTA 0xF81F
#define ST7789_YELLOW  0xFFE0  
#define ST7789_WHITE   0xFFFF


class ST7789 : public ERGFX {

 public:

  ST7789(int8_t _CS, int8_t _DC, int8_t _MOSI, int8_t _SCLK,
		   int8_t _RST);
  ST7789(int8_t _CS, int8_t _DC, int8_t _RST = -1);

  void     begin(void),
           setAddrWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1),
           pushColor(uint16_t color),
           fillScreen(uint16_t color),
           drawPixel(int16_t x, int16_t y, uint16_t color),
           drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color),
           drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color),
           fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
             uint16_t color),
           bmp16(uint16_t x, uint16_t y, const uint8_t *pBmp, uint16_t chWidth, uint16_t chHeight);      
  uint16_t color565(uint8_t r, uint8_t g, uint8_t b);

  /* These are not for current use, 8-bit protocol only! */
  uint8_t  readdata(void),
    readcommand8(uint8_t reg, uint8_t index = 0);
  /*
  uint16_t readcommand16(uint8_t);
  uint32_t readcommand32(uint8_t);
  void     dummyclock(void);
  */  

  void     spiwrite(uint8_t),
    writecommand(uint8_t c),
    writedata(uint8_t d),
    commandList(uint8_t *addr);
  uint8_t  spiread(void);

 private:
  uint8_t  tabcolor;



  boolean  hwSPI;
#if defined (__AVR__) || defined(TEENSYDUINO)

  uint8_t mySPCR;
  volatile uint8_t *mosiport, *clkport, *dcport, *rsport, *csport;
  int8_t  _cs, _dc, _rst, _mosi, _sclk;
  uint8_t  mosipinmask, clkpinmask, cspinmask, dcpinmask;
  
#elif defined (__arm__)
    volatile RwReg *mosiport, *clkport, *dcport, *rsport, *csport;
    uint32_t  _cs, _dc, _rst, _mosi, _sclk;
    uint32_t  mosipinmask, clkpinmask, cspinmask, dcpinmask;


#elif defined(ESP32)

   volatile uint32_t  *mosiport, *clkport, *dcport, *rsport, *csport;
    uint32_t  _cs, _dc, _rst, _mosi, _sclk;
    uint32_t  mosipinmask, clkpinmask, cspinmask, dcpinmask;
#endif
};

#endif
