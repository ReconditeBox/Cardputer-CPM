#include <Arduino.h>
#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"

#include "cardputer_adv_keyboard.h"

#ifdef NOP
#undef NOP
#endif

#define CPU "runcpm/cpu1.h"

#include "runcpm/globals.h"

#include <SPI.h>

#define SDFAT_FILE_TYPE 1
#define DISABLE_FS_H_WARNING

#include <SdFat.h>

#include "runcpm/hardware/esp32/cardputer_adv.h"


/*
 * ====================================================
 * Rename RunCPM's original serial console functions.
 * ====================================================
 */

#define _kbhit  _serial_kbhit
#define _getch  _serial_getch
#define _getche _serial_getche
#define _putch  _serial_putch
#define _clrscr _serial_clrscr

#include "runcpm/abstraction_arduino.h"

#undef _kbhit
#undef _getch
#undef _getche
#undef _putch
#undef _clrscr


/*
 * ====================================================
 * Cardputer console router
 * ====================================================
 */

enum CardConsoleMode
{
    CARD_CONSOLE_LOCAL  = 0,
    CARD_CONSOLE_USB    = 1,
    CARD_CONSOLE_BOTH   = 2,
    CARD_CONSOLE_TELNET = 3
};

/*
 * IMPORTANT:
 *
 * RunCPM itself already has a variable called
 * consoleMode, so ours deliberately has a different name.
 */
static uint8_t cardConsoleMode =
    CARD_CONSOLE_LOCAL;


/*
 * ====================================================
 * Telnet console
 * ====================================================
 */

#define TELNET_DEFAULT_PORT 23

static WiFiServer *telnetServer =
    NULL;

static uint16_t telnetPort =
    TELNET_DEFAULT_PORT;

static WiFiClient telnetClient;

static IPAddress telnetRemoteIP;
static bool telnetRemoteIPValid = false;

static bool telnetServerStarted = false;
static bool lastTelnetWasCR = false;

enum TelnetInputState
{
    TELNET_DATA = 0,
    TELNET_IAC,
    TELNET_OPTION,
    TELNET_SUBNEGOTIATION,
    TELNET_SUBNEGOTIATION_IAC
};

static uint8_t telnetInputState =
    TELNET_DATA;

static uint8_t telnetIacCommand = 0;

#include "cardputer_ftpd.h"


/*
 * ====================================================
 * Local Cardputer VT100 terminal
 *
 * Logical screen: 80 x 24
 * Physical viewport: 40 x 16
 * ====================================================
 */

#define TERM_COLS 80
#define TERM_ROWS 24

#define VIEW_COLS 40
#define VIEW_ROWS 16

#define VIEW_PAN_X_STEP 8
#define VIEW_PAN_Y_STEP 4

#define CHAR_W 6
#define CHAR_H 8

M5Canvas terminal(&M5Cardputer.Display);

static char termBuffer[TERM_ROWS][TERM_COLS];
static bool termReverse[TERM_ROWS][TERM_COLS];
static bool terminalReverseVideo = false;

static int cursorX = 0;
static int cursorY = 0;

static int savedCursorX = 0;
static int savedCursorY = 0;

static int viewportX = 0;
static int viewportY = 0;

/*
 * LCD refresh batching.
 *
 * CP/M/VT100 output updates the logical 80x24 buffer immediately.
 * The physical LCD is refreshed at most once every 20 ms (50 Hz).
 */
#define LCD_REFRESH_MS 20

static bool terminalDirty = false;
static uint32_t terminalLastRefresh = 0;

/*
 * VT100 cursor-key mode.
 *
 * false: ESC [ A/B/C/D
 * true : ESC O A/B/C/D
 */
static bool applicationCursorKeys = false;


/*
 * ANSI / VT100 parser.
 */
enum
{
    ANSI_NORMAL,
    ANSI_ESC,
    ANSI_CSI
};

static int ansiState = ANSI_NORMAL;

static int ansiParam[4];
static int ansiParamIndex = 0;
static bool ansiPrivate = false;


/*
 * Input queue.
 */

static uint8_t keyBuffer[128];

static uint8_t keyHead = 0;
static uint8_t keyTail = 0;

static bool lastUSBWasCR = false;


/*
 * Forward declarations.
 */

int _kbhit(void);
uint8 _getch(void);
uint8 _getche(void);
void _putch(uint8 ch);
void _clrscr(void);

static void setCardConsoleMode(uint8_t mode);
static void telnetResetInputState();
static void telnetFormatRemoteIP(char *buffer, size_t bufferSize);
static void cardputerCycleSystemMode();


/*
 * ====================================================
 * Console routing helpers
 * ====================================================
 */

static bool localOutputEnabled()
{
    return (
        cardConsoleMode == CARD_CONSOLE_LOCAL ||
        cardConsoleMode == CARD_CONSOLE_BOTH
    );
}


static bool localInputEnabled()
{
    return (
        cardConsoleMode == CARD_CONSOLE_LOCAL ||
        cardConsoleMode == CARD_CONSOLE_BOTH
    );
}


static bool usbOutputEnabled()
{
    return (
        cardConsoleMode == CARD_CONSOLE_USB ||
        cardConsoleMode == CARD_CONSOLE_BOTH
    );
}


static bool usbInputEnabled()
{
    return (
        cardConsoleMode == CARD_CONSOLE_USB ||
        cardConsoleMode == CARD_CONSOLE_BOTH
    );
}


static bool telnetOutputEnabled()
{
    return (
        cardConsoleMode ==
        CARD_CONSOLE_TELNET
    );
}


static bool telnetInputEnabled()
{
    return (
        cardConsoleMode ==
        CARD_CONSOLE_TELNET
    );
}


/*
 * ====================================================
 * Viewport helpers
 * ====================================================
 */

static int terminalMaxViewportX()
{
    return TERM_COLS - VIEW_COLS;
}


static int terminalMaxViewportY()
{
    return TERM_ROWS - VIEW_ROWS;
}


static void terminalClampViewport()
{
    if (viewportX < 0)
    {
        viewportX = 0;
    }

    if (viewportY < 0)
    {
        viewportY = 0;
    }

    int maxX = terminalMaxViewportX();
    int maxY = terminalMaxViewportY();

    if (viewportX > maxX)
    {
        viewportX = maxX;
    }

    if (viewportY > maxY)
    {
        viewportY = maxY;
    }
}


static bool terminalEnsureCursorVisible()
{
    int oldX = viewportX;
    int oldY = viewportY;

    if (cursorX < viewportX)
    {
        viewportX = cursorX;
    }
    else if (cursorX >= viewportX + VIEW_COLS)
    {
        viewportX =
            cursorX - VIEW_COLS + 1;
    }

    if (cursorY < viewportY)
    {
        viewportY = cursorY;
    }
    else if (cursorY >= viewportY + VIEW_ROWS)
    {
        viewportY =
            cursorY - VIEW_ROWS + 1;
    }

    terminalClampViewport();

    return (
        oldX != viewportX ||
        oldY != viewportY
    );
}


/*
 * ====================================================
 * Local display
 * ====================================================
 */

static void terminalClearBuffer()
{
    for (int row = 0; row < TERM_ROWS; row++)
    {
        for (int col = 0; col < TERM_COLS; col++)
        {
            termBuffer[row][col] = ' ';
            termReverse[row][col] = false;
        }
    }
}


static void terminalRenderAll(bool force = false)
{
    if (!force && !localOutputEnabled())
    {
        return;
    }

    terminal.fillSprite(BLACK);

    for (int screenRow = 0; screenRow < VIEW_ROWS; screenRow++)
    {
        int logicalRow =
            viewportY + screenRow;

        terminal.setCursor(
            0,
            screenRow * CHAR_H
        );

        for (int screenCol = 0; screenCol < VIEW_COLS; screenCol++)
        {
            int logicalCol =
                viewportX + screenCol;

            if (
                termReverse[logicalRow][logicalCol]
            )
            {
                terminal.setTextColor(
                    BLACK,
                    GREEN
                );
            }
            else
            {
                terminal.setTextColor(
                    GREEN,
                    BLACK
                );
            }

            terminal.write(
                (uint8_t)termBuffer[logicalRow][logicalCol]
            );
        }
    }

    terminal.setTextColor(
        GREEN,
        BLACK
    );

    terminal.pushSprite(0, 0);

    terminalDirty = false;
    terminalLastRefresh = millis();
}


static void terminalMarkDirty()
{
    terminalDirty = true;
}


static void terminalMaybeRefresh()
{
    if (
        !terminalDirty ||
        !localOutputEnabled()
    )
    {
        return;
    }

    uint32_t now = millis();

    if (
        (uint32_t)(
            now - terminalLastRefresh
        ) >= LCD_REFRESH_MS
    )
    {
        terminalRenderAll();
    }
}


static void terminalRenderCell(
    int logicalCol,
    int logicalRow
)
{
    /*
     * The logical screen buffer is authoritative.
     * Do not push the 240x135 sprite for each character;
     * simply schedule a batched LCD refresh.
     */
    (void)logicalCol;
    (void)logicalRow;

    terminalMarkDirty();
}


static void terminalFollowCursor()
{
    if (terminalEnsureCursorVisible())
    {
        terminalMarkDirty();
    }
}


static void terminalPanViewport(
    int deltaX,
    int deltaY
)
{
    viewportX += deltaX;
    viewportY += deltaY;

    terminalClampViewport();

    if (localOutputEnabled())
    {
        terminalRenderAll(true);
    }
    else
    {
        terminalMarkDirty();
    }
}


static void terminalInit()
{
    M5Cardputer.Display.setRotation(1);

    M5Cardputer.Display.fillScreen(BLACK);

    terminal.setColorDepth(8);

    terminal.createSprite(
        M5Cardputer.Display.width(),
        M5Cardputer.Display.height()
    );

    terminal.fillSprite(BLACK);

    terminal.setTextColor(
        GREEN,
        BLACK
    );

    terminal.setTextSize(1);
    terminal.setTextWrap(false);

    cursorX = 0;
    cursorY = 0;

    savedCursorX = 0;
    savedCursorY = 0;

    viewportX = 0;
    viewportY = 0;

    applicationCursorKeys = false;
    terminalReverseVideo = false;

    ansiState = ANSI_NORMAL;
    ansiPrivate = false;

    terminalClearBuffer();
    terminalRenderAll(true);
}


/*
 * ====================================================
 * Logical-screen scrolling
 * ====================================================
 */

static void terminalScroll()
{
    for (int row = 0; row < TERM_ROWS - 1; row++)
    {
        memcpy(
            termBuffer[row],
            termBuffer[row + 1],
            TERM_COLS
        );

        memcpy(
            termReverse[row],
            termReverse[row + 1],
            TERM_COLS * sizeof(bool)
        );
    }

    for (int col = 0; col < TERM_COLS; col++)
    {
        termBuffer[TERM_ROWS - 1][col] = ' ';
        termReverse[TERM_ROWS - 1][col] = false;
    }

    cursorY = TERM_ROWS - 1;

    terminalMarkDirty();
}


static void terminalCheckCursor()
{
    if (cursorX < 0)
    {
        cursorX = 0;
    }

    if (cursorX >= TERM_COLS)
    {
        cursorX = 0;
        cursorY++;
    }

    if (cursorY < 0)
    {
        cursorY = 0;
    }

    if (cursorY >= TERM_ROWS)
    {
        terminalScroll();
    }
}


/*
 * ====================================================
 * Character operations
 * ====================================================
 */

static void terminalPutPrintable(uint8_t ch)
{
    terminalCheckCursor();
    terminalFollowCursor();

    termBuffer[cursorY][cursorX] =
        (char)ch;

    termReverse[cursorY][cursorX] =
        terminalReverseVideo;

    terminalRenderCell(
        cursorX,
        cursorY
    );

    cursorX++;

    terminalCheckCursor();
    terminalFollowCursor();
}


static void terminalCR()
{
    cursorX = 0;
    terminalFollowCursor();
}


static void terminalLF()
{
    cursorY++;

    terminalCheckCursor();
    terminalFollowCursor();
}


static void terminalBackspace()
{
    if (cursorX > 0)
    {
        cursorX--;
    }
    else if (cursorY > 0)
    {
        cursorY--;
        cursorX = TERM_COLS - 1;
    }

    terminalFollowCursor();
}


static void terminalTab()
{
    int next =
        ((cursorX / 8) + 1) * 8;

    if (next >= TERM_COLS)
    {
        cursorX = 0;
        cursorY++;
    }
    else
    {
        cursorX = next;
    }

    terminalCheckCursor();
    terminalFollowCursor();
}


/*
 * ====================================================
 * ANSI / VT100 support
 * ====================================================
 */

static int ansiGetParam(
    int index,
    int defaultValue
)
{
    if (index > ansiParamIndex)
    {
        return defaultValue;
    }

    if (ansiParam[index] == 0)
    {
        return defaultValue;
    }

    return ansiParam[index];
}


static void terminalClearToEnd()
{
    for (
        int row = cursorY;
        row < TERM_ROWS;
        row++
    )
    {
        int start =
            (row == cursorY)
                ? cursorX
                : 0;

        for (
            int col = start;
            col < TERM_COLS;
            col++
        )
        {
            termBuffer[row][col] = ' ';
            termReverse[row][col] = false;
        }
    }

    terminalMarkDirty();
}


static void terminalClearFromStart()
{
    for (
        int row = 0;
        row <= cursorY;
        row++
    )
    {
        int end =
            (row == cursorY)
                ? cursorX
                : TERM_COLS - 1;

        for (
            int col = 0;
            col <= end;
            col++
        )
        {
            termBuffer[row][col] = ' ';
            termReverse[row][col] = false;
        }
    }

    terminalMarkDirty();
}


static void terminalEraseLine(int mode)
{
    if (mode == 0)
    {
        for (
            int col = cursorX;
            col < TERM_COLS;
            col++
        )
        {
            termBuffer[cursorY][col] = ' ';
            termReverse[cursorY][col] = false;
        }
    }
    else if (mode == 1)
    {
        for (
            int col = 0;
            col <= cursorX &&
            col < TERM_COLS;
            col++
        )
        {
            termBuffer[cursorY][col] = ' ';
            termReverse[cursorY][col] = false;
        }
    }
    else if (mode == 2)
    {
        for (
            int col = 0;
            col < TERM_COLS;
            col++
        )
        {
            termBuffer[cursorY][col] = ' ';
            termReverse[cursorY][col] = false;
        }
    }

    terminalMarkDirty();
}


static void ansiExecute(uint8_t command)
{
    int amount;

    switch (command)
    {
        case 'A':

            amount =
                ansiGetParam(0, 1);

            cursorY -= amount;

            if (cursorY < 0)
            {
                cursorY = 0;
            }

            terminalFollowCursor();
            break;


        case 'B':

            amount =
                ansiGetParam(0, 1);

            cursorY += amount;

            if (cursorY >= TERM_ROWS)
            {
                cursorY =
                    TERM_ROWS - 1;
            }

            terminalFollowCursor();
            break;


        case 'C':

            amount =
                ansiGetParam(0, 1);

            cursorX += amount;

            if (cursorX >= TERM_COLS)
            {
                cursorX =
                    TERM_COLS - 1;
            }

            terminalFollowCursor();
            break;


        case 'D':

            amount =
                ansiGetParam(0, 1);

            cursorX -= amount;

            if (cursorX < 0)
            {
                cursorX = 0;
            }

            terminalFollowCursor();
            break;


        case 'G':
        {
            int col =
                ansiGetParam(0, 1);

            cursorX = col - 1;

            if (cursorX < 0)
            {
                cursorX = 0;
            }

            if (cursorX >= TERM_COLS)
            {
                cursorX = TERM_COLS - 1;
            }

            terminalFollowCursor();
            break;
        }


        case 'd':
        {
            int row =
                ansiGetParam(0, 1);

            cursorY = row - 1;

            if (cursorY < 0)
            {
                cursorY = 0;
            }

            if (cursorY >= TERM_ROWS)
            {
                cursorY = TERM_ROWS - 1;
            }

            terminalFollowCursor();
            break;
        }


        case 'H':
        case 'f':
        {
            int row =
                ansiGetParam(0, 1);

            int col =
                ansiGetParam(1, 1);

            cursorY = row - 1;
            cursorX = col - 1;

            if (cursorY < 0)
            {
                cursorY = 0;
            }

            if (cursorY >= TERM_ROWS)
            {
                cursorY =
                    TERM_ROWS - 1;
            }

            if (cursorX < 0)
            {
                cursorX = 0;
            }

            if (cursorX >= TERM_COLS)
            {
                cursorX =
                    TERM_COLS - 1;
            }

            terminalFollowCursor();
            break;
        }


        case 'J':
        {
            int mode =
                ansiParam[0];

            if (mode == 2)
            {
                terminalClearBuffer();
                terminalMarkDirty();
            }
            else if (mode == 1)
            {
                terminalClearFromStart();
            }
            else
            {
                terminalClearToEnd();
            }

            break;
        }


        case 'K':

            terminalEraseLine(
                ansiParam[0]
            );

            break;


        case 's':

            savedCursorX = cursorX;
            savedCursorY = cursorY;

            break;


        case 'u':

            cursorX = savedCursorX;
            cursorY = savedCursorY;

            terminalCheckCursor();
            terminalFollowCursor();

            break;


        case 'h':

            if (
                ansiPrivate &&
                ansiParam[0] == 1
            )
            {
                applicationCursorKeys = true;
            }

            break;


        case 'l':

            if (
                ansiPrivate &&
                ansiParam[0] == 1
            )
            {
                applicationCursorKeys = false;
            }

            break;


        case 'm':
        {
            /*
             * Minimal SGR support used by CP/M applications and the
             * Lynx-style browser.  Keep colour handling deliberately
             * simple while modelling reverse video correctly.
             *
             *   0  reset attributes
             *   7  reverse video on
             *   27 reverse video off
             */
            for (
                int index = 0;
                index <= ansiParamIndex;
                index++
            )
            {
                int parameter =
                    ansiParam[index];

                if (parameter == 0)
                {
                    terminalReverseVideo =
                        false;
                }
                else if (parameter == 7)
                {
                    terminalReverseVideo =
                        true;
                }
                else if (parameter == 27)
                {
                    terminalReverseVideo =
                        false;
                }
            }

            break;
        }


        default:

            break;
    }
}


