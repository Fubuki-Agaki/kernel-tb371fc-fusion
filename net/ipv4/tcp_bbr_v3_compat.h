/* SPDX-License-Identifier: GPL-2.0 */
/* BBRv3 backport compat shims for the 4.19 TCP stack.
 *
 * The BBRv3 congestion-model code in tcp_bbr.c is upstream verbatim; this
 * header maps the handful of post-4.19 helpers/macros it references onto
 * their 4.19 equivalents. Separable 6.x features (PLB flow rehash, BPF
 * struct_ops export, EDT departure-time pacing, TLP-recovery events and
 * fast-ack mode) are adapted in tcp_bbr.c itself, not here.
 */
#ifndef _TCP_BBR_V3_COMPAT_H
#define _TCP_BBR_V3_COMPAT_H

#include <linux/prandom.h>
#include <net/tcp.h>
#include <net/inet_connection_sock.h>

/* 6.x annotates BBR hooks for BPF struct_ops export; 4.19 has no such path. */
#define __bpf_kfunc

/* 6.x snd_cwnd accessors -> direct field access. */
#define tcp_snd_cwnd(tp)		((tp)->snd_cwnd)
#define tcp_snd_cwnd_set(tp, val)	((tp)->snd_cwnd = (val))

/* 6.x legacy GSO ceiling; 4.19 GSO_MAX_SIZE is 64K. */
#ifndef GSO_LEGACY_MAX_SIZE
#define GSO_LEGACY_MAX_SIZE		65536
#endif

/* 6.x get_random_u32_below() -> 4.19 prandom_u32_max(). */
#define get_random_u32_below(n)		prandom_u32_max(n)

/* 6.x WANTS_CE_EVENTS asks for CE events; 4.19 NEEDS_ECN also negotiates ECN
 * in the handshake (same mechanism DCTCP uses on this kernel).
 */
#define TCP_CONG_WANTS_CE_EVENTS	TCP_CONG_NEEDS_ECN

/* Precise-ECN-ACK helpers from 6.x net/ipv4/tcp_dctcp.h, reproduced verbatim
 * (4.19 has __tcp_send_ack, ICSK_ACK_* and TCP_ECN_DEMAND_CWR).
 */
static inline void dctcp_ece_ack_cwr(struct sock *sk, u32 ce_state)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (ce_state == 1)
		tp->ecn_flags |= TCP_ECN_DEMAND_CWR;
	else
		tp->ecn_flags &= ~TCP_ECN_DEMAND_CWR;
}

static inline void dctcp_ece_ack_update(struct sock *sk, enum tcp_ca_event evt,
					u32 *prior_rcv_nxt, u32 *ce_state)
{
	u32 new_ce_state = (evt == CA_EVENT_ECN_IS_CE) ? 1 : 0;

	if (*ce_state != new_ce_state) {
		if (inet_csk(sk)->icsk_ack.pending & ICSK_ACK_TIMER) {
			dctcp_ece_ack_cwr(sk, *ce_state);
			__tcp_send_ack(sk, *prior_rcv_nxt);
		}
		inet_csk(sk)->icsk_ack.pending |= ICSK_ACK_NOW;
	}
	*prior_rcv_nxt = tcp_sk(sk)->rcv_nxt;
	*ce_state = new_ce_state;
	dctcp_ece_ack_cwr(sk, new_ce_state);
}

#endif /* _TCP_BBR_V3_COMPAT_H */