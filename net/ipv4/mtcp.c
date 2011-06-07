/*
 *	MPTCP implementation
 *
 *	Authors:
 *      Sébastien Barré		<sebastien.barre@uclouvain.be>
 *
 *      date : May 11
 *
 *      Important note:
 *            When one wants to add support for closing subsockets *during*
 *             a communication, he must ensure that all skbs belonging to
 *             that socket are removed from the meta-queues. Failing
 *             to do this would lead to General Protection Fault.
 *             See also comment in function mtcp_destroy_mpcb().
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/mtcp.h>
#include <net/mtcp_v6.h>
#include <net/ipv6.h>
#include <net/tcp.h>
#include <linux/list.h>
#include <linux/jhash.h>
#include <linux/tcp.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/random.h>
#include <linux/inetdevice.h>
#include <linux/workqueue.h>
#include <asm/atomic.h>
#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
#endif

/*=====================================*/
/*DEBUGGING*/

#ifdef MTCP_RCV_QUEUE_DEBUG
struct mtcp_debug mtcp_debug_array1[1000];
struct mtcp_debug mtcp_debug_array2[1000];

void print_debug_array(void)
{
	int i;
	printk(KERN_ERR "debug array, path index 1:\n");
	for (i = 0; i < 1000 && mtcp_debug_array1[i-1].end == 0; i++) {
		printk(KERN_ERR "\t%s:skb %x, len %d\n",
			mtcp_debug_array1[i].func_name,
			mtcp_debug_array1[i].seq,
			mtcp_debug_array1[i].len);
	}
	printk(KERN_ERR "debug array, path index 2:\n");

	for (i = 0; i < 1000 && mtcp_debug_array2[i-1].end == 0; i++) {
		printk(KERN_ERR "\t%s:skb %x, len %d\n",
			mtcp_debug_array2[i].func_name,
			mtcp_debug_array2[i].seq,
			mtcp_debug_array2[i].len);
	}
}



void freeze_rcv_queue(struct sock *sk, const char *func_name)
{
	int i;
	struct sk_buff *skb;
	struct tcp_sock *tp = tcp_sk(sk);
	int path_index = tp->path_index;
	struct mtcp_debug *mtcp_debug_array;

	if (path_index == 0 || path_index == 1)
		mtcp_debug_array=mtcp_debug_array1;
	else
		mtcp_debug_array=mtcp_debug_array2;

	for (skb = skb_peek(&sk->sk_receive_queue), i = 0;
	     skb && skb != (struct sk_buff *) &sk->sk_receive_queue;
	     skb = skb->next, i++) {
		mtcp_debug_array[i].func_name = func_name;
		mtcp_debug_array[i].seq = TCP_SKB_CB(skb)->seq;
		mtcp_debug_array[i].len = skb->len;
		mtcp_debug_array[i].end = 0;
		BUG_ON(i >= 999);
	}

	if (i > 0)
		mtcp_debug_array[i-1].end = 1;
	else {
		mtcp_debug_array[0].func_name = "NO_FUNC";
		mtcp_debug_array[0].end = 1;
	}
}

#endif
/*=====================================*/

/*Sysctl data*/

#ifdef CONFIG_SYSCTL

int sysctl_mptcp_mss = MPTCP_MSS;
int sysctl_mptcp_ndiffports = 1;
int sysctl_mptcp_enabled = 1;

static ctl_table mptcp_table[] = {
	{
		.procname = "mptcp_mss",
		.data = &sysctl_mptcp_mss,
		.maxlen = sizeof(int),
		.mode = 0644,
		.proc_handler = &proc_dointvec
	},
	{
		.procname = "mptcp_ndiffports",
		.data = &sysctl_mptcp_ndiffports,
		.maxlen = sizeof(int),
		.mode = 0644,
		.proc_handler = &proc_dointvec
	},
	{
		.procname = "mptcp_enabled",
		.data = &sysctl_mptcp_enabled,
		.maxlen = sizeof(int),
		.mode = 0644,
		.proc_handler = &proc_dointvec
	},
	{ }
};

static ctl_table mptcp_net_table[] = {
	{
		.procname = "mptcp",
		.maxlen = 0,
		.mode = 0555,
		.child = mptcp_table
	},
	{ }
};

static ctl_table mptcp_root_table[] = {
	{
		.procname = "net",
		.mode = 0555,
		.child = mptcp_net_table
	},
	{ }
};
#endif

/**
 * Equivalent of tcp_fin() for MPTCP
 * Can be called only when the FIN is validly part
 * of the data seqnum space. Not before when we get holes.
 */
static void mtcp_fin(struct sk_buff *skb, struct multipath_pcb *mpcb) {
	struct sock *meta_sk = (struct sock*) mpcb;

	if (is_dfin_seg(mpcb,skb)) {
		meta_sk->sk_shutdown |= RCV_SHUTDOWN;
		sock_set_flag(meta_sk, SOCK_DONE);
		if (meta_sk->sk_state == TCP_ESTABLISHED)
			tcp_set_state(meta_sk, TCP_CLOSE_WAIT);
	}
}

/*From sock_def_readable(*/
static void mtcp_def_readable(struct sock *sk, int len) {
	struct socket_wq *wq;
	struct multipath_pcb *mpcb = mpcb_from_tcpsock(tcp_sk(sk));
	struct sock *master_sk = mpcb->master_sk;

	BUG_ON(!mpcb);

	mtcp_debug("Waking up master subsock...\n");
	rcu_read_lock();

	wq = rcu_dereference(master_sk->sk_wq);
	if (wq_has_sleeper(wq))
		wake_up_interruptible_sync_poll(&wq->wait,POLLIN |
						POLLRDNORM | POLLRDBAND);

	sk_wake_async(master_sk, SOCK_WAKE_WAITD, POLL_IN);

	rcu_read_unlock();
}

void mtcp_data_ready(struct sock *sk) {
	struct multipath_pcb *mpcb = mpcb_from_tcpsock(tcp_sk(sk));
	BUG_ON(!mpcb);
	mpcb->master_sk->sk_data_ready(mpcb->master_sk, 0);
}

/**
 * Creates as many sockets as path indices announced by the Path Manager.
 * The first path indices are (re)allocated to existing sockets.
 * New sockets are created if needed.
 * Note that this is called only at client side.
 * Server calls mptcp_subflow_attach()
 *
 * WARNING: We make the assumption that this function is run in user context
 *      (we use sock_create_kern, that reserves ressources with GFP_KERNEL)
 */