static void ansiBeginCSI()
{
    memset(
        ansiParam,
        0,
        sizeof(ansiParam)
    );

    ansiParamIndex = 0;
    ansiPrivate = false;

    ansiState = ANSI_CSI;
}


static void terminalProcessCharacter(
    uint8_t ch
)
{
    if (ansiState == ANSI_ESC)
    {
        ansiState = ANSI_NORMAL;

        if (ch == '[')
        {
            ansiBeginCSI();
            return;
        }

        if (ch == '7')
        {
            savedCursorX = cursorX;
            savedCursorY = cursorY;
            return;
        }

        if (ch == '8')
        {
            cursorX = savedCursorX;
            cursorY = savedCursorY;

            terminalCheckCursor();
            terminalFollowCursor();

            return;
        }

        if (ch == 'D')
        {
            terminalLF();
            return;
        }

        if (ch == 'E')
        {
            terminalCR();
            terminalLF();
            return;
        }

        if (ch == 'c')
        {
            terminalClearBuffer();

            cursorX = 0;
            cursorY = 0;

            savedCursorX = 0;
            savedCursorY = 0;

            viewportX = 0;
            viewportY = 0;

            applicationCursorKeys = false;
            terminalReverseVideo = false;

            terminalMarkDirty();

            return;
        }

        return;
    }


    if (ansiState == ANSI_CSI)
    {
        if (ch >= '0' && ch <= '9')
        {
            ansiParam[ansiParamIndex] =
                (
                    ansiParam[ansiParamIndex] *
                    10
                )
                +
                (ch - '0');

            return;
        }

        if (ch == ';')
        {
            if (ansiParamIndex < 3)
            {
                ansiParamIndex++;
            }

            return;
        }

        if (ch == '?')
        {
            ansiPrivate = true;
            return;
        }

        ansiExecute(ch);

        ansiState = ANSI_NORMAL;

        return;
    }


    if (ch == 0x1B)
    {
        ansiState = ANSI_ESC;
        return;
    }


    if (ch == 0x0C)
    {
        terminalClearBuffer();

        cursorX = 0;
        cursorY = 0;

        viewportX = 0;
        viewportY = 0;

        terminalMarkDirty();

        return;
    }


    if (ch == 0x0D)
    {
        terminalCR();
        return;
    }


    if (ch == 0x0A)
    {
        terminalLF();
        return;
    }


    if (ch == 0x08)
    {
        terminalBackspace();
        return;
    }


    if (ch == 0x09)
    {
        terminalTab();
        return;
    }


    if (ch == 0x07)
    {
        return;
    }


    if (ch >= 0x20)
    {
        terminalPutPrintable(ch);
    }
}


/*
 * ====================================================
 * Input queue
 * ====================================================
 */

static void queueKey(uint8_t ch)
{
    uint8_t next =
        (keyHead + 1) %
        sizeof(keyBuffer);

    if (next == keyTail)
    {
        return;
    }

    keyBuffer[keyHead] = ch;

    keyHead = next;
}


/*
 * ====================================================
 * USB status screen
 * ====================================================
 */

static void showUSBStatus()
{
    M5Cardputer.Display.fillScreen(
        BLACK
    );

    M5Cardputer.Display.setTextColor(
        GREEN,
        BLACK
    );

    M5Cardputer.Display.setTextSize(1);

    M5Cardputer.Display.setCursor(
        0,
        0
    );

    M5Cardputer.Display.println(
        "USB CONSOLE"
    );

    M5Cardputer.Display.println(
        "-----------"
    );

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        "CON: USB"
    );

    M5Cardputer.Display.println(
        "115200 8N1"
    );

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        "Fn+=  LOCAL"
    );
}


/*
 * ====================================================
 * Telnet status screen
 * ====================================================
 */

static void showTelnetStatus()
{
    M5Cardputer.Display.fillScreen(
        BLACK
    );

    M5Cardputer.Display.setTextColor(
        GREEN,
        BLACK
    );

    M5Cardputer.Display.setTextSize(1);

    M5Cardputer.Display.setCursor(
        0,
        0
    );

    M5Cardputer.Display.println(
        "TELNETD"
    );

    M5Cardputer.Display.println(
        "-------"
    );

    M5Cardputer.Display.println();

    IPAddress ip =
        WiFi.localIP();

    M5Cardputer.Display.printf(
        "Listen: %u.%u.%u.%u:%u\n",
        ip[0],
        ip[1],
        ip[2],
        ip[3],
        telnetPort
    );

    M5Cardputer.Display.println(
        "CON:    TELNET"
    );

    if (
        telnetClient &&
        telnetClient.connected()
    )
    {
        M5Cardputer.Display.println(
            "State:  connected"
        );

        if (telnetRemoteIPValid)
        {
            M5Cardputer.Display.printf(
                "Client: %u.%u.%u.%u\n",
                telnetRemoteIP[0],
                telnetRemoteIP[1],
                telnetRemoteIP[2],
                telnetRemoteIP[3]
            );
        }
    }
    else
    {
        M5Cardputer.Display.println(
            "State:  waiting"
        );
    }

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        "Fn+=  LOCAL"
    );

    M5Cardputer.Display.println(
        "G0    FTPD"
    );
}


/*
 * ====================================================
 * Change console route
 * ====================================================
 */

static void setCardConsoleMode(
    uint8_t mode
)
{
    if (
        mode != CARD_CONSOLE_LOCAL &&
        mode != CARD_CONSOLE_USB &&
        mode != CARD_CONSOLE_BOTH &&
        mode != CARD_CONSOLE_TELNET
    )
    {
        return;
    }


    if (
        mode ==
        CARD_CONSOLE_TELNET
    )
    {
        if (
            WiFi.status() !=
            WL_CONNECTED
        )
        {
            _puts(
                "\r\n"
                "[TELNET unavailable: WiFi is offline]"
                "\r\n"
            );

            return;
        }

        if (
            !telnetServerStarted ||
            !telnetServer
        )
        {
            _puts(
                "\r\n"
                "[TELNET unavailable: run TELNETD]"
                "\r\n"
            );

            return;
        }
    }


    uint8_t previousMode =
        cardConsoleMode;


    if (
        mode == CARD_CONSOLE_LOCAL &&
        (
            previousMode ==
                CARD_CONSOLE_USB ||
            previousMode ==
                CARD_CONSOLE_BOTH
        )
    )
    {
        Serial.print(
            "\r\n"
            "[Switching to LOCAL console]"
            "\r\n"
        );
    }


    bool leavingTelnet =
        previousMode ==
            CARD_CONSOLE_TELNET &&
        mode !=
            CARD_CONSOLE_TELNET;

    bool closeTelnetClient =
        leavingTelnet &&
        telnetClient &&
        telnetClient.connected();

    if (closeTelnetClient)
    {
        telnetClient.print(
            "\r\n"
            "[TELNET service closed]"
            "\r\n"
        );

        telnetClient.flush();
        delay(10);
        telnetClient.stop();
        telnetClient =
            WiFiClient();
        telnetResetInputState();
    }

    if (leavingTelnet)
    {
        if (telnetServer)
        {
            telnetServer->stop();
            delete telnetServer;
            telnetServer =
                NULL;
        }

        telnetServerStarted =
            false;
    }


    cardConsoleMode = mode;


    if (
        cardConsoleMode ==
        CARD_CONSOLE_LOCAL
    )
    {
        terminalRenderAll(true);
    }
    else if (
        cardConsoleMode ==
        CARD_CONSOLE_USB
    )
    {
        showUSBStatus();

        Serial.print(
            "\r\n"
            "[USB console active]"
            "\r\n"
        );

        Serial.print(
            "[Fn+= returns LOCAL]"
            "\r\n"
        );
    }
    else if (
        cardConsoleMode ==
        CARD_CONSOLE_BOTH
    )
    {
        terminalRenderAll(true);

        Serial.print(
            "\r\n"
            "[BOTH consoles active]"
            "\r\n"
        );
    }
    else
    {
        showTelnetStatus();

        if (
            telnetClient &&
            telnetClient.connected()
        )
        {
            telnetClient.print(
                "\r\n"
                "[TELNET console active]"
                "\r\n"
            );

            telnetClient.print(
                "[Fn+= on Cardputer returns LOCAL]"
                "\r\n"
            );
        }
    }

    if (closeTelnetClient)
    {
        char peer[32];
        char announcement[80];

        telnetFormatRemoteIP(
            peer,
            sizeof(peer)
        );

        snprintf(
            announcement,
            sizeof(announcement),
            "\r\n[TELNET client disconnected from %s]\r\n",
            peer
        );

        _puts(
            announcement
        );

        telnetRemoteIPValid =
            false;
    }
}


/*
 * ====================================================
 * RunCPM ESP32-specific BDOS hook
 *
 * Function 232
 *
 * DE = 0      LOCAL
 * DE = 1      USB
 * DE = 2      BOTH
 * DE = 3      TELNET
 * DE = FFFF   query
 * ====================================================
 */

uint8 cardputerEsp32Bdos(
    uint16 value
)
{
    if (value == 0xFFFF)
    {
        return cardConsoleMode;
    }


    uint8_t requested =
        (uint8_t)(
            value &
            0x00FF
        );


    if (
        requested <=
        CARD_CONSOLE_TELNET
    )
    {
        setCardConsoleMode(
            requested
        );

        return cardConsoleMode;
    }


    return 0xFF;
}


/*
 * ====================================================
 * Cardputer keyboard
 * ====================================================
 */

static uint64_t cardputerPhysicalKeyMask()
{
    uint64_t mask = 0;

    const auto &keys =
        M5Cardputer.Keyboard.keyList();

    for (const auto &key : keys)
    {
        if (
            key.x < 0 ||
            key.x >= 14 ||
            key.y < 0 ||
            key.y >= 4
        )
        {
            continue;
        }

        uint8_t bit =
            (uint8_t)(
                key.y * 14 +
                key.x
            );

        mask |=
            ((uint64_t)1 << bit);
    }

    return mask;
}


static bool cardputerAnyPhysicalKeyPressed()
{
    M5Cardputer.update();

    return (
        cardputerPhysicalKeyMask() !=
        0
    );
}


static void cardputerWaitForAllKeysReleased()
{
    while (true)
    {
        M5Cardputer.update();

        if (
            cardputerPhysicalKeyMask() ==
            0
        )
        {
            return;
        }

        delay(5);
    }
}


static void cardputerWaitForPhysicalKeyPress()
{
    cardputerWaitForAllKeysReleased();

    while (true)
    {
        M5Cardputer.update();

        if (
            cardputerPhysicalKeyMask() !=
            0
        )
        {
            break;
        }

        delay(5);
    }

    cardputerWaitForAllKeysReleased();
}


/*
 * Display /SPLASH.PNG from the SD card and wait for one physical
 * Cardputer keypress. If the file is absent or cannot be decoded,
 * boot simply continues normally.
 */
static void showBootSplash()
{
    static const char *SPLASH_PATH =
        "SPLASH.PNG";

    if (!SD.exists(SPLASH_PATH))
    {
        return;
    }

    File splash =
        SD.open(
            SPLASH_PATH,
            O_READ
        );

    if (!splash)
    {
        return;
    }

    uint32_t size =
        splash.size();

    if (size == 0)
    {
        splash.close();
        return;
    }

    uint8_t *data =
        (uint8_t *)malloc(size);

    if (!data)
    {
        splash.close();
        return;
    }

    uint32_t readCount =
        splash.read(
            data,
            size
        );

    splash.close();

    if (readCount != size)
    {
        free(data);
        return;
    }

    M5Cardputer.Display.fillScreen(
        BLACK
    );

    bool drawn =
        M5Cardputer.Display.drawPng(
            data,
            size,
            0,
            0,
            M5Cardputer.Display.width(),
            M5Cardputer.Display.height()
        );

    free(data);

    if (!drawn)
    {
        return;
    }

    cardputerWaitForPhysicalKeyPress();

    /*
     * Restore the logical terminal after the splash. The terminal
     * buffer remains authoritative while the PNG is displayed.
     */
    terminalRenderAll(true);
}



static bool cardputerPhysicalKeyDown(
    uint64_t mask,
    uint8_t x,
    uint8_t y
)
{
    if (
        x >= 14 ||
        y >= 4
    )
    {
        return false;
    }

    uint8_t bit =
        (uint8_t)(
            y * 14 +
            x
        );

    return (
        mask &
        ((uint64_t)1 << bit)
    ) != 0;
}


/*
 * ====================================================
 * Physical removable-media UI
 *
 * A: = Fn+A
 * B: = Fn+B
 *
 * This UI is deliberately reachable only from the
 * Cardputer keyboard. USB and Telnet input never call it.
 * ====================================================
 */

#define MEDIA_MENU_MAX_ITEMS 32

static char mediaMenuNames[MEDIA_MENU_MAX_ITEMS][CARDPUTER_MEDIA_NAME_MAX + 1];


enum MediaMenuKey
{
    MEDIA_KEY_NONE = 0,
    MEDIA_KEY_UP,
    MEDIA_KEY_DOWN,
    MEDIA_KEY_ENTER,
    MEDIA_KEY_CANCEL
};


static void restoreCardputerScreen()
{
    if (localOutputEnabled())
    {
        terminalRenderAll(true);
    }
    else if (
        cardConsoleMode ==
        CARD_CONSOLE_USB
    )
    {
        showUSBStatus();
    }
    else
    {
        showTelnetStatus();
    }
}


static void mediaWaitForKeyRelease()
{
    cardputerWaitForAllKeysReleased();
}


static MediaMenuKey mediaWaitForKey()
{
    uint64_t previousMask = 0;

    while (true)
    {
        M5Cardputer.update();

        uint64_t mask =
            cardputerPhysicalKeyMask();

        if (
            mask ==
            previousMask
        )
        {
            delay(5);
            continue;
        }

        previousMask =
            mask;

        if (mask == 0)
        {
            continue;
        }

        Keyboard_Class::KeysState status =
            M5Cardputer.Keyboard.keysState();

        if (status.up)
        {
            return MEDIA_KEY_UP;
        }

        if (status.down)
        {
            return MEDIA_KEY_DOWN;
        }

        if (status.enter)
        {
            return MEDIA_KEY_ENTER;
        }

        if (
            status.esc ||
            status.backspace
        )
        {
            return MEDIA_KEY_CANCEL;
        }
    }
}


static uint8_t mediaReadDirectoryList()
{
    uint8_t count = 0;

    File mediaRoot =
        SD.open(
            CARDPUTER_MEDIA_ROOT,
            O_READ
        );

    if (!mediaRoot)
    {
        return 0;
    }

    File entry;

    while (
        count < MEDIA_MENU_MAX_ITEMS &&
        (entry = mediaRoot.openNextFile())
    )
    {
        if (entry.isDirectory())
        {
            char name[CARDPUTER_MEDIA_NAME_MAX + 2];

            memset(
                name,
                0,
                sizeof(name)
            );

            entry.getName(
                name,
                sizeof(name)
            );

            size_t length =
                strlen(name);

            if (
                length > 0 &&
                length <= CARDPUTER_MEDIA_NAME_MAX &&
                name[0] != '.'
            )
            {
                strncpy(
                    mediaMenuNames[count],
                    name,
                    CARDPUTER_MEDIA_NAME_MAX
                );

                mediaMenuNames[count][CARDPUTER_MEDIA_NAME_MAX] =
                    0;

                count++;
            }
        }

        entry.close();
    }

    mediaRoot.close();

    /*
     * Keep chooser order deterministic rather than depending on
     * FAT directory-entry order.
     */
    for (
        uint8_t i = 0;
        i < count;
        i++
    )
    {
        for (
            uint8_t j = i + 1;
            j < count;
            j++
        )
        {
            if (
                strcmp(
                    mediaMenuNames[i],
                    mediaMenuNames[j]
                ) > 0
            )
            {
                char temp[CARDPUTER_MEDIA_NAME_MAX + 1];

                strcpy(
                    temp,
                    mediaMenuNames[i]
                );

                strcpy(
                    mediaMenuNames[i],
                    mediaMenuNames[j]
                );

                strcpy(
                    mediaMenuNames[j],
                    temp
                );
            }
        }
    }

    return count;
}


static void mediaInvalidateDrive(
    uint8_t drive
)
{
    if (drive >= 2)
    {
        return;
    }

    loginVector &=
        ~((uint16)1 << drive);

    roVector &=
        ~((uint16)1 << drive);

    allUsers =
        FALSE;

    allExtents =
        FALSE;

    if (rootdir)
    {
        rootdir.close();
    }

    if (userdir)
    {
        userdir.close();
    }
}


static bool mediaMountDirectory(
    uint8_t drive,
    const char *name
)
{
    if (
        drive >= 2 ||
        !name ||
        !name[0]
    )
    {
        return false;
    }

    char root[HOST_FILENAME_MAX];

    int written =
        snprintf(
            root,
            sizeof(root),
            "%s/%s",
            CARDPUTER_MEDIA_ROOT,
            name
        );

    if (
        written <= 0 ||
        (size_t)written >= sizeof(root)
    )
    {
        return false;
    }

    File dir =
        SD.open(
            root,
            O_READ
        );

    if (
        !dir ||
        !dir.isDirectory()
    )
    {
        if (dir)
        {
            dir.close();
        }

        return false;
    }

    dir.close();

    strncpy(
        cardputerMediaMount[drive],
        name,
        CARDPUTER_MEDIA_NAME_MAX
    );

    cardputerMediaMount[drive][CARDPUTER_MEDIA_NAME_MAX] =
        0;

    /*
     * A media directory is a RunCPM drive root. Ensure user area 0
     * exists so an otherwise empty newly-created medium is usable.
     */
    char userZero[HOST_FILENAME_MAX];

    written =
        snprintf(
            userZero,
            sizeof(userZero),
            "%s/0",
            root
        );

    if (
        written > 0 &&
        (size_t)written < sizeof(userZero)
    )
    {
        SD.mkdir(userZero);
    }

    mediaInvalidateDrive(
        drive
    );

    return true;
}


