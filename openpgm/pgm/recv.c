/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * Transport recv API.
 *
 * Copyright (c) 2006-2009 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#ifdef CONFIG_HAVE_POLL
#	include <poll.h>
#endif
#ifdef CONFIG_HAVE_EPOLL
#	include <sys/epoll.h>
#endif
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include <glib.h>
#include <glib/gi18n-lib.h>

#include "pgm/transport.h"
#include "pgm/source.h"
#include "pgm/receiver.h"
#include "pgm/recv.h"
#include "pgm/if.h"
#include "pgm/ip.h"
#include "pgm/packet.h"
#include "pgm/math.h"
#include "pgm/net.h"
#include "pgm/txwi.h"
#include "pgm/rxwi.h"
#include "pgm/rate_control.h"
#include "pgm/sn.h"
#include "pgm/time.h"
#include "pgm/timer.h"
#include "pgm/checksum.h"
#include "pgm/reed_solomon.h"
#include "pgm/err.h"

#define RECV_DEBUG

#ifndef RECV_DEBUG
#	define g_trace(...)		while (0)
#else
#	define g_trace(...)		g_debug(__VA_ARGS__)
#endif


static PGMRecvError pgm_recv_error_from_errno (gint);


/* read a packet into a PGM skbuff
 * on success returns packet length, on closed socket returns 0,
 * on error returns -1.
 */

static
ssize_t
recvskb (
	pgm_transport_t* const		transport,
	struct pgm_sk_buff_t* const	skb,
	const int			flags,
	struct sockaddr* const		src_addr,
	const gsize			src_addrlen,
	struct sockaddr* const		dst_addr,
	const gsize			dst_addrlen
	)
{
/* pre-conditions */
	g_assert (NULL != transport);
	g_assert (NULL != skb);
	g_assert (NULL != src_addr);
	g_assert (src_addrlen > 0);
	g_assert (NULL != dst_addr);
	g_assert (dst_addrlen > 0);

	g_trace ("recvskb (transport:%p skb:%p flags:%d src-addr:%p src-addrlen:%" G_GSIZE_FORMAT " dst-addr:%p dst-addrlen:%" G_GSIZE_FORMAT ")",
		(gpointer)transport, (gpointer)skb, flags, (gpointer)src_addr, src_addrlen, (gpointer)dst_addr, dst_addrlen);

	struct iovec iov = {
		.iov_base	= skb->head,
		.iov_len	= transport->max_tpdu
	};
	size_t aux[1024 / sizeof(size_t)];
	struct msghdr msg = {
		.msg_name	= src_addr,
		.msg_namelen	= src_addrlen,
		.msg_iov	= &iov,
		.msg_iovlen	= 1,
		.msg_control	= aux,
		.msg_controllen = sizeof(aux),
		.msg_flags	= 0
	};

	ssize_t len = recvmsg (transport->recv_sock, &msg, flags);
	if (len) {
		skb->transport	= transport;
		skb->tstamp	= pgm_time_update_now();
		skb->data	= skb->head;
		skb->len	= len;
		skb->tail	= (guint8*)skb->data + len;
	}
	if (transport->udp_encap_ucast_port ||
	    AF_INET6 == pgm_sockaddr_family (&src_addr))
	{
		struct cmsghdr* cmsg;
		gpointer pktinfo = NULL;
		for (cmsg = CMSG_FIRSTHDR(&msg);
		     cmsg != NULL;
		     cmsg = CMSG_NXTHDR(&msg, cmsg))
		{
			if (IPPROTO_IP == cmsg->cmsg_level && 
			    IP_PKTINFO == cmsg->cmsg_type)
			{
				pktinfo				= CMSG_DATA(cmsg);
				const struct in_pktinfo* in	= pktinfo;
				struct sockaddr_in* sin		= (struct sockaddr_in*)dst_addr;
				sin->sin_family			= AF_INET;
				sin->sin_addr.s_addr		= in->ipi_addr.s_addr;
				break;
			}

			if (IPPROTO_IPV6 == cmsg->cmsg_level && 
			    IPV6_PKTINFO == cmsg->cmsg_type)
			{
				pktinfo				= CMSG_DATA(cmsg);
				const struct in6_pktinfo* in6	= pktinfo;
				struct sockaddr_in6* sin6	= (struct sockaddr_in6*)dst_addr;
				sin6->sin6_family		= AF_INET6;
				sin6->sin6_addr			= in6->ipi6_addr;
				sin6->sin6_scope_id		= in6->ipi6_ifindex;
/* does not set flow id */
				break;
			}
		}
/* discard on invalid address */
		if (NULL == pktinfo)
			return -1;
	}
	return len;
}

