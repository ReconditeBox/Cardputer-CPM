#ifndef CARDPUTER_ADV_H
#define CARDPUTER_ADV_H

/*
 * RunCPM hardware definition
 * M5Stack Cardputer Adv
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

/*
 * RunCPM's standard Arduino abstraction expects an
 * activity LED to exist.
 *
 * We do not want to assign real Cardputer hardware
 * to this yet, so 255 acts as a compile-time dummy.
 *
 * This will be removed when we write the proper
 * Cardputer abstraction layer.
 */
#define LED 255
#define LEDinv 0

#define BOARD "M5Stack Cardputer Adv"

#define board_esp32
#define board_digital_io


/*
 * ESP32-specific custom BDOS hook.
 * No Cardputer-specific functions yet.
 */
uint8 esp32bdos(uint16 dmaaddr)
{
    (void)dmaaddr;
    return 0x00;
}

#endif