static void mediaEject(
    uint8_t drive
)
{
    if (drive >= 2)
    {
        return;
    }

    cardputerMediaMount[drive][0] =
        0;

    mediaInvalidateDrive(
        drive
    );
}


static void mediaDrawEjectPrompt(
    uint8_t drive
)
{
    M5Cardputer.Display.fillScreen(
        BLACK
    );

    M5Cardputer.Display.setTextColor(
        GREEN,
        BLACK
    );

    M5Cardputer.Display.setTextSize(1);

    M5Cardputer.Display.setCursor(
        0,
        0
    );

    M5Cardputer.Display.printf(
        "DRIVE %c:\n",
        'A' + drive
    );

    M5Cardputer.Display.println(
        "--------"
    );

    M5Cardputer.Display.println();

    M5Cardputer.Display.print(
        "Mounted: "
    );

    M5Cardputer.Display.println(
        cardputerMediaName(drive)
    );

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        "Eject this media?"
    );

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        "ENTER  eject"
    );

    M5Cardputer.Display.println(
        "ESC    cancel"
    );
}


static void mediaDrawChooser(
    uint8_t drive,
    uint8_t count,
    uint8_t selected
)
{
    const uint8_t firstRow = 5;
    const uint8_t visibleRows = 11;

    uint8_t first =
        0;

    if (
        selected >=
        visibleRows
    )
    {
        first =
            selected -
            visibleRows +
            1;
    }

    M5Cardputer.Display.fillScreen(
        BLACK
    );

    M5Cardputer.Display.setTextColor(
        GREEN,
        BLACK
    );

    M5Cardputer.Display.setTextSize(1);

    M5Cardputer.Display.setCursor(
        0,
        0
    );

    M5Cardputer.Display.printf(
        "INSERT DRIVE %c:\n",
        'A' + drive
    );

    M5Cardputer.Display.println(
        "Fn+arrows  ENTER  ESC"
    );

    M5Cardputer.Display.println(
        "----------------------"
    );

    if (count == 0)
    {
        M5Cardputer.Display.println();
        M5Cardputer.Display.println(
            "No directories in MEDIA/"
        );

        return;
    }

    for (
        uint8_t row = 0;
        row < visibleRows;
        row++
    )
    {
        uint8_t index =
            first + row;

        if (index >= count)
        {
            break;
        }

        int y =
            (firstRow + row) *
            CHAR_H;

        if (index == selected)
        {
            M5Cardputer.Display.fillRect(
                0,
                y,
                M5Cardputer.Display.width(),
                CHAR_H,
                GREEN
            );

            M5Cardputer.Display.setTextColor(
                BLACK,
                GREEN
            );
        }
        else
        {
            M5Cardputer.Display.setTextColor(
                GREEN,
                BLACK
            );
        }

        M5Cardputer.Display.setCursor(
            0,
            y
        );

        M5Cardputer.Display.print(
            index == selected
                ? "> "
                : "  "
        );

        M5Cardputer.Display.print(
            mediaMenuNames[index]
        );
    }

    M5Cardputer.Display.setTextColor(
        GREEN,
        BLACK
    );
}


static void managePhysicalMediaSlot(
    uint8_t drive
)
{
    if (drive >= 2)
    {
        return;
    }

    /*
     * Fn+A / Fn+B is still held when we enter. Do not let that
     * keystroke immediately act on the menu.
     */
    mediaWaitForKeyRelease();

    if (cardputerMediaMounted(drive))
    {
        mediaDrawEjectPrompt(
            drive
        );

        while (true)
        {
            MediaMenuKey key =
                mediaWaitForKey();

            if (
                key ==
                MEDIA_KEY_ENTER
            )
            {
                mediaEject(
                    drive
                );

                break;
            }

            if (
                key ==
                MEDIA_KEY_CANCEL
            )
            {
                break;
            }

            mediaWaitForKeyRelease();
        }

        mediaWaitForKeyRelease();
        restoreCardputerScreen();
        return;
    }

    uint8_t count =
        mediaReadDirectoryList();

    uint8_t selected =
        0;

    mediaDrawChooser(
        drive,
        count,
        selected
    );

    while (true)
    {
        MediaMenuKey key =
            mediaWaitForKey();

        if (
            key ==
            MEDIA_KEY_CANCEL
        )
        {
            break;
        }

        if (
            count &&
            key ==
            MEDIA_KEY_UP
        )
        {
            if (selected > 0)
            {
                selected--;
            }

            mediaDrawChooser(
                drive,
                count,
                selected
            );
        }
        else if (
            count &&
            key ==
            MEDIA_KEY_DOWN
        )
        {
            if (
                selected + 1 <
                count
            )
            {
                selected++;
            }

            mediaDrawChooser(
                drive,
                count,
                selected
            );
        }
        else if (
            count &&
            key ==
            MEDIA_KEY_ENTER
        )
        {
            mediaMountDirectory(
                drive,
                mediaMenuNames[selected]
            );

            break;
        }

        mediaWaitForKeyRelease();
    }

    mediaWaitForKeyRelease();
    restoreCardputerScreen();
}


static void pollCardputerKeyboard()
{
    static uint64_t previousKeyMask = 0;

    M5Cardputer.update();

    /*
     * G0 is the physical machine-mode button.
     *
     * At runtime it cycles:
     *   LOCAL -> TELNETD -> FTPD -> LOCAL
     *
     * This is deliberately handled before the keyboard matrix so G0
     * never becomes a CP/M keypress.
     */
    if (M5Cardputer.BtnA.wasPressed())
    {
        cardputerCycleSystemMode();
        return;
    }


    /*
     * Do NOT use Keyboard.isChange() here.
     *
     * The M5Cardputer library's isChange() only compares
     * the NUMBER of pressed keys. Different combinations
     * containing the same number of keys can therefore be
     * missed completely.
     *
     * Instead compare the exact 56-key physical matrix.
     */
    uint64_t currentKeyMask =
        cardputerPhysicalKeyMask();


    if (
        currentKeyMask ==
        previousKeyMask
    )
    {
        return;
    }


    previousKeyMask =
        currentKeyMask;


    /*
     * A change to no keys pressed is still important:
     * it updates previousKeyMask so the next press is seen.
     * There is simply no CP/M key event to generate here.
     */
    if (currentKeyMask == 0)
    {
        return;
    }


    Keyboard_Class::KeysState status =
        M5Cardputer.Keyboard.keysState();


    /*
     * Emergency return to LOCAL.
     *
     * The M5Cardputer library names the physical
     * Fn + = combination "F12".
     */
    if (
        status.fn &&
        status.f12
    )
    {
        setCardConsoleMode(
            CARD_CONSOLE_LOCAL
        );

        return;
    }


    /*
     * Physical-only removable drive controls.
     *
     * These are detected from exact matrix positions rather than
     * translated characters and are handled before console routing,
     * so they remain local even while USB or Telnet owns CON:.
     *
     * Fn+A: row 2, column 2
     * Fn+B: row 3, column 7
     */
    if (
        status.fn &&
        cardputerPhysicalKeyDown(
            currentKeyMask,
            2,
            2
        )
    )
    {
        managePhysicalMediaSlot(0);
        previousKeyMask = 0;
        return;
    }

    if (
        status.fn &&
        cardputerPhysicalKeyDown(
            currentKeyMask,
            7,
            3
        )
    )
    {
        managePhysicalMediaSlot(1);
        previousKeyMask = 0;
        return;
    }


    bool arrowPressed =
        status.up ||
        status.down ||
        status.left ||
        status.right;


    /*
     * Manual local viewport pan:
     *
     * Aa + Fn + arrow
     *
     * Horizontal movement is 8 logical columns per
     * key press; vertical movement is 4 rows.
     *
     * These keys are consumed locally and are NOT
     * passed to CP/M.
     */
    if (
        status.shift &&
        status.fn &&
        arrowPressed &&
        localOutputEnabled()
    )
    {
        if (status.left)
        {
            terminalPanViewport(
                -VIEW_PAN_X_STEP,
                0
            );
        }
        else if (status.right)
        {
            terminalPanViewport(
                VIEW_PAN_X_STEP,
                0
            );
        }
        else if (status.up)
        {
            terminalPanViewport(
                0,
                -VIEW_PAN_Y_STEP
            );
        }
        else if (status.down)
        {
            terminalPanViewport(
                0,
                VIEW_PAN_Y_STEP
            );
        }

        return;
    }


    /*
     * Cardputer keyboard does not feed CP/M
     * while USB exclusively owns CON:.
     */
    if (!localInputEnabled())
    {
        return;
    }


    /*
     * Normal Cardputer arrow keys are Fn-layer keys.
     *
     * Send proper VT100 cursor-key sequences to CP/M.
     * DECCKM application mode uses ESC O x; normal
     * cursor mode uses ESC [ x.
     */
    if (
        status.fn &&
        arrowPressed
    )
    {
        queueKey(0x1B);

        queueKey(
            applicationCursorKeys
                ? 'O'
                : '['
        );

        if (status.up)
        {
            queueKey('A');
        }
        else if (status.down)
        {
            queueKey('B');
        }
        else if (status.right)
        {
            queueKey('C');
        }
        else if (status.left)
        {
            queueKey('D');
        }

        return;
    }


    for (auto c : status.word)
    {
        uint8_t ch =
            (uint8_t)c;


        if (status.ctrl)
        {
            if (
                ch >= 'a' &&
                ch <= 'z'
            )
            {
                ch =
                    (ch - 'a') + 1;
            }
            else if (
                ch >= 'A' &&
                ch <= 'Z'
            )
            {
                ch =
                    (ch - 'A') + 1;
            }
            else if (
                ch >= '@' &&
                ch <= '_'
            )
            {
                ch &= 0x1F;
            }
        }


        queueKey(ch);
    }


    if (status.enter)
    {
        queueKey(0x0D);
    }


    if (
        status.backspace ||
        status.del
    )
    {
        queueKey(0x08);
    }


    if (status.tab)
    {
        queueKey(0x09);
    }


    if (status.esc)
    {
        queueKey(0x1B);
    }
}


/*
 * ====================================================
 * USB keyboard
 * ====================================================
 */

static void pollUSBKeyboard()
{
    /*
     * Throw away USB keyboard input unless USB
     * currently owns the console.
     */
    if (!usbInputEnabled())
    {
        while (Serial.available())
        {
            Serial.read();
        }

        lastUSBWasCR = false;

        return;
    }


    while (Serial.available())
    {
        int value =
            Serial.read();


        if (value < 0)
        {
            break;
        }


        uint8_t ch =
            (uint8_t)value;


        /*
         * PC DEL -> CP/M backspace.
         */
        if (ch == 0x7F)
        {
            ch = 0x08;
        }


        /*
         * Normalise CR / LF / CRLF.
         */
        if (ch == 0x0D)
        {
            queueKey(0x0D);

            lastUSBWasCR = true;

            continue;
        }


        if (ch == 0x0A)
        {
            if (!lastUSBWasCR)
            {
                queueKey(0x0D);
            }

            lastUSBWasCR = false;

            continue;
        }


        lastUSBWasCR = false;


        queueKey(ch);
    }
}


static void telnetWriteNegotiation(
    uint8_t command,
    uint8_t option
)
{
    if (
        !telnetClient ||
        !telnetClient.connected()
    )
    {
        return;
    }

    uint8_t bytes[3];

    bytes[0] = 0xFF;
    bytes[1] = command;
    bytes[2] = option;

    telnetClient.write(
        bytes,
        sizeof(bytes)
    );
}


static void telnetResetInputState()
{
    lastTelnetWasCR = false;
    telnetInputState = TELNET_DATA;
    telnetIacCommand = 0;
}


static void telnetFormatRemoteIP(
    char *buffer,
    size_t bufferSize
)
{
    if (
        !buffer ||
        bufferSize == 0
    )
    {
        return;
    }

    if (!telnetRemoteIPValid)
    {
        snprintf(
            buffer,
            bufferSize,
            "unknown"
        );

        return;
    }

    snprintf(
        buffer,
        bufferSize,
        "%u.%u.%u.%u",
        telnetRemoteIP[0],
        telnetRemoteIP[1],
        telnetRemoteIP[2],
        telnetRemoteIP[3]
    );
}


static void telnetClientConnected()
{
    telnetResetInputState();

    telnetRemoteIP =
        telnetClient.remoteIP();

    telnetRemoteIPValid =
        true;

    /*
     * Tell the client that the server performs echoing
     * through CP/M and suppress Telnet go-ahead traffic.
     */
    telnetWriteNegotiation(
        0xFB,
        0x01
    );

    telnetWriteNegotiation(
        0xFB,
        0x03
    );

    telnetWriteNegotiation(
        0xFD,
        0x03
    );

    telnetClient.print(
        "\r\n"
        "Cardputer CP/M Telnet\r\n"
    );

    if (
        cardConsoleMode ==
        CARD_CONSOLE_TELNET
    )
    {
        telnetClient.print(
            "CON: routed to TELNET\r\n"
            "Press RETURN if a CP/M prompt is not visible.\r\n"
        );

        showTelnetStatus();
    }
    else
    {
        telnetClient.print(
            "Connected. Run TELNETD.COM on the Cardputer "
            "to route CP/M CON: here.\r\n"
        );
    }

    char peer[32];
    char announcement[80];

    telnetFormatRemoteIP(
        peer,
        sizeof(peer)
    );

    snprintf(
        announcement,
        sizeof(announcement),
        "\r\n[TELNET client connected from %s]\r\n",
        peer
    );

    _puts(
        announcement
    );
}


static void telnetServiceConnection()
{
    if (
        !telnetServerStarted ||
        !telnetServer ||
        cardConsoleMode !=
            CARD_CONSOLE_TELNET
    )
    {
        return;
    }

    WiFiClient incoming =
        telnetServer->available();

    if (incoming)
    {
        if (
            telnetClient &&
            telnetClient.connected()
        )
        {
            incoming.print(
                "\r\n"
                "Cardputer CP/M Telnet is already in use.\r\n"
            );

            incoming.stop();
        }
        else
        {
            telnetClient.stop();

            telnetRemoteIPValid =
                false;

            telnetClient =
                incoming;

            telnetClient.setNoDelay(
                true
            );

            telnetClientConnected();
        }
    }


    if (
        telnetClient &&
        !telnetClient.connected()
    )
    {
        bool wasTelnetConsole =
            cardConsoleMode ==
            CARD_CONSOLE_TELNET;

        telnetClient.stop();

        telnetClient =
            WiFiClient();

        telnetResetInputState();

        if (wasTelnetConsole)
        {
            /*
             * A dropped session terminates TELNETD completely.
             * setCardConsoleMode() also closes the listening socket,
             * so another client cannot connect until TELNETD is run again.
             */
            setCardConsoleMode(
                CARD_CONSOLE_LOCAL
            );
        }

        char peer[32];
        char announcement[80];

        telnetFormatRemoteIP(
            peer,
            sizeof(peer)
        );

        snprintf(
            announcement,
            sizeof(announcement),
            "\r\n[TELNET client disconnected from %s]\r\n",
            peer
        );

        _puts(
            announcement
        );

        telnetRemoteIPValid =
            false;
    }
}


static void telnetQueueDataByte(
    uint8_t ch
)
{
    /*
     * PC DEL -> CP/M backspace.
     */
    if (ch == 0x7F)
    {
        ch = 0x08;
    }


    /*
     * Telnet commonly sends CR LF or CR NUL.
     * CP/M wants a single CR.
     */
    if (ch == 0x0D)
    {
        queueKey(0x0D);

        lastTelnetWasCR = true;

        return;
    }


    if (
        ch == 0x0A ||
        ch == 0x00
    )
    {
        if (lastTelnetWasCR)
        {
            lastTelnetWasCR = false;
            return;
        }

        if (ch == 0x0A)
        {
            queueKey(0x0D);
        }

        return;
    }


    lastTelnetWasCR = false;

    queueKey(ch);
}


static void pollTelnetKeyboard()
{
    telnetServiceConnection();


    if (
        !telnetClient ||
        !telnetClient.connected()
    )
    {
        return;
    }


    while (telnetClient.available())
    {
        int value =
            telnetClient.read();

        if (value < 0)
        {
            break;
        }

        uint8_t ch =
            (uint8_t)value;


        if (
            telnetInputState ==
            TELNET_DATA
        )
        {
            if (ch == 0xFF)
            {
                telnetInputState =
                    TELNET_IAC;

                continue;
            }

            if (telnetInputEnabled())
            {
                telnetQueueDataByte(ch);
            }

            continue;
        }


        if (
            telnetInputState ==
            TELNET_IAC
        )
        {
            if (ch == 0xFF)
            {
                if (telnetInputEnabled())
                {
                    telnetQueueDataByte(
                        0xFF
                    );
                }

                telnetInputState =
                    TELNET_DATA;

                continue;
            }

            if (
                ch == 0xFB ||
                ch == 0xFC ||
                ch == 0xFD ||
                ch == 0xFE
            )
            {
                telnetIacCommand = ch;

                telnetInputState =
                    TELNET_OPTION;

                continue;
            }

            if (ch == 0xFA)
            {
                telnetInputState =
                    TELNET_SUBNEGOTIATION;

                continue;
            }

            telnetInputState =
                TELNET_DATA;

            continue;
        }


        if (
            telnetInputState ==
            TELNET_OPTION
        )
        {
            /*
             * Accept acknowledgements for the small Telnet option
             * set we explicitly requested:
             *
             *   ECHO (1) and SUPPRESS-GO-AHEAD (3) from us,
             *   SUPPRESS-GO-AHEAD (3) from the client.
             *
             * Reject everything else.
             */
            if (
                telnetIacCommand ==
                    0xFD
            )
            {
                if (
                    ch != 0x01 &&
                    ch != 0x03
                )
                {
                    telnetWriteNegotiation(
                        0xFC,
                        ch
                    );
                }
            }
            else if (
                telnetIacCommand ==
                    0xFB
            )
            {
                if (ch != 0x03)
                {
                    telnetWriteNegotiation(
                        0xFE,
                        ch
                    );
                }
            }

            telnetInputState =
                TELNET_DATA;

            continue;
        }


        if (
            telnetInputState ==
            TELNET_SUBNEGOTIATION
        )
        {
            if (ch == 0xFF)
            {
                telnetInputState =
                    TELNET_SUBNEGOTIATION_IAC;
            }

            continue;
        }


        if (
            telnetInputState ==
            TELNET_SUBNEGOTIATION_IAC
        )
        {
            if (ch == 0xF0)
            {
                telnetInputState =
                    TELNET_DATA;
            }
            else if (ch != 0xFF)
            {
                telnetInputState =
                    TELNET_SUBNEGOTIATION;
            }

            continue;
        }
    }
}


