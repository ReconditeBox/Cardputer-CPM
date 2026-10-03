#ifndef CARDPUTER_BROWSER_H
#define CARDPUTER_BROWSER_H

/*
 * Interactive Lynx-style text browser for Cardputer-CPM.
 *
 * BROWSE.COM [http://... | https://...]
 *
 * The network fetch, HTML parser and UI all live in the Cardputer host
 * firmware.  BROWSE.COM is only the tiny CP/M BDOS launcher.
 *
 * The browser intentionally targets the useful Lynx interaction model:
 *
 *   Up / Down       previous / next link
 *   Right / Enter   follow selected link
 *   Left / Backspace
 *                   go back
 *   Space / PgDn    page down
 *   - / PgUp        page up
 *   Home / End      top / bottom
 *   G               go to URL
 *   R               reload
 *   D               download selected link through WGET
 *   L               show selected link URL
 *   H / ?           help
 *   Q               quit
 *
 * Pages are fetched completely to SD before parsing/display.  This means the
 * user may spend as long as desired reading a page without holding an HTTP
 * socket open.
 *
 * CSS and JavaScript are not executed.
 */

#define BROWSER_URL_MAX         384
#define BROWSER_LINK_MAX        64
#define BROWSER_TAG_MAX         512
#define BROWSER_WORD_MAX        96
#define BROWSER_LINE_WIDTH      38
#define BROWSER_PAGE_LINES      384
#define BROWSER_VIEW_ROWS       14
#define BROWSER_HISTORY_MAX     8
#define BROWSER_FETCH_TIMEOUT   15000
#define BROWSER_TEMP_FILE       "/BROWSE.TMP"

struct CardputerBrowserLink
{
    char url[
        BROWSER_URL_MAX
    ];

    uint16_t firstLine;
    uint16_t lastLine;
};


/*
 * Browser state is deliberately static.
 *
 * The first browser implementation placed the link table and history on the
 * Arduino task stack, which was large enough to reset the ESP32 immediately
 * on entry.  Keeping the large page/link/session buffers in static DRAM avoids
 * that failure mode.
 */
static CardputerBrowserLink browserLinks[
    BROWSER_LINK_MAX
];

static char browserHistory[
    BROWSER_HISTORY_MAX
][
    BROWSER_URL_MAX
];

static char browserCurrent[
    BROWSER_URL_MAX
];

static char browserCommand[
    BROWSER_URL_MAX + 16
];

static char browserResolved[
    BROWSER_URL_MAX
];

static char browserPage[
    BROWSER_PAGE_LINES
][
    BROWSER_LINE_WIDTH + 1
];

static uint8_t browserPageLink[
    BROWSER_PAGE_LINES
][
    BROWSER_LINE_WIDTH
];

static uint16_t browserPageLineCount =
    1;

static bool browserPageTruncated =
    false;


/*
 * ====================================================
 * URL helpers
 * ====================================================
 */

static char browserDecodeEntity(
    const char *entity
)
{
    if (!entity)
    {
        return 0;
    }

    if (
        strcmp(
            entity,
            "amp"
        ) == 0
    )
    {
        return '&';
    }

    if (
        strcmp(
            entity,
            "lt"
        ) == 0
    )
    {
        return '<';
    }

    if (
        strcmp(
            entity,
            "gt"
        ) == 0
    )
    {
        return '>';
    }

    if (
        strcmp(
            entity,
            "quot"
        ) == 0
    )
    {
        return '"';
    }

    if (
        strcmp(
            entity,
            "apos"
        ) == 0
    )
    {
        return '\'';
    }

    if (
        strcmp(
            entity,
            "nbsp"
        ) == 0
    )
    {
        return ' ';
    }

    if (
        entity[0] ==
        '#'
    )
    {
        long value = 0;

        if (
            entity[1] == 'x' ||
            entity[1] == 'X'
        )
        {
            value =
                strtol(
                    entity + 2,
                    NULL,
                    16
                );
        }
        else
        {
            value =
                strtol(
                    entity + 1,
                    NULL,
                    10
                );
        }

        if (
            value >= 32 &&
            value <= 126
        )
        {
            return (char)value;
        }

        if (value == 160)
        {
            return ' ';
        }
    }

    return 0;
}


static void browserDecodeHrefEntities(
    char *text
)
{
    if (!text)
    {
        return;
    }

    char *read =
        text;

    char *write =
        text;

    while (*read)
    {
        if (*read == '&')
        {
            char entity[16];
            size_t length = 0;

            const char *cursor =
                read + 1;

            while (
                *cursor &&
                *cursor != ';' &&
                length + 1 <
                    sizeof(entity)
            )
            {
                entity[length++] =
                    *cursor++;
            }

            if (*cursor == ';')
            {
                entity[length] = 0;

                char decoded =
                    browserDecodeEntity(
                        entity
                    );

                if (decoded)
                {
                    *write++ =
                        decoded;

                    read =
                        (char *)cursor + 1;

                    continue;
                }
            }
        }

        *write++ =
            *read++;
    }

    *write = 0;
}


static bool browserExtractAttribute(
    const char *tag,
    const char *attribute,
    char *value,
    size_t valueSize
)
{
    if (
        !tag ||
        !attribute ||
        !value ||
        valueSize < 2
    )
    {
        return false;
    }

    size_t attributeLength =
        strlen(
            attribute
        );

    const char *cursor =
        tag;

    while (*cursor)
    {
        while (
            *cursor &&
            (
                isspace(
                    (unsigned char)*cursor
                ) ||
                *cursor == '/'
            )
        )
        {
            cursor++;
        }

        const char *nameStart =
            cursor;

        while (
            *cursor &&
            (
                isalnum(
                    (unsigned char)*cursor
                ) ||
                *cursor == '-' ||
                *cursor == '_'
            )
        )
        {
            cursor++;
        }

        size_t nameLength =
            (size_t)(
                cursor -
                nameStart
            );

        while (
            isspace(
                (unsigned char)*cursor
            )
        )
        {
            cursor++;
        }

        if (*cursor != '=')
        {
            while (
                *cursor &&
                !isspace(
                    (unsigned char)*cursor
                )
            )
            {
                cursor++;
            }

            continue;
        }

        cursor++;

        while (
            isspace(
                (unsigned char)*cursor
            )
        )
        {
            cursor++;
        }

        char quote = 0;

        if (
            *cursor == '"' ||
            *cursor == '\''
        )
        {
            quote =
                *cursor++;
        }

        const char *valueStart =
            cursor;

        if (quote)
        {
            while (
                *cursor &&
                *cursor != quote
            )
            {
                cursor++;
            }
        }
        else
        {
            while (
                *cursor &&
                !isspace(
                    (unsigned char)*cursor
                ) &&
                *cursor != '>'
            )
            {
                cursor++;
            }
        }

        size_t foundLength =
            (size_t)(
                cursor -
                valueStart
            );

        if (
            nameLength ==
                attributeLength &&
            strncasecmp(
                nameStart,
                attribute,
                attributeLength
            ) == 0
        )
        {
            if (
                foundLength >=
                    valueSize
            )
            {
                foundLength =
                    valueSize - 1;
            }

            memcpy(
                value,
                valueStart,
                foundLength
            );

            value[
                foundLength
            ] = 0;

            browserDecodeHrefEntities(
                value
            );

            return true;
        }

        if (
            quote &&
            *cursor == quote
        )
        {
            cursor++;
        }
    }

    return false;
}


