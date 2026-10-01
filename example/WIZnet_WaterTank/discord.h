/**
 * @file    discord.h
 * @brief   Posting a line of text to a Discord channel from the W6300.
 *
 * A webhook rather than a bot. The difference matters for this product: a bot
 * needs an application, an invite and a gateway connection it has to hold open,
 * while a webhook is one URL that accepts a POST. For a device whose entire job
 * is to say something a few times a year, holding a connection open is the
 * wrong shape.
 *
 * The URL is the credential. Anyone holding it can post to that channel, so it
 * belongs with the device's other secrets and not in a repository.
 *
 * Nothing here keeps a connection. Each call resolves, connects, hands over the
 * TLS handshake, posts and closes. That costs a second or two per message and
 * is the right trade when messages are rare: a socket held open for months is a
 * socket that has to survive every router reboot in between.
 */

#ifndef __DISCORD_H__
#define __DISCORD_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Post plain text to a webhook.
 *
 * @param webhook_url  https://discord.com/api/webhooks/<id>/<token>
 * @param content      UTF-8, up to 2000 characters by Discord's limit.
 *                     Quotes, backslashes and newlines are escaped here.
 * @return true when Discord answered 2xx.
 */
bool discord_post_text(const char *webhook_url, const char *content);

/**
 * Post text and a JPEG together, as one request.
 *
 * One request rather than two on purpose. A handshake costs about six seconds
 * on this board, and an alarm that sends the line and then the picture would
 * take twelve before the owner has both. Sent together they arrive at once,
 * and a QVGA frame is four or five kilobytes - small enough that the whole
 * envelope fits in one buffer with no chunking.
 *
 * @param jpeg      raw JPEG bytes, starting at the SOI marker
 * @param jpeg_len  length in bytes
 * @param filename  shown in Discord; NULL gives "snapshot.jpg"
 */
bool discord_post_photo(const char *webhook_url, const char *content,
                        const uint8_t *jpeg, size_t jpeg_len,
                        const char *filename);

/** HTTP status from the last call, or a negative value if it never got one. */
int discord_last_status(void);

#endif /* __DISCORD_H__ */
