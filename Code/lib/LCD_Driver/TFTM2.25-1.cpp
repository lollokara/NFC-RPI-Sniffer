/*************************************************** 
  www.buydisplay.com
 ****************************************************/

#include "TFTM2.25-1.h"
#include <limits.h>
#include "pins_arduino.h"
#include "wiring_private.h"
#include <SPI.h>
 #include "Arduino.h"

// Constructor when using software SPI.  All output pins are configurable.
ST7789::ST7789(int8_t cs, int8_t dc, int8_t mosi,
				   int8_t sclk, int8_t rst) : ERGFX(ST7789_TFTWIDTH, ST7789_TFTHEIGHT) {
  _cs   = cs;
  _dc   = dc;
  _mosi  = mosi;
  _sclk = sclk;
  _rst  = rst;
  hwSPI = false;
}


// Constructor when using hardware SPI.  Faster, but must use SPI pins
// specific to each board type (e.g. 11,13 for Uno, 51,52 for Mega, etc.)
ST7789::ST7789(int8_t cs, int8_t dc, int8_t rst) : ERGFX(ST7789_TFTWIDTH, ST7789_TFTHEIGHT) {
  _cs   = cs;
  _dc   = dc;
  _rst  = rst;
  hwSPI = true;
  _mosi  = _sclk = 0;
}

void ST7789::spiwrite(uint8_t c) {

  //Serial.print("0x"); Serial.print(c, HEX); Serial.print(", ");

  if (hwSPI) {
#if defined (__AVR__)
      uint8_t backupSPCR = SPCR;
    SPCR = mySPCR;
    SPDR = c;
    while(!(SPSR & _BV(SPIF)));
    SPCR = backupSPCR;
#elif defined(ESP32)|| defined(TEENSYDUINO)
    SPI.transfer(c);
#elif defined (__arm__)
    SPI.setClockDivider(11); // 8-ish MHz (full! speed!)
    SPI.setBitOrder(MSBFIRST);
    SPI.setDataMode(SPI_MODE0);
    SPI.transfer(c);
#endif
  } else {
    // Fast SPI bitbang swiped from LPD8806 library
    for(uint8_t bit = 0x80; bit; bit >>= 1) {
      if(c & bit) {
	//digitalWrite(_mosi, HIGH); 
	*mosiport |=  mosipinmask;
      } else {
	//digitalWrite(_mosi, LOW); 
	*mosiport &= ~mosipinmask;
      }
      //digitalWrite(_sclk, LOW);
      *clkport &= ~clkpinmask;
       //digitalWrite(_sclk, HIGH);
      *clkport |=  clkpinmask;
    }
  }
}


void ST7789::writecommand(uint8_t c) {
  *dcport &=  ~dcpinmask;
  //digitalWrite(_dc, LOW);
  //*clkport &= ~clkpinmask; // clkport is a NULL pointer when hwSPI==true
  //digitalWrite(_sclk, LOW);
  *csport &= ~cspinmask;
  //digitalWrite(_cs, LOW);

  spiwrite(c);

  *csport |= cspinmask;
  //digitalWrite(_cs, HIGH);
}


void ST7789::writedata(uint8_t c) {
  *dcport |=  dcpinmask;
  //digitalWrite(_dc, HIGH);
  //*clkport &= ~clkpinmask; // clkport is a NULL pointer when hwSPI==true
  //digitalWrite(_sclk, LOW);
  *csport &= ~cspinmask;
  //digitalWrite(_cs, LOW);
  
  spiwrite(c);

  //digitalWrite(_cs, HIGH);
  *csport |= cspinmask;
} 
/*
// If the SPI library has transaction support, these functions
// establish settings and protect from interference from other
// libraries.  Otherwise, they simply do nothing.
#ifdef SPI_HAS_TRANSACTION
static inline void spi_begin(void) __attribute__((always_inline));
static inline void spi_begin(void) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
}
static inline void spi_end(void) __attribute__((always_inline));
static inline void spi_end(void) {
  SPI.endTransaction();
}
#else
#define spi_begin()
#define spi_end()
#endif
*/

