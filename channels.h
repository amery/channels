#ifndef CHANNELS_H
#define CHANNELS_H

/** @file */

/** Opaque struct representing a channel */
struct channel;

/** Allocates a new channel.
 *
 * @param capacity  Amount of undelivered messages to buffer
 *                  before blocking.
 */
struct channel *channel_new(size_t capacity);

/** Flags a channel as closed.
 *
 * Closed channels won't allow new messages but consumers
 * can still recv any message already queued.
 *
 * @return 0 on success and -1 \c EPIPE if it was already closed
 */
int channel_close(struct channel *);

/** Deallocates a channel.
 *
 * The channel will be closed before freeing the structures
 */
void channel_free(struct channel *);

/** Sends a message through a channel.
 *
 * @param data Pointer to the message, could but \c NULL.
 *
 * It will block until the message can be enqueued.
 * A NULL pointer is treated just like any other message.
 *
 * @return 0 after the message was successfully enqueued or
 * -1 \c EPIPE if the channel has been closed.
 */
int channel_send(struct channel *, void *data);

/** Receives a message from a channel
 *
 * @param data Pointer to the message
 *
 * It will block until there is something to read, and keep in mind
 * that \c NULL is a valid message.
 *
 * @return 0 after receiving a message or -1 \c EPIPE if the channel
 * is empty and flagged as closed.
 */
int channel_recv(struct channel *, void **data);

#endif /* !CHANNELS_H */
