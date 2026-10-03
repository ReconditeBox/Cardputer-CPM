#ifndef CARDPUTER_NETUI_H
#define CARDPUTER_NETUI_H

static bool networkConsoleReadLine(
    char *buffer,
    size_t bufferSize,
    bool echo = true
)
{
    if (!buffer || bufferSize < 2)
        return false;

    size_t length = 0;
    buffer[0] = 0;

    while (true)
    {
        uint8_t ch = _getcon();

        if (ch == 0x03)
        {
            _puts("^C\r\n");
            buffer[0] = 0;
            return false;
        }

        if (ch == '\r' || ch == '\n')
        {
            _puts("\r\n");
            buffer[length] = 0;
            return true;
        }

        if (ch == 0x08 || ch == 0x7F)
        {
            if (length)
            {
                --length;

                if (echo)
                    _puts("\b \b");
            }

            continue;
        }

        if (ch < 32 || ch > 126)
            continue;

        if (length + 1 >= bufferSize)
            continue;

        buffer[length++] = (char)ch;

        if (echo)
            _putcon(ch);
        else
            _putcon('*');
    }
}


static char *networkConsoleTrim(char *text)
{
    return wifiTrim(text);
}


static void networkConsoleUpperWord(
    char *word
)
{
    if (!word)
        return;

    while (*word)
    {
        *word = (char)toupper((unsigned char)*word);
        ++word;
    }
}

#endif