static void telnetWriteDataByte(
    uint8_t ch
)
{
    if (
        !telnetClient ||
        !telnetClient.connected()
    )
    {
        return;
    }

    telnetClient.write(ch);

    /*
     * In Telnet data, literal IAC must be doubled.
     */
    if (ch == 0xFF)
    {
        telnetClient.write(ch);
    }
}


static void pollInputs()
{
    /*
     * Always poll the Cardputer because
     * Fn+= must remain available.
     */
    pollCardputerKeyboard();

    pollUSBKeyboard();

    pollTelnetKeyboard();

    terminalMaybeRefresh();
}


/*
 * ====================================================
 * RunCPM console functions
 * ====================================================
 */

int _kbhit(void)
{
    pollInputs();

    return (
        keyHead != keyTail
    );
}


uint8 _getch(void)
{
    while (
        keyHead == keyTail
    )
    {
        pollInputs();

        delay(1);
    }


    uint8 ch =
        keyBuffer[keyTail];


    keyTail =
        (keyTail + 1) %
        sizeof(keyBuffer);


    return ch;
}


uint8 _getche(void)
{
    uint8 ch =
        _getch();

    _putch(ch);

    return ch;
}


void _putch(uint8 ch)
{
    /*
     * Always keep the Cardputer's backing
     * terminal up to date.
     */
    terminalProcessCharacter(ch);

    /*
     * Refresh the physical LCD only when the 20 ms
     * batching interval has elapsed.
     */
    terminalMaybeRefresh();


    /*
     * Only transmit to USB when USB owns
     * or shares the console.
     */
    if (usbOutputEnabled())
    {
        Serial.write(ch);
    }


    if (telnetOutputEnabled())
    {
        telnetWriteDataByte(ch);
    }
}


void _clrscr(void)
{
    terminalClearBuffer();

    cursorX = 0;
    cursorY = 0;

    viewportX = 0;
    viewportY = 0;

    ansiState = ANSI_NORMAL;
    ansiPrivate = false;


    if (localOutputEnabled())
    {
        terminalRenderAll(true);
    }


    if (usbOutputEnabled())
    {
        Serial.print(
            "\x1B[H\x1B[2J"
        );
    }


    if (
        telnetOutputEnabled() &&
        telnetClient &&
        telnetClient.connected()
    )
    {
        telnetClient.print(
            "\x1B[H\x1B[2J"
        );
    }
}


/*
 * ====================================================
 * Cardputer AUX UART
 *
 * EXT 14-pin header:
 *   TX = GPIO 13
 *   RX = GPIO 15
 *
 * 115200 8N1.
 * ====================================================
 */

#define CARDPUTER_AUX_TX_PIN 13
#define CARDPUTER_AUX_RX_PIN 15
#define CARDPUTER_AUX_BAUD   115200

HardwareSerial cardputerAuxSerial(1);


static void cardputerAuxBegin()
{
    cardputerAuxSerial.setRxBufferSize(
        512
    );

    cardputerAuxSerial.begin(
        CARDPUTER_AUX_BAUD,
        SERIAL_8N1,
        CARDPUTER_AUX_RX_PIN,
        CARDPUTER_AUX_TX_PIN
    );
}


uint8 cardputerAuxRead()
{
    while (
        cardputerAuxSerial.available() <=
        0
    )
    {
        M5Cardputer.update();
        delay(1);
    }

    int value =
        cardputerAuxSerial.read();

    return (
        value < 0
            ? 0x1A
            : (uint8)value
    );
}


void cardputerAuxWrite(
    uint8 value
)
{
    cardputerAuxSerial.write(
        value
    );
}


uint8 cardputerAuxInputReady()
{
    return (
        cardputerAuxSerial.available() > 0
            ? 0xFF
            : 0x00
    );
}


uint8 cardputerAuxOutputReady()
{
    return 0xFF;
}


/*
 * ====================================================
 * Cardputer battery command
 * ====================================================
 */

uint16 cardputerBatteryBdos()
{
    M5Cardputer.update();

    int level =
        M5Cardputer.Power.getBatteryLevel();

    int millivolts =
        M5Cardputer.Power.getBatteryVoltage();

    _puts(
        "\r\n"
        "Battery\r\n"
        "-------\r\n"
    );

    if (millivolts <= 0)
    {
        _puts(
            "Battery status unavailable\r\n"
        );

        return 0x00FF;
    }

    char line[64];

    snprintf(
        line,
        sizeof(line),
        "Level:   %d%%\r\n"
        "Voltage: %d mV (%d.%03d V)\r\n",
        level,
        millivolts,
        millivolts / 1000,
        millivolts % 1000
    );

    _puts(
        line
    );

    return 0;
}


/*
 * ====================================================
 * RunCPM RDR: / PUN: / LST:
 * ====================================================
 */

#ifdef USE_RDR

File rdr_dev;
int rdr_open = FALSE;
int rdr_eof = FALSE;

#endif


#ifdef USE_PUN

File pun_dev;
int pun_open = FALSE;

#endif


#ifdef USE_LST

File lst_dev;
int lst_open = FALSE;

#endif


/*
 * ====================================================
 * RunCPM core
 * ====================================================
 */

#include "runcpm/ram.h"


/*
 * ====================================================
 * SETDEF host command
 *
 * CP/M command tail is passed at DE (normally 0080h).
 * Supported here:
 *
 *   SETDEF
 *   SETDEF *
 *   SETDEF *,C:
 *   SETDEF C:,*
 *   SETDEF A:,C:,*
 *
 * Up to four drive-chain entries.
 * ====================================================
 */

static char *setdefTrim(char *text)
{
    while (
        *text == ' ' ||
        *text == '\t'
    )
    {
        text++;
    }

    char *end =
        text + strlen(text);

    while (
        end > text &&
        (
            end[-1] == ' ' ||
            end[-1] == '\t'
        )
    )
    {
        end--;
    }

    *end = 0;

    return text;
}


static void setdefPrintChain()
{
    _puts(
        "\r\nDrive Search Chain: "
    );

    for (
        uint8_t index = 0;
        index < setdefDriveCount;
        index++
    )
    {
        if (index)
        {
            _puts(",");
        }

        uint8_t entry =
            setdefDriveChain[index];

        if (
            entry ==
            SETDEF_CURRENT_DRIVE
        )
        {
            _puts("*");
        }
        else
        {
            char drive[3];

            drive[0] =
                (char)(
                    'A' + entry
                );

            drive[1] = ':';
            drive[2] = 0;

            _puts(drive);
        }
    }

    _puts(
        "\r\n"
    );
}


static bool setdefParseChain(
    char *text
)
{
    uint8_t parsed[
        SETDEF_MAX_DRIVES
    ];

    uint8_t count = 0;

    char *cursor =
        text;

    while (*cursor)
    {
        if (
            count >=
            SETDEF_MAX_DRIVES
        )
        {
            _puts(
                "\r\nSETDEF: maximum is four drives\r\n"
            );

            return false;
        }

        while (
            *cursor == ' ' ||
            *cursor == '\t'
        )
        {
            cursor++;
        }

        if (*cursor == 0)
        {
            break;
        }

        uint8_t entry;

        if (*cursor == '*')
        {
            entry =
                SETDEF_CURRENT_DRIVE;

            cursor++;
        }
        else
        {
            char drive =
                *cursor;

            if (
                drive >= 'a' &&
                drive <= 'p'
            )
            {
                drive =
                    (char)(
                        drive - 'a' + 'A'
                    );
            }

            if (
                drive < 'A' ||
                drive > 'P'
            )
            {
                _puts(
                    "\r\nSETDEF: invalid drive search order\r\n"
                );

                return false;
            }

            entry =
                (uint8_t)(
                    drive - 'A'
                );

            cursor++;

            if (*cursor == ':')
            {
                cursor++;
            }
        }

        for (
            uint8_t index = 0;
            index < count;
            index++
        )
        {
            if (
                parsed[index] ==
                entry
            )
            {
                _puts(
                    "\r\nSETDEF: drive defined twice in search path\r\n"
                );

                return false;
            }
        }

        parsed[count++] =
            entry;

        while (
            *cursor == ' ' ||
            *cursor == '\t'
        )
        {
            cursor++;
        }

        if (*cursor == 0)
        {
            break;
        }

        if (*cursor != ',')
        {
            _puts(
                "\r\nSETDEF: invalid drive search order\r\n"
            );

            return false;
        }

        cursor++;

        while (
            *cursor == ' ' ||
            *cursor == '\t'
        )
        {
            cursor++;
        }

        if (*cursor == 0)
        {
            _puts(
                "\r\nSETDEF: drive expected after comma\r\n"
            );

            return false;
        }
    }

    if (count == 0)
    {
        _puts(
            "\r\nSETDEF: empty drive search order\r\n"
        );

        return false;
    }

    setdefDriveCount =
        count;

    for (
        uint8_t index = 0;
        index < SETDEF_MAX_DRIVES;
        index++
    )
    {
        if (index < count)
        {
            setdefDriveChain[index] =
                parsed[index];
        }
        else
        {
            setdefDriveChain[index] =
                0;
        }
    }

    return true;
}


static void memPrintBytes(
    const char *label,
    uint32_t bytes
)
{
    char line[96];

    uint32_t kibTenths =
        (bytes * 10UL) /
        1024UL;

    snprintf(
        line,
        sizeof(line),
        "%-18s %10lu bytes  (%lu.%lu KiB)\r\n",
        label,
        (unsigned long)bytes,
        (unsigned long)(
            kibTenths / 10UL
        ),
        (unsigned long)(
            kibTenths % 10UL
        )
    );

    _puts(
        line
    );
}


uint16 cardputerMemBdos()
{
    uint32_t heapTotal =
        ESP.getHeapSize();

    uint32_t heapFree =
        ESP.getFreeHeap();

    uint32_t heapMinimum =
        ESP.getMinFreeHeap();

    uint32_t heapLargest =
        ESP.getMaxAllocHeap();

    uint32_t sketchSize =
        ESP.getSketchSize();

    uint32_t sketchFree =
        ESP.getFreeSketchSpace();

    uint32_t psramTotal =
        ESP.getPsramSize();

    _puts(
        "\r\n"
        "Cardputer memory\r\n"
        "-----------------\r\n"
    );

    memPrintBytes(
        "Heap total:",
        heapTotal
    );

    memPrintBytes(
        "Heap free:",
        heapFree
    );

    memPrintBytes(
        "Heap minimum:",
        heapMinimum
    );

    memPrintBytes(
        "Largest block:",
        heapLargest
    );

    _puts(
        "\r\n"
    );

    memPrintBytes(
        "Firmware size:",
        sketchSize
    );

    memPrintBytes(
        "Firmware free:",
        sketchFree
    );

    _puts(
        "\r\n"
    );

    if (psramTotal)
    {
        memPrintBytes(
            "PSRAM total:",
            psramTotal
        );

        memPrintBytes(
            "PSRAM free:",
            ESP.getFreePsram()
        );
    }
    else
    {
        _puts(
            "PSRAM:             none\r\n"
        );
    }

    return 0;
}


uint16 cardputerSetdefBdos(
    uint16 commandTail
)
{
    uint8_t length =
        _RamRead(
            commandTail
        );

    if (length > 127)
    {
        length = 127;
    }

    char buffer[129];

    for (
        uint8_t index = 0;
        index < length;
        index++
    )
    {
        buffer[index] =
            (char)_RamRead(
                commandTail +
                1 +
                index
            );
    }

    buffer[length] = 0;

    char *text =
        setdefTrim(
            buffer
        );

    if (*text == 0)
    {
        setdefPrintChain();

        return 0;
    }

    /*
     * CP/M Plus SETDEF also has TEMPORARY, ORDER,
     * DISPLAY and PAGE options. They are deliberately
     * rejected here until their corresponding runtime
     * behaviour is implemented.
     */
    if (
        strchr(text, '[') ||
        strchr(text, ']')
    )
    {
        _puts(
            "\r\nSETDEF: bracket options are not implemented yet\r\n"
        );

        return 0x00FF;
    }

    if (!setdefParseChain(text))
    {
        return 0x00FF;
    }

    setdefPrintChain();

    return 0;
}


#include "runcpm/console.h"

#include CPU

#include "runcpm/disk.h"
#include "runcpm/host.h"
#include "runcpm/cpm.h"

#ifdef CCP_INTERNAL
#include "runcpm/ccp.h"
#endif


/*
 * ====================================================
 * WiFi configuration
 *
 * Root of SD card: /WIFI.CFG
 *
 * SSID1=First Network
 * PASS1=first password
 * IP1=192.168.1.50
 * MASK1=255.255.255.0
 * GW1=192.168.1.1
 * DNS1=192.168.1.1
 *
 * IP/MASK/GW are optional as a group. If absent, the
 * network uses DHCP. DNS is optional for static config;
 * the gateway is used as DNS when DNSn is omitted.
 *
 * Networks are tried in numeric order.
 * Missing file / failed connections are non-fatal.
 * ====================================================
 */

#define WIFI_CONFIG_FILE "/WIFI.CFG"
#define WIFI_CONFIG_TEMP_FILE "/WIFI.TMP"
#define WIFI_CONFIG_BACKUP_FILE "/WIFI.BAK"
#define WIFI_MAX_NETWORKS 10
#define WIFI_SSID_SIZE 33
#define WIFI_PASS_SIZE 65
#define WIFI_IP_SIZE 16
#define WIFI_LINE_SIZE 160
#define WIFI_CONNECT_TIMEOUT_MS 10000

struct WifiConfigEntry
{
    bool used;
    char ssid[WIFI_SSID_SIZE];
    char password[WIFI_PASS_SIZE];
    char ip[WIFI_IP_SIZE];
    char mask[WIFI_IP_SIZE];
    char gateway[WIFI_IP_SIZE];
    char dns[WIFI_IP_SIZE];
};


static int wifiActiveConfigIndex =
    -1;


static char *wifiTrim(char *text)
{
    while (
        *text == ' ' ||
        *text == '\t'
    )
    {
        text++;
    }

    char *end =
        text + strlen(text);

    while (
        end > text &&
        (
            end[-1] == ' ' ||
            end[-1] == '\t'
        )
    )
    {
        end--;
    }

    *end = 0;

    return text;
}


static bool wifiReadLine(
    File &file,
    char *buffer,
    size_t bufferSize
)
{
    size_t length = 0;
    bool gotAnything = false;
    bool overflow = false;

    while (file.available())
    {
        int value =
            file.read();

        if (value < 0)
        {
            break;
        }

        char ch =
            (char)value;

        gotAnything = true;

        if (ch == '\r')
        {
            continue;
        }

        if (ch == '\n')
        {
            break;
        }

        if (
            length + 1 <
            bufferSize
        )
        {
            buffer[length++] =
                ch;
        }
        else
        {
            overflow = true;
        }
    }

    if (!gotAnything)
    {
        return false;
    }

    buffer[length] = 0;

    /*
     * Ignore an overlong line completely rather than
     * parsing a truncated password, SSID or address.
     */
    if (overflow)
    {
        buffer[0] = 0;
    }

    return true;
}


static int wifiConfigIndex(
    const char *key,
    const char *prefix
)
{
    size_t prefixLength =
        strlen(prefix);

    if (
        strncmp(
            key,
            prefix,
            prefixLength
        ) != 0
    )
    {
        return -1;
    }

    const char *number =
        key + prefixLength;

    if (*number == 0)
    {
        return -1;
    }

    int value = 0;

    while (*number)
    {
        if (
            *number < '0' ||
            *number > '9'
        )
        {
            return -1;
        }

        value =
            value * 10 +
            (*number - '0');

        number++;
    }

    if (
        value < 1 ||
        value > WIFI_MAX_NETWORKS
    )
    {
        return -1;
    }

    return value - 1;
}


static void wifiCopyValue(
    char *destination,
    size_t destinationSize,
    const char *value
)
{
    if (
        !destination ||
        destinationSize == 0
    )
    {
        return;
    }

    if (!value)
    {
        destination[0] = 0;
        return;
    }

    strncpy(
        destination,
        value,
        destinationSize - 1
    );

    destination[
        destinationSize - 1
    ] = 0;
}


static bool wifiLoadConfig(
    WifiConfigEntry *entries
)
{
    memset(
        entries,
        0,
        sizeof(WifiConfigEntry) *
        WIFI_MAX_NETWORKS
    );

    File file =
        SD.open(
            WIFI_CONFIG_FILE,
            O_READ
        );

    if (!file)
    {
        return false;
    }

    char line[WIFI_LINE_SIZE];

    while (
        wifiReadLine(
            file,
            line,
            sizeof(line)
        )
    )
    {
        char *text =
            wifiTrim(line);

        if (
            *text == 0 ||
            *text == '#' ||
            *text == ';'
        )
        {
            continue;
        }

        char *equals =
            strchr(
                text,
                '='
            );

        if (!equals)
        {
            continue;
        }

        *equals = 0;

        char *key =
            wifiTrim(text);

        char *value =
            wifiTrim(
                equals + 1
            );

        int index =
            wifiConfigIndex(
                key,
                "SSID"
            );

        if (index >= 0)
        {
            wifiCopyValue(
                entries[index].ssid,
                sizeof(entries[index].ssid),
                value
            );

            entries[index].used =
                entries[index].ssid[0] != 0;

            continue;
        }

        index =
            wifiConfigIndex(
                key,
                "PASS"
            );

        if (index >= 0)
        {
            wifiCopyValue(
                entries[index].password,
                sizeof(entries[index].password),
                value
            );

            continue;
        }

        index =
            wifiConfigIndex(
                key,
                "MASK"
            );

        if (index >= 0)
        {
            wifiCopyValue(
                entries[index].mask,
                sizeof(entries[index].mask),
                value
            );

            continue;
        }

        index =
            wifiConfigIndex(
                key,
                "DNS"
            );

        if (index >= 0)
        {
            wifiCopyValue(
                entries[index].dns,
                sizeof(entries[index].dns),
                value
            );

            continue;
        }

        index =
            wifiConfigIndex(
                key,
                "GW"
            );

        if (index >= 0)
        {
            wifiCopyValue(
                entries[index].gateway,
                sizeof(entries[index].gateway),
                value
            );

            continue;
        }

        index =
            wifiConfigIndex(
                key,
                "IP"
            );

        if (index >= 0)
        {
            wifiCopyValue(
                entries[index].ip,
                sizeof(entries[index].ip),
                value
            );
        }
    }

    file.close();

    return true;
}


