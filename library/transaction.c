/*
 * Transaction routines for test server.
 *
 * Copyright (C) 2014-2015 SUSE
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h> /* for htons */

#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <ctype.h>

#include "protocol.h"
#include "transaction.h"


struct twopence_trans_channel {
	struct twopence_trans_channel *next;

	uint16_t		id;		/* The channel ID is a 16bit number; usually 0, 1, 2 for commands */
	const char *		name;		/* The channel'S name for debugging purposes */

	bool			sync;		/* if true, all writes are fully synchronous */

	twopence_sock_t *	socket;
	twopence_iostream_t *	stream;

	/* This is needed by the client side "inject" code:
	 * Before we start sending the actual file data, we want confirmation from
	 * the server that it was able to open the destination file.
	 * So even though we're adding the local sink channel early, we do not allow
	 * to transmit from right away. So initially, the channel is "plugged", and
	 * only when we receive a major status of 0, we will "unplug" it. */
	bool			plugged;

	struct {
	    void		(*read_eof)(twopence_transaction_t *, twopence_trans_channel_t *);
	    void		(*write_eof)(twopence_transaction_t *, twopence_trans_channel_t *);
	} callbacks;
};

static void	twopence_transaction_channel_trace_io_eof(twopence_transaction_t *trans);

/*
 * Transaction channel primitives
 */
static twopence_trans_channel_t *
twopence_transaction_channel_from_fd(int fd, int flags)
{
	twopence_trans_channel_t *sink;
	twopence_sock_t *sock;

	sock = twopence_sock_new_flags(fd, flags);

	sink = twopence_calloc(1, sizeof(*sink));
	sink->socket = sock;

	return sink;
}

static twopence_trans_channel_t *
twopence_transaction_channel_from_stream(twopence_iostream_t *stream, int flags)
{
	twopence_trans_channel_t *sink;

	sink = twopence_calloc(1, sizeof(*sink));
	sink->stream = stream;

	return sink;
}

void
twopence_transaction_channel_set_name(twopence_trans_channel_t *channel, const char *name)
{
	channel->name = name;
}

static const char *
__twopence_transaction_channel_name(uint16_t id)
{
	static char namebuf[16];

	if (id == TWOPENCE_TRANSACTION_CHANNEL_ID_ALL)
		return "all";

	snprintf(namebuf, sizeof(namebuf), "chan%u", id);
	return namebuf;
}

const char *
twopence_transaction_channel_name(const twopence_trans_channel_t *channel)
{
	if (channel->name)
		return channel->name;

	return __twopence_transaction_channel_name(channel->id);
}

static void
twopence_transaction_channel_free(twopence_trans_channel_t *sink)
{
	twopence_debug("%s(%s)", __func__, twopence_transaction_channel_name(sink));
	if (sink->socket)
		twopence_sock_free(sink->socket);
	sink->socket = NULL;

	/* Do NOT free the iostream */

	free(sink);
}

bool
twopence_transaction_channel_is_read_eof(const twopence_trans_channel_t *channel)
{
	twopence_sock_t *sock = channel->socket;

	if (sock)
		return twopence_sock_is_read_eof(sock);
	if (channel->stream)
		return twopence_iostream_eof(channel->stream);
	return false;
}

void
twopence_transaction_channel_set_plugged(twopence_trans_channel_t *channel, bool plugged)
{
	channel->plugged = plugged;
}

void
twopence_transaction_channel_set_callback_read_eof(twopence_trans_channel_t *channel, void (*fn)(twopence_transaction_t *, twopence_trans_channel_t *))
{
	channel->callbacks.read_eof = fn;
}

void
twopence_transaction_channel_set_callback_write_eof(twopence_trans_channel_t *channel, void (*fn)(twopence_transaction_t *, twopence_trans_channel_t *))
{
	channel->callbacks.write_eof = fn;
}