static bool browserResolveUrl(
    const char *base,
    const char *reference,
    char *resolved,
    size_t resolvedSize
)
{
    if (
        !reference ||
        !reference[0] ||
        !resolved ||
        resolvedSize < 16
    )
    {
        return false;
    }

    while (
        *reference &&
        isspace(
            (unsigned char)*reference
        )
    )
    {
        reference++;
    }

    if (
        !reference[0] ||
        reference[0] == '#'
    )
    {
        return false;
    }

    if (
        strncasecmp(
            reference,
            "http://",
            7
        ) == 0 ||
        strncasecmp(
            reference,
            "https://",
            8
        ) == 0
    )
    {
        if (
            strlen(reference) >=
                resolvedSize
        )
        {
            return false;
        }

        strcpy(
            resolved,
            reference
        );

        return true;
    }

    if (strchr(
        reference,
        ':'
    ))
    {
        /*
         * Ignore mailto:, javascript:, data:, ftp:, tel:, etc.
         */
        return false;
    }

    if (
        !base ||
        !base[0]
    )
    {
        return false;
    }

    const char *schemeEnd =
        strstr(
            base,
            "://"
        );

    if (!schemeEnd)
    {
        return false;
    }

    size_t schemeLength =
        (size_t)(
            schemeEnd -
            base
        );

    const char *authority =
        schemeEnd + 3;

    const char *basePath =
        strchr(
            authority,
            '/'
        );

    size_t originLength =
        basePath
            ? (size_t)(
                basePath -
                base
              )
            : strlen(base);

    int written = 0;

    if (
        reference[0] == '/' &&
        reference[1] == '/'
    )
    {
        written =
            snprintf(
                resolved,
                resolvedSize,
                "%.*s:%s",
                (int)schemeLength,
                base,
                reference
            );
    }
    else if (
        reference[0] ==
        '/'
    )
    {
        written =
            snprintf(
                resolved,
                resolvedSize,
                "%.*s%s",
                (int)originLength,
                base,
                reference
            );
    }
    else if (
        reference[0] ==
        '?'
    )
    {
        const char *query =
            strchr(
                base,
                '?'
            );

        const char *fragment =
            strchr(
                base,
                '#'
            );

        size_t prefixLength =
            strlen(base);

        if (
            query &&
            (size_t)(
                query - base
            ) <
                prefixLength
        )
        {
            prefixLength =
                (size_t)(
                    query - base
                );
        }

        if (
            fragment &&
            (size_t)(
                fragment - base
            ) <
                prefixLength
        )
        {
            prefixLength =
                (size_t)(
                    fragment - base
                );
        }

        written =
            snprintf(
                resolved,
                resolvedSize,
                "%.*s%s",
                (int)prefixLength,
                base,
                reference
            );
    }
    else
    {
        if (!basePath)
        {
            written =
                snprintf(
                    resolved,
                    resolvedSize,
                    "%s/%s",
                    base,
                    reference
                );
        }
        else
        {
            const char *end =
                basePath +
                strcspn(
                    basePath,
                    "?#"
                );

            const char *lastSlash =
                basePath;

            for (
                const char *scan =
                    basePath;
                scan < end;
                scan++
            )
            {
                if (*scan == '/')
                {
                    lastSlash =
                        scan;
                }
            }

            size_t directoryLength =
                (size_t)(
                    lastSlash -
                    base +
                    1
                );

            written =
                snprintf(
                    resolved,
                    resolvedSize,
                    "%.*s%s",
                    (int)directoryLength,
                    base,
                    reference
                );
        }
    }

    return (
        written > 0 &&
        (size_t)written <
            resolvedSize
    );
}


/*
 * ====================================================
 * Rendered page builder
 * ====================================================
 */

static void browserClearPage()
{
    for (
        uint16_t line = 0;
        line <
            BROWSER_PAGE_LINES;
        line++
    )
    {
        memset(
            browserPage[line],
            ' ',
            BROWSER_LINE_WIDTH
        );

        browserPage[
            line
        ][
            BROWSER_LINE_WIDTH
        ] = 0;

        memset(
            browserPageLink[line],
            0,
            BROWSER_LINE_WIDTH
        );
    }

    browserPageLineCount =
        1;

    browserPageTruncated =
        false;
}


struct CardputerBrowserParser
{
    const char *baseUrl;

    uint8_t linkCount;
    uint8_t activeLink;

    bool inTag;
    char tag[
        BROWSER_TAG_MAX
    ];
    size_t tagLength;

    bool inEntity;
    char entity[20];
    size_t entityLength;

    bool skipScript;
    bool skipStyle;
    bool preformatted;

    char word[
        BROWSER_WORD_MAX
    ];
    size_t wordLength;
    uint8_t wordLink;

    bool pendingSpace;

    uint16_t line;
    uint8_t column;
};


static void browserPageNewline(
    CardputerBrowserParser &parser
)
{
    parser.column = 0;

    if (
        parser.line + 1 >=
            BROWSER_PAGE_LINES
    )
    {
        browserPageTruncated =
            true;

        parser.line =
            BROWSER_PAGE_LINES - 1;

        return;
    }

    parser.line++;

    if (
        parser.line + 1 >
            browserPageLineCount
    )
    {
        browserPageLineCount =
            parser.line + 1;
    }
}


static void browserPagePut(
    CardputerBrowserParser &parser,
    char ch,
    uint8_t linkId
)
{
    if (
        parser.line >=
            BROWSER_PAGE_LINES
    )
    {
        browserPageTruncated =
            true;

        return;
    }

    if (
        parser.column >=
            BROWSER_LINE_WIDTH
    )
    {
        browserPageNewline(
            parser
        );
    }

    if (
        parser.line >=
            BROWSER_PAGE_LINES
    )
    {
        browserPageTruncated =
            true;

        return;
    }

    browserPage[
        parser.line
    ][
        parser.column
    ] = ch;

    browserPageLink[
        parser.line
    ][
        parser.column
    ] = linkId;

    if (
        linkId > 0 &&
        linkId <=
            parser.linkCount
    )
    {
        CardputerBrowserLink &link =
            browserLinks[
                linkId - 1
            ];

        if (
            link.firstLine ==
                0xFFFF
        )
        {
            link.firstLine =
                parser.line;
        }

        link.lastLine =
            parser.line;
    }

    parser.column++;

    if (
        parser.line + 1 >
            browserPageLineCount
    )
    {
        browserPageLineCount =
            parser.line + 1;
    }
}