/* upstream = receiver to source, peer-to-peer = receive to receiver
 *
 * NB: SPMRs can be upstream or peer-to-peer, if the packet is multicast then its
 *     a peer-to-peer message, if its unicast its an upstream message.
 *
 * returns TRUE on valid processed packet, returns FALSE on discarded packet.
 */

static
gboolean
on_upstream (
	pgm_transport_t* const		transport,
	struct pgm_sk_buff_t* const	skb
	)
{
/* pre-conditions */
	g_assert (NULL != transport);
	g_assert (NULL != skb);
	g_assert_cmpuint (skb->pgm_header->pgm_dport, ==, transport->tsi.sport);

	g_trace ("on_upstream (transport:%p skb:%p)",
		(gpointer)transport, (gpointer)skb);

	if (!transport->can_send_data) {
		g_trace ("Discarded packet for muted source.");
		goto out_discarded;
	}

/* unicast upstream message, note that dport & sport are reversed */
	if (skb->pgm_header->pgm_sport != transport->dport) {
/* its upstream/peer-to-peer for another session */
		g_trace ("Discarded packet on data-destination port mismatch.");
		goto out_discarded;
	}

	if (!pgm_gsi_equal (&skb->tsi.gsi, &transport->tsi.gsi)) {
/* its upstream/peer-to-peer for another session */
		g_trace ("Discarded packet on data-destination port mismatch.");
		goto out_discarded;
	}

/* advance SKB pointer to PGM type header */
	skb->data	= (guint8*)skb->data + sizeof(struct pgm_header);
	skb->len       -= sizeof(struct pgm_header);

	switch (skb->pgm_header->pgm_type) {
	case PGM_NAK:
		if (!pgm_on_nak (transport, skb))
			goto out_discarded;
		break;

	case PGM_NNAK:
		if (!pgm_on_nnak (transport, skb))
			goto out_discarded;
		break;

	case PGM_SPMR:
		if (!pgm_on_spmr (transport, NULL, skb))
			goto out_discarded;
		break;

	case PGM_POLR:
	default:
		g_trace ("Discarded unsupported PGM type packet.");
		goto out_discarded;
	}

	return TRUE;
out_discarded:
	transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
	return FALSE;
}

/* peer to peer message, either multicast NAK or multicast SPMR.
 *
 * returns TRUE on valid processed packet, returns FALSE on discarded packet.
 */

static
gboolean
on_peer (
	pgm_transport_t* const		transport,
	struct pgm_sk_buff_t* const	skb,
	pgm_peer_t**			source
	)
{
/* pre-conditions */
	g_assert (NULL != transport);
	g_assert (NULL != skb);
	g_assert_cmpuint (skb->pgm_header->pgm_dport, !=, transport->tsi.sport);
	g_assert (NULL != source);

	g_trace ("on_peer (transport:%p skb:%p source:%p)",
		(gpointer)transport, (gpointer)skb, (gpointer)source);

/* we are not the source */
	if (!transport->can_recv_data) {
		g_trace ("Discarded packet for muted receiver.");
		goto out_discarded;
	}

/* unicast upstream message, note that dport & sport are reversed */
	if (skb->pgm_header->pgm_sport != transport->dport) {
/* its upstream/peer-to-peer for another session */
		g_trace ("Discarded packet on data-destination port mismatch.");
		goto out_discarded;
	}

/* check to see the source this peer-to-peer message is about is in our peer list */
	g_static_rw_lock_reader_lock (&transport->peers_lock);
	*source = g_hash_table_lookup (transport->peers_hashtable, &skb->tsi);
	g_static_rw_lock_reader_unlock (&transport->peers_lock);
	if (NULL == *source) {
/* this source is unknown, we don't care about messages about it */
		g_trace ("Discarded packet about new source.");
		goto out_discarded;
	}

/* advance SKB pointer to PGM type header */
	skb->data	= (guint8*)skb->data + sizeof(struct pgm_header);
	skb->len       -= sizeof(struct pgm_header);

	switch (skb->pgm_header->pgm_type) {
	case PGM_NAK:
		if (!pgm_on_peer_nak (transport, *source, skb))
			goto out_discarded;
		break;

	case PGM_SPMR:
		if (!pgm_on_spmr (transport, *source, skb))
			goto out_discarded;
		break;

	case PGM_NNAK:
	case PGM_POLR:
	default:
		g_trace ("Discarded unsupported PGM type packet.");
		goto out_discarded;
	}

	return TRUE;
out_discarded:
	if (*source)
		(*source)->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
	else if (transport->can_send_data)
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
	return FALSE;
}