static void
twopence_transaction_channel_list_purge(twopence_trans_channel_t **list)
{
	twopence_trans_channel_t *channel;

	while ((channel = *list) != NULL) {
		if (channel->socket && twopence_sock_is_dead(channel->socket)) {
			*list = channel->next;
			twopence_transaction_channel_free(channel);
		} else {
			list = &channel->next;
		}
	}
}

static void
twopence_transaction_channel_list_close(twopence_trans_channel_t **list, uint16_t id)
{
	twopence_trans_channel_t *channel;

	while ((channel = *list) != NULL) {
		if (id == TWOPENCE_TRANSACTION_CHANNEL_ID_ALL || channel->id == id) {
			*list = channel->next;
			twopence_transaction_channel_free(channel);
		} else {
			list = &channel->next;
		}
	}
}

/*
 * twopence transactions as used by our own on-the-wire protocol
 */
twopence_transaction_t *
twopence_transaction_new(twopence_sock_t *transport, unsigned int type, const twopence_protocol_state_t *ps)
{
	twopence_transaction_t *trans;

	trans = twopence_calloc(1, sizeof(*trans));
	trans->ps = *ps;
	trans->id = ps->xid;
	trans->type = type;
	trans->socket = transport;

	twopence_debug("%s: created new transaction", twopence_transaction_describe(trans));
	return trans;
}

void
twopence_transaction_free(twopence_transaction_t *trans)
{
	assert(trans->prev == NULL);

	twopence_transaction_channel_trace_io_eof(trans);

	/* Do not free trans->socket, we don't own it */

	twopence_transaction_channel_list_close(&trans->local_sink, TWOPENCE_TRANSACTION_CHANNEL_ID_ALL);
	twopence_transaction_channel_list_close(&trans->local_source, TWOPENCE_TRANSACTION_CHANNEL_ID_ALL);

	memset(trans, 0, sizeof(*trans));
	free(trans);
}

const char *
twopence_transaction_describe(const twopence_transaction_t *trans)
{
	static char descbuf[64];

	snprintf(descbuf, sizeof(descbuf), "%s/%u",
			twopence_protocol_packet_type_to_string(trans->type), trans->ps.xid);
	return descbuf;
}

void
twopence_transaction_set_timeout(twopence_transaction_t *trans, long timeout)
{
	if (timeout > 0) {
		gettimeofday(&trans->client.deadline, NULL);
		trans->client.deadline.tv_sec += timeout;
	}
}

bool
twopence_transaction_update_timeout(const twopence_transaction_t *trans, twopence_timeout_t *tmo)
{
	if (trans->client.chat_deadline
	 && !twopence_timeout_update(tmo, trans->client.chat_deadline))
		return false;

	return twopence_timeout_update(tmo, &trans->client.deadline);
}

static inline void
twopence_transaction_channel_trace_io_data(twopence_transaction_t *trans)
{
	if (trans->client.print_dots) {
		write(1, ".", 1);
		trans->client.dots_printed++;
	}
}

static void
twopence_transaction_channel_trace_io_eof(twopence_transaction_t *trans)
{
	if (trans->client.print_dots) {
		trans->client.dots_printed = 0;
		write(1, "\n", 1);
	}
}

void
twopence_transaction_set_error(twopence_transaction_t *trans, int rc)
{
	twopence_debug("%s: set client side error to %d", twopence_transaction_describe(trans), rc);
	trans->client.exception = rc;
	trans->done = true;
}

unsigned int
twopence_transaction_num_channels(const twopence_transaction_t *trans)
{
	twopence_trans_channel_t *channel;
	unsigned int count = 0;

	for (channel = trans->local_sink; channel; channel = channel->next)
		count++;
	for (channel = trans->local_source; channel; channel = channel->next)
		count++;
	return count;
}

int
twopence_transaction_send_extract(twopence_transaction_t *trans, const twopence_file_xfer_t *xfer)
{
	twopence_buf_t *bp;

	bp = twopence_protocol_build_extract_packet(&trans->ps, xfer);
	if (twopence_sock_xmit(trans->socket, bp) < 0)
		return TWOPENCE_SEND_COMMAND_ERROR;
	return 0;
}

