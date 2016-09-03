#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sched.h>

#include "channels.h"

struct channel_queue {
	size_t slots, count, next;
	void *slot[];
};

enum {
	CH_CLOSED = 1 << 0,
};

struct channel {
	struct channel_queue queue;
	unsigned flags;
	size_t send_waiting, recv_waiting;

	pthread_mutex_t mutex;
	pthread_cond_t send_wait, recv_wait;
};

/* internal queue */
static inline void channel_queue_init(struct channel_queue *q, size_t slots)
{
	q->slots = slots;
}

static inline int channel_queue_push(struct channel_queue *q, void *data)
{
	int ret = 0;
	if (q->count != q->slots) {
		size_t pos;

		/* circular */
		pos = q->next + q->count;
		if (pos >= q->slots)
			pos -= q->slots;

		q->slot[pos] = data;
		q->count++;
		ret = 1;
	}
	return ret;
}

static inline int channel_queue_next(struct channel_queue *q, void **data)
{
	int ret = 0;
	if (q->count) {
		ret = 1;

		if (data)
			*data = q->slot[q->next];
	}
	return ret;
}

static inline int channel_queue_pop(struct channel_queue *q, void **data)
{
	int ret = channel_queue_next(q, data);
	if (ret) {
		q->count--;
		q->next++;

		/* circular */
		if (q->next == q->slots)
			q->next = 0;
	}
	return ret;
}

/* thread safe interface */
int channel_send(struct channel *chan, void *data)
{
	int ret = 0;
	pthread_mutex_lock(&chan->mutex);
	while (1) {
		if (chan->flags & CH_CLOSED) {
			/* sorry, we are closed */
			errno = EPIPE;
			ret = -1;
			break;
		} else if (channel_queue_push(&chan->queue, data)) {
			/* done. wake up receivers */
			if (chan->recv_waiting)
				pthread_cond_signal(&chan->recv_wait);
			break;
		} else {
			/* wait */
			chan->send_waiting++;
			pthread_cond_wait(&chan->send_wait, &chan->mutex);
			chan->send_waiting--;
		}
	}
	pthread_mutex_unlock(&chan->mutex);
	return ret;
}

int channel_recv(struct channel *chan, void **data)
{
	int ret = 0;
	pthread_mutex_lock(&chan->mutex);
	while (1) {
		if (channel_queue_pop(&chan->queue, data)) {
			/* done. wake up senders */
			if (chan->send_waiting)
				pthread_cond_signal(&chan->send_wait);
			break;
		} else if (chan->flags & CH_CLOSED) {
			/* empty and closed */
			errno = EPIPE;
			ret = -1;
			break;
		} else {
			/* wait */
			chan->recv_waiting++;
			pthread_cond_wait(&chan->recv_wait, &chan->mutex);
			chan->recv_waiting--;
		}
	}
	pthread_mutex_unlock(&chan->mutex);
	return ret;
}

int channel_close(struct channel *chan)
{
	int ret = 0;
	pthread_mutex_lock(&chan->mutex);
	if (chan->flags & CH_CLOSED) {
		/* already closed */
		errno = EPIPE;
		ret = -1;
	} else {
		/* close and wake up everyone */
		chan->flags &= ~CH_CLOSED;
		pthread_cond_broadcast(&chan->send_wait);
		pthread_cond_broadcast(&chan->recv_wait);
	}
	pthread_mutex_unlock(&chan->mutex);
	return ret;
}

/* constructor/destructor */
struct channel *channel_new(size_t capacity)
{
	struct channel *chan;
	size_t slots = capacity + 1;

	chan = calloc(1, sizeof(*chan) + slots * sizeof(void*));
	if (chan) {
		channel_queue_init(&chan->queue, slots);

		if (pthread_mutex_init(&chan->mutex, NULL))
			goto fail_1;
		else if (pthread_cond_init(&chan->send_wait, NULL))
			goto fail_2;
		else if (pthread_cond_init(&chan->recv_wait, NULL))
			goto fail_3;
	}
	return chan;
fail_3:
	pthread_cond_destroy(&chan->send_wait);
fail_2:
	pthread_mutex_destroy(&chan->mutex);
fail_1:
	free(chan);
	return NULL;

}

void channel_free(struct channel *chan)
{
	if (chan) {
		channel_close(chan);

		while (pthread_cond_destroy(&chan->recv_wait) == -1 && errno == EBUSY)
			sched_yield();
		while (pthread_cond_destroy(&chan->send_wait) == -1 && errno == EBUSY)
			sched_yield();
		while (pthread_mutex_destroy(&chan->mutex) == -1 && errno == EBUSY)
			sched_yield();

		free(chan);
	}
}