int mtcp_init_subsockets(struct multipath_pcb *mpcb, u32 path_indices) {
	int i, ret;
	struct socket *sock;
	struct tcp_sock *tp;
	struct sock *meta_sk = (struct sock *) mpcb;

	BUG_ON(!tcp_sk(mpcb->master_sk)->mpc);

	/* First, ensure that we keep existing path indices. */
	mtcp_for_each_tp(mpcb, tp)
		/* disable the corresponding bit of the existing subflow */
		path_indices &= ~PI_TO_FLAG(tp->path_index);

	for (i = 0; i < sizeof(path_indices) * 8; i++) {
		struct sockaddr *loculid, *remulid;
		struct sockaddr_in loculid_in, remulid_in;
		struct sockaddr_in6 loculid_in6, remulid_in6;
		int ulid_size = 0, newpi = i + 1;

		if (!((1 << i) & path_indices))
			continue;

		/* A new socket must be created */
		ret = sock_create_kern(meta_sk->sk_family, SOCK_STREAM,
				IPPROTO_MTCPSUB, &sock);
		if (ret < 0) {
			printk(KERN_ERR "%s: sock_create failed\n",
					__FUNCTION__);
			return ret;
		}
		tp = tcp_sk(sock->sk);

		/* Binding the new socket to the local ulid
		 * (except if we use the MPTCP default PM, in which
		 * case we bind the new socket, directly to its
		 * corresponding locators)
		 */
		switch (meta_sk->sk_family) {
		case AF_INET:
			loculid_in.sin_family = meta_sk->sk_family;
			remulid_in.sin_family = meta_sk->sk_family;

			/* let bind select an available port */
			loculid_in.sin_port = 0;
			remulid_in.sin_port = inet_sk(meta_sk)->inet_dport;

			memcpy(&loculid_in.sin_addr, mtcp_get_loc_addr(
					mpcb, newpi), sizeof(struct in_addr));
			memcpy(&remulid_in.sin_addr, mtcp_get_rem_addr(
					mpcb, newpi), sizeof(struct in_addr));

			loculid = (struct sockaddr *) &loculid_in;
			remulid = (struct sockaddr *) &remulid_in;
			ulid_size = sizeof(loculid_in);

			break;
		case AF_INET6:
			loculid_in6.sin6_family = meta_sk->sk_family;
			remulid_in6.sin6_family = meta_sk->sk_family;

			/* let bind select an available port */
			loculid_in6.sin6_port = 0;
			remulid_in6.sin6_port = inet_sk(meta_sk)->inet_dport;

			ipv6_addr_copy(&loculid_in6.sin6_addr,
				       &inet6_sk(meta_sk)->saddr);
			ipv6_addr_copy(&remulid_in6.sin6_addr,
				       &inet6_sk(meta_sk)->daddr);

			loculid = (struct sockaddr *) &loculid_in6;
			remulid = (struct sockaddr *) &remulid_in6;
			ulid_size = sizeof(loculid_in6);

			break;
		default:
			BUG();
		}
		tp->path_index = newpi;
		tp->mpc = 1;
		tp->slave_sk = 1;

		mtcp_add_sock(mpcb, tp);

		/* Redefine the sk_data_ready function */
		((struct sock *) tp)->sk_data_ready = mtcp_def_readable;

		ret = sock->ops->bind(sock, loculid, ulid_size);
		if (ret < 0)
			goto fail_bind;

		ret = sock->ops->connect(sock, remulid, ulid_size, O_NONBLOCK);
		if (ret < 0 && ret != -EINPROGRESS)
			goto fail_connect;

		if (meta_sk->sk_family == AF_INET)
			mtcp_debug("%s: token %d pi %d src_addr:"
				   "%pI4:%d dst_addr:%pI4:%d \n", __FUNCTION__,
				   loc_token(mpcb), newpi, &loculid_in.sin_addr,
				   ntohs(loculid_in.sin_port), &remulid_in.sin_addr,
				   ntohs(remulid_in.sin_port));
		else
			mtcp_debug("%s: token %d pi %d src_addr:"
				   "%pI6:%d dst_addr:%pI6:%d \n", __FUNCTION__,
				   loc_token(mpcb), newpi, &loculid_in6.sin6_addr,
				   ntohs(loculid_in6.sin6_port), &remulid_in6.sin6_addr,
				   ntohs(remulid_in6.sin6_port));
	}

	return 0;

fail_bind:
	printk(KERN_ERR "%s: MTCP subsocket bind() failed, error %d\n",
			__FUNCTION__, ret);
fail_connect:
	printk(KERN_ERR "%s: MTCP subsocket connect() failed, error %d\n",
			__FUNCTION__, ret);

	/* sock_release will indirectly call mtcp_del_sock() */
	sock_release(sock);
	return -1;
}

/* Defined in net/core/sock.c */
void mtcp_inherit_sk(struct sock *sk, struct sock *newsk);

struct multipath_pcb *mtcp_alloc_mpcb(struct sock *master_sk, gfp_t flags) {
	struct multipath_pcb *mpcb = kmalloc(sizeof(struct multipath_pcb),
					     flags);
	struct tcp_sock *meta_tp = mpcb_meta_tp(mpcb);
	struct sock *meta_sk = (struct sock *) meta_tp;
	struct inet_connection_sock *meta_icsk = inet_csk(meta_sk);

	/* Memory allocation failed.
	 * Stopping here.
	 */
	if (!mpcb)
		return NULL;

	memset(mpcb, 0, sizeof(struct multipath_pcb));
	BUG_ON(mpcb->connection_list);

	/* meta_sk inherits master sk */
	mtcp_inherit_sk(master_sk, meta_sk);
	BUG_ON(mpcb->connection_list);

	/* Will be replaced by the IDSN later. Currently the IDSN is zero */
	meta_tp->copied_seq = meta_tp->rcv_nxt = meta_tp->rcv_wup = 0;
	meta_tp->snd_sml = meta_tp->snd_una = meta_tp->snd_nxt = 0;
	meta_tp->write_seq = 0;

	meta_tp->mpcb = mpcb;
	meta_tp->mpc = 1;
	meta_tp->mss_cache = sysctl_mptcp_mss;

	skb_queue_head_init(&meta_tp->out_of_order_queue);
	skb_queue_head_init(&mpcb->reinject_queue);

	meta_sk->sk_rcvbuf = sysctl_rmem_default;
	meta_sk->sk_sndbuf = sysctl_wmem_default;
	meta_sk->sk_state = TCP_CLOSE;

	/* Inherit locks the meta_sk, so we must release it here. */
	bh_unlock_sock(meta_sk);
	sock_put(meta_sk);

	mpcb->master_sk = master_sk;
	sock_hold(master_sk);

	meta_tp->window_clamp = tcp_sk(master_sk)->window_clamp;
	meta_tp->rcv_ssthresh = tcp_sk(master_sk)->rcv_ssthresh;

	/* Init the accept_queue structure, we support a queue of 4 pending
	 * connections, it does not need to be huge, since we only store
	 * here pending subflow creations.
	 */
	reqsk_queue_alloc(&meta_icsk->icsk_accept_queue, 32, flags);
	/* Pi 1 is reserved for the master subflow */
	mpcb->next_unused_pi = 2;
	/* For the server side, the local token has already been allocated.
	 * Later, we should replace this strange condition (quite a quick hack)
	 * with a test_bit on the server flag. But this requires passing
	 * the server flag in arg of mtcp_alloc_mpcb(), so that we know here if
	 * we are at server or client side. At the moment the only way to know
	 * that is to check for uninitialized token (see tcp_check_req()).
	 */
	if (!tcp_sk(master_sk)->mtcp_loc_token) {
		meta_tp->mtcp_loc_token = mtcp_new_token();
		tcp_sk(master_sk)->mtcp_loc_token = loc_token(mpcb);
	} else {
		meta_tp->mtcp_loc_token = tcp_sk(master_sk)->mtcp_loc_token;
	}

	/* Adding the mpcb in the token hashtable */
	mtcp_hash_insert(mpcb, loc_token(mpcb));

	return mpcb;
}

void mpcb_release(struct multipath_pcb *mpcb)
{
	struct sock *meta_sk = (struct sock*) mpcb;

	/* Must have been destroyed previously */
	if (!sock_flag((struct sock*)mpcb, SOCK_DEAD)) {
		printk(KERN_ERR "Trying to free mpcb without having called "
			"mtcp_destroy_mpcb()\n");
		BUG();
	}

#ifdef CONFIG_MTCP_PM
	mtcp_pm_release(mpcb);
#endif
	mtcp_debug("%s: Will free mpcb\n", __FUNCTION__);
#ifdef CONFIG_SECURITY_NETWORK
	security_sk_free((struct sock *)mpcb);
#endif
	percpu_counter_dec(meta_sk->sk_prot->orphan_count);

	kfree(mpcb);
}

static void mtcp_destroy_mpcb(struct multipath_pcb *mpcb) {
	mtcp_debug("%s: Destroying mpcb with token:%d\n", __FUNCTION__,
			loc_token(mpcb));

	/* Detach the mpcb from the token hashtable */
	mtcp_hash_remove(mpcb);
	/* Accept any subsock waiting in the pending queue
	 * This is needed because those subsocks are established
	 * and still reachable by incoming packets. They will hence
	 * try to reference the mpcb, and need to take a ref
	 * to it to ensure the mpcb does not die before any of its
	 * childs.
	 */
	release_sock(mpcb->master_sk);
	lock_sock(mpcb->master_sk);

	sock_set_flag((struct sock *)mpcb, SOCK_DEAD);

	sock_put(mpcb->master_sk); /* grabbed by mtcp_alloc_mpcb */
}