int
twopence_transaction_send_inject(twopence_transaction_t *trans, const twopence_file_xfer_t *xfer)
{
	twopence_buf_t *bp;

	bp = twopence_protocol_build_inject_packet(&trans->ps, xfer);
	if (twopence_sock_xmit(trans->socket, bp) < 0)
		return TWOPENCE_SEND_COMMAND_ERROR;
	return 0;
}

int
twopence_transaction_send_command(twopence_transaction_t *trans, const twopence_command_t *cmd)
{
	twopence_buf_t *bp;

	bp = twopence_protocol_build_command_packet(&trans->ps, cmd);
	if (twopence_sock_xmit(trans->socket, bp) < 0)
		return TWOPENCE_SEND_COMMAND_ERROR;
	return 0;
}

int
twopence_transaction_send_interrupt(twopence_transaction_t *trans)
{
	twopence_buf_t *bp;

	bp = twopence_protocol_build_simple_packet_ps(&trans->ps, TWOPENCE_PROTO_TYPE_INTR);
	if (twopence_sock_xmit(trans->socket, bp) < 0)
		return TWOPENCE_SEND_COMMAND_ERROR;
	return 0;
}

twopence_trans_channel_t *
twopence_transaction_attach_local_sink(twopence_transaction_t *trans, uint16_t id, int fd)
{
	twopence_trans_channel_t *sink;

	/* Make I/O to this file descriptor non-blocking */
	fcntl(fd, F_SETFL, O_NONBLOCK);

	sink = twopence_transaction_channel_from_fd(fd, O_WRONLY);
	sink->id = id;

	sink->next = trans->local_sink;
	trans->local_sink = sink;
	return sink;
}

twopence_trans_channel_t *
twopence_transaction_attach_local_sink_stream(twopence_transaction_t *trans, uint16_t id, twopence_iostream_t *stream)
{
	twopence_trans_channel_t *sink;
	int fd;

	fd = twopence_iostream_getfd(stream);
	if (fd >= 0) {
		sink = twopence_transaction_attach_local_sink(trans, id, fd);
		twopence_sock_set_noclose(sink->socket);
		return sink;
	}

	sink = twopence_transaction_channel_from_stream(stream, O_WRONLY);
	sink->id = id;

	sink->next = trans->local_sink;
	trans->local_sink = sink;
	return sink;
}

void
twopence_transaction_close_sink(twopence_transaction_t *trans, uint16_t id)
{
	twopence_debug("%s: close sink %s\n", twopence_transaction_describe(trans), __twopence_transaction_channel_name(id));
	twopence_transaction_channel_list_close(&trans->local_sink, id);
}

twopence_trans_channel_t *
twopence_transaction_attach_local_source(twopence_transaction_t *trans, uint16_t channel_id, int fd)
{
	twopence_trans_channel_t *source;

	/* Make I/O to this file descriptor non-blocking */
	fcntl(fd, F_SETFL, O_NONBLOCK);

	source = twopence_transaction_channel_from_fd(fd, O_RDONLY);
	source->id = channel_id;

	source->next = trans->local_source;
	trans->local_source = source;
	return source;
}

twopence_trans_channel_t *
twopence_transaction_attach_local_source_stream(twopence_transaction_t *trans, uint16_t id, twopence_iostream_t *stream)
{
	twopence_trans_channel_t *source;
	int fd;

	fd = twopence_iostream_getfd(stream);
	if (fd >= 0) {
		source = twopence_transaction_attach_local_source(trans, id, fd);
		twopence_sock_set_noclose(source->socket);
		return source;
	}

	source = twopence_transaction_channel_from_stream(stream, O_RDONLY);
	source->id = id;

	source->next = trans->local_source;
	trans->local_source = source;
	return source;
}

