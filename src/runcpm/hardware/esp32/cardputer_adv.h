#ifndef CARDPUTER_ADV_H
#define CARDPUTER_ADV_H

/*
 * RunCPM hardware definition for M5Stack Cardputer Adv
 *
 * microSD:
 *   SCK  = GPIO 40
 *   MISO = GPIO 39
 *   MOSI = GPIO 14
 *   CS   = GPIO 12
 */

SdFat SD;

#define SPIINIT 40,39,14,12
#define SDMHZ 20
#define SDINIT 12, SD_SCK_MHZ(SDMHZ)

#define BOARD "M5Stack Cardputer Adv"

#define board_esp32
#define board_digital_io

/*
 * RunCPM Arduino abstraction expects an LED definition.
 * GPIO 2 is harmless for our current use.
 */
#define LED 2
#define LEDinv 0


/*
 * Implemented in main.cpp.
 *
 * RunCPM BDOS function 232 calls esp32bdos(DE).
 */
uint8 cardputerEsp32Bdos(uint16 value);


uint8 esp32bdos(uint16 value)
{
    return cardputerEsp32Bdos(value);
}

#endif