void mtcp_add_sock(struct multipath_pcb *mpcb, struct tcp_sock *tp) {
	struct sock *meta_sk = (struct sock*) mpcb;
	struct sock *sk = (struct sock*) tp;
	struct sk_buff *skb;

	/* We should not add a non-mpc socket */
	BUG_ON(!tp->mpc);

	/* first subflow */
	if (!tp->path_index)
		tp->path_index = 1;

	/* Adding new node to head of connection_list */
	if (!tp->mpcb) {
		tp->mpcb = mpcb;
		if (!is_master_tp(tp)) {
			/* The corresponding sock_put is in
			 * inet_sock_destruct(). It cannot be included in
			 * mtcp_del_sock(), because the mpcb must remain alive
			 * until the last subsocket is completely destroyed.
			 * The master_sk cannot sock_hold on itself,
			 * otherwise it will never be released.
			 */
			sock_hold(mpcb->master_sk);
		}
	}
	tp->next = mpcb->connection_list;
	mpcb->connection_list = tp;
	tp->attached = 1;

	/* Same token for all subflows */
	tp->rx_opt.mtcp_rem_token
			= tcp_sk(mpcb->master_sk)->rx_opt.mtcp_rem_token;

	mpcb->cnt_subflows++;
	mtcp_update_window_clamp(mpcb);
	atomic_add(
		atomic_read(&((struct sock *)tp)->sk_rmem_alloc),
		&meta_sk->sk_rmem_alloc);

	/* The socket is already established if it was in the
	 * accept queue of the mpcb
	 */
	if (((struct sock*) tp)->sk_state == TCP_ESTABLISHED) {
		mpcb->cnt_established++;
		mtcp_update_sndbuf(mpcb);
		if ((1 << meta_sk->sk_state) &
		    (TCPF_SYN_SENT | TCPF_SYN_RECV))
			meta_sk->sk_state = TCP_ESTABLISHED;
	}

	/* Empty the receive queue of the added new subsocket
	 * we do it with bh disabled, because before the mpcb is attached,
	 * all segs are received in subflow queue,and after the mpcb is
	 * attached, all segs are received in meta-queue. So moving segments
	 * from subflow to meta-queue must be done atomically with the
	 * setting of tp->mpcb.
	 */
	while ((skb = skb_peek(&sk->sk_receive_queue))) {
		int new_mapping;
		__skb_unlink(skb, &sk->sk_receive_queue);

		new_mapping = mtcp_get_dataseq_mapping(tp, skb);
		if (new_mapping < 0) {
			/* The sender manager to insert his segment
			 * in the subrcv queue, but the mapping is invalid.
			 * We should probably send a reset.
			 */
			BUG();
		}
		if (mtcp_queue_skb(sk, skb) == MTCP_EATEN)
			__kfree_skb(skb);
		if (new_mapping == 1)
			mtcp_data_ready(sk);
	}

	mtcp_debug("%s: token %d pi %d, src_addr:%pI4:%d dst_addr:%pI4:%d,"
			" cnt_subflows now %d\n", __FUNCTION__ ,
		   loc_token(mpcb),
		   tp->path_index, &((struct inet_sock *) tp)->inet_saddr,
		   ntohs(((struct inet_sock *) tp)->inet_sport),
		   &((struct inet_sock *) tp)->inet_daddr,
		   ntohs(((struct inet_sock *) tp)->inet_dport),
		   mpcb->cnt_subflows);
}

void mtcp_del_sock(struct multipath_pcb *mpcb, struct tcp_sock *tp) {
	struct tcp_sock *tp_prev;
	int done = 0;

	tp_prev = mpcb->connection_list;
	if (!tp->attached)
		return;

	if (tp_prev == tp) {
		mpcb->connection_list = tp->next;
		mpcb->cnt_subflows--;
		done = 1;
	} else
		for (; tp_prev && tp_prev->next; tp_prev = tp_prev->next) {
			if (tp_prev->next == tp) {
				tp_prev->next = tp->next;
				mpcb->cnt_subflows--;
				done = 1;
				break;
			}
		}

	tp->next = NULL;
	tp->attached = 0;

	BUG_ON(!done);
}

/**
 * Updates the metasocket ULID/port data, based on the given sock.
 * The argument sock must be the sock accessible to the application.
 * In this function, we update the meta socket info, based on the changes
 * in the application socket (bind, address allocation, ...)
 */
void mtcp_update_metasocket(struct sock *sk, struct multipath_pcb *mpcb)
{
	struct tcp_sock *tp;
	struct sock *meta_sk;

	if (sk->sk_protocol != IPPROTO_TCP) return;
	tp = tcp_sk(sk);
	meta_sk = (struct sock *) mpcb;

	meta_sk->sk_family = sk->sk_family;
	inet_sk(meta_sk)->inet_dport = inet_sk(sk)->inet_dport;
	inet_sk(meta_sk)->inet_sport = inet_sk(sk)->inet_sport;

	switch (sk->sk_family) {
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	case AF_INET6:
		/* For IPv6, we just point the meta_sk to the pinet6 struct
		 * of the master_sk, hence inheriting the ulids from there
		 */
		inet_sk(meta_sk)->pinet6 = inet6_sk(sk);
		/* If the socket is v4 mapped, we continue with v4 operations */
		if (!tcp_v6_is_v4_mapped(sk))
			break;
#endif
	case AF_INET:
		inet_sk(meta_sk)->inet_daddr = inet_sk(sk)->inet_daddr;
		inet_sk(meta_sk)->inet_saddr = inet_sk(sk)->inet_saddr;
		break;
	}
#ifdef CONFIG_MTCP_PM
	/* Searching for suitable local addresses,
	 * except is the socket is loopback, in which case we simply
	 * don't do multipath
	 */
	if (!ipv4_is_loopback(inet_sk(sk)->inet_saddr) &&
	    !ipv4_is_loopback(inet_sk(sk)->inet_daddr))
		mtcp_set_addresses(mpcb);

	/* If this added new local addresses, build new paths with them */
	if (mpcb->num_addr4 || mpcb->num_addr6)
		mtcp_update_patharray(mpcb);
#endif
}

/* copied from tcp_output.c */
static inline unsigned int tcp_cwnd_test(struct tcp_sock *tp) {
	u32 in_flight, cwnd;

	in_flight = tcp_packets_in_flight(tp);
	cwnd = tp->snd_cwnd;
	if (in_flight < cwnd)
		return (cwnd - in_flight);

	return 0;
}

int mtcp_is_available(struct sock *sk) {
	/* Set of states for which we are allowed to send data */
	if ((1 << sk->sk_state) & ~(TCPF_ESTABLISHED | TCPF_CLOSE_WAIT))
		return 0;
	if (tcp_sk(sk)->pf || (tcp_sk(sk)->mpcb->noneligible
			& PI_TO_FLAG(tcp_sk(sk)->path_index))
			|| inet_csk(sk)->icsk_ca_state == TCP_CA_Loss)
		return 0;
	if (tcp_cwnd_test(tcp_sk(sk)))
		return 1;
	return 0;
}

/**
 * This is the scheduler. This function decides on which flow to send
 * a given MSS. If all subflows are found to be busy, NULL is returned
 * The flow is selected based on the estimation of how much time will be
 * needed to send the segment. If all paths have full cong windows, we
 * simply block. The flow able to send the segment the soonest get it.
 */
struct sock* get_available_subflow(struct multipath_pcb *mpcb,
				   struct sk_buff *skb)
{
	struct tcp_sock *tp;
	struct sock *sk;
	struct sock *bestsk = NULL;
	unsigned int min_time_to_peer = 0xffffffff;

	if (!mpcb)
		return NULL;

	/* if there is only one subflow, bypass the scheduling function */
	if (mpcb->cnt_subflows == 1) {
		bestsk = (struct sock *) mpcb->connection_list;
		if (!mtcp_is_available(bestsk))
			bestsk = NULL;
		goto out;
	}

	/* First, find the best subflow */
	mtcp_for_each_sk(mpcb, sk, tp) {
		unsigned int time_to_peer;

		if (!mtcp_is_available(sk))
			continue;

		/* If the skb has already been enqueued in this sk, try to find
		 * another one
		 */
		if (PI_TO_FLAG(tp->path_index) & skb->path_mask)
			continue;

		/* If there is no bw estimation available currently,
		 * we only give it data when it has available space in the
		 * cwnd (see above)
		 */
		if (!tp->cur_bw_est) {
			/* If a subflow is available, send immediately */
			if (tcp_packets_in_flight(tp) < tp->snd_cwnd) {
				bestsk = sk;
				break;
			} else {
				continue;
			}
		}

		/* Time to reach peer, estimated in units of jiffies */
		time_to_peer = ((sk->sk_wmem_queued / tp->cur_bw_est) << tp->bw_est.shift) + /*time to reach network*/
				(tp->srtt >> 3); /*Time to reach peer*/

		if (time_to_peer < min_time_to_peer) {
			min_time_to_peer = time_to_peer;
			bestsk = sk;
		}
	}

out:
	return bestsk;
}

int mtcp_sendmsg(struct kiocb *iocb, struct sock *master_sk,
		struct msghdr *msg, size_t size) {
	struct tcp_sock *master_tp = tcp_sk(master_sk);
	struct multipath_pcb *mpcb = mpcb_from_tcpsock(tcp_sk(master_sk));
	struct sock *meta_sk = (struct sock *) mpcb;
	size_t copied = 0;
	int err;
	int flags = msg->msg_flags;
	long timeo = sock_sndtimeo(master_sk, flags & MSG_DONTWAIT);

	lock_sock(master_sk);

	/* If the master sk is not yet established, we need to wait
	 * until the establishment, so as to know whether the mpc option
	 * is present.
	 */
	if (!master_tp->mpc) {
		if ((1 << master_sk->sk_state) & ~(TCPF_ESTABLISHED
				| TCPF_CLOSE_WAIT)) {
			err = sk_stream_wait_connect(master_sk, &timeo);
			if (err) {
				printk(KERN_ERR "err is %d, state %d\n", err,
						master_sk->sk_state);
				goto out_err_nompc;
			}
			/* The flag must be re-checked, because it may have
			 * appeared during sk_stream_wait_connect
			 */
			if (!tcp_sk(master_sk)->mpc) {
				copied = subtcp_sendmsg(iocb, master_sk, msg,
							size);
				goto out;
			}

		} else {
			copied = subtcp_sendmsg(iocb, master_sk, msg, size);
			goto out;
		}
	}

	verif_wqueues(mpcb);

	copied = subtcp_sendmsg(NULL, meta_sk, msg, 0);
	if (copied < 0) {
		printk(KERN_ERR "%s: returning error "
		"to app:%d\n", __FUNCTION__, (int) copied);
		goto out;
	}

out:
	release_sock(master_sk);
	return copied;

out_err_nompc:
	err = sk_stream_error(master_sk, flags, err);
	TCP_CHECK_TIMER(master_sk);
	release_sock(master_sk);
	return err;
}