void
twopence_transaction_close_source(twopence_transaction_t *trans, uint16_t id)
{
	twopence_debug("%s: close source %s\n", twopence_transaction_describe(trans), __twopence_transaction_channel_name(id));
	twopence_transaction_channel_list_close(&trans->local_source, id);
}

/*
 * Write data to the sink.
 * Note that the buffer is a temporary one on the stack, so if we
 * want to enqueue it to the socket, it has to be cloned first.
 * This is taken care of by twopence_sock_xmit_shared()
 */
static bool
twopence_transaction_channel_write_data(twopence_transaction_t *trans, twopence_trans_channel_t *sink, twopence_buf_t *payload)
{
	unsigned int count = twopence_buf_count(payload);
	twopence_iostream_t *stream;
	twopence_sock_t *sock;

	twopence_debug("About to write %u bytes of data to local sink\n", count);
	if ((sock = sink->socket) != NULL) {
		if (twopence_sock_xmit_shared(sock, payload) < 0)
			return false;
	} else
	if ((stream = sink->stream) != NULL) {
		twopence_iostream_write(stream, twopence_buf_head(payload), count);
		twopence_buf_advance_head(payload, count);
	}

	twopence_transaction_channel_trace_io_data(trans);
	return true;
}

int
twopence_transaction_channel_flush(twopence_trans_channel_t *sink)
{
	twopence_sock_t *sock;

	if ((sock = sink->socket) == NULL)
		return 0;

	if (twopence_sock_xmit_queue_bytes(sock) == 0)
		return 0;

	twopence_debug("Flushing %u bytes queued to channel %s\n",
			twopence_sock_xmit_queue_bytes(sock), twopence_transaction_channel_name(sink));
	return twopence_sock_xmit_queue_flush(sock);
}

uint16_t
twopence_transaction_channel_id(const twopence_trans_channel_t *channel)
{
	return channel->id;
}

static void
twopence_transaction_channel_write_eof(twopence_trans_channel_t *sink)
{
	twopence_sock_t *sock = sink->socket;

	if (sock)
		twopence_sock_shutdown_write(sock);
}

int
twopence_transaction_channel_poll(twopence_trans_channel_t *channel, twopence_pollinfo_t *pinfo)
{
	twopence_sock_t *sock = channel->socket;

	if (sock && !twopence_sock_is_dead(sock)) {
		twopence_buf_t *bp;

		twopence_sock_prepare_poll(sock);

		/* If needed, post a new receive buffer to the socket.
		 * Note: this is a NOP for sink channels, as their socket
		 * already has read_eof set, so that a recvbuf is never
		 * posted to it.
		 */
		if (!channel->plugged
		 && !twopence_sock_is_read_eof(sock)
		 && (bp = twopence_sock_get_recvbuf(sock)) == NULL) {
			/* When we receive data from a command's output stream, or from
			 * a file that is being extracted, we do not want to copy
			 * the entire packet - instead, we reserve some room for the
			 * protocol header, which we just tack on once we have the data.
			 */
			bp = twopence_buf_new(TWOPENCE_PROTO_MAX_PACKET);
			twopence_buf_reserve_head(bp, TWOPENCE_PROTO_HEADER_SIZE + 2);

			twopence_sock_post_recvbuf(sock, bp);
		}

		if (twopence_sock_fill_poll(sock, pinfo))
			return 1;
	}

	return 0;
}