static void browserFlushWord(
    CardputerBrowserParser &parser
)
{
    if (
        parser.wordLength ==
        0
    )
    {
        return;
    }

    if (
        parser.pendingSpace &&
        parser.column
    )
    {
        if (
            parser.column +
            1 +
            parser.wordLength >
                BROWSER_LINE_WIDTH
        )
        {
            browserPageNewline(
                parser
            );
        }
        else
        {
            browserPagePut(
                parser,
                ' ',
                parser.wordLink
            );
        }
    }
    else if (
        parser.column &&
        parser.column +
        parser.wordLength >
            BROWSER_LINE_WIDTH
    )
    {
        browserPageNewline(
            parser
        );
    }

    for (
        size_t index = 0;
        index <
            parser.wordLength;
        index++
    )
    {
        browserPagePut(
            parser,
            parser.word[index],
            parser.wordLink
        );
    }

    parser.wordLength = 0;
    parser.wordLink = 0;
    parser.pendingSpace = false;
}


static void browserPageSpace(
    CardputerBrowserParser &parser
)
{
    browserFlushWord(
        parser
    );

    if (parser.column)
    {
        parser.pendingSpace =
            true;
    }
}


static void browserPageBreak(
    CardputerBrowserParser &parser
)
{
    browserFlushWord(
        parser
    );

    parser.pendingSpace =
        false;

    if (parser.column)
    {
        browserPageNewline(
            parser
        );
    }
}


static void browserPageBlankLine(
    CardputerBrowserParser &parser
)
{
    browserPageBreak(
        parser
    );

    if (
        parser.line <
            BROWSER_PAGE_LINES - 1
    )
    {
        browserPageNewline(
            parser
        );
    }
}


static void browserFeedVisibleCharacter(
    CardputerBrowserParser &parser,
    char ch
)
{
    if (parser.preformatted)
    {
        browserFlushWord(
            parser
        );

        parser.pendingSpace =
            false;

        if (
            ch == '\r'
        )
        {
            return;
        }

        if (
            ch == '\n'
        )
        {
            browserPageNewline(
                parser
            );

            return;
        }

        if (ch == '\t')
        {
            uint8_t spaces =
                4 -
                (
                    parser.column %
                    4
                );

            while (spaces--)
            {
                browserPagePut(
                    parser,
                    ' ',
                    parser.activeLink
                );
            }

            return;
        }

        if (
            (unsigned char)ch >=
                0x20
        )
        {
            browserPagePut(
                parser,
                ch,
                parser.activeLink
            );
        }

        return;
    }

    if (
        ch == '\r' ||
        ch == '\n' ||
        ch == '\t' ||
        ch == ' '
    )
    {
        browserPageSpace(
            parser
        );

        return;
    }

    if (
        (unsigned char)ch <
            0x20
    )
    {
        return;
    }

    if (
        parser.wordLength + 1 >=
            sizeof(parser.word)
    )
    {
        browserFlushWord(
            parser
        );
    }

    if (
        parser.wordLength ==
        0
    )
    {
        parser.wordLink =
            parser.activeLink;
    }

    parser.word[
        parser.wordLength++
    ] = ch;
}


static uint8_t browserAddLink(
    CardputerBrowserParser &parser,
    const char *url
)
{
    if (
        !url ||
        !url[0] ||
        parser.linkCount >=
            BROWSER_LINK_MAX ||
        parser.line >=
            BROWSER_PAGE_LINES
    )
    {
        return 0;
    }

    CardputerBrowserLink &link =
        browserLinks[
            parser.linkCount
        ];

    strncpy(
        link.url,
        url,
        BROWSER_URL_MAX - 1
    );

    link.url[
        BROWSER_URL_MAX - 1
    ] = 0;

    link.firstLine =
        0xFFFF;

    link.lastLine =
        0xFFFF;

    parser.linkCount++;

    return parser.linkCount;
}


static void browserFeedAttributeText(
    CardputerBrowserParser &parser,
    const char *text
)
{
    if (!text)
    {
        return;
    }

    while (*text)
    {
        browserFeedVisibleCharacter(
            parser,
            *text++
        );
    }
}


static void browserProcessTag(
    CardputerBrowserParser &parser
)
{
    parser.tag[
        parser.tagLength
    ] = 0;

    char *tag =
        parser.tag;

    while (
        *tag &&
        isspace(
            (unsigned char)*tag
        )
    )
    {
        tag++;
    }

    if (
        strncmp(
            tag,
            "!--",
            3
        ) == 0 ||
        tag[0] == '!'
    )
    {
        return;
    }

    bool closing =
        false;

    if (*tag == '/')
    {
        closing =
            true;

        tag++;

        while (
            *tag &&
            isspace(
                (unsigned char)*tag
            )
        )
        {
            tag++;
        }
    }

    char tagName[16];

    size_t tagNameLength =
        0;

    while (
        *tag &&
        !isspace(
            (unsigned char)*tag
        ) &&
        *tag != '/' &&
        tagNameLength + 1 <
            sizeof(tagName)
    )
    {
        tagName[
            tagNameLength++
        ] =
            (char)tolower(
                (unsigned char)*tag++
            );
    }

    tagName[
        tagNameLength
    ] = 0;

    if (
        strcmp(
            tagName,
            "script"
        ) == 0
    )
    {
        parser.skipScript =
            !closing;

        return;
    }

    if (
        strcmp(
            tagName,
            "style"
        ) == 0
    )
    {
        parser.skipStyle =
            !closing;

        return;
    }

    if (
        parser.skipScript ||
        parser.skipStyle
    )
    {
        return;
    }

    if (
        strcmp(
            tagName,
            "pre"
        ) == 0
    )
    {
        browserPageBreak(
            parser
        );

        parser.preformatted =
            !closing;

        if (closing)
        {
            browserPageBreak(
                parser
            );
        }

        return;
    }

    if (
        strcmp(
            tagName,
            "a"
        ) == 0
    )
    {
        browserFlushWord(
            parser
        );

        if (closing)
        {
            parser.activeLink = 0;
            return;
        }

        char href[
            BROWSER_URL_MAX
        ];

        char resolved[
            BROWSER_URL_MAX
        ];

        if (
            browserExtractAttribute(
                parser.tag,
                "href",
                href,
                sizeof(href)
            ) &&
            browserResolveUrl(
                parser.baseUrl,
                href,
                resolved,
                sizeof(resolved)
            )
        )
        {
            parser.activeLink =
                browserAddLink(
                    parser,
                    resolved
                );
        }
        else
        {
            parser.activeLink = 0;
        }

        return;
    }

    if (
        !closing &&
        strcmp(
            tagName,
            "img"
        ) == 0
    )
    {
        char alt[
            BROWSER_WORD_MAX
        ];

        if (browserExtractAttribute(
            parser.tag,
            "alt",
            alt,
            sizeof(alt)
        ))
        {
            browserFeedAttributeText(
                parser,
                alt
            );
        }

        return;
    }

    if (
        strcmp(
            tagName,
            "br"
        ) == 0 ||
        strcmp(
            tagName,
            "hr"
        ) == 0
    )
    {
        browserPageBreak(
            parser
        );

        return;
    }

    if (
        !closing &&
        strcmp(
            tagName,
            "li"
        ) == 0
    )
    {
        browserPageBreak(
            parser
        );

        browserFeedVisibleCharacter(
            parser,
            '*'
        );

        browserPageSpace(
            parser
        );

        return;
    }

    if (
        strcmp(
            tagName,
            "p"
        ) == 0 ||
        strcmp(
            tagName,
            "div"
        ) == 0 ||
        strcmp(
            tagName,
            "section"
        ) == 0 ||
        strcmp(
            tagName,
            "article"
        ) == 0 ||
        strcmp(
            tagName,
            "header"
        ) == 0 ||
        strcmp(
            tagName,
            "footer"
        ) == 0 ||
        strcmp(
            tagName,
            "table"
        ) == 0 ||
        strcmp(
            tagName,
            "tr"
        ) == 0 ||
        strcmp(
            tagName,
            "h1"
        ) == 0 ||
        strcmp(
            tagName,
            "h2"
        ) == 0 ||
        strcmp(
            tagName,
            "h3"
        ) == 0 ||
        strcmp(
            tagName,
            "h4"
        ) == 0 ||
        strcmp(
            tagName,
            "h5"
        ) == 0 ||
        strcmp(
            tagName,
            "h6"
        ) == 0
    )
    {
        browserPageBreak(
            parser
        );
    }
}


