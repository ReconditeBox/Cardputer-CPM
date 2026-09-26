#include <Arduino.h>

/*
 * Arduino's ESP32 headers define NOP() as a macro.
 * RunCPM uses NOP as the Z80 opcode value 0x00.
 *
 * Remove the Arduino definition before including
 * the RunCPM core.
 */
#ifdef NOP
#undef NOP
#endif


/*
 * RunCPM CPU core.
 */
#define CPU "runcpm/cpu1.h"


/*
 * Main RunCPM configuration and global definitions.
 */
#include "runcpm/globals.h"


/*
 * SD card support.
 */
#include <SPI.h>

#define SDFAT_FILE_TYPE 1
#define DISABLE_FS_H_WARNING

#include <SdFat.h>


/*
 * Cardputer Adv hardware definition.
 */
#include "runcpm/hardware/esp32/cardputer_adv.h"


/*
 * Standard RunCPM Arduino abstraction.
 *
 * Later we will replace this with our own
 * Cardputer-specific abstraction which uses:
 *
 *   Cardputer keyboard
 *   Cardputer display
 *   Telnet/VT100
 */
#include "runcpm/abstraction_arduino.h"


/*
 * RunCPM auxiliary devices.
 *
 * globals.h enables these in the normal
 * Arduino configuration.
 */

#ifdef USE_PUN

File pun_dev;
int pun_open = FALSE;

#endif


#ifdef USE_LST

File lst_dev;
int lst_open = FALSE;

#endif


/*
 * RunCPM core.
 */

#include "runcpm/ram.h"

#include "runcpm/console.h"

#include CPU

#include "runcpm/disk.h"

#include "runcpm/host.h"

#include "runcpm/cpm.h"


#ifdef CCP_INTERNAL

#include "runcpm/ccp.h"

#endif


/*
 * This is currently only a compile test.
 *
 * We are proving that the complete RunCPM core
 * can be compiled for the Cardputer Adv's
 * ESP32-S3 toolchain.
 */

void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("--------------------------------");
    Serial.println("Cardputer Adv RunCPM compile test");
    Serial.println("--------------------------------");
    Serial.println();
    Serial.println("RunCPM core compiled.");
}


void loop()
{
    delay(1000);
}