/* This should be executed for source channels only! */
static void
twopence_transaction_channel_forward(twopence_transaction_t *trans, twopence_trans_channel_t *channel)
{
	twopence_iostream_t *stream = channel->stream;

	if (!channel->plugged && stream != NULL) {
		while (twopence_sock_xmit_queue_allowed(trans->socket) && !twopence_iostream_eof(stream)) {
			twopence_buf_t *bp;
			int count;

			bp = twopence_protocol_command_buffer_new();
			twopence_buf_reserve_head(bp, TWOPENCE_PROTO_HEADER_SIZE + 2); /* ugly */
			do {
				count = twopence_iostream_read(stream,
						twopence_buf_tail(bp),
						twopence_buf_tailroom(bp));
			} while (count < 0 && errno == EINTR);

			if (count > 0) {
				twopence_buf_advance_tail(bp, count);
				twopence_protocol_build_data_header(bp, &trans->ps, channel->id);
				twopence_transaction_send_client(trans, bp);

				twopence_transaction_channel_trace_io_data(trans);
				continue;
			}

			twopence_buf_free(bp);
			if (count == 0)
				break;
			if (count < 0) {
				if (errno != EAGAIN) {
					twopence_log_error("%s: error on channel %s",
							twopence_transaction_describe(trans),
							twopence_transaction_channel_name(channel));
					twopence_transaction_set_error(trans, count);
				}
				return;
			}
		}

		twopence_transaction_channel_trace_io_eof(trans);
		if (twopence_iostream_eof(stream) && channel->callbacks.read_eof) {
			twopence_debug("%s: EOF on channel %s", twopence_transaction_describe(trans),
					twopence_transaction_channel_name(channel));
			channel->callbacks.read_eof(trans, channel);
			channel->callbacks.read_eof = NULL;
			channel->stream = NULL;
		}
	}
}

static void
twopence_transaction_channel_doio(twopence_transaction_t *trans, twopence_trans_channel_t *channel)
{
	twopence_sock_t *sock = channel->socket;

	if (sock) {
		twopence_buf_t *bp;

		if (twopence_sock_doio(sock) < 0) {
			twopence_transaction_fail(trans, errno);
			twopence_sock_mark_dead(sock);
			return;
		}

		/* Only source channels will even have a recv buffer posted
		 * to them. If that is non-empty, queue it to the transport
		 * socket. */
		if ((bp = twopence_sock_take_recvbuf(sock)) != NULL) {
			twopence_debug2("%s: %u bytes from local source %s", twopence_transaction_describe(trans),
					twopence_buf_count(bp), twopence_transaction_channel_name(channel));
			twopence_protocol_build_data_header(bp, &trans->ps, channel->id);
			twopence_sock_queue_xmit(trans->socket, bp);

			twopence_transaction_channel_trace_io_data(trans);
		}

		/* For file extractions, we want to send an EOF packet
		 * when the file has been transmitted in its entirety.
		 */
		if (twopence_sock_is_read_eof(sock) && channel->callbacks.read_eof) {
			twopence_debug("%s: EOF on channel %s", twopence_transaction_describe(trans),
					twopence_transaction_channel_name(channel));
			channel->callbacks.read_eof(trans, channel);
			channel->callbacks.read_eof = NULL;
		}
	}
}

int
twopence_transaction_fill_poll(twopence_transaction_t *trans, twopence_pollinfo_t *pinfo)
{
	if (!twopence_timeout_update(&pinfo->timeout, &trans->client.deadline))
		return TWOPENCE_COMMAND_TIMEOUT_ERROR;

	if (trans->local_sink != NULL) {
		twopence_trans_channel_t *sink;

		for (sink = trans->local_sink; sink; sink = sink->next)
			twopence_transaction_channel_poll(sink, pinfo);
	}

	/* If the client socket's write queue is already bursting with data,
	 * refrain from queuing more until some of it has been drained */
	if (twopence_sock_xmit_queue_allowed(trans->socket)) {
		twopence_trans_channel_t *source;

		for (source = trans->local_source; source; source = source->next) {
			if (!twopence_transaction_channel_poll(source, pinfo)) {
				/* This is a source not backed by a file descriptor but
				 * something else (such as a buffer).
				 * This means we cannot poll, so we just forward all data
				 * we have. */
				twopence_transaction_channel_forward(trans, source);
			}
		}
	}

	return 0;
}

