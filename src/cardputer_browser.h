#ifndef CARDPUTER_BROWSER_H
#define CARDPUTER_BROWSER_H

/*
 * Small Lynx-style text browser for Cardputer-CPM.
 *
 * BROWSE.COM [http://... | https://...]
 *
 * It intentionally implements the useful text-browser subset: readable HTML
 * text, numbered links, relative-link navigation, back, reload and go-to.
 * CSS and JavaScript are not executed.
 */

#define BROWSER_URL_MAX       384
#define BROWSER_LINK_MAX      64
#define BROWSER_TAG_MAX       512
#define BROWSER_WORD_MAX      96
#define BROWSER_LINE_WIDTH    78
#define BROWSER_HISTORY_MAX   8
#define BROWSER_FETCH_TIMEOUT 15000

struct CardputerBrowserLink
{
    char url[
        BROWSER_URL_MAX
    ];
};


struct CardputerBrowserParser
{
    const char *baseUrl;

    CardputerBrowserLink *links;
    uint8_t linkCount;

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

    char word[
        BROWSER_WORD_MAX
    ];
    size_t wordLength;

    bool pendingSpace;
    uint8_t column;

    uint8_t pageLines;
    bool pagerStopped;
};


static void browserOutputRaw(
    const char *text
)
{
    if (!text)
    {
        return;
    }

    while (*text)
    {
        _putcon(
            (uint8_t)*text++
        );
    }
}


static void browserNewline(
    CardputerBrowserParser &parser
);


static void browserPagerAfterLine(
    CardputerBrowserParser &parser
)
{
    if (parser.pagerStopped)
    {
        return;
    }

    parser.pageLines++;

    if (
        parser.pageLines <
        18
    )
    {
        return;
    }

    _puts(
        "-- More --  SPACE/ENTER=next  Q=stop display"
    );

    uint8_t ch =
        _getcon();

    _puts(
        "\r                                             \r"
    );

    parser.pageLines =
        0;

    if (
        ch == 'q' ||
        ch == 'Q' ||
        ch == 0x03
    )
    {
        parser.pagerStopped =
            true;
    }
}


static void browserEmitNewline(
    CardputerBrowserParser &parser
)
{
    parser.column =
        0;

    if (parser.pagerStopped)
    {
        return;
    }

    _puts(
        "\r\n"
    );

    browserPagerAfterLine(
        parser
    );
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
            browserEmitNewline(
                parser
            );

            if (parser.pagerStopped)
            {
                parser.wordLength = 0;
                parser.pendingSpace = false;
                return;
            }
        }
        else
        {
            _putcon(' ');

            parser.column++;
        }
    }
    else if (
        parser.column &&
        parser.column +
        parser.wordLength >
            BROWSER_LINE_WIDTH
    )
    {
        browserEmitNewline(
            parser
        );
    }

    for (
        size_t index = 0;
        index < parser.wordLength;
        index++
    )
    {
        if (
            parser.column >=
                BROWSER_LINE_WIDTH
        )
        {
            browserEmitNewline(
                parser
            );

            if (parser.pagerStopped)
            {
                parser.wordLength = 0;
                parser.pendingSpace = false;
                return;
            }
        }

        _putcon(
            (uint8_t)parser.word[index]
        );

        parser.column++;
    }

    parser.wordLength =
        0;

    parser.pendingSpace =
        false;
}


