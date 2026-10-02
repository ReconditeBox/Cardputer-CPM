#include <Arduino.h>
#include <M5Cardputer.h>
#include <string.h>

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
    CARD_CONSOLE_LOCAL = 0,
    CARD_CONSOLE_USB   = 1,
    CARD_CONSOLE_BOTH  = 2
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
        mode != CARD_CONSOLE_BOTH
    )
    {
        return;
    }


    if (
        mode == CARD_CONSOLE_LOCAL &&
        usbOutputEnabled()
    )
    {
        Serial.print(
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
    else
    {
        terminalRenderAll(true);

        Serial.print(
            "\r\n"
            "[BOTH consoles active]"
            "\r\n"
        );
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
        CARD_CONSOLE_BOTH
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

static void pollCardputerKeyboard()
{
    M5Cardputer.update();


    if (
        !M5Cardputer.Keyboard.isChange()
    )
    {
        return;
    }


    if (
        !M5Cardputer.Keyboard.isPressed()
    )
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


static void pollInputs()
{
    /*
     * Always poll the Cardputer because
     * Fn+= must remain available.
     */
    pollCardputerKeyboard();

    pollUSBKeyboard();

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


    _puts(
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
            if (_sys_exists(
                (uint8 *)AUTOEXEC
            ))
            {
                uint16 cmd =
                    CCPaddr + 8;


                uint8 bytesread =
                    (uint8)_RamLoad(
                        (uint8 *)AUTOEXEC,
                        cmd,
                        125
                    );


                uint8 blen = 0;


                while (
                    blen < bytesread &&
                    _RamRead(
                        cmd + blen
                    ) > 31
                )
                {
                    blen++;
                }


                _RamWrite(
                    cmd + blen,
                    0x00
                );


                _RamWrite(
                    --cmd,
                    blen
                );
            }


            if (BOOTONLY)
            {
                firstBoot =
                    FALSE;
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