void
twopence_transaction_doio(twopence_transaction_t *trans)
{
	twopence_trans_channel_t *channel;

	twopence_debug2("%s: twopence_transaction_doio()\n", twopence_transaction_describe(trans));
	for (channel = trans->local_sink; channel; channel = channel->next)
		twopence_transaction_channel_doio(trans, channel);
	twopence_transaction_channel_list_purge(&trans->local_sink);

	for (channel = trans->local_source; channel; channel = channel->next)
		twopence_transaction_channel_doio(trans, channel);

	twopence_debug2("twopence_transaction_doio(): calling trans->send()\n");
	if (trans->send)
		trans->send(trans);

	/* Purge the source list *after* calling trans->send().
	 * This is because server_extract_file_send needs to detect
	 * the EOF condition on the source file and send an EOF packet.
	 * Once we wrap this inside the twopence_trans_channel handling,
	 * then this requirement goes away. */
	twopence_transaction_channel_list_purge(&trans->local_source);
}

/*
 * This function is called from connection_doio when we have an incoming packet
 * for this transaction
 */
void
twopence_transaction_recv_packet(twopence_transaction_t *trans, const twopence_hdr_t *hdr, twopence_buf_t *payload)
{
	twopence_trans_channel_t *sink;

	if (trans->done) {
		/* Coming late to the party, huh? */
		return;
	}

	if (hdr->type == TWOPENCE_PROTO_TYPE_CHAN_DATA) {
		uint16_t channel_id;

		/* This should go to protocol.c */
		if (!twopence_buf_get(payload, &channel_id, sizeof(channel_id)))
			return;

		channel_id = ntohs(channel_id);
		sink = twopence_transaction_find_sink(trans, channel_id);
		if (sink != NULL) {
			twopence_debug("%s: received %u bytes of data on channel %s\n",
					twopence_transaction_describe(trans), twopence_buf_count(payload),
					twopence_transaction_channel_name(sink));

			trans->stats.nbytes_received += twopence_buf_count(payload);
			if (!twopence_transaction_channel_write_data(trans, sink, payload))
				twopence_transaction_fail(trans, errno);
			return;
		}

		twopence_debug("%s: received %u bytes of data on unknown channel %u\n",
				twopence_transaction_describe(trans), twopence_buf_count(payload),
				channel_id);
		return;
	}

	if (hdr->type == TWOPENCE_PROTO_TYPE_CHAN_EOF) {
		uint16_t channel_id;

		/* This should go to protocol.c */
		if (!twopence_buf_get(payload, &channel_id, sizeof(channel_id)))
			return;

		channel_id = ntohs(channel_id);
		sink = twopence_transaction_find_sink(trans, channel_id);
		if (sink == NULL) {
			twopence_debug("%s: received %u bytes of data on unknown channel %u\n",
					twopence_transaction_describe(trans), twopence_buf_count(payload),
					channel_id);
			return;
		}

		twopence_debug("%s: received EOF on channel %s\n",
				twopence_transaction_describe(trans),
				twopence_transaction_channel_name(sink));

		twopence_transaction_channel_trace_io_eof(trans);
		twopence_transaction_channel_write_eof(sink);
		if (sink->callbacks.write_eof) {
			sink->callbacks.write_eof(trans, sink);
			sink->callbacks.write_eof = NULL;
		}

		/* Do NOT close the sink yet; it may have data queued to it.
		 * Strictly speaking, we also should not send a success notification
		 * to the client yet. */
		return;
	}

	if (trans->recv == NULL) {
		twopence_log_error("%s: unexpected %s packet\n", twopence_transaction_describe(trans),
				twopence_protocol_packet_type_to_string(hdr->type));
		twopence_transaction_fail(trans, EPROTO);
		return;
	}

	trans->recv(trans, hdr, payload);
}


void
twopence_transaction_send_client(twopence_transaction_t *trans, twopence_buf_t *bp)
{
	const twopence_hdr_t *h = (const twopence_hdr_t *) twopence_buf_head(bp);

	if (h == NULL)
		return;

	twopence_debug("%s: sending packet type=%s, payload=%u\n", twopence_transaction_describe(trans),
			twopence_protocol_packet_type_to_string(h->type),
			ntohs(h->len) - TWOPENCE_PROTO_HEADER_SIZE);
	twopence_sock_queue_xmit(trans->socket, bp);
}