void mtcp_ofo_queue(struct multipath_pcb *mpcb) {
	struct sk_buff *skb = NULL;
	struct sock *meta_sk = (struct sock *) mpcb;
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);

	while ((skb = skb_peek(&meta_tp->out_of_order_queue)) != NULL) {
		if (after(TCP_SKB_CB(skb)->data_seq, meta_tp->rcv_nxt))
			break;

		if (!after(TCP_SKB_CB(skb)->end_data_seq, meta_tp->rcv_nxt)) {
			struct sk_buff *skb_tail = skb_peek_tail(
					&meta_sk->sk_receive_queue);
			printk(KERN_ERR "ofo packet was already received."
					"skb->end_data_seq:%#x,exp. rcv_nxt:%#x, "
					"skb->dsn:%#x,skb->len:%d\n",
					TCP_SKB_CB(skb)->end_data_seq,
					meta_tp->rcv_nxt,
					TCP_SKB_CB(skb)->data_seq,
					skb->len);
			if (skb_tail)
				printk(KERN_ERR "last packet of the rcv queue:"
						"dsn %#x, last dsn %#x, len %d\n",
						TCP_SKB_CB(skb_tail)->data_seq,
						TCP_SKB_CB(skb_tail)->end_data_seq,
						skb_tail->len);
			/* Should not happen in the current design */
			BUG();
		}

		__skb_unlink(skb, &meta_tp->out_of_order_queue);

		__skb_queue_tail(&meta_sk->sk_receive_queue, skb);
		meta_tp->rcv_nxt = TCP_SKB_CB(skb)->end_data_seq;

		if (tcp_hdr(skb)->fin)
			mtcp_fin(skb, mpcb);
	}
}

/* Clean up the receive buffer for full frames taken by the user,
 * then send an ACK if necessary.  COPIED is the number of bytes
 * tcp_recvmsg has given to the user so far, it speeds up the
 * calculation of whether or not we must ACK for the sake of
 * a window update.
 */
void mtcp_cleanup_rbuf(struct sock *meta_sk, int copied) {
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct multipath_pcb *mpcb = meta_tp->mpcb;
	struct sock *sk;
	struct tcp_sock *tp;
	int time_to_ack = 0;

	mtcp_for_each_sk(mpcb, sk, tp) {
		const struct inet_connection_sock *icsk = inet_csk(sk);
		if (!inet_csk_ack_scheduled(sk))
			continue;
		/* Delayed ACKs frequently hit locked sockets during bulk
		 * receive. */
		if (icsk->icsk_ack.blocked ||
		    /* Once-per-two-segments ACK was not sent by tcp_input.c */
		    tp->rcv_nxt - tp->rcv_wup > icsk->icsk_ack.rcv_mss ||
		    /*
		     * If this read emptied read buffer, we send ACK, if
		     * connection is not bidirectional, user drained
		     * receive buffer and there was a small segment
		     * in queue.
		     */
		    (copied > 0 && ((icsk->icsk_ack.pending & ICSK_ACK_PUSHED2)
				|| ((icsk->icsk_ack.pending & ICSK_ACK_PUSHED)
				&& !icsk->icsk_ack.pingpong))
				&& !atomic_read(&meta_sk->sk_rmem_alloc))) {
			time_to_ack = 1;
		}
	}

	/* We send an ACK if we can now advertise a non-zero window
	 * which has been raised "significantly".
	 *
	 * Even if window raised up to infinity, do not send window open ACK
	 * in states, where we will not receive more. It is useless.
	 */
	if (copied > 0 && !time_to_ack
			&& !(meta_sk->sk_shutdown & RCV_SHUTDOWN)) {
		__u32 rcv_window_now = tcp_receive_window(meta_tp);

		/* Optimize, __tcp_select_window() is not cheap. */
		if (2 * rcv_window_now <= meta_tp->window_clamp) {
			__u32 new_window = __tcp_select_window(mpcb->master_sk);

			/* Send ACK now, if this read freed lots of space
			 * in our buffer. Certainly, new_window is new window.
			 * We can advertise it now, if it is not less than
			 * current one.
			 * "Lots" means "at least twice" here.
			 */
			if (new_window && new_window >= 2 * rcv_window_now)
				time_to_ack = 1;
		}
	}
	/* If we need to send an explicit window update, we need to choose
	   some subflow to send it. At the moment, we use the master subsock
	   for this. */
	if (time_to_ack) {
		/* We send it on all the subflows
		 * that are able to receive data.*/
		mtcp_for_each_sk(mpcb, sk, tp) {
			if (sk->sk_state == TCP_ESTABLISHED ||
			    sk->sk_state == TCP_FIN_WAIT1 ||
			    sk->sk_state == TCP_FIN_WAIT2)
				tcp_send_ack(sk);
		}
	}
}

/* Eats data from the meta-receive queue */
int mtcp_check_rcv_queue(struct multipath_pcb *mpcb, struct msghdr *msg,
		size_t *len, u32 *data_seq, int *copied, int flags) {
	struct sk_buff *skb;
	struct sock *meta_sk = (struct sock*) mpcb;
	int err;
	struct tcp_sock *tp;

	do {
		u32 data_offset = 0;
		unsigned long used;
		int dfin = 0;

		skb = skb_peek(&meta_sk->sk_receive_queue);

		do {
			if (!skb)
				goto exit;

			tp = tcp_sk(skb->sk);

			if (is_dfin_seg(mpcb,skb))
				dfin = 1;

			if (before(*data_seq, TCP_SKB_CB(skb)->data_seq)) {
				printk(KERN_ERR "%s bug: copied %X "
				"dataseq %X\n", __FUNCTION__, *data_seq,
						TCP_SKB_CB(skb)->data_seq);
				BUG();
			}
			data_offset = *data_seq - TCP_SKB_CB(skb)->data_seq;
			if (data_offset < skb->len)
				goto found_ok_skb;
			if (dfin)
				goto found_fin_ok;

			if (skb->len + dfin != TCP_SKB_CB(skb)->end_data_seq
					- TCP_SKB_CB(skb)->data_seq) {
				printk(
						KERN_ERR "skb->len:%d, should be %d\n",
						skb->len,
						TCP_SKB_CB(skb)->end_data_seq
								- TCP_SKB_CB(skb)->data_seq);
				BUG();
			}
			WARN_ON(!(flags & MSG_PEEK));
			skb = skb->next;
		} while (skb != (struct sk_buff *) &meta_sk->sk_receive_queue);
		found_ok_skb: if (skb
				== (struct sk_buff *) &meta_sk->sk_receive_queue)
			goto exit;

		used = skb->len - data_offset;
		if (*len < used)
			used = *len;

		err = skb_copy_datagram_iovec(skb, data_offset, msg->msg_iov,
				used);
		if (err) {
			int iovlen = msg->msg_iovlen;
			struct iovec* iov = msg->msg_iov;
			int msg_size = 0;
			while (iovlen-- > 0) {
				msg_size += iov->iov_len;
				iov++;
			}
			printk(KERN_ERR "err in skb_copy_datagram_iovec:"
			"skb:%p,data_offset:%d, iov:%p,used:%lu,"
			"msg_size:%d,err:%d,skb->len:%ul,*len:%d,"
			"dfin:%d\n", skb, data_offset, iov, used, msg_size,
					err, skb->len, (int) *len, dfin);
			BUG();
		}

		*data_seq += used;
		*copied += used;
		*len -= used;

		if (dfin)
			goto found_fin_ok;

		if (*data_seq == TCP_SKB_CB(skb)->end_data_seq && !(flags
				& MSG_PEEK))
			sk_eat_skb(meta_sk, skb, 0);
		else if (!(flags & MSG_PEEK) && *len != 0) {
			printk(KERN_ERR
			"%s bug: copied %#x "
			"dataseq %#x, *len %d, used:%d\n", __FUNCTION__,
			       *data_seq, TCP_SKB_CB(skb)->data_seq,
			       (int) *len, (int) used);
			BUG();
		}
		continue;

found_fin_ok:
		/* Process the FIN. */
		++*data_seq;
		if (!(flags & MSG_PEEK))
			sk_eat_skb(meta_sk, skb, 0);
		break;
	} while (*len > 0);
	/* This checks whether an explicit window update is needed to unblock
	 * the receiver
	 */
exit:
	mtcp_cleanup_rbuf(meta_sk, *copied);
	return 0;
}