static void browserFeedHtmlByte(
    CardputerBrowserParser &parser,
    char ch
)
{
    if (parser.inTag)
    {
        if (ch == '>')
        {
            parser.inTag =
                false;

            browserProcessTag(
                parser
            );

            parser.tagLength = 0;

            return;
        }

        if (
            parser.tagLength + 1 <
                sizeof(parser.tag)
        )
        {
            parser.tag[
                parser.tagLength++
            ] = ch;
        }

        return;
    }

    if (ch == '<')
    {
        browserFlushWord(
            parser
        );

        if (parser.inEntity)
        {
            browserFeedVisibleCharacter(
                parser,
                '&'
            );

            for (
                size_t index = 0;
                index <
                    parser.entityLength;
                index++
            )
            {
                browserFeedVisibleCharacter(
                    parser,
                    parser.entity[index]
                );
            }

            parser.inEntity = false;
            parser.entityLength = 0;
        }

        parser.inTag = true;
        parser.tagLength = 0;

        return;
    }

    if (
        parser.skipScript ||
        parser.skipStyle
    )
    {
        return;
    }

    if (parser.inEntity)
    {
        if (ch == ';')
        {
            parser.entity[
                parser.entityLength
            ] = 0;

            char decoded =
                browserDecodeEntity(
                    parser.entity
                );

            if (decoded)
            {
                browserFeedVisibleCharacter(
                    parser,
                    decoded
                );
            }

            parser.inEntity = false;
            parser.entityLength = 0;

            return;
        }

        if (
            parser.entityLength + 1 <
                sizeof(parser.entity) &&
            (
                isalnum(
                    (unsigned char)ch
                ) ||
                ch == '#' ||
                ch == 'x' ||
                ch == 'X'
            )
        )
        {
            parser.entity[
                parser.entityLength++
            ] = ch;

            return;
        }

        browserFeedVisibleCharacter(
            parser,
            '&'
        );

        for (
            size_t index = 0;
            index <
                parser.entityLength;
            index++
        )
        {
            browserFeedVisibleCharacter(
                parser,
                parser.entity[index]
            );
        }

        parser.inEntity = false;
        parser.entityLength = 0;
    }

    if (ch == '&')
    {
        parser.inEntity = true;
        parser.entityLength = 0;

        return;
    }

    browserFeedVisibleCharacter(
        parser,
        ch
    );
}


static void browserFinishParser(
    CardputerBrowserParser &parser
)
{
    if (parser.inEntity)
    {
        browserFeedVisibleCharacter(
            parser,
            '&'
        );

        for (
            size_t index = 0;
            index <
                parser.entityLength;
            index++
        )
        {
            browserFeedVisibleCharacter(
                parser,
                parser.entity[index]
            );
        }
    }

    browserFlushWord(
        parser
    );

    while (
        browserPageLineCount > 1
    )
    {
        uint16_t last =
            browserPageLineCount - 1;

        bool empty =
            true;

        for (
            uint8_t col = 0;
            col <
                BROWSER_LINE_WIDTH;
            col++
        )
        {
            if (
                browserPage[
                    last
                ][
                    col
                ] != ' '
            )
            {
                empty = false;
                break;
            }
        }

        if (!empty)
        {
            break;
        }

        browserPageLineCount--;
    }
}


/*
 * ====================================================
 * HTTP fetch + parse
 * ====================================================
 */

