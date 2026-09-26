#ifndef GIGA_H
#define GIGA_H

#include <SPI.h>

// Arduino Giga R1 with Wireless SD shield
// IMPORTANT: ICSP connector uses SPI1, not default SPI
#define SDMHZ 25
#define SD_CS_PIN 10
#define LED LED_BUILTIN
#define LEDinv 1
#define board_analog_io
#define board_digital_io
SdFat SD;
// Use SPI1 explicitly for ICSP connector
#define SDINIT SdSpiConfig(SD_CS_PIN, DEDICATED_SPI, SD_SCK_MHZ(SDMHZ), &SPI)
#define BOARD "Arduino Giga R1"
#define board_giga

#endif
