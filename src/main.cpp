#include <Arduino.h>
#include <M5Cardputer.h>
#include <WiFi.h>
#include <string.h>

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

#define TELNET_PORT 23

static WiFiServer telnetServer(
    TELNET_PORT
);

static WiFiClient telnetClient;

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

            terminal.write(
                (uint8_t)termBuffer[logicalRow][logicalCol]
            );
        }
    }

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
    }

    for (int col = 0; col < TERM_COLS; col++)
    {
        termBuffer[TERM_ROWS - 1][col] = ' ';
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

            /*
             * SGR is accepted for compatibility.
             * Colour/attribute rendering is not yet modelled.
             */
            break;


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
        "CP/M USB CONSOLE"
    );

    M5Cardputer.Display.println(
        "---------------"
    );

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        "CON: routed to USB"
    );

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        "Fn+= = LOCAL"
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
        "CP/M TELNET CONSOLE"
    );

    M5Cardputer.Display.println(
        "------------------"
    );

    M5Cardputer.Display.println();

    IPAddress ip =
        WiFi.localIP();

    M5Cardputer.Display.printf(
        "%u.%u.%u.%u:%u\n",
        ip[0],
        ip[1],
        ip[2],
        ip[3],
        TELNET_PORT
    );

    M5Cardputer.Display.println();

    if (
        telnetClient &&
        telnetClient.connected()
    )
    {
        M5Cardputer.Display.println(
            "Telnet client connected"
        );
    }
    else
    {
        M5Cardputer.Display.println(
            "Waiting for client..."
        );
    }

    M5Cardputer.Display.println();

    M5Cardputer.Display.println(
        "Fn+= = LOCAL"
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
        mode == CARD_CONSOLE_TELNET &&
        (
            WiFi.status() !=
                WL_CONNECTED ||
            !telnetServerStarted
        )
    )
    {
        _puts(
            "\r\n"
            "[TELNET unavailable: WiFi is offline]"
            "\r\n"
        );

        return;
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


    if (
        mode == CARD_CONSOLE_LOCAL &&
        previousMode ==
            CARD_CONSOLE_TELNET &&
        telnetClient &&
        telnetClient.connected()
    )
    {
        telnetClient.print(
            "\r\n"
            "[Switching to LOCAL console]"
            "\r\n"
        );
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


static void pollCardputerKeyboard()
{
    static uint64_t previousKeyMask = 0;

    M5Cardputer.update();


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


static void telnetClientConnected()
{
    telnetResetInputState();

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
            "Connected. Run TELNET.COM on the Cardputer "
            "to route CP/M CON: here.\r\n"
        );
    }
}


static void telnetServiceConnection()
{
    if (!telnetServerStarted)
    {
        return;
    }

    WiFiClient incoming =
        telnetServer.available();

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
        telnetClient.stop();

        telnetResetInputState();

        if (
            cardConsoleMode ==
            CARD_CONSOLE_TELNET
        )
        {
            setCardConsoleMode(
                CARD_CONSOLE_LOCAL
            );
        }
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
 * RunCPM PUN: / LST:
 * ====================================================
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
 * SSID2=Second Network
 * PASS2=second password
 *
 * Networks are tried in numeric order.
 * Missing file / failed connections are non-fatal.
 * ====================================================
 */

#define WIFI_CONFIG_FILE "/WIFI.CFG"
#define WIFI_MAX_NETWORKS 10
#define WIFI_SSID_SIZE 33
#define WIFI_PASS_SIZE 65
#define WIFI_LINE_SIZE 160
#define WIFI_CONNECT_TIMEOUT_MS 10000

struct WifiConfigEntry
{
    bool used;
    char ssid[WIFI_SSID_SIZE];
    char password[WIFI_PASS_SIZE];
};


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
     * parsing a truncated password or SSID.
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
            strncpy(
                entries[index].ssid,
                value,
                WIFI_SSID_SIZE - 1
            );

            entries[index].ssid[
                WIFI_SSID_SIZE - 1
            ] = 0;

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
            strncpy(
                entries[index].password,
                value,
                WIFI_PASS_SIZE - 1
            );

            entries[index].password[
                WIFI_PASS_SIZE - 1
            ] = 0;
        }
    }

    file.close();

    return true;
}


static bool wifiConnectFromConfig()
{
    WifiConfigEntry entries[
        WIFI_MAX_NETWORKS
    ];

    _puts(
        "WiFi: reading /WIFI.CFG\r\n"
    );

    if (!wifiLoadConfig(entries))
    {
        _puts(
            "WiFi: no WIFI.CFG - offline\r\n"
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
        if (!entries[index].used)
        {
            continue;
        }

        haveNetwork = true;

        _puts(
            "WiFi: trying "
        );

        _puts(
            entries[index].ssid
        );

        _puts(
            "\r\n"
        );

        WiFi.disconnect(
            false,
            false
        );

        delay(100);

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
            terminalMaybeRefresh();

            delay(50);
        }

        if (
            WiFi.status() ==
            WL_CONNECTED
        )
        {
            _puts(
                "WiFi: connected to "
            );

            _puts(
                entries[index].ssid
            );

            _puts(
                "\r\n"
            );

            IPAddress ip =
                WiFi.localIP();

            char address[32];

            snprintf(
                address,
                sizeof(address),
                "WiFi: IP %u.%u.%u.%u\r\n",
                ip[0],
                ip[1],
                ip[2],
                ip[3]
            );

            _puts(
                address
            );

            WiFi.setAutoReconnect(
                true
            );


            telnetServer.begin();

            telnetServerStarted =
                true;

            _puts(
                "Telnet: listening on port 23\r\n"
            );

            return true;
        }
    }

    if (!haveNetwork)
    {
        _puts(
            "WiFi: WIFI.CFG has no SSID entries\r\n"
        );
    }
    else
    {
        _puts(
            "WiFi: no configured network available\r\n"
        );
    }

    _puts(
        "WiFi: offline\r\n"
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
     * LOCAL is always the boot default.
     */
    cardConsoleMode =
        CARD_CONSOLE_LOCAL;


    terminalInit();


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
    );


    _puts(
        "Initializing SD...\r\n"
    );


    SPI.begin(
        SPIINIT
    );


    if (!SD.begin(SDINIT))
    {
        _puts(
            "\r\n"
            "SD CARD FAILED\r\n"
        );

        return;
    }


    _puts(
        "SD card OK\r\n"
    );


    wifiConnectFromConfig();


    _puts(
        "\r\n"
        "RunCPM "
    );


    _puts(
        VERSION
    );


    _puts(
        "\r\n"
    );


    _puts(
        "Board: "
    );


    _puts(
        BOARD
    );


    _puts(
        "\r\n"
    );


    _puts(
        "CPU: "
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