static bool wifiEntryHasAnyStaticAddress(
    const WifiConfigEntry &entry
)
{
    return (
        entry.ip[0] ||
        entry.mask[0] ||
        entry.gateway[0] ||
        entry.dns[0]
    );
}


static bool wifiEntryHasStaticAddress(
    const WifiConfigEntry &entry
)
{
    return (
        entry.ip[0] &&
        entry.mask[0] &&
        entry.gateway[0]
    );
}


static bool wifiParseAddress(
    const char *text,
    IPAddress &address
)
{
    if (
        !text ||
        !text[0]
    )
    {
        return false;
    }

    return address.fromString(
        text
    );
}


static bool wifiApplyAddressConfig(
    const WifiConfigEntry &entry
)
{
    if (!wifiEntryHasAnyStaticAddress(entry))
    {
        /*
         * INADDR_NONE tells Arduino-ESP32 to start the DHCP client.
         */
        return WiFi.config(
            INADDR_NONE,
            INADDR_NONE,
            INADDR_NONE
        );
    }

    if (!wifiEntryHasStaticAddress(entry))
    {
        return false;
    }

    IPAddress ip;
    IPAddress mask;
    IPAddress gateway;
    IPAddress dns;

    if (
        !wifiParseAddress(
            entry.ip,
            ip
        ) ||
        !wifiParseAddress(
            entry.mask,
            mask
        ) ||
        !wifiParseAddress(
            entry.gateway,
            gateway
        )
    )
    {
        return false;
    }

    if (entry.dns[0])
    {
        if (!wifiParseAddress(
            entry.dns,
            dns
        ))
        {
            return false;
        }
    }
    else
    {
        dns =
            gateway;
    }

    return WiFi.config(
        ip,
        gateway,
        mask,
        dns
    );
}


static void wifiPrintAddress(
    const char *label,
    const IPAddress &address
)
{
    char line[64];

    snprintf(
        line,
        sizeof(line),
        "%-6s %u.%u.%u.%u\r\n",
        label,
        address[0],
        address[1],
        address[2],
        address[3]
    );

    _puts(
        line
    );
}


static int wifiFindActiveConfigIndex(
    WifiConfigEntry *entries
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        return -1;
    }

    String activeSsid =
        WiFi.SSID();

    if (
        wifiActiveConfigIndex >= 0 &&
        wifiActiveConfigIndex <
            WIFI_MAX_NETWORKS &&
        entries[
            wifiActiveConfigIndex
        ].used &&
        activeSsid.equals(
            entries[
                wifiActiveConfigIndex
            ].ssid
        )
    )
    {
        return wifiActiveConfigIndex;
    }

    for (
        int index = 0;
        index < WIFI_MAX_NETWORKS;
        index++
    )
    {
        if (
            entries[index].used &&
            activeSsid.equals(
                entries[index].ssid
            )
        )
        {
            wifiActiveConfigIndex =
                index;

            return index;
        }
    }

    return -1;
}


static void wifiPrintStatus()
{
    _puts(
        "\r\n"
    );

    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "WiFi: offline\r\n"
        );

        return;
    }

    WifiConfigEntry entries[
        WIFI_MAX_NETWORKS
    ];

    bool haveConfig =
        wifiLoadConfig(
            entries
        );

    int index =
        haveConfig
            ? wifiFindActiveConfigIndex(
                entries
            )
            : -1;

    String ssid =
        WiFi.SSID();

    _puts(
        "WiFi: connected\r\n"
    );

    _puts(
        "SSID:  "
    );

    _puts(
        ssid.c_str()
    );

    _puts(
        "\r\n"
    );

    wifiPrintAddress(
        "IP:",
        WiFi.localIP()
    );

    wifiPrintAddress(
        "Mask:",
        WiFi.subnetMask()
    );

    wifiPrintAddress(
        "GW:",
        WiFi.gatewayIP()
    );

    wifiPrintAddress(
        "DNS:",
        WiFi.dnsIP()
    );

    _puts(
        "Mode:  "
    );

    if (
        index >= 0 &&
        wifiEntryHasStaticAddress(
            entries[index]
        )
    )
    {
        _puts(
            "STATIC\r\n"
        );
    }
    else
    {
        _puts(
            "DHCP\r\n"
        );
    }
}


static bool wifiAddressKeyForIndex(
    const char *key,
    int index
)
{
    return (
        wifiConfigIndex(
            key,
            "IP"
        ) == index ||
        wifiConfigIndex(
            key,
            "MASK"
        ) == index ||
        wifiConfigIndex(
            key,
            "GW"
        ) == index ||
        wifiConfigIndex(
            key,
            "DNS"
        ) == index
    );
}


static bool wifiReplaceConfigWithTemp()
{
    if (SD.exists(
        WIFI_CONFIG_BACKUP_FILE
    ))
    {
        SD.remove(
            WIFI_CONFIG_BACKUP_FILE
        );
    }

    File original =
        SD.open(
            WIFI_CONFIG_FILE,
            O_WRITE | O_APPEND
        );

    if (!original)
    {
        return false;
    }

    if (!original.rename(
        WIFI_CONFIG_BACKUP_FILE
    ))
    {
        original.close();

        return false;
    }

    original.close();

    File temp =
        SD.open(
            WIFI_CONFIG_TEMP_FILE,
            O_WRITE | O_APPEND
        );

    if (
        !temp ||
        !temp.rename(
            WIFI_CONFIG_FILE
        )
    )
    {
        if (temp)
        {
            temp.close();
        }

        File backup =
            SD.open(
                WIFI_CONFIG_BACKUP_FILE,
                O_WRITE | O_APPEND
            );

        if (backup)
        {
            backup.rename(
                WIFI_CONFIG_FILE
            );

            backup.close();
        }

        return false;
    }

    temp.close();

    SD.remove(
        WIFI_CONFIG_BACKUP_FILE
    );

    return true;
}


static bool wifiWriteAddressConfig(
    int index,
    const char *ip,
    const char *mask,
    const char *gateway,
    const char *dns
)
{
    if (
        index < 0 ||
        index >= WIFI_MAX_NETWORKS
    )
    {
        return false;
    }

    File source =
        SD.open(
            WIFI_CONFIG_FILE,
            O_READ
        );

    if (!source)
    {
        return false;
    }

    if (SD.exists(
        WIFI_CONFIG_TEMP_FILE
    ))
    {
        SD.remove(
            WIFI_CONFIG_TEMP_FILE
        );
    }

    File output =
        SD.open(
            WIFI_CONFIG_TEMP_FILE,
            O_CREAT | O_WRITE | O_TRUNC
        );

    if (!output)
    {
        source.close();

        return false;
    }

    char rawLine[WIFI_LINE_SIZE];

    while (
        wifiReadLine(
            source,
            rawLine,
            sizeof(rawLine)
        )
    )
    {
        bool skip =
            false;

        char parseLine[
            WIFI_LINE_SIZE
        ];

        strncpy(
            parseLine,
            rawLine,
            sizeof(parseLine) - 1
        );

        parseLine[
            sizeof(parseLine) - 1
        ] = 0;

        char *text =
            wifiTrim(
                parseLine
            );

        if (
            *text &&
            *text != '#' &&
            *text != ';'
        )
        {
            char *equals =
                strchr(
                    text,
                    '='
                );

            if (equals)
            {
                *equals = 0;

                char *key =
                    wifiTrim(
                        text
                    );

                skip =
                    wifiAddressKeyForIndex(
                        key,
                        index
                    );
            }
        }

        if (!skip)
        {
            output.print(
                rawLine
            );

            output.print(
                "\r\n"
            );
        }
    }

    source.close();

    if (
        ip &&
        mask &&
        gateway
    )
    {
        output.print(
            "\r\nIP"
        );
        output.print(
            index + 1
        );
        output.print(
            "="
        );
        output.print(
            ip
        );

        output.print(
            "\r\nMASK"
        );
        output.print(
            index + 1
        );
        output.print(
            "="
        );
        output.print(
            mask
        );

        output.print(
            "\r\nGW"
        );
        output.print(
            index + 1
        );
        output.print(
            "="
        );
        output.print(
            gateway
        );

        if (
            dns &&
            dns[0]
        )
        {
            output.print(
                "\r\nDNS"
            );
            output.print(
                index + 1
            );
            output.print(
                "="
            );
            output.print(
                dns
            );
        }

        output.print(
            "\r\n"
        );
    }

    output.flush();
    output.close();

    if (!wifiReplaceConfigWithTemp())
    {
        SD.remove(
            WIFI_CONFIG_TEMP_FILE
        );

        return false;
    }

    return true;
}


static void wifiStopTelnetForReconnect()
{
    /*
     * IFCONFIG changes are rejected while a client is connected, so
     * only the listening server needs to be stopped during reconnect.
     */
    if (telnetServer)
    {
        telnetServer->stop();
        delete telnetServer;
        telnetServer =
            NULL;
    }

    telnetServerStarted =
        false;
}


static bool wifiReconnectEntry(
    const WifiConfigEntry &entry,
    int index
)
{
    wifiStopTelnetForReconnect();

    WiFi.setAutoReconnect(
        false
    );

    WiFi.disconnect(
        false,
        false
    );

    delay(100);

    WiFi.mode(
        WIFI_STA
    );

    if (!wifiApplyAddressConfig(
        entry
    ))
    {
        _puts(
            "\r\nIFCONFIG: invalid saved IP configuration\r\n"
        );

        return false;
    }

    _puts(
        "\r\nIFCONFIG: reconnecting to "
    );

    _puts(
        entry.ssid
    );

    _puts(
        "\r\n"
    );

    if (entry.password[0])
    {
        WiFi.begin(
            entry.ssid,
            entry.password
        );
    }
    else
    {
        WiFi.begin(
            entry.ssid
        );
    }

    uint32_t started =
        millis();

    while (
        WiFi.status() !=
            WL_CONNECTED &&
        (
            uint32_t
        )(
            millis() -
            started
        ) <
            WIFI_CONNECT_TIMEOUT_MS
    )
    {
        terminalMaybeRefresh();

        delay(50);
    }

    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "IFCONFIG: reconnect failed; WiFi offline\r\n"
        );

        wifiActiveConfigIndex =
            -1;

        WiFi.disconnect(
            false,
            false
        );

        WiFi.mode(
            WIFI_OFF
        );

        return false;
    }

    wifiActiveConfigIndex =
        index;

    WiFi.setAutoReconnect(
        true
    );

    telnetServerStarted =
        false;

    _puts(
        "IFCONFIG: WiFi connected\r\n"
    );

    return true;
}


static bool wifiEqualsIgnoreCase(
    const char *left,
    const char *right
)
{
    if (
        !left ||
        !right
    )
    {
        return false;
    }

    while (
        *left &&
        *right
    )
    {
        if (
            toupper(
                (unsigned char)*left
            ) !=
            toupper(
                (unsigned char)*right
            )
        )
        {
            return false;
        }

        left++;
        right++;
    }

    return (
        *left == 0 &&
        *right == 0
    );
}


static int wifiTokenize(
    char *text,
    char **arguments,
    int maxArguments
)
{
    int count = 0;

    while (*text)
    {
        while (
            *text == ' ' ||
            *text == '\t'
        )
        {
            text++;
        }

        if (!*text)
        {
            break;
        }

        if (
            count >=
            maxArguments
        )
        {
            return count + 1;
        }

        arguments[count++] =
            text;

        while (
            *text &&
            *text != ' ' &&
            *text != '\t'
        )
        {
            text++;
        }

        if (*text)
        {
            *text++ = 0;
        }
    }

    return count;
}


static bool telnetParsePort(
    const char *text,
    uint16_t &port
)
{
    if (
        !text ||
        !text[0]
    )
    {
        port =
            TELNET_DEFAULT_PORT;

        return true;
    }

    uint32_t value = 0;

    while (*text)
    {
        if (
            *text < '0' ||
            *text > '9'
        )
        {
            return false;
        }

        value =
            value * 10 +
            (uint32_t)(
                *text - '0'
            );

        if (value > 65535)
        {
            return false;
        }

        text++;
    }

    if (value == 0)
    {
        return false;
    }

    port =
        (uint16_t)value;

    return true;
}


static void telnetPrintUsage()
{
    _puts(
        "\r\n"
        "Usage:\r\n"
        "  TELNETD\r\n"
        "  TELNETD port\r\n"
    );
}


static uint16 cardputerStartTelnetd(
    uint16_t requestedPort
)
{
    if (ftpIsActive())
    {
        _puts(
            "\r\nTELNETD: unavailable while FTPD is active\r\n"
        );

        return 0x00FF;
    }

    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nTELNETD: WiFi is offline\r\n"
        );

        return 0x00FF;
    }

    if (
        cardConsoleMode ==
            CARD_CONSOLE_TELNET ||
        telnetServerStarted ||
        (
            telnetClient &&
            telnetClient.connected()
        )
    )
    {
        _puts(
            "\r\nTELNETD: already active\r\n"
        );

        return 0x00FF;
    }

    if (telnetServer)
    {
        telnetServer->stop();
        delete telnetServer;
        telnetServer =
            NULL;
    }

    telnetPort =
        requestedPort;

    telnetRemoteIPValid =
        false;

    telnetServer =
        new WiFiServer(
            telnetPort
        );

    if (!telnetServer)
    {
        _puts(
            "\r\nTELNETD: unable to create server\r\n"
        );

        return 0x00FF;
    }

    telnetServer->begin();

    telnetServerStarted =
        true;

    char message[64];

    snprintf(
        message,
        sizeof(message),
        "\r\nTELNETD: listening on port %u\r\n",
        telnetPort
    );

    _puts(
        message
    );

    setCardConsoleMode(
        CARD_CONSOLE_TELNET
    );

    if (
        cardConsoleMode !=
        CARD_CONSOLE_TELNET
    )
    {
        telnetServer->stop();
        delete telnetServer;
        telnetServer =
            NULL;

        telnetServerStarted =
            false;

        return 0x00FF;
    }

    return 0;
}


uint16 cardputerTelnetdBdos(
    uint16 commandTail
)
{
    uint8_t length =
        _RamRead(
            commandTail
        );

    if (length > 127)
    {
        length = 127;
    }

    char buffer[129];

    for (
        uint8_t index = 0;
        index < length;
        index++
    )
    {
        buffer[index] =
            (char)_RamRead(
                commandTail +
                1 +
                index
            );
    }

    buffer[length] = 0;

    char *text =
        wifiTrim(
            buffer
        );

    char *arguments[2];

    int argumentCount =
        wifiTokenize(
            text,
            arguments,
            2
        );

    if (argumentCount > 1)
    {
        telnetPrintUsage();

        return 0x00FF;
    }

    uint16_t requestedPort =
        TELNET_DEFAULT_PORT;

    if (
        argumentCount == 1 &&
        !telnetParsePort(
            arguments[0],
            requestedPort
        )
    )
    {
        _puts(
            "\r\nTELNETD: invalid port\r\n"
        );

        telnetPrintUsage();

        return 0x00FF;
    }

    return cardputerStartTelnetd(
        requestedPort
    );
}

static void wifiPrintIfconfigUsage()
{
    _puts(
        "\r\n"
        "Usage:\r\n"
        "  IFCONFIG\r\n"
        "  IFCONFIG DHCP\r\n"
        "  IFCONFIG ip mask gateway [dns]\r\n"
    );
}


uint16 cardputerFtpdBdos()
{
    if (
        cardConsoleMode ==
            CARD_CONSOLE_TELNET ||
        telnetServerStarted ||
        (
            telnetClient &&
            telnetClient.connected()
        )
    )
    {
        _puts(
            "\r\nFTPD: unavailable while TELNETD is active\r\n"
        );

        return 0x00FF;
    }

    if (!ftpStart())
    {
        return 0x00FF;
    }

    /*
     * FTPD is a foreground service, like TELNETD is a dedicated
     * operating mode. Do not return to the CP/M prompt while the
     * daemon is active.
     *
     * Physical Fn+= is the emergency LOCAL escape.
     * Physical G0 is the normal mode-cycle button and advances:
     *
     *   FTPD -> LOCAL
     *
     * Unlike TELNETD, an FTP client disconnect does not terminate
     * the daemon; ftpService() returns to listening for the next client.
     */
    while (ftpIsActive())
    {
        ftpService();

        if (ftpPhysicalStopRequested())
        {
            bool stoppedByG0 =
                ftpG0StopRequested;

            ftpStop();

            if (!stoppedByG0)
            {
                cardputerWaitForAllKeysReleased();
            }

            break;
        }

        delay(2);
    }

    bool stoppedByG0 =
        ftpG0StopRequested;

    ftpG0StopRequested =
        false;

    if (stoppedByG0)
    {
        setCardConsoleMode(
            CARD_CONSOLE_LOCAL
        );

        _puts(
            "\r\n[G0: LOCAL]\r\n"
        );
    }

    terminalRenderAll(
        true
    );

    return 0;
}


/*
 * ====================================================
 * G0 physical system-mode switch
 *
 * Runtime sequence:
 *
 *   LOCAL -> TELNETD -> FTPD -> LOCAL
 *
 * G0 retains its ESP32 download-mode purpose when held during
 * power-on; this handler only applies after Cardputer-CPM is running.
 * ====================================================
 */