static bool browserFetchPage(
    const char *url,
    uint8_t &linkCount
)
{
    if (
        !url ||
        (
            strncasecmp(
                url,
                "http://",
                7
            ) != 0 &&
            strncasecmp(
                url,
                "https://",
                8
            ) != 0
        )
    )
    {
        return false;
    }

    bool secure =
        strncasecmp(
            url,
            "https://",
            8
        ) == 0;

    HTTPClient http;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;

    if (secure)
    {
        secureClient.setInsecure();

        if (!http.begin(
            secureClient,
            url
        ))
        {
            return false;
        }
    }
    else
    {
        if (!http.begin(
            plainClient,
            url
        ))
        {
            return false;
        }
    }

    http.useHTTP10(
        true
    );

    http.setReuse(
        false
    );

    http.setTimeout(
        BROWSER_FETCH_TIMEOUT
    );

    http.setFollowRedirects(
        HTTPC_STRICT_FOLLOW_REDIRECTS
    );

    http.addHeader(
        "Accept",
        "text/html,text/plain;q=0.9,*/*;q=0.1"
    );

    http.addHeader(
        "Accept-Encoding",
        "identity"
    );

    int response =
        http.GET();

    if (
        response < 200 ||
        response >= 300
    )
    {
        http.end();
        return false;
    }

    SD.remove(
        BROWSER_TEMP_FILE
    );

    File cachedPage =
        SD.open(
            BROWSER_TEMP_FILE,
            O_CREAT |
            O_WRITE |
            O_TRUNC
        );

    if (!cachedPage)
    {
        http.end();
        return false;
    }

    WiFiClient *stream =
        http.getStreamPtr();

    int remaining =
        http.getSize();

    uint8_t buffer[512];

    uint32_t lastData =
        millis();

    bool okay =
        true;

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
                sizeof(buffer)
            )
            {
                wanted =
                    sizeof(buffer);
            }

            int got =
                stream->readBytes(
                    buffer,
                    wanted
                );

            if (got <= 0)
            {
                okay = false;
                break;
            }

            size_t written =
                cachedPage.write(
                    buffer,
                    (size_t)got
                );

            if (
                written !=
                (size_t)got
            )
            {
                okay = false;
                break;
            }

            if (remaining > 0)
            {
                remaining -=
                    got;

                if (remaining < 0)
                {
                    remaining = 0;
                }
            }

            lastData =
                millis();

            continue;
        }

        if (remaining == 0)
        {
            break;
        }

        if (!http.connected())
        {
            if (remaining < 0)
            {
                break;
            }

            okay = false;
            break;
        }

        if (
            (uint32_t)(
                millis() -
                lastData
            ) >
                BROWSER_FETCH_TIMEOUT
        )
        {
            okay = false;
            break;
        }

        delay(1);
    }

    cachedPage.flush();
    cachedPage.close();

    http.end();

    if (!okay)
    {
        SD.remove(
            BROWSER_TEMP_FILE
        );

        return false;
    }

    File page =
        SD.open(
            BROWSER_TEMP_FILE,
            O_READ
        );

    if (!page)
    {
        SD.remove(
            BROWSER_TEMP_FILE
        );

        return false;
    }

    browserClearPage();

    for (
        uint8_t index = 0;
        index <
            BROWSER_LINK_MAX;
        index++
    )
    {
        browserLinks[
            index
        ].url[0] = 0;

        browserLinks[
            index
        ].firstLine = 0xFFFF;

        browserLinks[
            index
        ].lastLine = 0xFFFF;
    }

    CardputerBrowserParser parser = {};

    parser.baseUrl =
        url;

    while (page.available())
    {
        int value =
            page.read();

        if (value < 0)
        {
            break;
        }

        browserFeedHtmlByte(
            parser,
            (char)value
        );
    }

    page.close();

    SD.remove(
        BROWSER_TEMP_FILE
    );

    browserFinishParser(
        parser
    );

    /*
     * Drop anchors which had no visible text/ALT content.  They cannot be
     * selected meaningfully in a text browser.
     */
    uint8_t compacted =
        0;

    for (
        uint8_t oldIndex = 0;
        oldIndex <
            parser.linkCount;
        oldIndex++
    )
    {
        if (
            browserLinks[
                oldIndex
            ].firstLine ==
                0xFFFF
        )
        {
            uint8_t oldId =
                oldIndex + 1;

            for (
                uint16_t line = 0;
                line <
                    browserPageLineCount;
                line++
            )
            {
                for (
                    uint8_t col = 0;
                    col <
                        BROWSER_LINE_WIDTH;
                    col++
                )
                {
                    if (
                        browserPageLink[
                            line
                        ][
                            col
                        ] ==
                            oldId
                    )
                    {
                        browserPageLink[
                            line
                        ][
                            col
                        ] = 0;
                    }
                }
            }

            continue;
        }

        if (
            compacted !=
                oldIndex
        )
        {
            browserLinks[
                compacted
            ] =
                browserLinks[
                    oldIndex
                ];

            uint8_t oldId =
                oldIndex + 1;

            uint8_t newId =
                compacted + 1;

            for (
                uint16_t line = 0;
                line <
                    browserPageLineCount;
                line++
            )
            {
                for (
                    uint8_t col = 0;
                    col <
                        BROWSER_LINE_WIDTH;
                    col++
                )
                {
                    if (
                        browserPageLink[
                            line
                        ][
                            col
                        ] ==
                            oldId
                    )
                    {
                        browserPageLink[
                            line
                        ][
                            col
                        ] =
                            newId;
                    }
                }
            }
        }

        compacted++;
    }

    linkCount =
        compacted;

    return true;
}


/*
 * ====================================================
 * Interactive terminal UI
 * ====================================================
 */

static void browserWritePaddedLine(
    const char *text,
    bool reverse
)
{
    if (reverse)
    {
        _puts(
            "\x1B[7m"
        );
    }

    size_t length =
        text
            ? strlen(text)
            : 0;

    if (
        length >
        BROWSER_LINE_WIDTH
    )
    {
        length =
            BROWSER_LINE_WIDTH;
    }

    for (
        size_t index = 0;
        index < length;
        index++
    )
    {
        _putcon(
            (uint8_t)text[index]
        );
    }

    for (
        size_t index = length;
        index <
            BROWSER_LINE_WIDTH;
        index++
    )
    {
        _putcon(' ');
    }

    if (reverse)
    {
        _puts(
            "\x1B[0m"
        );
    }
}


static void browserPosition(
    uint8_t row,
    uint8_t column = 1
)
{
    char sequence[24];

    snprintf(
        sequence,
        sizeof(sequence),
        "\x1B[%u;%uH",
        row,
        column
    );

    _puts(
        sequence
    );
}


static int browserFirstSelectableLink(
    uint8_t linkCount
)
{
    for (
        uint8_t index = 0;
        index < linkCount;
        index++
    )
    {
        if (
            browserLinks[
                index
            ].firstLine !=
                0xFFFF
        )
        {
            return index;
        }
    }

    return -1;
}


static void browserClampViewport(
    uint16_t &topLine
)
{
    uint16_t maximum =
        browserPageLineCount >
            BROWSER_VIEW_ROWS
            ? browserPageLineCount -
                BROWSER_VIEW_ROWS
            : 0;

    if (
        topLine >
            maximum
    )
    {
        topLine =
            maximum;
    }
}


static void browserEnsureSelectionVisible(
    int selectedLink,
    uint16_t &topLine
)
{
    if (
        selectedLink < 0 ||
        selectedLink >=
            BROWSER_LINK_MAX
    )
    {
        browserClampViewport(
            topLine
        );

        return;
    }

    CardputerBrowserLink &link =
        browserLinks[
            selectedLink
        ];

    if (
        link.firstLine ==
            0xFFFF
    )
    {
        return;
    }

    if (
        link.firstLine <
            topLine
    )
    {
        topLine =
            link.firstLine;
    }
    else if (
        link.lastLine >=
            topLine +
            BROWSER_VIEW_ROWS
    )
    {
        topLine =
            link.lastLine -
            BROWSER_VIEW_ROWS +
            1;
    }

    browserClampViewport(
        topLine
    );
}


static int browserLinkOnOrAfterLine(
    uint8_t linkCount,
    uint16_t line
)
{
    for (
        uint8_t index = 0;
        index < linkCount;
        index++
    )
    {
        if (
            browserLinks[
                index
            ].firstLine !=
                0xFFFF &&
            browserLinks[
                index
            ].lastLine >=
                line
        )
        {
            return index;
        }
    }

    return browserFirstSelectableLink(
        linkCount
    );
}


