/**
 * @file    discord.c
 * @brief   See discord.h.
 */

#include <stdio.h>
#include <string.h>

#include "config.h"      /* SOCK_HTTPS - the socket this client borrows */
#include "discord.h"
#include "wiz_claw_http.h"

#define DISCORD_BODY_MAX    2600    /* 2000 chars of content, escaped, + wrapper */

/*
 * The HTTP layer needs to be told which socket to use, and it will not pick one
 * for itself - passing NULL here returns INVALID_ARG before any network work
 * happens, which looks like an instant failure with no status and no elapsed
 * time.
 *
 * Socket 0. The W6300 has eight and they are a fixed resource: socket 5 is the
 * DNS resolver, and the product's own web server will want several more, so the
 * assignment is written down rather than left to whichever module asks first.
 *
 * Thirty seconds of timeout is generous on purpose. A TLS handshake against a
 * cold DNS cache over a domestic connection can take several seconds, and the
 * cost of waiting too long is a late alert while the cost of giving up too
 * early is no alert at all.
 */
static wiz_claw_http_ctx_t s_http_ctx = {
    .socket_no  = SOCK_HTTPS,
    /*
     * Eight seconds, not thirty.
     *
     * A connection that works is set up in thirteen milliseconds, so this is
     * three orders of margin for a healthy network. What it bounds is the
     * unhealthy one: at thirty, a site whose uplink was down froze the web
     * server and the live view for thirty-two seconds per attempt. Failing
     * quickly and trying again later is strictly better, and now that there is
     * a "later" it costs nothing.
     */
    .timeout_ms = 8000,
};

static int s_last_status = -1;

/* Decode the transport error, because the number alone sent a previous debug
 * session looking at Discord when the fault was two frames up the stack. */
static const char *err_name(wiz_claw_err_t e)
{
    switch (e) {
    case WIZ_CLAW_OK:              return "OK";
    case WIZ_CLAW_ERR_INVALID_ARG: return "INVALID_ARG (bad url/ctx, never hit the network)";
    case WIZ_CLAW_ERR_NO_MEM:      return "NO_MEM";
    case WIZ_CLAW_ERR_NOT_FOUND:   return "NOT_FOUND (DNS could not resolve the host)";
    case WIZ_CLAW_ERR_HTTP:        return "HTTP (connect, TLS or response failed)";
    case WIZ_CLAW_ERR_JSON:        return "JSON";
    default:                       return "FAIL";
    }
}

/*
 * Escape a string into a JSON value.
 *
 * Done by hand rather than with cJSON because the whole body is one field.
 * cJSON is 3,470 lines and would be carried into the firmware to build
 * {"content":"..."} once every few months.
 *
 * Returns false if the result would not fit, rather than truncating: a message
 * cut in the middle of an escape sequence is invalid JSON, and Discord would
 * reject the whole request with a status that says nothing about the cause.
 */
static bool json_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        const char *esc = NULL;
        char ubuf[7];

        switch (*p) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (*p < 0x20) {
                snprintf(ubuf, sizeof(ubuf), "\\u%04x", *p);
                esc = ubuf;
            }
            break;
        }

        if (esc) {
            size_t n = strlen(esc);
            if (o + n >= out_size) return false;
            memcpy(out + o, esc, n);
            o += n;
        } else {
            /* UTF-8 continuation bytes pass through untouched - Discord takes
             * UTF-8 directly, so Korean text needs no further encoding. */
            if (o + 1 >= out_size) return false;
            out[o++] = (char)*p;
        }
    }

    out[o] = '\0';
    return true;
}