static void browserSpace(
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


static void browserNewline(
    CardputerBrowserParser &parser
)
{
    browserFlushWord(
        parser
    );

    parser.pendingSpace =
        false;

    if (
        parser.column &&
        !parser.pagerStopped
    )
    {
        _puts(
            "\r\n"
        );

        parser.column =
            0;
    }
}


static void browserBlankLine(
    CardputerBrowserParser &parser
)
{
    browserNewline(
        parser
    );

    browserEmitNewline(
        parser
    );
}


static void browserFeedVisibleCharacter(
    CardputerBrowserParser &parser,
    char ch
)
{
    if (parser.pagerStopped)
    {
        return;
    }

    if (
        ch == '\r' ||
        ch == '\n' ||
        ch == '\t' ||
        ch == ' '
    )
    {
        browserSpace(
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

    parser.word[
        parser.wordLength++
    ] = ch;
}


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

    *write =
        0;
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

    if (
        strchr(
            reference,
            ':'
        )
    )
    {
        /*
         * mailto:, javascript:, data:, tel:, ftp:, etc.
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
        const char *path =
            basePath;

        if (!path)
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
                path +
                strcspn(
                    path,
                    "?#"
                );

            const char *lastSlash =
                path;

            for (
                const char *scan = path;
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


static void browserRenderMarker(
    CardputerBrowserParser &parser,
    uint8_t number
)
{
    char marker[12];

    snprintf(
        marker,
        sizeof(marker),
        "[%u]",
        number
    );

    browserSpace(
        parser
    );

    for (
        const char *cursor = marker;
        *cursor;
        cursor++
    )
    {
        browserFeedVisibleCharacter(
            parser,
            *cursor
        );
    }

    browserSpace(
        parser
    );
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
        tag[0] ==
            '!'
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
            "br"
        ) == 0 ||
        strcmp(
            tagName,
            "hr"
        ) == 0
    )
    {
        browserNewline(
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
        browserNewline(
            parser
        );
    }

    if (
        !closing &&
        strcmp(
            tagName,
            "li"
        ) == 0
    )
    {
        browserNewline(
            parser
        );

        browserFeedVisibleCharacter(
            parser,
            '*'
        );

        browserSpace(
            parser
        );

        return;
    }

    if (
        closing ||
        strcmp(
            tagName,
            "a"
        ) != 0
    )
    {
        return;
    }

    char href[
        BROWSER_URL_MAX
    ];

    if (!browserExtractAttribute(
        parser.tag,
        "href",
        href,
        sizeof(href)
    ))
    {
        return;
    }

    char resolved[
        BROWSER_URL_MAX
    ];

    if (!browserResolveUrl(
        parser.baseUrl,
        href,
        resolved,
        sizeof(resolved)
    ))
    {
        return;
    }

    if (
        parser.linkCount >=
            BROWSER_LINK_MAX
    )
    {
        return;
    }

    strncpy(
        parser.links[
            parser.linkCount
        ].url,
        resolved,
        BROWSER_URL_MAX - 1
    );

    parser.links[
        parser.linkCount
    ].url[
        BROWSER_URL_MAX - 1
    ] = 0;

    parser.linkCount++;

    browserRenderMarker(
        parser,
        parser.linkCount
    );
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

            parser.tagLength =
                0;

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
        if (parser.inEntity)
        {
            for (
                size_t index = 0;
                index < parser.entityLength;
                index++
            )
            {
                browserFeedVisibleCharacter(
                    parser,
                    parser.entity[index]
                );
            }

            parser.inEntity =
                false;

            parser.entityLength =
                0;
        }

        parser.inTag =
            true;

        parser.tagLength =
            0;

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

            parser.inEntity =
                false;

            parser.entityLength =
                0;

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
            index < parser.entityLength;
            index++
        )
        {
            browserFeedVisibleCharacter(
                parser,
                parser.entity[index]
            );
        }

        parser.inEntity =
            false;

        parser.entityLength =
            0;
    }

    if (ch == '&')
    {
        parser.inEntity =
            true;

        parser.entityLength =
            0;

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
            index < parser.entityLength;
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

    if (parser.column)
    {
        _puts(
            "\r\n"
        );
    }
}


static bool browserFetchPage(
    const char *url,
    CardputerBrowserLink *links,
    uint8_t &linkCount
)
{
    linkCount =
        0;

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
        _puts(
            "BROWSE: URL must begin with http:// or https://\r\n"
        );

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
        /*
         * Same TLS policy as WGET: encrypted transport, no CA bundle yet.
         */
        secureClient.setInsecure();

        if (!http.begin(
            secureClient,
            url
        ))
        {
            _puts(
                "BROWSE: unable to initialise HTTPS\r\n"
            );

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
            _puts(
                "BROWSE: unable to initialise HTTP\r\n"
            );

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

    _puts(
        "\r\nBROWSE: "
    );

    _puts(
        url
    );

    _puts(
        "\r\n"
        "----------------------------------------------------------------------------\r\n"
    );

    int response =
        http.GET();

    if (
        response < 200 ||
        response >= 300
    )
    {
        char message[64];

        snprintf(
            message,
            sizeof(message),
            "BROWSE: HTTP error %d\r\n",
            response
        );

        _puts(
            message
        );

        http.end();

        return false;
    }

    WiFiClient *stream =
        http.getStreamPtr();

    int remaining =
        http.getSize();

    CardputerBrowserParser parser = {};

    parser.baseUrl =
        url;

    parser.links =
        links;

    parser.linkCount =
        0;

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
                okay =
                    false;

                break;
            }

            for (
                int index = 0;
                index < got;
                index++
            )
            {
                browserFeedHtmlByte(
                    parser,
                    (char)buffer[index]
                );
            }

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

            okay =
                false;

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
            _puts(
                "\r\nBROWSE: receive timeout\r\n"
            );

            okay =
                false;

            break;
        }

        delay(1);
    }

    browserFinishParser(
        parser
    );

    linkCount =
        parser.linkCount;

    http.end();

    if (!okay)
    {
        _puts(
            "BROWSE: page transfer incomplete\r\n"
        );

        return false;
    }

    char summary[80];

    snprintf(
        summary,
        sizeof(summary),
        "----------------------------------------------------------------------------\r\n"
        "%u link%s.  number=follow  B=back  G=go  R=reload  L=links  Q=quit\r\n",
        linkCount,
        linkCount == 1
            ? ""
            : "s"
    );

    _puts(
        summary
    );

    return true;
}


static void browserListLinks(
    CardputerBrowserLink *links,
    uint8_t linkCount
)
{
    if (!linkCount)
    {
        _puts(
            "No links on this page.\r\n"
        );

        return;
    }

    for (
        uint8_t index = 0;
        index < linkCount;
        index++
    )
    {
        char number[12];

        snprintf(
            number,
            sizeof(number),
            "%u: ",
            index + 1
        );

        _puts(
            number
        );

        _puts(
            links[index].url
        );

        _puts(
            "\r\n"
        );
    }
}


static bool browserParseLinkNumber(
    const char *text,
    uint8_t &number
)
{
    if (
        !text ||
        !text[0]
    )
    {
        return false;
    }

    unsigned long value =
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

        value =
            value * 10 +
            (
                *text - '0'
            );

        if (
            value >
            BROWSER_LINK_MAX
        )
        {
            return false;
        }

        text++;
    }

    if (
        value < 1 ||
        value >
            BROWSER_LINK_MAX
    )
    {
        return false;
    }

    number =
        (uint8_t)value;

    return true;
}