static void browserRender(
    uint16_t topLine,
    int selectedLink,
    uint8_t linkCount
)
{
    _puts(
        "\x1B[2J\x1B[H"
    );

    for (
        uint8_t screenRow = 0;
        screenRow <
            BROWSER_VIEW_ROWS;
        screenRow++
    )
    {
        browserPosition(
            screenRow + 1
        );

        uint16_t pageLine =
            topLine +
            screenRow;

        bool reverse =
            false;

        for (
            uint8_t col = 0;
            col <
                BROWSER_LINE_WIDTH;
            col++
        )
        {
            char ch = ' ';
            uint8_t linkId = 0;

            if (
                pageLine <
                    browserPageLineCount
            )
            {
                ch =
                    browserPage[
                        pageLine
                    ][
                        col
                    ];

                linkId =
                    browserPageLink[
                        pageLine
                    ][
                        col
                    ];
            }

            bool shouldReverse =
                (
                    selectedLink >= 0 &&
                    linkId ==
                        (uint8_t)(
                            selectedLink +
                            1
                        )
                );

            if (
                shouldReverse !=
                    reverse
            )
            {
                _puts(
                    shouldReverse
                        ? "\x1B[7m"
                        : "\x1B[0m"
                );

                reverse =
                    shouldReverse;
            }

            _putcon(
                (uint8_t)ch
            );
        }

        if (reverse)
        {
            _puts(
                "\x1B[0m"
            );
        }
    }

    char status[
        BROWSER_LINE_WIDTH + 1
    ];

    memset(
        status,
        ' ',
        BROWSER_LINE_WIDTH
    );

    status[
        BROWSER_LINE_WIDTH
    ] = 0;

    const char *statusText =
        browserCurrent;

    if (
        selectedLink >= 0 &&
        selectedLink <
            linkCount
    )
    {
        statusText =
            browserLinks[
                selectedLink
            ].url;
    }

    if (browserPageTruncated)
    {
        snprintf(
            status,
            sizeof(status),
            "TRUNC %u/%u %.24s",
            (unsigned)(
                topLine + 1
            ),
            (unsigned)
                browserPageLineCount,
            statusText
        );
    }
    else
    {
        snprintf(
            status,
            sizeof(status),
            "%u/%u %.30s",
            (unsigned)(
                topLine + 1
            ),
            (unsigned)
                browserPageLineCount,
            statusText
        );
    }

    browserPosition(
        15
    );

    browserWritePaddedLine(
        status,
        true
    );

    browserPosition(
        16
    );

    browserWritePaddedLine(
        "Arrows links  Space page  G H Q",
        false
    );

    /*
     * Keep the cursor at the bottom without moving beyond the physical
     * 16-row viewport.
     */
    browserPosition(
        16,
        BROWSER_LINE_WIDTH
    );

    terminalMaybeRefresh();
}


enum CardputerBrowserKey
{
    BROWSER_KEY_CHARACTER =
        0,

    BROWSER_KEY_UP,
    BROWSER_KEY_DOWN,
    BROWSER_KEY_LEFT,
    BROWSER_KEY_RIGHT,
    BROWSER_KEY_PAGE_UP,
    BROWSER_KEY_PAGE_DOWN,
    BROWSER_KEY_HOME,
    BROWSER_KEY_END,
    BROWSER_KEY_ENTER,
    BROWSER_KEY_BACKSPACE
};


struct CardputerBrowserInput
{
    CardputerBrowserKey key;
    uint8_t character;
};


static bool browserReadEscapeByte(
    uint8_t &value
)
{
    uint32_t started =
        millis();

    while (!_chready())
    {
        if (
            (uint32_t)(
                millis() -
                started
            ) >
                100
        )
        {
            return false;
        }

        delay(1);
    }

    value =
        _getcon();

    return true;
}


static CardputerBrowserInput browserReadInput()
{
    CardputerBrowserInput input =
    {
        BROWSER_KEY_CHARACTER,
        0
    };

    uint8_t ch =
        _getcon();

    if (
        ch == '\r' ||
        ch == '\n'
    )
    {
        input.key =
            BROWSER_KEY_ENTER;

        return input;
    }

    if (
        ch == 0x08 ||
        ch == 0x7F
    )
    {
        input.key =
            BROWSER_KEY_BACKSPACE;

        return input;
    }

    if (ch != 0x1B)
    {
        input.character =
            ch;

        return input;
    }

    uint8_t second = 0;

    if (!browserReadEscapeByte(
        second
    ))
    {
        input.character =
            0x1B;

        return input;
    }

    if (
        second == '['
    )
    {
        uint8_t third = 0;

        if (!browserReadEscapeByte(
            third
        ))
        {
            return input;
        }

        if (third == 'A')
        {
            input.key =
                BROWSER_KEY_UP;
        }
        else if (third == 'B')
        {
            input.key =
                BROWSER_KEY_DOWN;
        }
        else if (third == 'C')
        {
            input.key =
                BROWSER_KEY_RIGHT;
        }
        else if (third == 'D')
        {
            input.key =
                BROWSER_KEY_LEFT;
        }
        else if (
            third == 'H'
        )
        {
            input.key =
                BROWSER_KEY_HOME;
        }
        else if (
            third == 'F'
        )
        {
            input.key =
                BROWSER_KEY_END;
        }
        else if (
            third >= '0' &&
            third <= '9'
        )
        {
            uint16_t number =
                third - '0';

            while (true)
            {
                uint8_t next = 0;

                if (!browserReadEscapeByte(
                    next
                ))
                {
                    break;
                }

                if (
                    next >= '0' &&
                    next <= '9'
                )
                {
                    number =
                        number * 10 +
                        (
                            next - '0'
                        );

                    continue;
                }

                if (next == '~')
                {
                    if (
                        number == 1 ||
                        number == 7
                    )
                    {
                        input.key =
                            BROWSER_KEY_HOME;
                    }
                    else if (
                        number == 4 ||
                        number == 8
                    )
                    {
                        input.key =
                            BROWSER_KEY_END;
                    }
                    else if (
                        number == 5
                    )
                    {
                        input.key =
                            BROWSER_KEY_PAGE_UP;
                    }
                    else if (
                        number == 6
                    )
                    {
                        input.key =
                            BROWSER_KEY_PAGE_DOWN;
                    }
                }

                break;
            }
        }

        return input;
    }

    if (
        second == 'O'
    )
    {
        uint8_t third = 0;

        if (!browserReadEscapeByte(
            third
        ))
        {
            return input;
        }

        if (third == 'A')
        {
            input.key =
                BROWSER_KEY_UP;
        }
        else if (third == 'B')
        {
            input.key =
                BROWSER_KEY_DOWN;
        }
        else if (third == 'C')
        {
            input.key =
                BROWSER_KEY_RIGHT;
        }
        else if (third == 'D')
        {
            input.key =
                BROWSER_KEY_LEFT;
        }
        else if (third == 'H')
        {
            input.key =
                BROWSER_KEY_HOME;
        }
        else if (third == 'F')
        {
            input.key =
                BROWSER_KEY_END;
        }

        return input;
    }

    return input;
}


static void browserPushHistory(
    const char *url,
    uint8_t &historyCount
)
{
    if (
        !url ||
        !url[0]
    )
    {
        return;
    }

    if (
        historyCount <
            BROWSER_HISTORY_MAX
    )
    {
        strncpy(
            browserHistory[
                historyCount++
            ],
            url,
            BROWSER_URL_MAX - 1
        );

        browserHistory[
            historyCount - 1
        ][
            BROWSER_URL_MAX - 1
        ] = 0;

        return;
    }

    for (
        uint8_t index = 1;
        index <
            BROWSER_HISTORY_MAX;
        index++
    )
    {
        strcpy(
            browserHistory[
                index - 1
            ],
            browserHistory[
                index
            ]
        );
    }

    strncpy(
        browserHistory[
            BROWSER_HISTORY_MAX - 1
        ],
        url,
        BROWSER_URL_MAX - 1
    );

    browserHistory[
        BROWSER_HISTORY_MAX - 1
    ][
        BROWSER_URL_MAX - 1
    ] = 0;
}