static void cardputerCycleSystemMode()
{
    /*
     * FTPD does not call pollCardputerKeyboard() while its foreground
     * service loop owns the machine. G0 stopping FTPD is therefore
     * handled inside cardputerFtpdBdos()/ftpTransferCanContinue().
     */
    if (ftpIsActive())
    {
        return;
    }

    bool telnetdActive =
        cardConsoleMode ==
            CARD_CONSOLE_TELNET ||
        telnetServerStarted ||
        (
            telnetClient &&
            telnetClient.connected()
        );

    if (telnetdActive)
    {
        /*
         * TELNETD -> FTPD.
         *
         * Close the Telnet listener/client cleanly first because the two
         * foreground server modes intentionally remain mutually exclusive.
         */
        setCardConsoleMode(
            CARD_CONSOLE_LOCAL
        );

        _puts(
            "\r\n[G0: FTPD]\r\n"
        );

        cardputerFtpdBdos();

        return;
    }

    /*
     * Any ordinary console mode (LOCAL/USB/BOTH) enters TELNETD.
     * TELNETD itself switches CON: to the Telnet route.
     */
    if (
        cardConsoleMode !=
        CARD_CONSOLE_LOCAL
    )
    {
        setCardConsoleMode(
            CARD_CONSOLE_LOCAL
        );
    }

    _puts(
        "\r\n[G0: TELNETD]\r\n"
    );

    if (
        cardputerStartTelnetd(
            TELNET_DEFAULT_PORT
        ) != 0
    )
    {
        _puts(
            "[G0: remaining in LOCAL]\r\n"
        );

        setCardConsoleMode(
            CARD_CONSOLE_LOCAL
        );
    }
}


uint16 cardputerIfconfigBdos(
    uint16 commandTail
)
{
    /*
     * Network administration is deliberately unavailable from the
     * Telnet console. A remote session must not be able to change or
     * inspect the interface through IFCONFIG.
     */
    if (
        cardConsoleMode ==
        CARD_CONSOLE_TELNET
    )
    {
        _puts(
            "\r\n"
            "IFCONFIG: unavailable from TELNET console"
            "\r\n"
        );

        return 0x00FF;
    }

    uint8_t length =
        _RamRead(
            commandTail
        );

    if (length > 127)
    {
        length = 127;
    }

    char buffer[129];

    for (
        uint8_t index = 0;
        index < length;
        index++
    )
    {
        buffer[index] =
            (char)_RamRead(
                commandTail +
                1 +
                index
            );
    }

    buffer[length] = 0;

    char *text =
        wifiTrim(
            buffer
        );

    if (*text == 0)
    {
        wifiPrintStatus();

        return 0;
    }

    char *arguments[5];

    int argumentCount =
        wifiTokenize(
            text,
            arguments,
            5
        );

    bool requestDhcp =
        argumentCount == 1 &&
        wifiEqualsIgnoreCase(
            arguments[0],
            "DHCP"
        );

    bool requestStatic =
        argumentCount == 3 ||
        argumentCount == 4;

    if (
        !requestDhcp &&
        !requestStatic
    )
    {
        wifiPrintIfconfigUsage();

        return 0x00FF;
    }

    if (
        telnetClient &&
        telnetClient.connected()
    )
    {
        _puts(
            "\r\n"
            "IFCONFIG: cannot change settings while a TELNET client is connected"
            "\r\n"
        );

        return 0x00FF;
    }

    if (ftpIsActive())
    {
        _puts(
            "\r\n"
            "IFCONFIG: cannot change settings while FTPD is active"
            "\r\n"
        );

        return 0x00FF;
    }

    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nIFCONFIG: WiFi is offline; no active WIFI.CFG entry\r\n"
        );

        return 0x00FF;
    }

    WifiConfigEntry entries[
        WIFI_MAX_NETWORKS
    ];

    if (!wifiLoadConfig(
        entries
    ))
    {
        _puts(
            "\r\nIFCONFIG: cannot read /WIFI.CFG\r\n"
        );

        return 0x00FF;
    }

    int activeIndex =
        wifiFindActiveConfigIndex(
            entries
        );

    if (activeIndex < 0)
    {
        _puts(
            "\r\nIFCONFIG: connected SSID is not present in /WIFI.CFG\r\n"
        );

        return 0x00FF;
    }

    if (requestDhcp)
    {
        if (!wifiWriteAddressConfig(
            activeIndex,
            NULL,
            NULL,
            NULL,
            NULL
        ))
        {
            _puts(
                "\r\nIFCONFIG: failed to update /WIFI.CFG\r\n"
            );

            return 0x00FF;
        }
    }
    else
    {
        IPAddress ip;
        IPAddress mask;
        IPAddress gateway;
        IPAddress dns;

        if (
            !wifiParseAddress(
                arguments[0],
                ip
            ) ||
            !wifiParseAddress(
                arguments[1],
                mask
            ) ||
            !wifiParseAddress(
                arguments[2],
                gateway
            ) ||
            (
                argumentCount == 4 &&
                !wifiParseAddress(
                    arguments[3],
                    dns
                )
            )
        )
        {
            _puts(
                "\r\nIFCONFIG: invalid IPv4 address\r\n"
            );

            wifiPrintIfconfigUsage();

            return 0x00FF;
        }

        if (!wifiWriteAddressConfig(
            activeIndex,
            arguments[0],
            arguments[1],
            arguments[2],
            argumentCount == 4
                ? arguments[3]
                : NULL
        ))
        {
            _puts(
                "\r\nIFCONFIG: failed to update /WIFI.CFG\r\n"
            );

            return 0x00FF;
        }
    }

    if (!wifiLoadConfig(
        entries
    ))
    {
        _puts(
            "\r\nIFCONFIG: configuration saved but cannot be reloaded\r\n"
        );

        return 0x00FF;
    }

    if (!wifiReconnectEntry(
        entries[activeIndex],
        activeIndex
    ))
    {
        _puts(
            "IFCONFIG: setting saved; it will be retried on next boot\r\n"
        );

        return 0x00FF;
    }

    wifiPrintStatus();

    return 0;
}



/*
 * ====================================================
 * Outbound CP/M network utilities
 *
 * DNS.COM     BDOS 239
 * PING.COM    BDOS 240
 * TELNET.COM  BDOS 241
 * ====================================================
 */

static bool networkReadCommandTail(
    uint16 commandTail,
    char *buffer,
    size_t bufferSize
)
{
    if (
        !buffer ||
        bufferSize < 2
    )
    {
        return false;
    }

    uint8_t length =
        _RamRead(
            commandTail
        );

    if (length > 127)
    {
        length = 127;
    }

    if (
        (size_t)length >=
        bufferSize
    )
    {
        length =
            (uint8_t)(
                bufferSize - 1
            );
    }

    for (
        uint8_t index = 0;
        index < length;
        index++
    )
    {
        buffer[index] =
            (char)_RamRead(
                commandTail +
                1 +
                index
            );
    }

    buffer[length] =
        0;

    return true;
}


static bool networkParseNumber(
    const char *text,
    uint32_t minimum,
    uint32_t maximum,
    uint32_t &value
)
{
    if (
        !text ||
        !*text
    )
    {
        return false;
    }

    uint32_t parsed =
        0;

    while (*text)
    {
        if (
            *text < '0' ||
            *text > '9'
        )
        {
            return false;
        }

        parsed =
            parsed * 10 +
            (uint32_t)(
                *text - '0'
            );

        if (parsed > maximum)
        {
            return false;
        }

        text++;
    }

    if (
        parsed < minimum ||
        parsed > maximum
    )
    {
        return false;
    }

    value =
        parsed;

    return true;
}


static void networkFormatAddress(
    const IPAddress &address,
    char *buffer,
    size_t bufferSize
)
{
    if (
        !buffer ||
        bufferSize == 0
    )
    {
        return;
    }

    snprintf(
        buffer,
        bufferSize,
        "%u.%u.%u.%u",
        address[0],
        address[1],
        address[2],
        address[3]
    );
}


static bool networkResolve(
    const char *host,
    IPAddress &address
)
{
    if (
        !host ||
        !host[0] ||
        WiFi.status() !=
            WL_CONNECTED
    )
    {
        return false;
    }

    return (
        WiFi.hostByName(
            host,
            address
        ) == 1
    );
}


uint16 cardputerDnsBdos(
    uint16 commandTail
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nDNS: WiFi is offline\r\n"
        );

        return 0x00FF;
    }

    char buffer[129];

    if (!networkReadCommandTail(
        commandTail,
        buffer,
        sizeof(buffer)
    ))
    {
        return 0x00FF;
    }

    char *text =
        wifiTrim(
            buffer
        );

    char *arguments[2];

    int argumentCount =
        wifiTokenize(
            text,
            arguments,
            2
        );

    if (argumentCount == 0)
    {
        IPAddress dns =
            WiFi.dnsIP(
                0
            );

        char dnsText[32];

        networkFormatAddress(
            dns,
            dnsText,
            sizeof(dnsText)
        );

        if (
            dns[0] == 0 &&
            dns[1] == 0 &&
            dns[2] == 0 &&
            dns[3] == 0
        )
        {
            _puts(
                "\r\nDNS: no active server configured\r\n"
            );

            return 0x00FF;
        }

        char message[80];

        snprintf(
            message,
            sizeof(message),
            "\r\nDNS server: %s\r\n",
            dnsText
        );

        _puts(
            message
        );

        return 0;
    }

    if (argumentCount != 1)
    {
        _puts(
            "\r\n"
            "Usage: DNS [host]\r\n"
        );

        return 0x00FF;
    }

    IPAddress address;

    if (!networkResolve(
        arguments[0],
        address
    ))
    {
        _puts(
            "\r\nDNS: lookup failed\r\n"
        );

        return 0x00FF;
    }

    char addressText[32];

    networkFormatAddress(
        address,
        addressText,
        sizeof(addressText)
    );

    char message[192];

    snprintf(
        message,
        sizeof(message),
        "\r\n%s = %s\r\n",
        arguments[0],
        addressText
    );

    _puts(
        message
    );

    return 0;
}


enum CardputerPingEventType
{
    CARDPUTER_PING_EVENT_NONE =
        0,

    CARDPUTER_PING_EVENT_REPLY,
    CARDPUTER_PING_EVENT_TIMEOUT
};


struct CardputerPingState
{
    volatile uint32_t eventSerial;
    volatile uint8_t eventType;
    volatile uint16_t sequence;
    volatile uint8_t ttl;
    volatile uint32_t size;
    volatile uint32_t elapsed;

    volatile bool done;
    volatile uint32_t transmitted;
    volatile uint32_t received;
    volatile uint32_t duration;
};


static void cardputerPingSuccess(
    esp_ping_handle_t handle,
    void *argument
)
{
    CardputerPingState *state =
        (CardputerPingState *)argument;

    if (!state)
    {
        return;
    }

    uint16_t sequence =
        0;

    uint8_t ttl =
        0;

    uint32_t size =
        0;

    uint32_t elapsed =
        0;

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_SEQNO,
        &sequence,
        sizeof(sequence)
    );

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_TTL,
        &ttl,
        sizeof(ttl)
    );

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_SIZE,
        &size,
        sizeof(size)
    );

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_TIMEGAP,
        &elapsed,
        sizeof(elapsed)
    );

    state->sequence =
        sequence;

    state->ttl =
        ttl;

    state->size =
        size;

    state->elapsed =
        elapsed;

    state->eventType =
        CARDPUTER_PING_EVENT_REPLY;

    state->eventSerial++;
}


static void cardputerPingTimeout(
    esp_ping_handle_t handle,
    void *argument
)
{
    CardputerPingState *state =
        (CardputerPingState *)argument;

    if (!state)
    {
        return;
    }

    uint16_t sequence =
        0;

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_SEQNO,
        &sequence,
        sizeof(sequence)
    );

    state->sequence =
        sequence;

    state->eventType =
        CARDPUTER_PING_EVENT_TIMEOUT;

    state->eventSerial++;
}


static void cardputerPingEnd(
    esp_ping_handle_t handle,
    void *argument
)
{
    CardputerPingState *state =
        (CardputerPingState *)argument;

    if (!state)
    {
        return;
    }

    uint32_t transmitted =
        0;

    uint32_t received =
        0;

    uint32_t duration =
        0;

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_REQUEST,
        &transmitted,
        sizeof(transmitted)
    );

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_REPLY,
        &received,
        sizeof(received)
    );

    esp_ping_get_profile(
        handle,
        ESP_PING_PROF_DURATION,
        &duration,
        sizeof(duration)
    );

    state->transmitted =
        transmitted;

    state->received =
        received;

    state->duration =
        duration;

    state->done =
        true;
}


uint16 cardputerPingBdos(
    uint16 commandTail
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nPING: WiFi is offline\r\n"
        );

        return 0x00FF;
    }

    char buffer[129];

    if (!networkReadCommandTail(
        commandTail,
        buffer,
        sizeof(buffer)
    ))
    {
        return 0x00FF;
    }

    char *text =
        wifiTrim(
            buffer
        );

    char *arguments[3];

    int argumentCount =
        wifiTokenize(
            text,
            arguments,
            3
        );

    if (
        argumentCount < 1 ||
        argumentCount > 2
    )
    {
        _puts(
            "\r\n"
            "Usage: PING host [count]\r\n"
            "Count: 1-20, default 4\r\n"
        );

        return 0x00FF;
    }

    uint32_t count =
        4;

    if (
        argumentCount == 2 &&
        !networkParseNumber(
            arguments[1],
            1,
            20,
            count
        )
    )
    {
        _puts(
            "\r\nPING: invalid count\r\n"
        );

        return 0x00FF;
    }

    IPAddress address;

    if (!networkResolve(
        arguments[0],
        address
    ))
    {
        _puts(
            "\r\nPING: host lookup failed\r\n"
        );

        return 0x00FF;
    }

    char addressText[32];

    networkFormatAddress(
        address,
        addressText,
        sizeof(addressText)
    );

    char message[192];

    snprintf(
        message,
        sizeof(message),
        "\r\nPING %s (%s):\r\n",
        arguments[0],
        addressText
    );

    _puts(
        message
    );

    ip_addr_t target;

    IP_ADDR4(
        &target,
        address[0],
        address[1],
        address[2],
        address[3]
    );

    esp_ping_config_t config =
        ESP_PING_DEFAULT_CONFIG();

    config.target_addr =
        target;

    config.count =
        count;

    config.interval_ms =
        1000;

    config.timeout_ms =
        1000;

    CardputerPingState state = {};

    esp_ping_callbacks_t callbacks = {};

    callbacks.on_ping_success =
        cardputerPingSuccess;

    callbacks.on_ping_timeout =
        cardputerPingTimeout;

    callbacks.on_ping_end =
        cardputerPingEnd;

    callbacks.cb_args =
        &state;

    esp_ping_handle_t handle =
        NULL;

    if (
        esp_ping_new_session(
            &config,
            &callbacks,
            &handle
        ) !=
            ESP_OK ||
        !handle
    )
    {
        _puts(
            "PING: unable to create ICMP session\r\n"
        );

        return 0x00FF;
    }

    if (
        esp_ping_start(
            handle
        ) !=
        ESP_OK
    )
    {
        esp_ping_delete_session(
            handle
        );

        _puts(
            "PING: unable to start ICMP session\r\n"
        );

        return 0x00FF;
    }

    uint32_t seenEvent =
        0;

    bool cancelled =
        false;

    while (!state.done)
    {
        uint32_t currentEvent =
            state.eventSerial;

        if (
            currentEvent !=
            seenEvent
        )
        {
            uint8_t type =
                state.eventType;

            uint16_t sequence =
                state.sequence;

            if (
                type ==
                CARDPUTER_PING_EVENT_REPLY
            )
            {
                snprintf(
                    message,
                    sizeof(message),
                    "%lu bytes from %s: seq=%u ttl=%u time=%lu ms\r\n",
                    (unsigned long)state.size,
                    addressText,
                    (unsigned)sequence,
                    (unsigned)state.ttl,
                    (unsigned long)state.elapsed
                );

                _puts(
                    message
                );
            }
            else if (
                type ==
                CARDPUTER_PING_EVENT_TIMEOUT
            )
            {
                snprintf(
                    message,
                    sizeof(message),
                    "Request timeout: seq=%u\r\n",
                    (unsigned)sequence
                );

                _puts(
                    message
                );
            }

            seenEvent =
                currentEvent;
        }

        if (_chready())
        {
            uint8_t ch =
                _getconNB();

            if (ch == 0x03)
            {
                cancelled =
                    true;

                esp_ping_stop(
                    handle
                );

                break;
            }
        }

        delay(1);
    }

    if (cancelled)
    {
        delay(10);

        esp_ping_get_profile(
            handle,
            ESP_PING_PROF_REQUEST,
            (void *)&state.transmitted,
            sizeof(state.transmitted)
        );

        esp_ping_get_profile(
            handle,
            ESP_PING_PROF_REPLY,
            (void *)&state.received,
            sizeof(state.received)
        );

        esp_ping_get_profile(
            handle,
            ESP_PING_PROF_DURATION,
            (void *)&state.duration,
            sizeof(state.duration)
        );
    }
    else
    {
        /*
         * Print any final per-packet event that arrived immediately before
         * the end callback set done.
         */
        if (
            state.eventSerial !=
            seenEvent
        )
        {
            if (
                state.eventType ==
                CARDPUTER_PING_EVENT_REPLY
            )
            {
                snprintf(
                    message,
                    sizeof(message),
                    "%lu bytes from %s: seq=%u ttl=%u time=%lu ms\r\n",
                    (unsigned long)state.size,
                    addressText,
                    (unsigned)state.sequence,
                    (unsigned)state.ttl,
                    (unsigned long)state.elapsed
                );

                _puts(
                    message
                );
            }
            else if (
                state.eventType ==
                CARDPUTER_PING_EVENT_TIMEOUT
            )
            {
                snprintf(
                    message,
                    sizeof(message),
                    "Request timeout: seq=%u\r\n",
                    (unsigned)state.sequence
                );

                _puts(
                    message
                );
            }
        }
    }

    esp_ping_delete_session(
        handle
    );

    uint32_t transmitted =
        state.transmitted;

    uint32_t received =
        state.received;

    uint32_t loss =
        transmitted
            ? (
                (
                    transmitted -
                    received
                ) *
                100
              ) /
                transmitted
            : 0;

    snprintf(
        message,
        sizeof(message),
        "--- %s ping statistics ---\r\n"
        "%lu transmitted, %lu received, %lu%% loss\r\n",
        arguments[0],
        (unsigned long)transmitted,
        (unsigned long)received,
        (unsigned long)loss
    );

    _puts(
        message
    );

    if (cancelled)
    {
        _puts(
            "PING: cancelled\r\n"
        );
    }

    return received
        ? 0
        : 0x00FF;
}


