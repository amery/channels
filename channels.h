#ifndef CHANNELS_H
#define CHANNELS_H

struct channel;

struct channel *channel_new(size_t capacity);
int channel_close(struct channel *);
void channel_free(struct channel *);

int channel_send(struct channel *, void *data);
int channel_recv(struct channel *, void **data);

#endif /* !CHANNELS_H */