void ST7789::begin(void) {
  if (_rst > 0) {
    pinMode(_rst, OUTPUT);
    digitalWrite(_rst, LOW);
  }

  pinMode(_dc, OUTPUT);
  pinMode(_cs, OUTPUT);
  
  csport    = portOutputRegister(digitalPinToPort(_cs));
  cspinmask = digitalPinToBitMask(_cs);
  dcport    = portOutputRegister(digitalPinToPort(_dc));
  dcpinmask = digitalPinToBitMask(_dc);

  if(hwSPI) { // Using hardware SPI
#if defined (__AVR__)
    SPI.begin();
    SPI.setClockDivider(SPI_CLOCK_DIV2); // 8 MHz (full! speed!)
    SPI.setBitOrder(MSBFIRST);
    SPI.setDataMode(SPI_MODE0);
    mySPCR = SPCR;
#elif defined(ESP32)
    // SPI.begin(); // Do not re-initialize SPI as it is done in main code with custom pins
    SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
#elif defined(TEENSYDUINO)
    SPI.begin();
    SPI.setClockDivider(SPI_CLOCK_DIV2); // 8 MHz (full! speed!)
    SPI.setBitOrder(MSBFIRST);
    SPI.setDataMode(SPI_MODE0);
#elif defined (__arm__)
      SPI.begin();
      SPI.setClockDivider(11); // 8-ish MHz (full! speed!)
      SPI.setBitOrder(MSBFIRST);
      SPI.setDataMode(SPI_MODE0);
#endif
  } else {
    pinMode(_sclk, OUTPUT);
    pinMode(_mosi, OUTPUT);

    clkport     = portOutputRegister(digitalPinToPort(_sclk));
    clkpinmask  = digitalPinToBitMask(_sclk);
    mosiport    = portOutputRegister(digitalPinToPort(_mosi));
    mosipinmask = digitalPinToBitMask(_mosi);
    *clkport   &= ~clkpinmask;
    *mosiport  &= ~mosipinmask;
  }

  // toggle RST low to reset
  if (_rst > 0) {
    digitalWrite(_rst, HIGH);
    delay(5);
    digitalWrite(_rst, LOW);
    delay(20);
    digitalWrite(_rst, HIGH);
    delay(150);
  }

  // if (hwSPI) SPI.begin(); // Removed to avoid pin reset

 
	writecommand(0xB2);
	writedata(0x0C);
	writedata(0x0C);
	writedata(0x00);
	writedata(0x33);
	writedata(0x33);

	writecommand(0xB0);
	writedata(0x00);
	writedata(0xE0);

	writecommand(0x36);
        writedata(0x70); // Landscape (Rotation 3: MV=1, MX=1, MY=1)
        
	writecommand(0x3A);
	writedata(0x05);

	writecommand(0xB7);
	writedata(0x45);

	writecommand(0xBB);
	writedata(0x1D);

	writecommand(0xC0);
	writedata(0x2C);

	writecommand(0xC2);
	writedata(0x01);

	writecommand(0xC3);
	writedata(0x19);

	writecommand(0xC4);
	writedata(0x20);

	writecommand(0xC6);
	writedata(0x0F);

	writecommand(0xD0);
	writedata(0xA4);
	writedata(0xA1);

	writecommand(0xD6);
	writedata(0xA1);

	writecommand(0xE0);
	writedata(0xD0);
	writedata(0x10);
	writedata(0x21);
	writedata(0x14);
	writedata(0x15);
	writedata(0x2D);
	writedata(0x41);
	writedata(0x44);
	writedata(0x4F);
	writedata(0x28);
	writedata(0x0E);
	writedata(0x0C);
	writedata(0x1D);
	writedata(0x1F);

	writecommand(0xE1);
	writedata(0xD0);
	writedata(0x0F);
	writedata(0x1B);
	writedata(0x0D);
	writedata(0x0D);
	writedata(0x26);
	writedata(0x42);
	writedata(0x54);
	writedata(0x50);
	writedata(0x3E);
	writedata(0x1A);
	writedata(0x18);
	writedata(0x22);
	writedata(0x25);

        writecommand(0x11);    //Exit Sleep 
      //  if (hwSPI) spi_end();
        delay(120); 		
      //  if (hwSPI) spi_begin();
        writecommand(0x29);    //Display on 
      //  if (hwSPI) spi_end();

}


void ST7789::setAddrWindow(uint16_t x0, uint16_t y0, uint16_t x1,
 uint16_t y1) {
 x0+=0x12; x1+=0x12;   y0+=0x52; y1+=0x52; // Offsets swapped for Landscape
  writecommand(ST7789_CASET); // Column addr set
  writedata(x0 >> 8);
  writedata(x0);     // XSTART 
  writedata(x1 >> 8);
  writedata(x1);     // XEND

  writecommand(ST7789_PASET); // Row addr set
  writedata(y0>>8);
  writedata(y0);     // YSTART
  writedata(y1>>8);
  writedata(y1);     // YEND

  writecommand(ST7789_RAMWR); // write to RAM
}


void ST7789::pushColor(uint16_t color) {
  //if (hwSPI) spi_begin();
  //digitalWrite(_dc, HIGH);
  *dcport |=  dcpinmask;
  //digitalWrite(_cs, LOW);
  *csport &= ~cspinmask;

  spiwrite(color >> 8);
  spiwrite(color);

  *csport |= cspinmask;
  //digitalWrite(_cs, HIGH);
 // if (hwSPI) spi_end();
}