static bool browserGoBack(
    uint8_t &historyCount,
    uint8_t &linkCount,
    int &selectedLink,
    uint16_t &topLine
)
{
    if (!historyCount)
    {
        return false;
    }

    char previous[
        BROWSER_URL_MAX
    ];

    strcpy(
        previous,
        browserHistory[
            historyCount - 1
        ]
    );

    historyCount--;

    browserLoadingScreen(
        previous
    );

    if (!browserFetchPage(
        previous,
        linkCount
    ))
    {
        historyCount++;
        return false;
    }

    strcpy(
        browserCurrent,
        previous
    );

    topLine = 0;

    selectedLink =
        browserFirstSelectableLink(
            linkCount
        );

    browserEnsureSelectionVisible(
        selectedLink,
        topLine
    );

    return true;
}


static bool browserFollowLink(
    int selectedLink,
    uint8_t &historyCount,
    uint8_t &linkCount,
    uint16_t &topLine
)
{
    if (
        selectedLink < 0 ||
        selectedLink >=
            linkCount
    )
    {
        return false;
    }

    char target[
        BROWSER_URL_MAX
    ];

    strcpy(
        target,
        browserLinks[
            selectedLink
        ].url
    );

    browserPushHistory(
        browserCurrent,
        historyCount
    );

    browserLoadingScreen(
        target
    );

    if (!browserFetchPage(
        target,
        linkCount
    ))
    {
        if (historyCount)
        {
            historyCount--;
        }

        return false;
    }

    strcpy(
        browserCurrent,
        target
    );

    topLine = 0;

    return true;
}


static void browserLoadingScreen(
    const char *url
)
{
    _puts(
        "\x1B[2J\x1B[H"
        "\x1B[7m"
        "BROWSE loading"
        "\x1B[0m\r\n\r\n"
    );

    if (url)
    {
        _puts(
            url
        );
    }

    _puts(
        "\r\n"
    );

    terminalMaybeRefresh();
}


static void browserMessageScreen(
    const char *title,
    const char *text
)
{
    _puts(
        "\x1B[2J\x1B[H"
    );

    if (title)
    {
        _puts(
            "\x1B[7m"
        );

        browserWritePaddedLine(
            title,
            false
        );

        _puts(
            "\x1B[0m\r\n"
        );
    }

    if (text)
    {
        _puts(
            text
        );
    }

    _puts(
        "\r\n\r\nPress any key..."
    );

    _getcon();
}


static void browserHelp()
{
    browserMessageScreen(
        "BROWSE help",
        "Up/Down     previous/next link\r\n"
        "Right/Enter follow selected link\r\n"
        "Left/Bksp   back\r\n"
        "Space/PgDn  page down\r\n"
        "-/PgUp      page up\r\n"
        "Home/End    top/bottom\r\n"
        "G           go to URL\r\n"
        "R           reload\r\n"
        "D           download selected link\r\n"
        "L           show selected URL\r\n"
        "H or ?      this help\r\n"
        "Q           quit"
    );
}


static void browserShowSelectedUrl(
    int selectedLink,
    uint8_t linkCount
)
{
    if (
        selectedLink < 0 ||
        selectedLink >=
            linkCount
    )
    {
        browserMessageScreen(
            "Selected link",
            "No link is selected."
        );

        return;
    }

    browserMessageScreen(
        "Selected link",
        browserLinks[
            selectedLink
        ].url
    );
}


static void browserDownloadSelected(
    int selectedLink,
    uint8_t linkCount
)
{
    if (
        selectedLink < 0 ||
        selectedLink >=
            linkCount
    )
    {
        browserMessageScreen(
            "Download",
            "No link is selected."
        );

        return;
    }

    _puts(
        "\x1B[2J\x1B[H"
        "Download selected link\r\n"
        "----------------------\r\n"
    );

    _puts(
        browserLinks[
            selectedLink
        ].url
    );

    _puts(
        "\r\n\r\n"
    );

    char destination[
        WGET_NAME_SIZE + 3
    ];

    if (!cardputerNetworkReadLine(
        "Save as (blank=URL name): ",
        destination,
        sizeof(destination)
    ))
    {
        return;
    }

    char tail[128];

    int written = 0;

    if (destination[0])
    {
        written =
            snprintf(
                tail,
                sizeof(tail),
                "%s %s",
                browserLinks[
                    selectedLink
                ].url,
                destination
            );
    }
    else
    {
        written =
            snprintf(
                tail,
                sizeof(tail),
                "%s",
                browserLinks[
                    selectedLink
                ].url
            );
    }

    if (
        written <= 0 ||
        written > 127
    )
    {
        browserMessageScreen(
            "Download",
            "URL is too long for the CP/M WGET command tail."
        );

        return;
    }

    _RamWrite(
        defDMA,
        (uint8_t)written
    );

    for (
        int index = 0;
        index < written;
        index++
    )
    {
        _RamWrite(
            defDMA + 1 + index,
            (uint8_t)tail[index]
        );
    }

    _puts(
        "\r\n"
    );

    cardputerWgetBdos(
        defDMA
    );

    _puts(
        "\r\nPress any key..."
    );

    _getcon();
}


static bool browserPromptUrl(
    char *resolved,
    size_t resolvedSize
)
{
    _puts(
        "\x1B[2J\x1B[H"
        "Go to URL\r\n"
        "---------\r\n"
    );

    char entered[
        BROWSER_URL_MAX
    ];

    if (!cardputerNetworkReadLine(
        "URL: ",
        entered,
        sizeof(entered)
    ))
    {
        return false;
    }

    char *url =
        wifiTrim(
            entered
        );

    if (!url[0])
    {
        return false;
    }

    if (browserResolveUrl(
        browserCurrent,
        url,
        resolved,
        resolvedSize
    ))
    {
        return true;
    }

    if (
        (
            strncasecmp(
                url,
                "http://",
                7
            ) == 0 ||
            strncasecmp(
                url,
                "https://",
                8
            ) == 0
        ) &&
        strlen(url) <
            resolvedSize
    )
    {
        strcpy(
            resolved,
            url
        );

        return true;
    }

    return false;
}


/*
 * ====================================================
 * CP/M entry point
 * ====================================================
 */

