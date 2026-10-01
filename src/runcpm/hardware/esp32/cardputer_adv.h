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
 * Temporary RunCPM activity output.
 * GPIO2 is exposed on the Cardputer expansion connector and
 * is unused by our present setup.
 */
#define LED 2
#define LEDinv 0

uint8 esp32bdos(uint16 dmaaddr)
{
    (void)dmaaddr;
    return 0x00;
}

#endif