/* source to receiver message
 *
 * returns TRUE on valid processed packet, returns FALSE on discarded packet.
 */
static
gboolean
on_downstream (
	pgm_transport_t* const		transport,
	struct pgm_sk_buff_t* const	skb,
	struct sockaddr* const		src_addr,
	struct sockaddr* const		dst_addr,
	pgm_peer_t**			source
	)
{
/* pre-conditions */
	g_assert (NULL != transport);
	g_assert (NULL != skb);
	g_assert (NULL != src_addr);
	g_assert (NULL != dst_addr);
	g_assert (NULL != source);

#ifdef RECV_DEBUG
	char saddr[INET6_ADDRSTRLEN], daddr[INET6_ADDRSTRLEN];
	pgm_sockaddr_ntop (src_addr, saddr, sizeof(saddr));
	pgm_sockaddr_ntop (dst_addr, daddr, sizeof(daddr));
	g_trace ("on_downstream (transport:%p skb:%p src-addr:%s dst-addr:%s source:%p)",
		(gpointer)transport, (gpointer)skb, saddr, daddr, (gpointer)source);
#endif

	if (!transport->can_recv_data) {
		g_trace ("Discarded packet for muted receiver.");
		goto out_discarded;
	}

/* pgm packet DPORT contains our transport DPORT */
	if (skb->pgm_header->pgm_dport != transport->dport) {
		g_trace ("Discarded packet on data-destination port mismatch.");
		goto out_discarded;
	}

/* search for TSI peer context or create a new one */
	g_static_rw_lock_reader_lock (&transport->peers_lock);
	*source = g_hash_table_lookup (transport->peers_hashtable, &skb->tsi);
	g_static_rw_lock_reader_unlock (&transport->peers_lock);
	if (NULL == *source) {
		*source = pgm_new_peer (transport,
				       &skb->tsi,
				       (struct sockaddr*)src_addr, pgm_sockaddr_len(src_addr),
				       (struct sockaddr*)dst_addr, pgm_sockaddr_len(dst_addr));
	}
g_trace ("source:%p", (gpointer)*source);

	(*source)->cumulative_stats[PGM_PC_RECEIVER_BYTES_RECEIVED] += skb->len;
	(*source)->last_packet = skb->tstamp;

	skb->data       = (gpointer)( skb->pgm_header + 1 );
	skb->len       -= sizeof(struct pgm_header);

/* handle PGM packet type */
	switch (skb->pgm_header->pgm_type) {
	case PGM_ODATA:
	case PGM_RDATA:
		if (!pgm_on_data (transport, *source, skb))
			goto out_discarded;
		transport->rx_buffer = pgm_alloc_skb (transport->max_tpdu);
		break;

	case PGM_NCF:
		if (!pgm_on_ncf (transport, *source, skb))
			goto out_discarded;
		break;

	case PGM_SPM:
		if (!pgm_on_spm (transport, *source, skb))
			goto out_discarded;

/* update group NLA if appropriate */
		if (pgm_sockaddr_is_addr_multicast ((struct sockaddr*)dst_addr))
			memcpy (&(*source)->group_nla, dst_addr, pgm_sockaddr_len(dst_addr));
		break;

	default:
		g_trace ("Discarded unsupported PGM type packet.");
		goto out_discarded;
	}

	return TRUE;
out_discarded:
	if (*source)
		(*source)->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
	else if (transport->can_send_data)
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
	return FALSE;
}

/* process a pgm packet
 *
 * returns TRUE on valid processed packet, returns FALSE on discarded packet.
 */