uint16 cardputerBrowserBdos(
    uint16 commandTail
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        _puts(
            "\r\nBROWSE: WiFi is offline\r\n"
        );

        return 0x00FF;
    }

    if (ftpIsActive())
    {
        _puts(
            "\r\nBROWSE: unavailable while FTPD is active\r\n"
        );

        return 0x00FF;
    }

    char tail[129];

    if (!networkReadCommandTail(
        commandTail,
        tail,
        sizeof(tail)
    ))
    {
        return 0x00FF;
    }

    char *initial =
        wifiTrim(
            tail
        );

    if (initial[0])
    {
        if (
            strlen(initial) >=
                sizeof(browserCurrent)
        )
        {
            _puts(
                "\r\nBROWSE: URL is too long\r\n"
            );

            return 0x00FF;
        }

        strcpy(
            browserCurrent,
            initial
        );
    }
    else
    {
        _puts(
            "\x1B[2J\x1B[H"
            "BROWSE\r\n"
            "------\r\n"
        );

        if (!cardputerNetworkReadLine(
            "URL: ",
            browserCurrent,
            sizeof(browserCurrent)
        ))
        {
            return 0;
        }
    }

    uint8_t linkCount =
        0;

    browserLoadingScreen(
        browserCurrent
    );

    if (!browserFetchPage(
        browserCurrent,
        linkCount
    ))
    {
        _puts(
            "\r\nBROWSE: unable to load page\r\n"
        );

        return 0x00FF;
    }

    uint8_t historyCount =
        0;

    int selectedLink =
        browserFirstSelectableLink(
            linkCount
        );

    uint16_t topLine =
        0;

    browserEnsureSelectionVisible(
        selectedLink,
        topLine
    );

    while (true)
    {
        browserRender(
            topLine,
            selectedLink,
            linkCount
        );

        CardputerBrowserInput input =
            browserReadInput();

        if (
            input.key ==
                BROWSER_KEY_UP ||
            (
                input.key ==
                    BROWSER_KEY_CHARACTER &&
                (
                    input.character == 'k' ||
                    input.character == 'K'
                )
            )
        )
        {
            if (
                linkCount &&
                selectedLink > 0
            )
            {
                selectedLink--;

                browserEnsureSelectionVisible(
                    selectedLink,
                    topLine
                );
            }

            continue;
        }

        if (
            input.key ==
                BROWSER_KEY_DOWN ||
            (
                input.key ==
                    BROWSER_KEY_CHARACTER &&
                (
                    input.character == 'j' ||
                    input.character == 'J'
                )
            )
        )
        {
            if (
                linkCount &&
                selectedLink >= 0 &&
                selectedLink + 1 <
                    linkCount
            )
            {
                selectedLink++;

                browserEnsureSelectionVisible(
                    selectedLink,
                    topLine
                );
            }
            else if (
                linkCount &&
                selectedLink < 0
            )
            {
                selectedLink =
                    browserFirstSelectableLink(
                        linkCount
                    );

                browserEnsureSelectionVisible(
                    selectedLink,
                    topLine
                );
            }

            continue;
        }

        if (
            input.key ==
                BROWSER_KEY_RIGHT ||
            input.key ==
                BROWSER_KEY_ENTER
        )
        {
            if (browserFollowLink(
                selectedLink,
                historyCount,
                linkCount,
                topLine
            ))
            {
                selectedLink =
                    browserFirstSelectableLink(
                        linkCount
                    );

                browserEnsureSelectionVisible(
                    selectedLink,
                    topLine
                );
            }
            else
            {
                browserMessageScreen(
                    "BROWSE",
                    "Unable to follow selected link."
                );
            }

            continue;
        }

        if (
            input.key ==
                BROWSER_KEY_LEFT ||
            input.key ==
                BROWSER_KEY_BACKSPACE
        )
        {
            if (!browserGoBack(
                historyCount,
                linkCount,
                selectedLink,
                topLine
            ))
            {
                browserMessageScreen(
                    "BROWSE",
                    "No previous page."
                );
            }

            continue;
        }

        if (
            input.key ==
                BROWSER_KEY_PAGE_DOWN ||
            (
                input.key ==
                    BROWSER_KEY_CHARACTER &&
                input.character == ' '
            )
        )
        {
            if (
                topLine +
                    BROWSER_VIEW_ROWS <
                browserPageLineCount
            )
            {
                topLine +=
                    BROWSER_VIEW_ROWS - 1;

                browserClampViewport(
                    topLine
                );

                selectedLink =
                    browserLinkOnOrAfterLine(
                        linkCount,
                        topLine
                    );
            }

            continue;
        }

        if (
            input.key ==
                BROWSER_KEY_PAGE_UP ||
            (
                input.key ==
                    BROWSER_KEY_CHARACTER &&
                input.character == '-'
            )
        )
        {
            if (
                topLine >=
                    BROWSER_VIEW_ROWS - 1
            )
            {
                topLine -=
                    BROWSER_VIEW_ROWS - 1;
            }
            else
            {
                topLine = 0;
            }

            selectedLink =
                browserLinkOnOrAfterLine(
                    linkCount,
                    topLine
                );

            continue;
        }

        if (
            input.key ==
                BROWSER_KEY_HOME
        )
        {
            topLine = 0;

            selectedLink =
                browserLinkOnOrAfterLine(
                    linkCount,
                    topLine
                );

            continue;
        }

        if (
            input.key ==
                BROWSER_KEY_END
        )
        {
            topLine =
                browserPageLineCount >
                    BROWSER_VIEW_ROWS
                    ? browserPageLineCount -
                        BROWSER_VIEW_ROWS
                    : 0;

            selectedLink =
                browserLinkOnOrAfterLine(
                    linkCount,
                    topLine
                );

            continue;
        }

        if (
            input.key !=
                BROWSER_KEY_CHARACTER
        )
        {
            continue;
        }

        uint8_t command =
            (uint8_t)toupper(
                input.character
            );

        if (command == 'Q')
        {
            break;
        }

        if (command == 'H' ||
            input.character == '?')
        {
            browserHelp();
            continue;
        }

        if (command == 'L')
        {
            browserShowSelectedUrl(
                selectedLink,
                linkCount
            );

            continue;
        }

        if (command == 'D')
        {
            browserDownloadSelected(
                selectedLink,
                linkCount
            );

            continue;
        }

        if (command == 'R')
        {
            browserLoadingScreen(
                browserCurrent
            );

            if (!browserFetchPage(
                browserCurrent,
                linkCount
            ))
            {
                browserMessageScreen(
                    "BROWSE",
                    "Reload failed."
                );

                continue;
            }

            selectedLink =
                browserFirstSelectableLink(
                    linkCount
                );

            topLine = 0;

            browserEnsureSelectionVisible(
                selectedLink,
                topLine
            );

            continue;
        }

        if (command == 'G')
        {
            if (!browserPromptUrl(
                browserResolved,
                sizeof(browserResolved)
            ))
            {
                continue;
            }

            browserPushHistory(
                browserCurrent,
                historyCount
            );

            browserLoadingScreen(
                browserResolved
            );

            if (!browserFetchPage(
                browserResolved,
                linkCount
            ))
            {
                if (historyCount)
                {
                    historyCount--;
                }

                browserMessageScreen(
                    "BROWSE",
                    "Unable to load URL."
                );

                continue;
            }

            strcpy(
                browserCurrent,
                browserResolved
            );

            selectedLink =
                browserFirstSelectableLink(
                    linkCount
                );

            topLine = 0;

            browserEnsureSelectionVisible(
                selectedLink,
                topLine
            );

            continue;
        }
    }

    _puts(
        "\x1B[0m\x1B[2J\x1B[H"
        "BROWSE: exit\r\n"
    );

    return 0;
}

#endif