int mtcp_queue_skb(struct sock *sk, struct sk_buff *skb) {
	struct tcp_sock *tp = tcp_sk(sk);
	struct multipath_pcb *mpcb;
	int fin = tcp_hdr(skb)->fin;
	struct sock *meta_sk;
	struct tcp_sock *meta_tp;
	int ans;

	mpcb = mpcb_from_tcpsock(tp);
	meta_sk = (struct sock *) mpcb;
	meta_tp = tcp_sk(meta_sk);

	if (!tp->mpc) {
		/* skb_set_owner_r may already have been called by
		 * tcp_data_queue when the skb has been added to the ofo-queue,
		 * and we are coming from tcp_ofo_queue
		 */
                if (skb->sk != sk)
                        skb_set_owner_r(skb, sk);
		__skb_queue_tail(&sk->sk_receive_queue, skb);
		ans = MTCP_QUEUED;
		goto out;
	}

	if (!skb->len && tcp_hdr(skb)->fin && !tp->rx_opt.saw_dfin) {
		/* Pure subflow FIN (without DFIN)
		 * just update subflow and return
		 */
		++tp->copied_seq;
		ans = MTCP_EATEN;
		goto out;
	}

	/* In all cases, we remove it from the subsock, so copied_seq
	 * must be advanced
	 */
	tp->copied_seq = TCP_SKB_CB(skb)->end_seq + fin;
	tcp_rcv_space_adjust(sk);

	/* Verify that the mapping info has been read */
	if (TCP_SKB_CB(skb)->data_len) {
		mtcp_get_dataseq_mapping(tp, skb);
	}

	/* Is this a duplicate segment? */
	if (!before(meta_tp->rcv_nxt, TCP_SKB_CB(skb)->end_data_seq)) {
		/* Duplicate segment. We can arrive here only if a segment
		 * has been retransmitted by the sender on another subflow.
		 * Retransmissions on the same subflow are handled at the
		 * subflow level.
		 */

		/* We do not read the skb, since it was already received on
		 * another subflow.
		 */
		ans = MTCP_EATEN;
		goto out;
	}

	if (before(meta_tp->rcv_nxt, TCP_SKB_CB(skb)->data_seq)) {
		if (!skb_peek(&meta_tp->out_of_order_queue)) {
			/* Initial out of order segment */
			__skb_queue_head(&meta_tp->out_of_order_queue, skb);
			ans = MTCP_QUEUED;
			goto queued;
		} else {
			struct sk_buff *skb1 = meta_tp->out_of_order_queue.prev;
			/* Find place to insert this segment. */
			do {
				if (!after(TCP_SKB_CB(skb1)->data_seq,
						TCP_SKB_CB(skb)->data_seq))
					break;
			} while ((skb1 = skb1->prev)
				!= (struct sk_buff *)
				&meta_tp->out_of_order_queue);

			/* Do skb overlap to previous one? */
			if (skb1 != (struct sk_buff *)
				&meta_tp->out_of_order_queue
				&& before(TCP_SKB_CB(skb)->data_seq,
					TCP_SKB_CB(skb1)->end_data_seq)) {
				if (!after(TCP_SKB_CB(skb)->end_data_seq,
						TCP_SKB_CB(skb1)->
						end_data_seq)) {
					/* All the bits are present. Drop. */
					/* We do not read the skb, since it was
					 * already received on
					 * another subflow
					 */
					ans = MTCP_EATEN;
					goto out;
				}
				if (!after(TCP_SKB_CB(skb)->data_seq,
						TCP_SKB_CB(skb1)->data_seq)) {
					/* skb and skb1 have the same starting
					 * point, but skb terminates after skb1
					 */
					printk(KERN_ERR "skb->data_seq:%x,"
						"skb->end_data_seq:%x,"
						"skb1->data_seq:%x,"
						"skb1->end_data_seq:%x,"
						"skb->seq:%x,"
						"skb1->seq:%x""\n",
						TCP_SKB_CB(skb)->data_seq,
						TCP_SKB_CB(skb)->end_data_seq,
						TCP_SKB_CB(skb1)->data_seq,
						TCP_SKB_CB(skb1)->end_data_seq,
						TCP_SKB_CB(skb)->seq,
						TCP_SKB_CB(skb1)->seq);
					BUG();
					skb1 = skb1->prev;
				}
			}
			__skb_insert(skb, skb1, skb1->next,
					&meta_tp->out_of_order_queue);
			/* And clean segments covered by new one as whole. */
			while ((skb1 = skb->next) != (struct sk_buff *)
				&meta_tp->out_of_order_queue &&
				after(TCP_SKB_CB(skb)->end_data_seq,
					TCP_SKB_CB(skb1)->data_seq) &&
				!before(TCP_SKB_CB(skb)->end_data_seq,
					TCP_SKB_CB(skb1)->end_data_seq)) {
				skb_unlink(skb1, &meta_tp->out_of_order_queue);
				__kfree_skb(skb1);
			}
			ans = MTCP_QUEUED;
			goto queued;
		}
	} else {
		__skb_queue_tail(&meta_sk->sk_receive_queue, skb);
		meta_tp->rcv_nxt = TCP_SKB_CB(skb)->end_data_seq;

		if (fin)
			mtcp_fin(skb, mpcb);

		/* Check if this fills a gap in the ofo queue */
		if (!skb_queue_empty(&meta_tp->out_of_order_queue))
			mtcp_ofo_queue(mpcb);

		ans = MTCP_QUEUED;
		goto queued;
	}

queued:
	/* Reassign the skb to the meta-socket */
	skb_set_owner_r(skb, meta_sk);
out:
	return ans;
}

/**
 * specific version of skb_entail (tcp.c),that allows appending to any
 * subflow.
 * Here, we do not set the data seq, since it remains the same. However,
 * we do change the subflow seqnum.
 *
 * Note that we make the assumption that, within the local system, every
 * segment has tcb->sub_seq==tcb->seq, that is, the dataseq is not shifted
 * compared to the subflow seqnum. Put another way, the dataseq referenced
 * is actually the number of the first data byte in the segment.
 */
void mtcp_skb_entail(struct sock *sk, struct sk_buff *skb) {
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
	int fin = (tcb->flags & TCPHDR_FIN) ? 1 : 0;

	tcb->seq = tcb->end_seq = tcb->sub_seq = tp->write_seq;
	tcb->sacked = 0; /* reset the sacked field: from the point of view
			  * of this subflow, we are sending a brand new
			  * segment
			  */
	tcp_add_write_queue_tail(sk, skb);
	sk->sk_wmem_queued += skb->truesize;
	sk_mem_charge(sk, skb->truesize);

	/* Take into account seg len */
	tp->write_seq += skb->len + fin;
	tcb->end_seq += skb->len + fin;
}

/* Algorithm by Bryan Kernighan to count bits in a word */
static inline int count_bits(unsigned int v) {
	unsigned int c; /* c accumulates the total bits set in v */
	for (c = 0; v; c++) {
		v &= v - 1; /* clear the least significant bit set */
	}
	return c;
}

/**
 * Reinject data from one TCP subflow to the meta_sk
 * The @skb given pertains to the original tp, that keeps it
 * because the skb is still sent on the original tp. But additionnally,
 * it is sent on the other subflow.
 *
 * @pre : @sk must be the meta_sk
 */
int __mtcp_reinject_data(struct sk_buff *orig_skb, struct sock *sk) {
	struct sk_buff *skb;
	struct tcp_sock *tp = tcp_sk(sk), *tmp_tp;
	struct tcphdr *th;
	struct sock *sk_it;

	/* A segment can be added to the reinject queue only if
	   there is at least one working subflow that has never sent
	   this data */
	mtcp_for_each_sk(tp->mpcb, sk_it, tmp_tp) {
		if (sk_it->sk_state != TCP_ESTABLISHED)
			continue;
		/* If the skb has already been enqueued in this sk, try to find
		   another one */
		if (PI_TO_FLAG(tmp_tp->path_index) & orig_skb->path_mask)
			continue;

		/* candidate subflow found, we can reinject */
		break;
	}

	if (!sk_it) {
		if ((PI_TO_FLAG(1) & orig_skb->path_mask) && (PI_TO_FLAG(9)
				& orig_skb->path_mask))
			tcpprobe_logmsg(sk, "skb already injected to all "
					"paths");
		return 0; /* no candidate found */
	}

	skb = skb_clone(orig_skb, GFP_ATOMIC);
	if (unlikely(!skb))
		return -ENOBUFS;
	skb->sk = sk;

	th = tcp_hdr(skb);

	BUG_ON(!skb);
	BUG_ON(skb->path_mask!=orig_skb->path_mask);

	skb_queue_tail(&tp->mpcb->reinject_queue, skb);
	return 0;
}