void ST7789::drawPixel(int16_t x, int16_t y, uint16_t color) {

  if((x < 0) ||(x >= _width) || (y < 0) || (y >= _height)) return;

  //if (hwSPI) spi_begin();
  setAddrWindow(x,y,x+1,y+1);

  //digitalWrite(_dc, HIGH);
  *dcport |=  dcpinmask;
  //digitalWrite(_cs, LOW);
  *csport &= ~cspinmask;

  spiwrite(color >> 8);
  spiwrite(color);

  *csport |= cspinmask;
  //digitalWrite(_cs, HIGH);
 // if (hwSPI) spi_end();
}


void ST7789::drawFastVLine(int16_t x, int16_t y, int16_t h,
 uint16_t color) {

  // Rudimentary clipping
  if((x >= _width) || (y >= _height)) return;

  if((y+h-1) >= _height) 
    h = _height-y;

  //if (hwSPI) spi_begin();
  setAddrWindow(x, y, x, y+h-1);

  uint8_t hi = color >> 8, lo = color;

  *dcport |=  dcpinmask;
  //digitalWrite(_dc, HIGH);
  *csport &= ~cspinmask;
  //digitalWrite(_cs, LOW);

  while (h--) {
    spiwrite(hi);
    spiwrite(lo);
  }
  *csport |= cspinmask;
  //digitalWrite(_cs, HIGH);
 // if (hwSPI) spi_end();
}


void ST7789::drawFastHLine(int16_t x, int16_t y, int16_t w,
  uint16_t color) {

  // Rudimentary clipping
  if((x >= _width) || (y >= _height)) return;
  if((x+w-1) >= _width)  w = _width-x;
  //if (hwSPI) spi_begin();
  setAddrWindow(x, y, x+w-1, y);

  uint8_t hi = color >> 8, lo = color;
  *dcport |=  dcpinmask;
  *csport &= ~cspinmask;
  //digitalWrite(_dc, HIGH);
  //digitalWrite(_cs, LOW);
  while (w--) {
    spiwrite(hi);
    spiwrite(lo);
  }
  *csport |= cspinmask;
  //digitalWrite(_cs, HIGH);
  //if (hwSPI) spi_end();
}

void ST7789::fillScreen(uint16_t color) {
  fillRect(0, 0,  _width, _height, color);
}

// fill a rectangle
void ST7789::fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
  uint16_t color) {

  // rudimentary clipping (drawChar w/big text requires this)
  if((x >= _width) || (y >= _height)) return;
  if((x + w - 1) >= _width)  w = _width  - x;
  if((y + h - 1) >= _height) h = _height - y;

 // if (hwSPI) spi_begin();
  setAddrWindow(x, y, x+w-1, y+h-1);

  uint8_t hi = color >> 8, lo = color;

  *dcport |=  dcpinmask;
  //digitalWrite(_dc, HIGH);
  *csport &= ~cspinmask;
  //digitalWrite(_cs, LOW);

  for(y=h; y>0; y--) {
    for(x=w; x>0; x--) {
      spiwrite(hi);
      spiwrite(lo);
    }
  }
  //digitalWrite(_cs, HIGH);
  *csport |= cspinmask;
  //if (hwSPI) spi_end();
}

void ST7789::bmp16(uint16_t x, uint16_t y, const uint8_t *pBmp, uint16_t chWidth, uint16_t chHeight) {
    uint16_t i, j;
    uint16_t hwColor;
     uint8_t hi , lo ;

  // rudimentary clipping (drawChar w/big text requires this)
  if((x >= _width) || (y >= _height)) return;
  if((x + chWidth - 1) >= _width)  chWidth = _width  - x;
  if((y + chHeight - 1) >= _height) chHeight = _height - y;

 // if (hwSPI) SPI.begin();
    setAddrWindow(x, y, x+chWidth-1, y+chHeight-1);

  *dcport |=  dcpinmask;
  //digitalWrite(_dc, HIGH);
  *csport &= ~cspinmask;
  //digitalWrite(_cs, LOW);

    for(j = 0; j < chHeight; j++) {
        for(i = 0; i < chWidth; i++) {
          hi= pgm_read_byte(pBmp++);lo= pgm_read_byte(pBmp++);
          spiwrite(hi);
          spiwrite(lo);
        }
    }
   //digitalWrite(_cs, HIGH);
  *csport |= cspinmask;
 // if (hwSPI) SPI.end();   
}

// Pass 8-bit (each) R,G,B, get back 16-bit packed color
uint16_t ST7789::color565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

 