static
gboolean
on_pgm (
	pgm_transport_t* const		transport,
	struct pgm_sk_buff_t* const	skb,
	struct sockaddr* const		src_addr,
	struct sockaddr* const		dst_addr,
	pgm_peer_t**			source
	)
{
/* pre-conditions */
	g_assert (NULL != transport);
	g_assert (NULL != skb);
	g_assert (NULL != src_addr);
	g_assert (NULL != dst_addr);
	g_assert (NULL != source);

#ifdef RECV_DEBUG
	char saddr[INET6_ADDRSTRLEN], daddr[INET6_ADDRSTRLEN];
	pgm_sockaddr_ntop (src_addr, saddr, sizeof(saddr));
	pgm_sockaddr_ntop (dst_addr, daddr, sizeof(daddr));
	g_trace ("on_pgm (transport:%p skb:%p src-addr:%s dst-addr:%s source:%p)",
		(gpointer)transport, (gpointer)skb, saddr, daddr, (gpointer)source);
#endif

	if (pgm_is_downstream (skb->pgm_header->pgm_type))
		return on_downstream (transport, skb, src_addr, dst_addr, source);
	if (skb->pgm_header->pgm_dport == transport->tsi.sport)
	{
		if (pgm_is_upstream (skb->pgm_header->pgm_type) ||
		    pgm_is_peer (skb->pgm_header->pgm_type))
		{
			*source = NULL;
			return on_upstream (transport, skb);
		}
	}
	else if (pgm_is_peer (skb->pgm_header->pgm_type))
		return on_peer (transport, skb, source);

	g_trace ("Discarded unknown PGM packet.");
	if (transport->can_send_data)
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
	return FALSE;
}

/* block on receiving socket whilst holding transport::waiting-mutex
 * returns EAGAIN for waiting data, returns EINTR for waiting timer event,
 * returns EFAULT for libc error.
 */

static
int
wait_for_event (
	pgm_transport_t* const	transport,
	const long		timeout		/* microseconds (μs) */
	)
{
	int n_fds = 2;

/* pre-conditions */
	g_assert (NULL != transport);
	g_assert (0 != timeout);

	g_trace ("wait_for_event (transport:%p timeout:%ld)", (gpointer)transport, timeout);

#ifdef CONFIG_HAVE_POLL
	struct pollfd fds[ n_fds ];
	memset (fds, 0, sizeof(fds));
	if (-1 == pgm_transport_poll_info (transport, fds, &n_fds, POLLIN)) {
		g_trace ("poll_info returned errno=%i",errno);
		return EFAULT;
	}
#else
	fd_set readfds;
	FD_ZERO(&readfds);
	if (-1 == pgm_transport_select_info (transport, &readfds, NULL, &n_fds)) {
		g_trace ("select_info returned errno=%i",errno);
		return EFAULT;
	}
#endif /* CONFIG_HAVE_POLL */

/* flush any waiting notifications */
	if (transport->is_pending_read) {
		pgm_notify_clear (&transport->pending_notify);
		transport->is_pending_read = FALSE;
	}

/* spin the locks to allow other thread to set waiting state,
 * first run should trigger waiting pipe event which will flush and loop.
 */
	g_static_mutex_unlock (&transport->pending_mutex);

#ifdef CONFIG_HAVE_POLL
	const int ready = poll (fds, n_fds, timeout /* μs */ / 1000 /* to ms */);
#else
	struct timeval tv_timeout = {
		.tv_sec		= timeout > 1000000UL ? timeout / 1000000UL : 0,
		.tv_usec	= timeout > 1000000UL ? timeout % 1000000UL : timeout
	};
	const int ready = select (n_fds, &readfds, NULL, NULL, &tv_timeout);
#endif

	if (-1 == ready) {
		g_trace ("block returned errno=%i",errno);
		return EFAULT;
	}
	g_static_mutex_lock (&transport->pending_mutex);

#ifdef CONFIG_HAVE_POLL
	if (ready > 0 && fds[0].revents)
#else
	if (ready > 0 && FD_ISSET(transport->recv_sock, &readfds))
#endif
	{
		g_trace ("recv again on empty");
		return EAGAIN;
	}

	g_trace ("state generated event");
	return EINTR;
}

/* data incoming on receive sockets, can be from a sender or receiver, or simply bogus.
 * for IPv4 we receive the IP header to handle fragmentation, for IPv6 we cannot, but the
 * underlying stack handles this for us.
 *
 * recvmsgv reads a vector of apdus each contained in a IO scatter/gather array.
 *
 * can be called due to event from incoming socket(s) or timer induced data loss.
 *
 * on success, returns bytes read, on error returns -1.
 */