/* Inserts data into the reinject queue */
void mtcp_reinject_data(struct sock *orig_sk) {
	struct sk_buff *skb_it;
	struct tcp_sock *orig_tp = tcp_sk(orig_sk);
	struct multipath_pcb *mpcb = orig_tp->mpcb;
	struct sock *meta_sk = (struct sock*) mpcb;

	BUG_ON(is_meta_sk(orig_sk));

	verif_wqueues(mpcb);

	tcp_for_write_queue(skb_it,orig_sk) {
		skb_it->path_mask |= PI_TO_FLAG(orig_tp->path_index);
		if (unlikely(__mtcp_reinject_data(skb_it,meta_sk)<0))
			break;
	}

	tcpprobe_logmsg(orig_sk, "after reinj, reinj queue size:%d",
			skb_queue_len(&mpcb->reinject_queue));

	tcp_push(meta_sk, 0, sysctl_mptcp_mss, TCP_NAGLE_PUSH);

	if (orig_tp->pf == 0)
		tcpprobe_logmsg(orig_sk, "pi %d: entering pf state",
				orig_tp->path_index);
	orig_tp->pf = 1;

	verif_wqueues(mpcb);
}

/**
 * We are short of flags at the moment in tcp_skb_cb to
 * remember that the dfin has been seen in this segment.
 * Hence, as a quick hack, we currently re-check manually.
 * Anyway, this only happens at the end of the communication.
 */
static int mtcp_check_dfin(struct sk_buff *skb) {
	struct tcphdr *th = tcp_hdr(skb);
	unsigned char *ptr;
	int length = (th->doff * 4) - sizeof(struct tcphdr);

	/* Jump through the options to check whether JOIN is there */
	ptr = (unsigned char *) (th + 1);
	while (length > 0) {
		int opcode = *ptr++;
		int opsize;

		switch (opcode) {
		case TCPOPT_EOL:
			return 0;
		case TCPOPT_NOP: /* Ref: RFC 793 section 3.1 */
			length--;
			continue;
		default:
			opsize = *ptr++;
			if (opsize < 2) /* "silly options" */
				return 0;
			if (opsize > length)
				return 0; /* don't parse partial options */

			if (opcode == TCPOPT_MPTCP) {
				struct mptcp_option *mp_opt = (struct mptcp_option *) ptr;

				if (mp_opt->sub == MPTCP_SUB_DSS) {
					struct mp_dss *mdss = (struct mp_dss *) ptr;

					if (mdss->F)
						return 1;
				}
			}
			ptr += opsize - 2;
			length -= opsize;
		}
	}
	return 0;
}

void mtcp_parse_options(uint8_t *ptr, int opsize,
		struct tcp_options_received *opt_rx,
		struct multipath_options *mopt,
		struct sk_buff *skb)
{
	struct mptcp_option *mp_opt = (struct mptcp_option *) ptr;

	switch (mp_opt->sub) {
	case MPTCP_SUB_CAPABLE:
		if (opsize != MPTCP_SUB_LEN_CAPABLE) {
			mtcp_debug("%s: mp_capable: bad option size %d\n",
					__FUNCTION__, opsize);
			break;
		}

		opt_rx->saw_mpc = 1;
		if (mopt)
			mopt->list_rcvd = 1;
		opt_rx->mtcp_rem_token = ntohl(*((u32*)(ptr + 2)));
		break;
	case MPTCP_SUB_JOIN:
		if (opsize != MPTCP_SUB_LEN_JOIN) {
			mtcp_debug("%s: mp_join: bad option size %d\n",
					__FUNCTION__, opsize);
			break;
		}

		opt_rx->mtcp_recv_token = ntohl(*((u32*)(ptr + 2)));
		break;
	case MPTCP_SUB_DSS:
	{
		struct mp_dss *mdss = (struct mp_dss *) ptr;

		ptr += 2;

		if (mdss->A) {
			TCP_SKB_CB(skb)->data_ack = ntohl(*(uint32_t *)ptr);
			TCP_SKB_CB(skb)->mptcp_flags |= MPTCPHDR_ACK;
			ptr += MPTCP_SUB_LEN_ACK;
		}

		if (mdss->M) {
			TCP_SKB_CB(skb)->data_seq = ntohl(*(uint32_t *) ptr);
			TCP_SKB_CB(skb)->sub_seq = ntohl(*(uint32_t *)(ptr + 4)) +
					opt_rx->rcv_isn;
			TCP_SKB_CB(skb)->data_len = ntohs(*(uint16_t *)(ptr + 8));
			TCP_SKB_CB(skb)->end_data_seq =
				TCP_SKB_CB(skb)->data_seq +
				TCP_SKB_CB(skb)->end_seq -
				TCP_SKB_CB(skb)->seq;

			ptr += MPTCP_SUB_LEN_SEQ;
		}

		if (mdss->F) {
			TCP_SKB_CB(skb)->end_data_seq++;
			if (mopt) {
				mopt->dfin_rcvd = opt_rx->saw_dfin = 1;
				mopt->fin_dsn = TCP_SKB_CB(skb)->data_seq +
						TCP_SKB_CB(skb)->data_len;
			}
		}
		break;
	}
	case MPTCP_SUB_ADD_ADDR:
	{
		struct mp_add_addr *mpadd = (struct mp_add_addr *) ptr;

		if (opsize != MPTCP_SUB_LEN_ADD_ADDR) {
			mtcp_debug("%s: mp_add_addr: bad option size %d\n",
					__FUNCTION__, opsize);
			break;
		}

		ptr += 2; /* Move the pointer to the addr */
		if (mpadd->ipver == 4) {
			mtcp_v4_add_raddress(mopt, (struct in_addr*) ptr,
					mpadd->addr_id);
		} else {
			/* Add IPv6 stuff here */
		}
		break;
	}
	default:
		mtcp_debug("%s: Received unkown subtype: %d\n", __FUNCTION__,
				mp_opt->sub);
		break;
	}
}

/**
 * To be called when a segment is in order. That is, either when it is received
 * and is immediately in subflow-order, or when it is stored in the ofo-queue
 * and becomes in-order. This function retrieves the data_seq and end_data_seq
 * values, needed for that segment to be transmitted to the meta-flow.
 * *If the segment already holds a mapping, the current mapping is replaced
 *  with the one provided in the segment.
 * *If the segment contains no mapping, we check if its dataseq can be derived
 *  from the currently stored mapping. If it cannot, then there is an error,
 *  and it must be dropped.
 *
 * - If the mapping has been correctly updated, or the skb has correctly
 *   been given its dataseq, we then check if the segment is in meta-order.
 *   i) if it is: we return 1
 *   ii) if it is not in meta-order (keep in mind that the precondition
 *        requires that it is in subflow order): we return 0
 * - If the skb is faulty (does not contain a dataseq option, and seqnum
 *   not contained in currently stored mapping), we return -1
 */