void
twopence_transaction_send_major(twopence_transaction_t *trans, unsigned int code)
{
	twopence_debug("%s: send status.major=%u", twopence_transaction_describe(trans), code);
	assert(!trans->major_sent);
	twopence_transaction_send_client(trans, twopence_protocol_build_major_packet(&trans->ps, code));
	trans->major_sent = true;
}

void
twopence_transaction_send_minor(twopence_transaction_t *trans, unsigned int code)
{
	twopence_debug("%s: send status.minor=%u", twopence_transaction_describe(trans), code);
	assert(!trans->minor_sent);
	twopence_transaction_send_client(trans, twopence_protocol_build_minor_packet(&trans->ps, code));
	trans->minor_sent = true;
}

/* XXX obsolete */
void
twopence_transaction_send_status(twopence_transaction_t *trans, twopence_status_t *st)
{
	if (trans->done) {
		twopence_log_error("%s called twice\n", __func__);
		return;
	}
	twopence_transaction_send_client(trans, twopence_protocol_build_major_packet(&trans->ps, st->major));
	twopence_transaction_send_client(trans, twopence_protocol_build_minor_packet(&trans->ps, st->minor));
	trans->done = true;
}

void
twopence_transaction_fail(twopence_transaction_t *trans, int code)
{
	trans->done = true;
	if (!trans->major_sent) {
		twopence_transaction_send_major(trans, code);
		return;
	}
	if (!trans->minor_sent) {
		twopence_transaction_send_minor(trans, code);
		return;
	}
	twopence_debug("%s: already sent major and minor status\n", __func__);
	abort();
}

void
twopence_transaction_fail2(twopence_transaction_t *trans, int major, int minor)
{
	twopence_transaction_send_major(trans, major);
	twopence_transaction_send_minor(trans, minor);
	trans->done = 1;
}

/*
 * Command timed out.
 * We used to send an ETIME error, but it's been changed to
 * its own packet type.
 */
void
twopence_transaction_send_timeout(twopence_transaction_t *trans)
{
	twopence_buf_t *bp;

	bp = twopence_protocol_command_buffer_new();
	twopence_protocol_push_header_ps(bp, &trans->ps, TWOPENCE_PROTO_TYPE_TIMEOUT);
	twopence_transaction_send_client(trans, bp);
	trans->done = 1;
}

/*
 * Find the local sink corresponding to the given id.
 * For now, the "id" is a packet type, such as '0' or 'd'
 */
twopence_trans_channel_t *
twopence_transaction_find_sink(twopence_transaction_t *trans, uint16_t id)
{
	twopence_trans_channel_t *sink;

	for (sink = trans->local_sink; sink; sink = sink->next) {
		if (sink->id == id)
			return sink;
	}
	return NULL;
}

twopence_trans_channel_t *
twopence_transaction_find_source(twopence_transaction_t *trans, uint16_t id)
{
	twopence_trans_channel_t *channel;

	for (channel = trans->local_source; channel; channel = channel->next) {
		if (channel->id == id)
			return channel;
	}
	return NULL;
}

/*
 * Transaction list primitives
 */
void
twopence_transaction_list_insert(twopence_transaction_list_t *list, twopence_transaction_t *trans)
{
	twopence_transaction_t *next;

	assert(trans->prev == NULL);

	if ((next = list->head) != NULL)
		next->prev = &trans->next;
	trans->next = next;
	trans->prev = &list->head;
	list->head = trans;
}

void
twopence_transaction_unlink(twopence_transaction_t *trans)
{
	if (trans->prev)
		*(trans->prev) = trans->next;
	if (trans->next)
		trans->next->prev = trans->prev;
	trans->prev = NULL;
	trans->next = NULL;
}