GIOStatus
pgm_recvmsgv (
	pgm_transport_t* const	transport,
	pgm_msgv_t* const	msg_start,
	const gsize		msg_len,
	const int		flags,		/* MSG_DONTWAIT for non-blocking */
	gsize*			_bytes_read,	/* may be NULL */
	GError**		error
	)
{
	pgm_peer_t* peer;

	g_return_val_if_fail (NULL != transport, G_IO_STATUS_ERROR);
	if (msg_len) g_return_val_if_fail (NULL != msg_start, G_IO_STATUS_ERROR);
	g_return_val_if_fail (transport->is_bound, G_IO_STATUS_ERROR);
	g_return_val_if_fail (!transport->is_destroyed, G_IO_STATUS_ERROR);

/* pre-conditions */
	g_assert (NULL != transport->rx_buffer);
	g_assert (transport->max_tpdu > 0);
	g_assert (NULL != transport->peers_hashtable);
	g_assert (NULL != transport->rand_);
	g_assert_cmpuint (transport->nak_bo_ivl, >, 1);
	g_assert (pgm_notify_is_valid (&transport->pending_notify));

	g_trace ("pgm_recvmsgv (transport:%p msg-start:%p msg-len:%" G_GSIZE_FORMAT " flags:%d bytes-read:%p error:%p)",
		(gpointer)transport, (gpointer)msg_start, msg_len, flags, (gpointer)_bytes_read, (gpointer)error);

	if (transport->is_reset) {
		g_assert (NULL != transport->peers_pending);
		g_assert (NULL != transport->peers_pending->data);
		peer = transport->peers_pending->data;
		if (flags & MSG_ERRQUEUE)
			pgm_set_reset_error (transport, peer, msg_start);
		else if (error) {
			char tsi[PGM_TSISTRLEN];
			pgm_tsi_print_r (&peer->tsi, tsi, sizeof(tsi));
			g_set_error (error,
				     PGM_RECV_ERROR,
				     PGM_RECV_ERROR_CONNRESET,
				     _("Transport has been reset on unrecoverable loss from %s."),
				     tsi);
		}
		if (!transport->is_abort_on_reset)
			transport->is_reset = !transport->is_reset;
		return G_IO_STATUS_EOF;
	}

/* lock waiting so extra events are not generated during call */
	g_static_mutex_lock (&transport->pending_mutex);

/* timer status */
	if (pgm_timer_check (transport)) {
		pgm_timer_dispatch (transport);
		pgm_timer_prepare (transport);
	}

	gsize bytes_read = 0;
	guint data_read = 0;
	pgm_msgv_t* pmsg = msg_start;
	const pgm_msgv_t* msg_end = msg_start + msg_len;

/* second, flush any remaining contiguous messages from previous call(s) */
	if (transport->peers_pending) {
		if (0 != pgm_flush_peers_pending (transport, &pmsg, msg_end, &bytes_read, &data_read))
			goto out;
	}

/* read the data:
 *
 * We cannot actually block here as packets pushed by the timers need to be addressed too.
 */
	struct sockaddr_storage src, dst;
	ssize_t len;
	gsize bytes_received = 0;

recv_again:

	len = recvskb (transport,
		       transport->rx_buffer,		/* PGM skbuff */
		       MSG_DONTWAIT,
		       (struct sockaddr*)&src,
		       sizeof(src),
		       (struct sockaddr*)&dst,
		       sizeof(dst));
	if (len < 0)
		goto check_for_repeat;
	else if (0 == len)
		goto out;
	else
		bytes_received += len;

	GError* err = NULL;
	const gboolean is_valid = (transport->udp_encap_ucast_port || AF_INET6 == pgm_sockaddr_family (&src)) ?
					pgm_parse_udp_encap (transport->rx_buffer, &err) :
					pgm_parse_raw (transport->rx_buffer, (struct sockaddr*)&dst, &err);
	if (!is_valid)
	{
/* inherently cannot determine PGM_PC_RECEIVER_CKSUM_ERRORS unless only one receiver */
		g_trace ("Discarded invalid packet.");
		if (transport->can_send_data) {
			if (err && PGM_PACKET_ERROR_CKSUM == err->code)
				transport->cumulative_stats[PGM_PC_SOURCE_CKSUM_ERRORS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		}
		goto recv_again;
	}

	pgm_peer_t* source;
	if (!on_pgm (transport, transport->rx_buffer, (struct sockaddr*)&src, (struct sockaddr*)&dst, &source))
		goto recv_again;

/* check whether this source has waiting data */
	if (source && pgm_peer_has_pending (source)) {
		g_trace ("new pending data.");
		pgm_peer_set_pending (transport, source);
	}

flush_pending:
/* flush any congtiguous packets generated by the receipt of this packet */
	if (transport->peers_pending) {
		if (0 != pgm_flush_peers_pending (transport, &pmsg, msg_end, &bytes_read, &data_read))
			goto out;
	}

check_for_repeat:
/* repeat if non-blocking and not full */
	if (flags & MSG_DONTWAIT)
	{
		if (len > 0 && pmsg < msg_end) {
			g_trace ("recv again on not-full");
			goto recv_again;		/* \:D/ */
		}
	}
	else
	{
/* repeat if blocking and empty, i.e. received non data packet.
 */
		if (0 == data_read) {
			const int wait_status = wait_for_event (transport, pgm_timer_expiration (transport));
			if (EAGAIN == wait_status)
				goto recv_again;
			else if (EINTR == wait_status) {
				pgm_timer_dispatch (transport);
				goto flush_pending;
			} else if (EFAULT == wait_status) {
				g_set_error (error,
					     PGM_RECV_ERROR,
					     pgm_recv_error_from_errno (errno),
					     _("Waiting for event: %s"),
					     g_strerror (errno));
				return G_IO_STATUS_ERROR;
			}
		}
	}

out:
	if (0 == data_read)
	{
/* clear event notification */
		if (transport->is_pending_read) {
			pgm_notify_clear (&transport->pending_notify);
			transport->is_pending_read = FALSE;
		}
		g_static_mutex_unlock (&transport->pending_mutex);
/* report data loss */
		if (transport->is_reset) {
			g_assert (NULL != transport->peers_pending);
			g_assert (NULL != transport->peers_pending->data);
			peer = transport->peers_pending->data;
			if (flags & MSG_ERRQUEUE)
				pgm_set_reset_error (transport, peer, msg_start);
			else if (error) {
				char tsi[PGM_TSISTRLEN];
				pgm_tsi_print_r (&peer->tsi, tsi, sizeof(tsi));
				g_set_error (error,
					     PGM_RECV_ERROR,
					     PGM_RECV_ERROR_CONNRESET,
					     _("Transport has been reset on unrecoverable loss from %s."),
					     tsi);
			}
			if (!transport->is_abort_on_reset)
				transport->is_reset = !transport->is_reset;
			return G_IO_STATUS_EOF;
		}
/* return reset on zero bytes instead of waiting for next call */
		return G_IO_STATUS_AGAIN;
	}

	if (transport->peers_pending)
	{
/* set event notification for additional available data */
		if (transport->is_pending_read && transport->is_edge_triggered_recv)
		{
/* empty pending-pipe */
			pgm_notify_clear (&transport->pending_notify);
			transport->is_pending_read = FALSE;
		}
		else if (!transport->is_pending_read && !transport->is_edge_triggered_recv)
		{
/* fill pending-pipe */
			pgm_notify_send (&transport->pending_notify);
			transport->is_pending_read = TRUE;
		}
	}

	g_static_mutex_unlock (&transport->pending_mutex);
	if (_bytes_read)
		*_bytes_read = bytes_read;
	return G_IO_STATUS_NORMAL;
}

/* read one contiguous apdu and return as a IO scatter/gather array.  msgv is owned by
 * the caller, tpdu contents are owned by the receive window.
 *
 * on success, returns the number of bytes read.  on error, -1 is returned, and
 * errno is set appropriately.
 */

GIOStatus
pgm_recvmsg (
	pgm_transport_t* const	transport,
	pgm_msgv_t* const	msgv,
	const int		flags,		/* MSG_DONTWAIT for non-blocking */
	gsize*			bytes_read,	/* may be NULL */
	GError**		error
	)
{
	g_return_val_if_fail (NULL != transport, G_IO_STATUS_ERROR);
	g_return_val_if_fail (NULL != msgv, G_IO_STATUS_ERROR);

	g_trace ("pgm_recvmsg (transport:%p msgv:%p flags:%d bytes_read:%p error:%p)",
		(gpointer)transport, (gpointer)msgv, flags, (gpointer)bytes_read, (gpointer)error);

	return pgm_recvmsgv (transport, msgv, 1, flags, bytes_read, error);
}

/* vanilla read function.  copies from the receive window to the provided buffer
 * location.  the caller must provide an adequately sized buffer to store the largest
 * expected apdu or else it will be truncated.
 *
 * on success, returns the number of bytes read.  on error, -1 is returned, and
 * errno is set appropriately.
 */

GIOStatus
pgm_recvfrom (
	pgm_transport_t* const	transport,
	gpointer		data,
	const gsize		len,
	const int		flags,		/* MSG_DONTWAIT for non-blocking */
	gsize*			_bytes_read,	/* may be NULL */
	pgm_tsi_t*		from,		/* may be NULL */
	GError**		error
	)
{
	pgm_msgv_t msgv;
	gsize bytes_read;

	g_return_val_if_fail (NULL != transport, G_IO_STATUS_ERROR);
	if (len) g_return_val_if_fail (NULL != data, G_IO_STATUS_ERROR);

	g_trace ("pgm_recvfrom (transport:%p data:%p len:%" G_GSIZE_FORMAT " flags:%d bytes-read:%p from:%p error:%p)",
		(gpointer)transport, data, len, flags, (gpointer)_bytes_read, (gpointer)from, (gpointer)error);

	const GIOStatus status = pgm_recvmsg (transport, &msgv, flags & ~(MSG_FIN | MSG_ERRQUEUE), &bytes_read, error);
	if (G_IO_STATUS_NORMAL != status)
		return status;

	gsize bytes_copied = 0;
	struct pgm_sk_buff_t* skb = msgv.msgv_skb[0];

	if (from) {
		memcpy (&from->gsi, &skb->tsi.gsi, sizeof(pgm_gsi_t));
		from->sport = g_ntohs (skb->tsi.sport);
	}

	while (bytes_copied < bytes_read) {
		gsize copy_len = skb->len;
		if (bytes_copied + copy_len > len) {
			g_error ("APDU truncated, original length %" G_GSIZE_FORMAT " bytes.",
				bytes_read);
			copy_len = len - bytes_copied;
			bytes_read = len;
		}
		memcpy ((guint8*)data + bytes_copied, skb->data, copy_len);
		bytes_copied += copy_len;
		skb++;
	}
	if (_bytes_read)
		*_bytes_read = bytes_copied;
	return G_IO_STATUS_NORMAL;
}

GIOStatus
pgm_recv (
	pgm_transport_t* const	transport,
	gpointer		data,
	const gsize		len,
	const int		flags,		/* MSG_DONTWAIT for non-blocking */
	gsize* const		bytes_read,	/* may be NULL */
	GError**		error
	)
{
	g_return_val_if_fail (NULL != transport, G_IO_STATUS_ERROR);
	if (len) g_return_val_if_fail (NULL != data, G_IO_STATUS_ERROR);

	g_trace ("pgm_recv (transport:%p data:%p len:%" G_GSIZE_FORMAT " flags:%d bytes-read:%p error:%p)",
		(gpointer)transport, data, len, flags, (gpointer)bytes_read, (gpointer)error);

	return pgm_recvfrom (transport, data, len, flags, bytes_read, NULL, error);
}

GQuark
pgm_recv_error_quark (void)
{
	return g_quark_from_static_string ("pgm-recv-error-quark");
}

static
PGMRecvError
pgm_recv_error_from_errno (
	gint		err_no
        )
{
	switch (err_no) {
#ifdef EBADF
	case EBADF:
		return PGM_RECV_ERROR_BADF;
		break;
#endif

#ifdef EFAULT
	case EFAULT:
		return PGM_RECV_ERROR_FAULT;
		break;
#endif

#ifdef EINTR
	case EINTR:
		return PGM_RECV_ERROR_INTR;
		break;
#endif

#ifdef EINVAL
	case EINVAL:
		return PGM_RECV_ERROR_INVAL;
		break;
#endif

#ifdef ENOMEM
	case ENOMEM:
		return PGM_RECV_ERROR_NOMEM;
		break;
#endif

	default :
		return PGM_RECV_ERROR_FAILED;
		break;
	}
}


/* eof */