int mtcp_get_dataseq_mapping(struct tcp_sock *tp, struct sk_buff *skb) {
	int changed = 0;
	struct multipath_pcb *mpcb = mpcb_from_tcpsock(tp);
	int ans = 0;

	BUG_ON(!mpcb);

	if (TCP_SKB_CB(skb)->data_len) {
		tp->map_data_seq = TCP_SKB_CB(skb)->data_seq;
		tp->map_data_len = TCP_SKB_CB(skb)->data_len;
		tp->map_subseq = TCP_SKB_CB(skb)->sub_seq;
		changed = 1;
	}

	/* Is it a subflow only FIN ? */
	if (tcp_hdr(skb)->fin && !tp->rx_opt.saw_dfin && !skb->len)
		return 0;

	if (before(TCP_SKB_CB(skb)->seq, tp->map_subseq)
	    || after(TCP_SKB_CB(skb)->end_seq,
		     tp->map_subseq+tp->map_data_len+tcp_hdr(skb)->fin)) {

		printk(KERN_ERR "seq:%x,tp->map_subseq:%x,"
		       "end_seq:%x,tp->map_data_len:%d,changed:%d\n",
		       TCP_SKB_CB(skb)->seq, tp->map_subseq,
		       TCP_SKB_CB(skb)->end_seq,
		       tp->map_data_len, changed);
		BUG(); /* If we only speak with our own implementation,
			* reaching this point can only be a bug, later we
			* can remove this.
			*/
		return -1;
	}
	/* OK, the segment is inside the mapping, we can
	 * derive the dataseq. Note that:
	 * -we maintain TCP_SKB_CB(skb)->data_len to zero, so as not to mix
	 *  received mappings and derived dataseqs.
	 * -Even if we have received a mapping update, it may differ from
	 *  the seqnum contained in the
	 *  TCP header. In that case we must recompute the data_seq and
	 *  end_data_seq accordingly. This is what happens in case of TSO,
	 *  because the NIC keeps the option as is.
	 */
	TCP_SKB_CB(skb)->data_seq = tp->map_data_seq
		+ (TCP_SKB_CB(skb)->seq - tp->map_subseq);
	TCP_SKB_CB(skb)->end_data_seq = TCP_SKB_CB(skb)->data_seq
		+ skb->len;
	if (mpcb->received_options.dfin_rcvd &&
	    TCP_SKB_CB(skb)->end_data_seq + 1 ==
	    mpcb->received_options.fin_dsn) {
		/* This condition is not enough yet. It is possible that
		 * the skb is in fact the last data segment, and the dfin has
		 * been rcvd out of order separately. If this happens,
		 * we enter this conditional, while the end_data_seq must not
		 * be incremented because the dfin is not there.
		 */
		if (mtcp_check_dfin(skb))
			TCP_SKB_CB(skb)->end_data_seq++;
	}
	TCP_SKB_CB(skb)->data_len = 0; /* To indicate that there is not anymore
					* general mapping information in that
					* segment (the mapping info is now
					* consumed)
					*/

	/* Check now if the segment is in meta-order, it is considered
	 * in meta-order if the next expected DSN is contained in the
	 * segment
	 */

	if (!before(mpcb_meta_tp(mpcb)->copied_seq, TCP_SKB_CB(skb)->data_seq) &&
	     before(mpcb_meta_tp(mpcb)->copied_seq, TCP_SKB_CB(skb)->end_data_seq))
		ans = 1;

	return ans;
}

/**
 * Cleans the meta-socket retransmission queue.
 * @sk must be the metasocket.
 */
void mtcp_clean_rtx_queue(struct sock *sk) {
	struct sk_buff *skb;
	struct tcp_sock *tp = tcp_sk(sk);

	BUG_ON(!is_meta_tp(tp));

	while ((skb = tcp_write_queue_head(sk)) && skb != tcp_send_head(sk)) {
		struct tcp_skb_cb *scb = TCP_SKB_CB(skb);
		if (before(tp->snd_una, scb->end_data_seq))
			break;

		tcp_unlink_write_queue(skb, sk);
		tp->packets_out -= tcp_skb_pcount(skb);
		sk_wmem_free_skb(sk, skb);
	}
}

/**
 * At the moment we apply a simple addition algorithm.
 * We will complexify later
 */
void mtcp_update_window_clamp(struct multipath_pcb *mpcb) {
	struct sock *sk;
	struct tcp_sock *meta_tp = (struct tcp_sock *) mpcb, *tp;
	struct sock *meta_sk = (struct sock *) mpcb;
	u32 new_clamp = 0;
	u32 new_rcv_ssthresh = 0;
	u32 new_rcvbuf = 0;

	/* Can happen if called from non mpcb sock. */
	if (!mpcb)
		return;

	mtcp_for_each_sk(mpcb, sk, tp) {
		new_clamp += tp->window_clamp;
		new_rcv_ssthresh += tp->rcv_ssthresh;
		new_rcvbuf += sk->sk_rcvbuf;
	}
	meta_tp->window_clamp = new_clamp;
	meta_tp->rcv_ssthresh = new_rcv_ssthresh;
	meta_sk->sk_rcvbuf = new_rcvbuf;
}

/**
 * Update the mpcb send window, based on the contributions
 * of each subflow
 */
void mtcp_update_sndbuf(struct multipath_pcb *mpcb) {
	struct sock *meta_sk = (struct sock*) mpcb, *sk;
	struct tcp_sock *tp;
	int new_sndbuf = 0;
	mtcp_for_each_sk(mpcb,sk,tp)
		new_sndbuf += sk->sk_sndbuf;
	meta_sk->sk_sndbuf = new_sndbuf;
}

extern void tcp_check_space(struct sock *sk);

#ifdef DEBUG_WQUEUES
void verif_wqueues(struct multipath_pcb *mpcb)
{
	struct sock *sk;
	struct sock *meta_sk=(struct sock*)mpcb;
	struct tcp_sock *tp;
	struct sk_buff *skb;
	int sum;

	local_bh_disable();
	mtcp_for_each_sk(mpcb, sk, tp) {
		sum=0;
		tcp_for_write_queue(skb, sk) {
			sum+=skb->truesize;
		}
		if (sum!=sk->sk_wmem_queued) {
			printk(KERN_ERR "wqueue leak_1: enqueued:%d, recorded "
					"value:%d\n",
					sum,sk->sk_wmem_queued);

			tcp_for_write_queue(skb,sk) {
				printk(KERN_ERR "skb truesize:%d\n",
						skb->truesize);
			}

			local_bh_enable();
			BUG();
		}
	}
	sum = 0;
	tcp_for_write_queue(skb, meta_sk)
	sum += skb->truesize;
	BUG_ON(sum != meta_sk->sk_wmem_queued);
	local_bh_enable();
}
#else
void verif_wqueues(struct multipath_pcb *mpcb) {
	return;
}
#endif

#ifdef DEBUG_RQUEUES
void verif_rqueues(struct multipath_pcb *mpcb)
{
	struct sock *sk;
	struct sock *meta_sk = (struct sock*)mpcb;
	struct tcp_sock *tp;
	struct sk_buff *skb;
	int sum;

	local_bh_disable();
	mtcp_for_each_sk(mpcb, sk, tp) {
		sum = 0;
		skb_queue_walk(&sk->sk_receive_queue, skb) {
			sum += skb->truesize;
		}
		skb_queue_walk(&tp->out_of_order_queue, skb) {
			sum += skb->truesize;
		}
		/* TODO: add meta-rcv and meta-ofo-queues */
		if (sum != atomic_read(&sk->sk_rmem_alloc)) {
			printk(KERN_ERR "rqueue leak: enqueued:%d, recorded "
					"value:%d\n",
					sum, sk->sk_rmem_alloc);

			local_bh_enable();
			BUG();
		}
	}
	local_bh_enable();
}
#else
void verif_rqueues(struct multipath_pcb *mpcb) {
	return;
}
#endif

/**
 * Returns the next segment to be sent from the mptcp meta-queue.
 * (chooses the reinject queue if any segment is waiting in it, otherwise,
 * chooses the normal write queue).
 * Sets *@reinject to 1 if the returned segment comes from the
 * reinject queue. Otherwise sets @reinject to 0.
 */
struct sk_buff *mtcp_next_segment(struct sock *sk, int *reinject) {
	struct multipath_pcb *mpcb = tcp_sk(sk)->mpcb;
	struct sk_buff *skb;
	if (reinject)
		*reinject = 0;
	if (!is_meta_sk(sk))
		return tcp_send_head(sk);
	if ((skb = skb_peek(&mpcb->reinject_queue))) {
		if (reinject)
			*reinject = 1;
		return skb;
	} else {
		return tcp_send_head(sk);
	}
}

/**
 * Sets the socket pointer of the meta_sk after an accept at the socket level
 * Set also the sk_wq pointer, because it has just been copied by
 * sock_graft()
 */
void mtcp_check_socket(struct sock *sk) {
	if (sk->sk_protocol == IPPROTO_TCP && tcp_sk(sk)->mpcb) {
		struct sock *meta_sk = (struct sock*) (tcp_sk(sk)->mpcb);
		sk_set_socket(meta_sk, sk->sk_socket);
		meta_sk->sk_wq = sk->sk_wq;
	}
}
EXPORT_SYMBOL(mtcp_check_socket);

extern void tcp_queue_skb(struct sock *sk, struct sk_buff *skb);
void tcp_init_nondata_skb(struct sk_buff *skb, u32 seq, u8 flags);

/* Sends the datafin */
void mtcp_send_fin(struct sock *meta_sk) {
	struct sk_buff *skb;
	struct multipath_pcb *mpcb = (struct multipath_pcb *) meta_sk;
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	if (tcp_send_head(meta_sk)) {
		skb = tcp_write_queue_tail(meta_sk);
		TCP_SKB_CB(skb)->flags |= TCPHDR_FIN;
		TCP_SKB_CB(skb)->data_len++;
		TCP_SKB_CB(skb)->end_data_seq++;
		meta_tp->write_seq++;
	} else {
		for (;;) {
			skb = alloc_skb_fclone(MAX_TCP_HEADER, GFP_KERNEL);
			if (skb)
				break;
			yield();
		}
		/* Reserve space for headers and prepare control bits. */
		skb_reserve(skb, MAX_TCP_HEADER);
		tcp_init_nondata_skb(skb, 0, TCPHDR_ACK | TCPHDR_FIN);
		TCP_SKB_CB(skb)->data_seq = meta_tp->write_seq;
		TCP_SKB_CB(skb)->data_len = 1;
		TCP_SKB_CB(skb)->end_data_seq = meta_tp->write_seq + 1;
		/* FIN eats a sequence byte, write_seq advanced by
		 * tcp_queue_skb().
		 */
		tcp_queue_skb(meta_sk, skb);
	}
	set_bit(MPCB_FLAG_FIN_ENQUEUED, &mpcb->flags);
	__tcp_push_pending_frames(meta_sk, sysctl_mptcp_mss, TCP_NAGLE_OFF);
}