static void browserHelp()
{
    _puts(
        "\r\n"
        "BROWSE commands\r\n"
        "---------------\r\n"
        "number       follow numbered link\r\n"
        "B            back\r\n"
        "G url        go to URL\r\n"
        "R            reload\r\n"
        "L            list links and URLs\r\n"
        "H or ?       help\r\n"
        "Q            quit\r\n"
    );
}


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

    char current[
        BROWSER_URL_MAX
    ];

    char *initial =
        wifiTrim(
            tail
        );

    if (initial[0])
    {
        if (
            strlen(initial) >=
                sizeof(current)
        )
        {
            _puts(
                "\r\nBROWSE: URL is too long\r\n"
            );

            return 0x00FF;
        }

        strcpy(
            current,
            initial
        );
    }
    else
    {
        if (!cardputerNetworkReadLine(
            "URL: ",
            current,
            sizeof(current)
        ))
        {
            return 0;
        }
    }

    CardputerBrowserLink links[
        BROWSER_LINK_MAX
    ];

    uint8_t linkCount =
        0;

    char history[
        BROWSER_HISTORY_MAX
    ][
        BROWSER_URL_MAX
    ];

    uint8_t historyCount =
        0;

    bool needLoad =
        true;

    char command[
        BROWSER_URL_MAX + 16
    ];

    while (true)
    {
        if (needLoad)
        {
            if (!browserFetchPage(
                current,
                links,
                linkCount
            ))
            {
                _puts(
                    "BROWSE: load failed. B=back G=go Q=quit\r\n"
                );
            }

            needLoad =
                false;
        }

        if (!cardputerNetworkReadLine(
            "browse> ",
            command,
            sizeof(command)
        ))
        {
            break;
        }

        char *text =
            wifiTrim(
                command
            );

        if (!text[0])
        {
            continue;
        }

        uint8_t linkNumber;

        if (browserParseLinkNumber(
            text,
            linkNumber
        ))
        {
            if (
                linkNumber < 1 ||
                linkNumber >
                    linkCount
            )
            {
                _puts(
                    "BROWSE: no such link\r\n"
                );

                continue;
            }

            if (
                historyCount <
                    BROWSER_HISTORY_MAX
            )
            {
                strncpy(
                    history[
                        historyCount++
                    ],
                    current,
                    BROWSER_URL_MAX - 1
                );

                history[
                    historyCount - 1
                ][
                    BROWSER_URL_MAX - 1
                ] = 0;
            }
            else
            {
                for (
                    uint8_t index = 1;
                    index <
                        BROWSER_HISTORY_MAX;
                    index++
                )
                {
                    strcpy(
                        history[
                            index - 1
                        ],
                        history[
                            index
                        ]
                    );
                }

                strncpy(
                    history[
                        BROWSER_HISTORY_MAX - 1
                    ],
                    current,
                    BROWSER_URL_MAX - 1
                );

                history[
                    BROWSER_HISTORY_MAX - 1
                ][
                    BROWSER_URL_MAX - 1
                ] = 0;
            }

            strncpy(
                current,
                links[
                    linkNumber - 1
                ].url,
                sizeof(current) - 1
            );

            current[
                sizeof(current) - 1
            ] = 0;

            needLoad =
                true;

            continue;
        }

        if (
            (
                text[0] == 'Q' ||
                text[0] == 'q'
            ) &&
            text[1] == 0
        )
        {
            break;
        }

        if (
            (
                text[0] == 'B' ||
                text[0] == 'b'
            ) &&
            text[1] == 0
        )
        {
            if (!historyCount)
            {
                _puts(
                    "BROWSE: history is empty\r\n"
                );

                continue;
            }

            strcpy(
                current,
                history[
                    --historyCount
                ]
            );

            needLoad =
                true;

            continue;
        }

        if (
            (
                text[0] == 'R' ||
                text[0] == 'r'
            ) &&
            text[1] == 0
        )
        {
            needLoad =
                true;

            continue;
        }

        if (
            (
                text[0] == 'L' ||
                text[0] == 'l'
            ) &&
            text[1] == 0
        )
        {
            browserListLinks(
                links,
                linkCount
            );

            continue;
        }

        if (
            (
                text[0] == 'H' ||
                text[0] == 'h' ||
                text[0] == '?'
            ) &&
            text[1] == 0
        )
        {
            browserHelp();
            continue;
        }

        if (
            (
                text[0] == 'G' ||
                text[0] == 'g'
            ) &&
            isspace(
                (unsigned char)text[1]
            )
        )
        {
            char *url =
                wifiTrim(
                    text + 2
                );

            if (!url[0])
            {
                _puts(
                    "Usage: G http://...\r\n"
                );

                continue;
            }

            char resolved[
                BROWSER_URL_MAX
            ];

            bool okay =
                browserResolveUrl(
                    current,
                    url,
                    resolved,
                    sizeof(resolved)
                );

            if (!okay)
            {
                if (
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
                )
                {
                    if (
                        strlen(url) <
                            sizeof(resolved)
                    )
                    {
                        strcpy(
                            resolved,
                            url
                        );

                        okay =
                            true;
                    }
                }
            }

            if (!okay)
            {
                _puts(
                    "BROWSE: invalid URL\r\n"
                );

                continue;
            }

            if (
                historyCount <
                    BROWSER_HISTORY_MAX
            )
            {
                strncpy(
                    history[
                        historyCount++
                    ],
                    current,
                    BROWSER_URL_MAX - 1
                );

                history[
                    historyCount - 1
                ][
                    BROWSER_URL_MAX - 1
                ] = 0;
            }

            strcpy(
                current,
                resolved
            );

            needLoad =
                true;

            continue;
        }

        _puts(
            "BROWSE: number, B, G url, R, L, H, Q\r\n"
        );
    }

    _puts(
        "\r\nBROWSE: exit\r\n"
    );

    return 0;
}

#endif