#define OUT_TELNET_IAC  255
#define OUT_TELNET_DONT 254
#define OUT_TELNET_DO   253
#define OUT_TELNET_WONT 252
#define OUT_TELNET_WILL 251
#define OUT_TELNET_SB   250
#define OUT_TELNET_SE   240

#define OUT_TELNET_OPT_ECHO 1
#define OUT_TELNET_OPT_SGA  3


static void outboundTelnetNegotiation(
    WiFiClient &client,
    uint8_t command,
    uint8_t option
)
{
    uint8_t reply =
        0;

    if (
        command ==
        OUT_TELNET_WILL
    )
    {
        reply =
            (
                option ==
                    OUT_TELNET_OPT_ECHO ||
                option ==
                    OUT_TELNET_OPT_SGA
            )
                ? OUT_TELNET_DO
                : OUT_TELNET_DONT;
    }
    else if (
        command ==
        OUT_TELNET_DO
    )
    {
        reply =
            (
                option ==
                    OUT_TELNET_OPT_SGA
            )
                ? OUT_TELNET_WILL
                : OUT_TELNET_WONT;
    }
    else
    {
        return;
    }

    uint8_t response[3] =
    {
        OUT_TELNET_IAC,
        reply,
        option
    };

    client.write(
        response,
        sizeof(response)
    );
}


uint16 cardputerTelnetBdos(
    uint16 commandTail
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nTELNET: WiFi is offline\r\n"
        );

        return 0x00FF;
    }

    if (ftpIsActive())
    {
        _puts(
            "\r\nTELNET: unavailable while FTPD is active\r\n"
        );

        return 0x00FF;
    }

    char buffer[129];

    if (!networkReadCommandTail(
        commandTail,
        buffer,
        sizeof(buffer)
    ))
    {
        return 0x00FF;
    }

    char *text =
        wifiTrim(
            buffer
        );

    char *arguments[3];

    int argumentCount =
        wifiTokenize(
            text,
            arguments,
            3
        );

    if (
        argumentCount < 1 ||
        argumentCount > 2
    )
    {
        _puts(
            "\r\n"
            "Usage: TELNET host [port]\r\n"
        );

        return 0x00FF;
    }

    uint32_t port =
        23;

    if (
        argumentCount == 2 &&
        !networkParseNumber(
            arguments[1],
            1,
            65535,
            port
        )
    )
    {
        _puts(
            "\r\nTELNET: invalid port\r\n"
        );

        return 0x00FF;
    }

    IPAddress address;

    if (!networkResolve(
        arguments[0],
        address
    ))
    {
        _puts(
            "\r\nTELNET: host lookup failed\r\n"
        );

        return 0x00FF;
    }

    char addressText[32];

    networkFormatAddress(
        address,
        addressText,
        sizeof(addressText)
    );

    char message[192];

    snprintf(
        message,
        sizeof(message),
        "\r\nTELNET: connecting to %s (%s):%lu...\r\n",
        arguments[0],
        addressText,
        (unsigned long)port
    );

    _puts(
        message
    );

    WiFiClient client;

    client.setNoDelay(
        true
    );

    if (!client.connect(
        address,
        (uint16_t)port
    ))
    {
        _puts(
            "TELNET: connection failed\r\n"
        );

        return 0x00FF;
    }

    _puts(
        "TELNET: connected\r\n"
        "Ctrl-] or Fn+= disconnects\r\n"
        "\r\n"
    );

    enum
    {
        OUT_TELNET_DATA =
            0,

        OUT_TELNET_COMMAND,
        OUT_TELNET_OPTION,
        OUT_TELNET_SUBNEG,
        OUT_TELNET_SUBNEG_IAC
    };

    uint8_t state =
        OUT_TELNET_DATA;

    uint8_t telnetCommand =
        0;

    bool remoteWasCR =
        false;

    bool done =
        false;

    while (
        (
            client.connected() ||
            client.available()
        ) &&
        !done
    )
    {
        while (
            client.available() &&
            !done
        )
        {
            int incoming =
                client.read();

            if (incoming < 0)
            {
                break;
            }

            uint8_t ch =
                (uint8_t)incoming;

            if (
                state ==
                OUT_TELNET_DATA
            )
            {
                if (
                    ch ==
                    OUT_TELNET_IAC
                )
                {
                    state =
                        OUT_TELNET_COMMAND;

                    continue;
                }

                /*
                 * Telnet NVT permits CR NUL. Suppress the NUL while leaving
                 * ordinary CR/LF intact for the VT100 terminal.
                 */
                if (
                    ch == 0 &&
                    remoteWasCR
                )
                {
                    remoteWasCR =
                        false;

                    continue;
                }

                _putcon(
                    ch
                );

                remoteWasCR =
                    ch == '\r';

                continue;
            }

            if (
                state ==
                OUT_TELNET_COMMAND
            )
            {
                if (
                    ch ==
                    OUT_TELNET_IAC
                )
                {
                    _putcon(
                        ch
                    );

                    state =
                        OUT_TELNET_DATA;

                    continue;
                }

                if (
                    ch ==
                        OUT_TELNET_WILL ||
                    ch ==
                        OUT_TELNET_WONT ||
                    ch ==
                        OUT_TELNET_DO ||
                    ch ==
                        OUT_TELNET_DONT
                )
                {
                    telnetCommand =
                        ch;

                    state =
                        OUT_TELNET_OPTION;

                    continue;
                }

                if (
                    ch ==
                    OUT_TELNET_SB
                )
                {
                    state =
                        OUT_TELNET_SUBNEG;

                    continue;
                }

                state =
                    OUT_TELNET_DATA;

                continue;
            }

            if (
                state ==
                OUT_TELNET_OPTION
            )
            {
                outboundTelnetNegotiation(
                    client,
                    telnetCommand,
                    ch
                );

                state =
                    OUT_TELNET_DATA;

                continue;
            }

            if (
                state ==
                OUT_TELNET_SUBNEG
            )
            {
                if (
                    ch ==
                    OUT_TELNET_IAC
                )
                {
                    state =
                        OUT_TELNET_SUBNEG_IAC;
                }

                continue;
            }

            if (
                state ==
                OUT_TELNET_SUBNEG_IAC
            )
            {
                state =
                    (
                        ch ==
                        OUT_TELNET_SE
                    )
                        ? OUT_TELNET_DATA
                        : OUT_TELNET_SUBNEG;
            }
        }

        while (
            _chready() &&
            !done
        )
        {
            uint8_t ch =
                _getconNB();

            if (
                ch ==
                0x1D
            )
            {
                done =
                    true;

                break;
            }

            if (
                ch ==
                OUT_TELNET_IAC
            )
            {
                uint8_t escaped[2] =
                {
                    OUT_TELNET_IAC,
                    OUT_TELNET_IAC
                };

                client.write(
                    escaped,
                    sizeof(escaped)
                );
            }
            else if (
                ch ==
                '\r'
            )
            {
                uint8_t newline[2] =
                {
                    '\r',
                    '\n'
                };

                client.write(
                    newline,
                    sizeof(newline)
                );
            }
            else
            {
                client.write(
                    ch
                );
            }
        }

        M5Cardputer.update();

        Keyboard_Class::KeysState status =
            M5Cardputer.Keyboard.keysState();

        if (
            status.fn &&
            status.f12
        )
        {
            done =
                true;

            cardputerWaitForAllKeysReleased();

            break;
        }

        terminalMaybeRefresh();

        delay(1);
    }

    client.stop();

    _puts(
        "\r\nTELNET: disconnected\r\n"
    );

    return 0;
}



/*
 * ====================================================
 * WGET / NTP / TIME
 *
 * WGET.COM : HTTP/HTTPS downloader into CP/M storage.
 * NTP.COM  : synchronise the ESP32 system clock using NTP.
 * TIME.COM : display the current UTC date/time without resyncing.
 * ====================================================
 */

#define WGET_BUFFER_SIZE 1024
#define WGET_PATH_SIZE   128
#define WGET_NAME_SIZE   13
#define NTP_DEFAULT_SERVER "pool.ntp.org"

static char cardputerNtpServer[64] =
    NTP_DEFAULT_SERVER;


static bool cardputerClockValid()
{
    time_t now =
        time(NULL);

    /*
     * Treat anything before 2024-01-01 as unsynchronised. ESP32 starts
     * with an epoch-like clock until SNTP or another source sets it.
     */
    return now >=
        (time_t)1704067200;
}


static bool cardputerFormatUtc(
    char *buffer,
    size_t bufferSize
)
{
    if (
        !buffer ||
        bufferSize == 0 ||
        !cardputerClockValid()
    )
    {
        return false;
    }

    time_t now =
        time(NULL);

    struct tm utc;

    if (!gmtime_r(
        &now,
        &utc
    ))
    {
        return false;
    }

    return strftime(
        buffer,
        bufferSize,
        "%Y-%m-%d %H:%M:%S UTC",
        &utc
    ) > 0;
}


static void cardputerConfigureNtp(
    const char *server
)
{
    if (
        !server ||
        !server[0]
    )
    {
        server =
            NTP_DEFAULT_SERVER;
    }

    strncpy(
        cardputerNtpServer,
        server,
        sizeof(cardputerNtpServer) - 1
    );

    cardputerNtpServer[
        sizeof(cardputerNtpServer) - 1
    ] = 0;

    /*
     * Keep the host clock in UTC. Local timezone policy can be layered on
     * later without changing the underlying absolute time.
     */
    configTime(
        0,
        0,
        cardputerNtpServer
    );
}


static void cardputerPrintUtc()
{
    char timeText[48];

    if (!cardputerFormatUtc(
        timeText,
        sizeof(timeText)
    ))
    {
        _puts(
            "Time: not synchronised\r\n"
        );

        return;
    }

    char message[96];

    snprintf(
        message,
        sizeof(message),
        "Time: %s\r\n",
        timeText
    );

    _puts(
        message
    );
}


uint16 cardputerTimeBdos()
{
    _puts(
        "\r\n"
    );

    if (!cardputerClockValid())
    {
        _puts(
            "TIME: clock is not synchronised\r\n"
            "Run NTP first.\r\n"
        );

        return 0x00FF;
    }

    cardputerPrintUtc();

    return 0;
}


uint16 cardputerNtpBdos(
    uint16 commandTail
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nNTP: WiFi is offline\r\n"
        );

        return 0x00FF;
    }

    char buffer[129];

    if (!networkReadCommandTail(
        commandTail,
        buffer,
        sizeof(buffer)
    ))
    {
        return 0x00FF;
    }

    char *text =
        wifiTrim(
            buffer
        );

    char *arguments[2];

    int argumentCount =
        wifiTokenize(
            text,
            arguments,
            2
        );

    if (argumentCount > 1)
    {
        _puts(
            "\r\n"
            "Usage: NTP [server]\r\n"
        );

        return 0x00FF;
    }

    const char *server =
        argumentCount == 1
            ? arguments[0]
            : NTP_DEFAULT_SERVER;

    cardputerConfigureNtp(
        server
    );

    char message[128];

    snprintf(
        message,
        sizeof(message),
        "\r\nNTP: synchronising with %s...\r\n",
        cardputerNtpServer
    );

    _puts(
        message
    );

    struct tm utc;

    if (!getLocalTime(
        &utc,
        10000
    ))
    {
        _puts(
            "NTP: synchronisation failed\r\n"
        );

        return 0x00FF;
    }

    snprintf(
        message,
        sizeof(message),
        "NTP server: %s\r\n",
        cardputerNtpServer
    );

    _puts(
        message
    );

    cardputerPrintUtc();

    return 0;
}


static bool wgetValidFilenameCharacter(
    char ch
)
{
    if (
        ch >= 'A' &&
        ch <= 'Z'
    )
    {
        return true;
    }

    if (
        ch >= '0' &&
        ch <= '9'
    )
    {
        return true;
    }

    const char *extra =
        "$#@!%&'()-^_{}~+";

    return strchr(
        extra,
        ch
    ) != NULL;
}


static bool wgetCanonicalFilename(
    const char *input,
    char *output,
    size_t outputSize
)
{
    if (
        !input ||
        !input[0] ||
        !output ||
        outputSize <
            WGET_NAME_SIZE
    )
    {
        return false;
    }

    char upper[
        WGET_NAME_SIZE
    ];

    size_t length =
        strlen(
            input
        );

    if (
        length == 0 ||
        length >=
            sizeof(upper)
    )
    {
        return false;
    }

    for (
        size_t index = 0;
        index < length;
        index++
    )
    {
        char ch =
            input[index];

        if (
            ch >= 'a' &&
            ch <= 'z'
        )
        {
            ch =
                (char)(
                    ch - 'a' + 'A'
                );
        }

        upper[index] =
            ch;
    }

    upper[length] =
        0;

    char *dot =
        strchr(
            upper,
            '.'
        );

    size_t baseLength =
        dot
            ? (size_t)(
                dot - upper
              )
            : length;

    size_t extensionLength =
        dot
            ? strlen(
                dot + 1
              )
            : 0;

    if (
        baseLength < 1 ||
        baseLength > 8 ||
        extensionLength > 3 ||
        (
            dot &&
            (
                extensionLength == 0 ||
                strchr(
                    dot + 1,
                    '.'
                )
            )
        )
    )
    {
        return false;
    }

    for (
        size_t index = 0;
        index < length;
        index++
    )
    {
        if (
            upper[index] ==
            '.'
        )
        {
            continue;
        }

        if (!wgetValidFilenameCharacter(
            upper[index]
        ))
        {
            return false;
        }
    }

    if (
        strcmp(
            upper,
            "WGETTMP.$$"
        ) == 0 ||
        strcmp(
            upper,
            "WGETBAK.$$"
        ) == 0
    )
    {
        return false;
    }

    strcpy(
        output,
        upper
    );

    return true;
}


static bool wgetFilenameFromUrl(
    const char *url,
    char *filename,
    size_t filenameSize
)
{
    if (
        !url ||
        !filename
    )
    {
        return false;
    }

    const char *scheme =
        strstr(
            url,
            "://"
        );

    if (!scheme)
    {
        return false;
    }

    const char *path =
        strchr(
            scheme + 3,
            '/'
        );

    if (!path)
    {
        return wgetCanonicalFilename(
            "INDEX.HTM",
            filename,
            filenameSize
        );
    }

    const char *lastSlash =
        strrchr(
            path,
            '/'
        );

    const char *name =
        lastSlash
            ? lastSlash + 1
            : path;

    size_t length =
        strcspn(
            name,
            "?#"
        );

    if (length == 0)
    {
        return wgetCanonicalFilename(
            "INDEX.HTM",
            filename,
            filenameSize
        );
    }

    if (
        length >=
        WGET_NAME_SIZE
    )
    {
        return false;
    }

    char candidate[
        WGET_NAME_SIZE
    ];

    memcpy(
        candidate,
        name,
        length
    );

    candidate[length] =
        0;

    return wgetCanonicalFilename(
        candidate,
        filename,
        filenameSize
    );
}


static bool wgetParseDestination(
    const char *argument,
    uint8_t &drive,
    char *filename,
    size_t filenameSize
)
{
    drive =
        cDrive;

    if (
        !argument ||
        !argument[0]
    )
    {
        return false;
    }

    const char *name =
        argument;

    if (
        argument[0] &&
        argument[1] ==
            ':'
    )
    {
        char driveLetter =
            argument[0];

        if (
            driveLetter >= 'a' &&
            driveLetter <= 'p'
        )
        {
            driveLetter =
                (char)(
                    driveLetter - 'a' + 'A'
                );
        }

        if (
            driveLetter < 'A' ||
            driveLetter > 'P'
        )
        {
            return false;
        }

        drive =
            (uint8_t)(
                driveLetter - 'A'
            );

        name =
            argument + 2;
    }

    if (
        strchr(
            name,
            ':'
        ) ||
        strchr(
            name,
            '/'
        ) ||
        strchr(
            name,
            '\\'
        )
    )
    {
        return false;
    }

    return wgetCanonicalFilename(
        name,
        filename,
        filenameSize
    );
}


static bool wgetBuildUserPath(
    uint8_t drive,
    char *userPath,
    size_t userPathSize
)
{
    uint8_t root[
        HOST_FILENAME_MAX
    ];

    if (!_sysBuildDriveRoot(
        drive,
        root,
        sizeof(root)
    ))
    {
        return false;
    }

    File rootDirectory =
        SD.open(
            (char *)root,
            O_READ
        );

    if (
        !rootDirectory ||
        !rootDirectory.isDirectory()
    )
    {
        if (rootDirectory)
        {
            rootDirectory.close();
        }

        return false;
    }

    rootDirectory.close();

    char userFolder =
        (char)toupper(
            tohex(
                userCode
            )
        );

    int written =
        snprintf(
            userPath,
            userPathSize,
            "%s/%c",
            (char *)root,
            userFolder
        );

    if (
        written <= 0 ||
        (size_t)written >=
            userPathSize
    )
    {
        return false;
    }

    File existing =
        SD.open(
            userPath,
            O_READ
        );

    if (existing)
    {
        bool okay =
            existing.isDirectory();

        existing.close();

        return okay;
    }

    return SD.mkdir(
        userPath
    );
}