extern int tcp_close_state(struct sock *sk);
void mtcp_close(struct sock *master_sk, long timeout) {
	struct multipath_pcb *mpcb;
	struct sock *meta_sk = NULL;
	struct tcp_sock *meta_tp = NULL;
	struct sock *subsk;
	struct tcp_sock *subtp;
	struct sk_buff *skb;
	int data_was_unread = 0;
	int state;

	mtcp_debug("%s: Close of meta_sk\n",__FUNCTION__);

	lock_sock(master_sk);
	mpcb = (tcp_sk(master_sk)->mpc) ? tcp_sk(master_sk)->mpcb : NULL;

	/* destroy the mpcb, it will really disappear when the last subsock
	 * is destroyed
	 */
	if (mpcb) {
		meta_sk = (struct sock *) mpcb;
		meta_tp = tcp_sk(meta_sk);
		sock_hold(master_sk);
		mtcp_destroy_mpcb(mpcb);
	} else {
		sock_hold(master_sk); /* needed to keep the pointer until the
				       * release_sock()
				       */
		tcp_close(master_sk, timeout);
		release_sock(master_sk);
		sock_put(master_sk);
		return;
	}

	BUG_ON(!mpcb);

	meta_sk->sk_shutdown = SHUTDOWN_MASK;

	/* We need to flush the recv. buffs.  We do this only on the
	 * descriptor close, not protocol-sourced closes, because the
	 * reader process may not have drained the data yet!
	 */
	while ((skb = __skb_dequeue(&meta_sk->sk_receive_queue)) != NULL) {
		u32 len = TCP_SKB_CB(skb)->end_data_seq
				- TCP_SKB_CB(skb)->data_seq
				- (is_dfin_seg(mpcb,skb) ? 1 : 0);
		data_was_unread += len;
		__kfree_skb(skb);
	}

	sk_mem_reclaim(meta_sk);

	if (tcp_close_state(meta_sk))
		mtcp_send_fin(meta_sk);
	else if (meta_tp->snd_nxt == meta_tp->write_seq) {
		struct sock *sk_it, *sk_tmp;
		/* The FIN has been sent already, we need to
		 * call tcp_close() on the subsocks
		 * ourselves.
		 */
		mtcp_for_each_sk_safe(mpcb,sk_it,sk_tmp)
			tcp_close(sk_it, 0);
	}

	sk_stream_wait_close(meta_sk, timeout);

	state = meta_sk->sk_state;
	sock_orphan(meta_sk);
	percpu_counter_inc(meta_sk->sk_prot->orphan_count);

	mtcp_for_each_sk(mpcb,subsk,subtp) {
		/* The socket may have been orphaned by the tcp_close()
		 * above, in that case SOCK_DEAD is set already
		 */
		if (!sock_flag(subsk, SOCK_DEAD)) {
			sock_orphan(subsk);
			percpu_counter_inc(subsk->sk_prot->orphan_count);
		}
	}

	/* It is the last release_sock in its life. It will remove backlog. */
	release_sock(master_sk);
	sock_put(master_sk); /* Taken by sock_hold */
}

/**
 * When a listening sock is closed with established children still pending,
 * those children have created already an mpcb (tcp_check_req()).
 * Moreover, that mpcb has possibly received additional children,
 * from JOIN subflows. All this must be cleaned correctly, which is done
 * here. Later we should use a more generic approach, reusing more of
 * the regular TCP stack.
 */
void mtcp_detach_unused_child(struct sock *sk)

{
	struct multipath_pcb *mpcb;
	struct sock *child;
	struct tcp_sock *child_tp;
	if (!sk->sk_protocol == IPPROTO_TCP)
		return;
	mpcb = tcp_sk(sk)->mpcb;
	if (!mpcb)
		return;
	mtcp_destroy_mpcb(mpcb);
	/* Now all subflows of the mpcb are attached,
	 * so we can destroy them, being sure that the mpcb
	 * will be correctly destroyed last.
	 */
	mtcp_for_each_sk(mpcb, child, child_tp) {
		if (child == sk)
			continue; /* master_sk will be freed last
				   * as part of the normal
				   * net_csk_listen_stop() function
				   */
		/* This section is copied from
		 * inet_csk_listen_stop()
		 */
		local_bh_disable();
		WARN_ON(sock_owned_by_user(child));
		sock_hold(child);

		sk->sk_prot->disconnect(child, O_NONBLOCK);

		sock_orphan(child);

		percpu_counter_inc(sk->sk_prot->orphan_count);

		inet_csk_destroy_sock(child);

		local_bh_enable();
		sock_put(child);
	}
}

/**
 * Returns 1 if we should enable MPTCP for that socket.
 */
int do_mptcp(struct sock *sk)
{
	if (!sysctl_mptcp_enabled)
		return 0;
	if ((sk->sk_family == AF_INET &&
	     ipv4_is_loopback(inet_sk(sk)->inet_daddr)) ||
	    (sk->sk_family == AF_INET6 &&
	     ipv6_addr_loopback(&inet6_sk(sk)->daddr)))
		return 0;
	if (is_local_addr4(inet_sk(sk)->inet_daddr))
		return 0;
	return 1;
}

/**
 * Prepares fallback to regular TCP.
 * The master sk is detached and the mpcb structure is destroyed.
 */
static void __mptcp_fallback(struct sock *master_sk)
{
	struct tcp_sock *master_tp = tcp_sk(master_sk);
	struct multipath_pcb *mpcb = mpcb_from_tcpsock(master_tp);
	struct sock *meta_sk = (struct sock *)mpcb;

	if (!mpcb)
		return; /* Fallback is already done */

	if (sock_flag(meta_sk, SOCK_DEAD))
		return; /* mtcp_destroy_mpcb() already called. No need
			 * to fallback.
			 */

	sock_hold(master_sk);
	mtcp_destroy_mpcb(mpcb);
	mpcb_release(mpcb);
	master_tp->mpcb = NULL;
	sock_put(master_sk);
}

void mptcp_fallback_wq(struct work_struct *work)
{
	struct sock *master_sk = *(struct sock **)(work + 1);
	lock_sock(master_sk);
	__mptcp_fallback(master_sk);
	release_sock(master_sk);
	sock_put(master_sk);
	kfree(work);
}

void mptcp_fallback(struct sock *master_sk)
{
	if (in_interrupt()) {
		struct work_struct *work = kmalloc(sizeof(*work) +
						sizeof(struct sock *),
						GFP_ATOMIC);
		struct sock **sk = (struct sock **)(work + 1);

		*sk = master_sk;
		sock_hold(master_sk);
		INIT_WORK(work, mptcp_fallback_wq);
		schedule_work(work);
	} else {
		__mptcp_fallback(master_sk);
	}
}

#ifdef MTCP_DEBUG_PKTS_OUT
int check_pkts_out(struct sock* sk) {
	int cnt=0;
	struct sk_buff *skb;
	struct tcp_sock *tp = tcp_sk(sk);
	/* TODEL: sanity check on packets_out */
	if (tp->mpc && !is_meta_tp(tp)) {
		tcp_for_write_queue(skb,sk) {
			if (skb == tcp_send_head(sk))
			break;
			else cnt+=tcp_skb_pcount(skb);
		}
		BUG_ON(tp->packets_out!=cnt);
	}
	else cnt=-10;

	return cnt;
}

void check_send_head(struct sock *sk, int num) {
	struct sk_buff *head = tcp_send_head(sk);
	struct sk_buff *skb;
	int found = 0;
	if (head) {
		tcp_for_write_queue(skb,sk) {
			if (skb == head) {
				found = 1;
				break;
			}
		}
	} else {
		found = 1;
	}

	if(!found) {
		printk(KERN_ERR "num:%d\n",num);
		BUG();
	}
}
#endif

/* General initialization of mptcp */
static int __init mptcp_init(void) {
#ifdef CONFIG_SYSCTL
	register_sysctl_table(mptcp_root_table);
#endif
	return 0;
}
module_init(mptcp_init);

MODULE_LICENSE("GPL");

EXPORT_SYMBOL(mtcp_sendmsg);
