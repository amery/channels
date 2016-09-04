#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#include <unistd.h>
#ifdef _POSIX_PRIORITY_SCHEDULING
#include <sched.h>
#else
#define sched_yield() sleep(0)
#endif

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
static inline int channel_queue_is_empty(const struct channel_queue *q)
{
	return q->count == 0;
}

static inline int channel_queue_is_full(const struct channel_queue *q)
{
	return q->count == q->slots;
}

static inline void channel_queue_init(struct channel_queue *q, size_t slots)
{
	q->slots = slots;
}

static inline int channel_queue_push(struct channel_queue *q, void *data)
{
	int ret = 0;
	if (!channel_queue_is_full(q)) {
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

static inline int channel_queue_next(const struct channel_queue *q, void **data)
{
	int ret = !channel_queue_is_empty(q);
	if (ret && data)
		*data = q->slot[q->next];
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

/* channel */
#define channel_is_empty(C) channel_queue_is_empty(&(C)->queue)
#define channel_is_full(C)  channel_queue_is_full(&(C)->queue)

static inline int channel_is_closed(const struct channel *chan)
{
	return !!(chan->flags & CH_CLOSED);
}

/* thread safe interface */
int channel_send(struct channel *chan, void *data)
{
	int ret = 0;
	pthread_mutex_lock(&chan->mutex);
	while (1) {
		if (channel_is_closed(chan)) {
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
		} else if (channel_is_closed(chan)) {
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

static inline int channel_select_can_recv(const struct channel_option *p)
{
	return p->recv && !channel_is_empty(p->chan);
}

static inline int channel_select_can_send(const struct channel_option *p)
{
	return !channel_is_closed(p->chan) && !channel_is_full(p->chan);
}

static inline int channel_select_can_do(const struct channel_option *p)
{
	return p->chan && (channel_select_can_recv(p) || channel_select_can_send(p));
}

int channel_select(struct channel_option *options, size_t count)
{
	const struct channel_option *pe = options + count;
	struct channel_option *p;
	int ret = count;
	unsigned viables = 0;

	/* lock all channels */
	for (p = options; p < pe; p++) {
		if (p->chan)
			pthread_mutex_lock(&p->chan->mutex);
	}

	/* count viable options */
	for (p = options; p < pe; p++) {
		if (channel_select_can_do(p))
			viables++;
	}

	if (viables) {
		/* and attempt to do one of those randomly chosen */
		unsigned chosen = rand() % viables;
		unsigned i;

		for (i = 0, p = options; i < count; p++, i++) {
			struct channel *chan = p->chan;

			if (!channel_select_can_do(p)) {
				; /* skip unviable */
			} else if (chosen) {
				/* skip viable */
				chosen--;
			} else if (p->recv && channel_queue_pop(&chan->queue, p->recv)) {
				/* done. wake up senders */
				if (chan->send_waiting)
					pthread_cond_signal(&chan->send_wait);
				break;
			} else if (channel_queue_push(&chan->queue, p->send)) {
				/* done. wake up receivers */
				if (chan->recv_waiting)
					pthread_cond_signal(&chan->recv_wait);
				break;
			}
		}

		ret = i;
	}

	/* unlock all channels */
	for (p = options; p < pe; p++) {
		if (p->chan)
			pthread_mutex_unlock(&p->chan->mutex);
	}

	return ret;
}

int channel_close(struct channel *chan)
{
	int ret = 0;
	pthread_mutex_lock(&chan->mutex);
	if (channel_is_closed(chan)) {
		/* already closed */
		errno = EPIPE;
		ret = -1;
	} else {
		/* close and wake up everyone */
		chan->flags |= CH_CLOSED;
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
		int waiting = 1;

		pthread_mutex_lock(&chan->mutex);
		if (!channel_is_closed(chan)) {
			/* do as channel_close() if the user forgot */
			chan->flags |= CH_CLOSED;

			if (!chan->recv_waiting && !chan->send_waiting) {
				/* but no one was waiting anyway */
				waiting = 0;
			} else {
				pthread_cond_broadcast(&chan->send_wait);
				pthread_cond_broadcast(&chan->recv_wait);
			}
		} else if (!chan->recv_waiting && !chan->send_waiting) {
			waiting = 0;
		}
		pthread_mutex_unlock(&chan->mutex);

		while (waiting) {
			sched_yield();

			pthread_mutex_lock(&chan->mutex);
			if (!chan->recv_waiting && !chan->send_waiting)
				waiting = 0;
			pthread_mutex_unlock(&chan->mutex);
		}

		/* no one is waiting, proceed */
		pthread_cond_destroy(&chan->recv_wait);
		pthread_cond_destroy(&chan->send_wait);

		/* just some extra paranoia. wait until the lock is free. */
		while (pthread_mutex_destroy(&chan->mutex) == -1 && errno == EBUSY)
			sched_yield();

		free(chan);
	}
}