bool discord_post_text(const char *webhook_url, const char *content)
{
    static char escaped[DISCORD_BODY_MAX];
    static char body[DISCORD_BODY_MAX + 32];

    s_last_status = -1;

    if (!webhook_url || !content) return false;

    if (!json_escape(content, escaped, sizeof(escaped))) {
        printf("[discord] message too long after escaping\n");
        return false;
    }

    int n = snprintf(body, sizeof(body), "{\"content\":\"%s\"}", escaped);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        printf("[discord] body does not fit\n");
        return false;
    }

    /*
     * No auth header. The token is already inside the URL, which is what makes
     * a webhook a webhook - and also why the URL is a secret.
     *
     * The response is discarded. Discord answers 204 with no body on success,
     * and when it does send one (an error) the status is the part worth acting
     * on.
     */
    char *response = NULL;
    wiz_claw_err_t err = wiz_claw_http_post_cb(webhook_url, NULL, body,
                                               &response, &s_http_ctx);

    s_last_status = wiz_claw_http_last_status();

    if (response) free(response);

    if (err != WIZ_CLAW_OK) {
        printf("[discord] post failed: %s (status %d)\n",
               err_name(err), s_last_status);
        return false;
    }

    if (s_last_status < 200 || s_last_status >= 300) {
        /*
         * Worth separating, because these mean different things to whoever is
         * holding the board:
         *   401/404  the URL is wrong or the webhook was deleted
         *   429      rate limited - Discord allows roughly 5 per 2 s per hook
         *   5xx      Discord's problem, retry later
         */
        printf("[discord] rejected with status %d\n", s_last_status);
        return false;
    }

    return true;
}

int discord_last_status(void)
{
    return s_last_status;
}

/* ------------------------------------------------------------- multipart */

/*
 * Buffer for the whole request body.
 *
 * Built in one piece rather than streamed because the whole thing is small:
 * a QVGA JPEG runs four to five kilobytes, and the envelope around it is a few
 * hundred bytes. Streaming would mean handing the HTTP layer a callback and
 * teaching it to ask for more, which is real complexity bought for a saving of
 * eight kilobytes on a board with half a megabyte.
 *
 * Sized for HD as well, so raising the camera resolution later does not come
 * back as a truncated upload.
 */
#define DISCORD_MULTIPART_MAX   (64 * 1024)

static uint8_t s_mp[DISCORD_MULTIPART_MAX];

/*
 * A fixed boundary is safe here in a way it would not be in general: the only
 * part that is not ours is the JPEG, and a JPEG cannot contain this string -
 * its bytes are entropy-coded and any 0xFF is followed by a marker byte, never
 * by ASCII text that happens to spell a boundary.
 */
#define MP_BOUNDARY  "----wiznetW6300boundary"

bool discord_post_photo(const char *webhook_url, const char *content,
                        const uint8_t *jpeg, size_t jpeg_len,
                        const char *filename)
{
    static char escaped[DISCORD_BODY_MAX];

    s_last_status = -1;

    if (!webhook_url || !jpeg || jpeg_len == 0) return false;
    if (!filename) filename = "snapshot.jpg";

    if (content && !json_escape(content, escaped, sizeof(escaped))) {
        printf("[discord] caption too long after escaping\n");
        return false;
    }

    size_t off = 0;

    /*
     * payload_json rather than a plain content field. Discord accepts either,
     * but the JSON form is what takes embeds later - a coloured sidebar and
     * named fields read better on a phone than a line of text, and switching
     * to it then would otherwise mean rewriting this.
     */
    int n = snprintf((char *)s_mp, sizeof(s_mp),
        "--" MP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"payload_json\"\r\n"
        "Content-Type: application/json\r\n"
        "\r\n"
        "{\"content\":\"%s\"}\r\n"
        "--" MP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"files[0]\"; filename=\"%s\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n",
        content ? escaped : "", filename);

    if (n < 0) return false;
    off = (size_t)n;

    const size_t tail_len = sizeof("\r\n--" MP_BOUNDARY "--\r\n") - 1;

    if (off + jpeg_len + tail_len > sizeof(s_mp)) {
        printf("[discord] image too large: %u bytes, %u of buffer left\n",
               (unsigned)jpeg_len, (unsigned)(sizeof(s_mp) - off - tail_len));
        return false;
    }

    memcpy(s_mp + off, jpeg, jpeg_len);
    off += jpeg_len;

    memcpy(s_mp + off, "\r\n--" MP_BOUNDARY "--\r\n", tail_len);
    off += tail_len;

    char *response = NULL;
    wiz_claw_err_t err = wiz_claw_http_post_raw(
        webhook_url,
        "multipart/form-data; boundary=" MP_BOUNDARY,
        s_mp, off,
        &response, &s_http_ctx);

    s_last_status = wiz_claw_http_last_status();

    if (response) free(response);

    if (err != WIZ_CLAW_OK) {
        printf("[discord] photo post failed: %s (status %d)\n",
               err_name(err), s_last_status);
        return false;
    }

    if (s_last_status < 200 || s_last_status >= 300) {
        printf("[discord] photo rejected with status %d\n", s_last_status);
        return false;
    }

    return true;
}