static bool wgetBuildPaths(
    uint8_t drive,
    const char *filename,
    char *destination,
    size_t destinationSize,
    char *temporary,
    size_t temporarySize,
    char *backup,
    size_t backupSize
)
{
    char userPath[
        WGET_PATH_SIZE
    ];

    if (!wgetBuildUserPath(
        drive,
        userPath,
        sizeof(userPath)
    ))
    {
        return false;
    }

    int written =
        snprintf(
            destination,
            destinationSize,
            "%s/%s",
            userPath,
            filename
        );

    if (
        written <= 0 ||
        (size_t)written >=
            destinationSize
    )
    {
        return false;
    }

    written =
        snprintf(
            temporary,
            temporarySize,
            "%s/WGETTMP.$$",
            userPath
        );

    if (
        written <= 0 ||
        (size_t)written >=
            temporarySize
    )
    {
        return false;
    }

    written =
        snprintf(
            backup,
            backupSize,
            "%s/WGETBAK.$$",
            userPath
        );

    return (
        written > 0 &&
        (size_t)written <
            backupSize
    );
}


static bool wgetCommitTemporaryFile(
    const char *temporary,
    const char *destination,
    const char *backup
)
{
    SD.remove(
        backup
    );

    bool hadDestination =
        SD.exists(
            destination
        );

    if (hadDestination)
    {
        File oldFile =
            SD.open(
                destination,
                O_READ
            );

        if (
            !oldFile ||
            !oldFile.rename(
                backup
            )
        )
        {
            if (oldFile)
            {
                oldFile.close();
            }

            return false;
        }

        oldFile.close();
    }

    File downloaded =
        SD.open(
            temporary,
            O_READ
        );

    if (
        !downloaded ||
        !downloaded.rename(
            destination
        )
    )
    {
        if (downloaded)
        {
            downloaded.close();
        }

        if (hadDestination)
        {
            File backupFile =
                SD.open(
                    backup,
                    O_READ
                );

            if (backupFile)
            {
                backupFile.rename(
                    destination
                );

                backupFile.close();
            }
        }

        return false;
    }

    downloaded.close();

    if (hadDestination)
    {
        SD.remove(
            backup
        );
    }

    return true;
}


uint16 cardputerWgetBdos(
    uint16 commandTail
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nWGET: WiFi is offline\r\n"
        );

        return 0x00FF;
    }

    if (ftpIsActive())
    {
        _puts(
            "\r\nWGET: unavailable while FTPD is active\r\n"
        );

        return 0x00FF;
    }

    char buffer[129];

    if (!networkReadCommandTail(
        commandTail,
        buffer,
        sizeof(buffer)
    ))
    {
        return 0x00FF;
    }

    char *text =
        wifiTrim(
            buffer
        );

    char *arguments[3];

    int argumentCount =
        wifiTokenize(
            text,
            arguments,
            3
        );

    if (
        argumentCount < 1 ||
        argumentCount > 2
    )
    {
        _puts(
            "\r\n"
            "Usage: WGET url [[drive:]file]\r\n"
        );

        return 0x00FF;
    }

    const char *url =
        arguments[0];

    bool isHttp =
        strncasecmp(
            url,
            "http://",
            7
        ) == 0;

    bool isHttps =
        strncasecmp(
            url,
            "https://",
            8
        ) == 0;

    if (
        !isHttp &&
        !isHttps
    )
    {
        _puts(
            "\r\nWGET: URL must begin with http:// or https://\r\n"
        );

        return 0x00FF;
    }

    uint8_t drive =
        cDrive;

    char filename[
        WGET_NAME_SIZE
    ];

    if (argumentCount == 2)
    {
        if (!wgetParseDestination(
            arguments[1],
            drive,
            filename,
            sizeof(filename)
        ))
        {
            _puts(
                "\r\nWGET: invalid CP/M 8.3 destination\r\n"
            );

            return 0x00FF;
        }
    }
    else
    {
        if (!wgetFilenameFromUrl(
            url,
            filename,
            sizeof(filename)
        ))
        {
            _puts(
                "\r\n"
                "WGET: URL filename is not CP/M 8.3\r\n"
                "Specify a destination: WGET url [drive:]file\r\n"
            );

            return 0x00FF;
        }
    }

    char destination[
        WGET_PATH_SIZE
    ];

    char temporary[
        WGET_PATH_SIZE
    ];

    char backup[
        WGET_PATH_SIZE
    ];

    if (!wgetBuildPaths(
        drive,
        filename,
        destination,
        sizeof(destination),
        temporary,
        sizeof(temporary),
        backup,
        sizeof(backup)
    ))
    {
        _puts(
            "\r\nWGET: destination drive/user is unavailable\r\n"
        );

        return 0x00FF;
    }

    if (
        SD.exists(
            destination
        ) &&
        _sys_isreadonly(
            (uint8 *)destination
        )
    )
    {
        _puts(
            "\r\nWGET: destination is read-only\r\n"
        );

        return 0x00FF;
    }

    SD.remove(
        temporary
    );

    File output =
        SD.open(
            temporary,
            O_CREAT |
            O_WRITE |
            O_TRUNC
        );

    if (!output)
    {
        _puts(
            "\r\nWGET: cannot create temporary file\r\n"
        );

        return 0x00FF;
    }

    HTTPClient http;

    WiFiClient plainClient;
    WiFiClientSecure secureClient;

    if (isHttps)
    {
        /*
         * HTTPS is encrypted, but this first Cardputer implementation does
         * not carry a CA bundle. Do not pretend certificate identity is
         * verified: setInsecure() is explicit and documented.
         */
        secureClient.setInsecure();

        if (!http.begin(
            secureClient,
            url
        ))
        {
            output.close();
            SD.remove(
                temporary
            );

            _puts(
                "\r\nWGET: unable to initialise HTTPS\r\n"
            );

            return 0x00FF;
        }
    }
    else
    {
        if (!http.begin(
            plainClient,
            url
        ))
        {
            output.close();
            SD.remove(
                temporary
            );

            _puts(
                "\r\nWGET: unable to initialise HTTP\r\n"
            );

            return 0x00FF;
        }
    }

    /*
     * HTTP/1.0 avoids chunked transfer framing in the raw body stream,
     * which lets us stream large downloads directly to SdFat without
     * buffering the whole response in RAM.
     */
    http.useHTTP10(
        true
    );

    http.setReuse(
        false
    );

    http.setTimeout(
        15000
    );

    http.setFollowRedirects(
        HTTPC_STRICT_FOLLOW_REDIRECTS
    );

    char status[256];

    snprintf(
        status,
        sizeof(status),
        "\r\nWGET: %s\r\n"
        "      -> %c%u:%s\r\n",
        url,
        'A' + drive,
        userCode,
        filename
    );

    _puts(
        status
    );

    int response =
        http.GET();

    if (
        response < 200 ||
        response >= 300
    )
    {
        snprintf(
            status,
            sizeof(status),
            "WGET: HTTP error %d\r\n",
            response
        );

        _puts(
            status
        );

        http.end();
        output.close();

        SD.remove(
            temporary
        );

        return 0x00FF;
    }

    WiFiClient *stream =
        http.getStreamPtr();

    int remaining =
        http.getSize();

    uint8_t ioBuffer[
        WGET_BUFFER_SIZE
    ];

    uint32_t byteCount =
        0;

    bool okay =
        true;

    uint32_t lastData =
        millis();

    while (true)
    {
        int available =
            stream
                ? stream->available()
                : 0;

        if (available > 0)
        {
            size_t wanted =
                (size_t)available;

            if (
                wanted >
                sizeof(ioBuffer)
            )
            {
                wanted =
                    sizeof(ioBuffer);
            }

            int got =
                stream->readBytes(
                    ioBuffer,
                    wanted
                );

            if (got <= 0)
            {
                okay =
                    false;

                break;
            }

            size_t written =
                output.write(
                    ioBuffer,
                    (size_t)got
                );

            if (
                written !=
                (size_t)got
            )
            {
                _puts(
                    "WGET: SD write failed\r\n"
                );

                okay =
                    false;

                break;
            }

            byteCount +=
                (uint32_t)got;

            if (remaining > 0)
            {
                remaining -=
                    got;

                if (remaining < 0)
                {
                    remaining =
                        0;
                }
            }

            lastData =
                millis();

            continue;
        }

        if (
            remaining == 0
        )
        {
            break;
        }

        if (
            !http.connected()
        )
        {
            /*
             * Unknown-length HTTP/1.0 bodies finish by closing the
             * connection. Known-length responses must reach zero.
             */
            if (remaining < 0)
            {
                break;
            }

            okay =
                false;

            break;
        }

        if (
            (uint32_t)(
                millis() -
                lastData
            ) >
                15000
        )
        {
            _puts(
                "WGET: receive timeout\r\n"
            );

            okay =
                false;

            break;
        }

        delay(1);
    }

    output.flush();
    output.close();

    http.end();

    if (!okay)
    {
        SD.remove(
            temporary
        );

        _puts(
            "WGET: download failed\r\n"
        );

        return 0x00FF;
    }

    if (!wgetCommitTemporaryFile(
        temporary,
        destination,
        backup
    ))
    {
        SD.remove(
            temporary
        );

        _puts(
            "WGET: unable to install downloaded file\r\n"
        );

        return 0x00FF;
    }

    snprintf(
        status,
        sizeof(status),
        "WGET: %lu bytes saved as %c%u:%s\r\n",
        (unsigned long)byteCount,
        'A' + drive,
        userCode,
        filename
    );

    _puts(
        status
    );

    return 0;
}


#include "cardputer_ftp_client.h"
#include "cardputer_browser.h"


static bool wifiConnectFromConfig()
{
    WifiConfigEntry entries[
        WIFI_MAX_NETWORKS
    ];

    wifiActiveConfigIndex =
        -1;

    _puts(
        "WiFi: connecting (press any key to skip)\r\n"
    );

    if (!wifiLoadConfig(entries))
    {
        _puts(
            "WiFi: offline (no WIFI.CFG)\r\n"
        );

        telnetServerStarted =
            false;

        WiFi.mode(
            WIFI_OFF
        );

        return false;
    }

    bool haveNetwork = false;

    WiFi.mode(
        WIFI_STA
    );

    for (
        int index = 0;
        index < WIFI_MAX_NETWORKS;
        index++
    )
    {
        if (cardputerAnyPhysicalKeyPressed())
        {
            cardputerWaitForAllKeysReleased();

            _puts(
                "WiFi: skipped\r\n"
            );

            WiFi.disconnect(
                false,
                false
            );

            telnetServerStarted =
                false;

            WiFi.mode(
                WIFI_OFF
            );

            return false;
        }

        if (!entries[index].used)
        {
            continue;
        }

        haveNetwork = true;

        WiFi.disconnect(
            false,
            false
        );

        uint32_t disconnectStarted =
            millis();

        while (
            (
                uint32_t
            )(
                millis() -
                disconnectStarted
            ) <
                100
        )
        {
            if (cardputerAnyPhysicalKeyPressed())
            {
                cardputerWaitForAllKeysReleased();

                _puts(
                    "WiFi: skipped\r\n"
                );

                WiFi.disconnect(
                    false,
                    false
                );

                telnetServerStarted =
                    false;

                WiFi.mode(
                    WIFI_OFF
                );

                return false;
            }

            delay(10);
        }

        if (!wifiApplyAddressConfig(
            entries[index]
        ))
        {
            _puts(
                "WiFi: invalid IP configuration; skipping network\r\n"
            );

            continue;
        }

        if (
            entries[index].password[0]
        )
        {
            WiFi.begin(
                entries[index].ssid,
                entries[index].password
            );
        }
        else
        {
            /*
             * Empty PASSn means an open network.
             */
            WiFi.begin(
                entries[index].ssid
            );
        }

        uint32_t started =
            millis();

        while (
            WiFi.status() !=
                WL_CONNECTED &&
            (
                uint32_t
            )(
                millis() -
                started
            ) <
                WIFI_CONNECT_TIMEOUT_MS
        )
        {
            if (cardputerAnyPhysicalKeyPressed())
            {
                cardputerWaitForAllKeysReleased();

                _puts(
                    "WiFi: skipped\r\n"
                );

                WiFi.disconnect(
                    false,
                    false
                );

                telnetServerStarted =
                    false;

                WiFi.mode(
                    WIFI_OFF
                );

                return false;
            }

            terminalMaybeRefresh();

            delay(50);
        }

        if (
            WiFi.status() ==
            WL_CONNECTED
        )
        {
            wifiActiveConfigIndex =
                index;

            IPAddress ip =
                WiFi.localIP();

            char networkStatus[128];

            snprintf(
                networkStatus,
                sizeof(networkStatus),
                "WiFi: %s\r\n"
                "IP:   %u.%u.%u.%u\r\n"
                "Net:  TELNETD  FTPD\r\n",
                entries[index].ssid,
                ip[0],
                ip[1],
                ip[2],
                ip[3]
            );

            _puts(
                networkStatus
            );

            WiFi.setAutoReconnect(
                true
            );

            /*
             * Start default UTC SNTP immediately but do not delay boot.
             * NTP.COM can later force/wait for synchronisation or select
             * another server for the current session.
             */
            cardputerConfigureNtp(
                NTP_DEFAULT_SERVER
            );

            telnetServerStarted =
                false;

            return true;
        }
    }

    if (!haveNetwork)
    {
        _puts(
            "WiFi: offline (no configured networks)\r\n"
        );
    }
    else
    {
        _puts(
            "WiFi: offline (no network found)\r\n"
        );
    }

    WiFi.disconnect(
        false,
        false
    );

    telnetServerStarted =
        false;

    WiFi.mode(
        WIFI_OFF
    );

    wifiActiveConfigIndex =
        -1;

    return false;
}


/*
 * ====================================================
 * Start CP/M
 * ====================================================
 */

void setup()
{
    /*
     * ESP32-S3 USB CDC.
     */
    Serial.begin();


    /*
     * Cardputer.
     */
    auto cfg =
        M5.config();


    M5Cardputer.begin(
        cfg,
        true
    );


    /*
     * Replace M5Stack's interrupt-driven ADV keyboard reader
     * with our FIFO-draining polling reader.
     *
     * The stock reader installs an ISR on GPIO 11. Detach it
     * before replacing the reader so no stale callback remains.
     */
    detachInterrupt(
        digitalPinToInterrupt(11)
    );

    M5Cardputer.Keyboard.begin(
        std::unique_ptr<KeyboardReader>(
            new CardputerAdvPollingKeyboardReader()
        )
    );


    /*
     * Hardware auxiliary serial port on the EXT header.
     */
    cardputerAuxBegin();


    /*
     * LOCAL is always the boot default.
     */
    cardConsoleMode =
        CARD_CONSOLE_LOCAL;


    terminalInit();


    /*
     * Initialise the SD card before printing boot text so /SPLASH.PNG
     * is the first content displayed during a normal successful boot.
     */
    SPI.begin(
        SPIINIT
    );


    if (!SD.begin(SDINIT))
    {
        _puts(
            "CARDPUTER CP/M\r\n"
            "--------------\r\n"
            "\r\n"
            "SD CARD FAILED\r\n"
        );

        return;
    }


    showBootSplash();


    /*
     * Start the normal boot display after the splash has been dismissed.
     */
    terminalClearBuffer();

    cursorX = 0;
    cursorY = 0;

    viewportX = 0;
    viewportY = 0;

    terminalRenderAll(true);


    _puts(
        "CARDPUTER CP/M\r\n"
    );


    _puts(
        "--------------\r\n"
    );


    _puts(
        "\r\n"
    );


    _puts(
        "Console: LOCAL\r\n"
        "AUX:     115200 8N1\r\n"
        "SD:      ready\r\n"
    );


    /*
     * Removable A:/B: media live as directories below MEDIA/.
     * Both slots intentionally start empty on every power-up.
     */
    if (!SD.exists(CARDPUTER_MEDIA_ROOT))
    {
        SD.mkdir(CARDPUTER_MEDIA_ROOT);
    }


    wifiConnectFromConfig();


    _puts(
        "RunCPM "
    );

    _puts(
        VERSION
    );

    _puts(
        " / "
    );

    _puts(
        CPU_IS
    );

    _puts(
        "\r\n\r\n"
    );


#ifndef DEBUG

    Z80estimateClock();

#endif


#ifdef DEBUGLOG

    _sys_deletefile(
        (uint8 *)LogName
    );

#endif


    if (!(
        VersionCCP >= 0x10 ||
        SD.exists(CCPname)
    ))
    {
        _puts(
            "Unable to load CCP.\r\n"
        );

        return;
    }


#ifdef ABDOS

    _PatchBIOS();

#endif


    /*
     * Main CP/M loop.
     */
    while (true)
    {
        _puts(
            CCPHEAD
        );


        _PatchCPM();


        Status =
            STATUS_RUNNING;


#ifdef CCP_INTERNAL

        _ccp();

#else

        if (!_RamLoad(
            (uint8 *)CCPname,
            CCPaddr,
            0
        ))
        {
            _puts(
                "Unable to load CCP.\r\n"
            );

            break;
        }


        if (firstBoot)
        {
            firstBoot =
                FALSE;


            if (_sys_exists(
                (uint8 *)STARTUP_PROFILE_PATH
            ))
            {
                const char *startup =
                    STARTUP_PROFILE_COMMAND;


                uint16 cmd =
                    CCPaddr + 8;


                uint8 blen = 0;


                while (
                    startup[blen] &&
                    blen < 125
                )
                {
                    _RamWrite(
                        cmd + blen,
                        (uint8)startup[blen]
                    );

                    blen++;
                }


                _RamWrite(
                    cmd + blen,
                    0x00
                );


                _RamWrite(
                    CCPaddr + 7,
                    blen
                );
            }
        }


        Z80reset();


        SET_LOW_REGISTER(
            BC,
            _RamRead(
                DSKByte
            )
        );


        PC =
            CCPaddr;


        Z80run(
            cpuDelayInstructions
        );

#endif


        if (
            Status ==
            STATUS_EXIT
        )
        {
            break;
        }


#ifdef USE_PUN

        if (pun_dev)
        {
            _sys_fflush(
                pun_dev
            );
        }

#endif


#ifdef USE_LST

        if (lst_dev)
        {
            _sys_fflush(
                lst_dev
            );
        }

#endif
    }


    _puts(
        "\r\nCP/M halted.\r\n"
    );
}


void loop()
{
    pollInputs();

    delay(5);
}