/*
 *	MPTCP PM implementation
 *
 *	Authors:
 *      Sébastien Barré		<sebastien.barre@uclouvain.be>
 *
 *      date : May 11
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <net/mptcp.h>
#include <net/mptcp_v6.h>
#include <net/mptcp_pm.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/list.h>
#include <linux/tcp.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>	/* Needed by proc_net_fops_create */
#include <net/inet_sock.h>
#include <net/tcp.h>
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
#include <net/if_inet6.h>
#include <net/ipv6.h>
#include <net/ip6_checksum.h>
#include <net/inet6_connection_sock.h>
#endif

#define MPTCP_HASH_SIZE                16
#define hash_tk(token) \
	jhash_1word(token,0)%MPTCP_HASH_SIZE

/* This couple of extern functions should be replaced ASAP
 * with more modular things, because they become quickly a
 * nightmare when we want to upgrade to recent kernel versions.
 */
extern struct ip_options *tcp_v4_save_options(struct sock *sk,
					      struct sk_buff *skb);
extern void tcp_init_nondata_skb(struct sk_buff *skb, u32 seq, u8 flags);
extern void tcp_options_write(__be32 *ptr, struct tcp_sock *tp,
			      struct tcp_out_options *opts);
extern void tcp_v4_send_ack(struct sk_buff *skb, u32 seq, u32 ack,
			    u32 win, u32 ts, int oif,
			    struct tcp_md5sig_key *key, int reply_flags);
extern void __tcp_v4_send_check(struct sk_buff *skb,
				__be32 saddr, __be32 daddr);
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
extern void tcp_v6_send_ack(struct sk_buff *skb, u32 seq, u32 ack, u32 win,
		u32 ts, struct tcp_md5sig_key *key);
extern void	__tcp_v6_send_check(struct sk_buff *skb, struct in6_addr *saddr,
		struct in6_addr *daddr);
extern int tcp_v6_do_rcv(struct sock *sk, struct sk_buff *skb);
#endif

static struct list_head tk_hashtable[MPTCP_HASH_SIZE];
static rwlock_t tk_hash_lock;	/* hashtable protection */

/* This second hashtable is needed to retrieve request socks
 * created as a result of a join request. While the SYN contains
 * the token, the final ack does not, so we need a separate hashtable
 * to retrieve the mpcb.
 */
static struct list_head tuple_hashtable[MPTCP_HASH_SIZE];
static spinlock_t tuple_hash_lock;	/* hashtable protection */

/* consumer of interface UP/DOWN events */
static int mptcp_pm_inetaddr_event(struct notifier_block *this,
				unsigned long event, void *ptr);
static struct notifier_block mptcp_pm_inetaddr_notifier = {
        .notifier_call = mptcp_pm_inetaddr_event,
};

void mptcp_hash_insert(struct multipath_pcb *mpcb, u32 token)
{
	int hash = hash_tk(token);

	mptcp_debug("%s: add mpcb to hash-table with loc_token %d\n",
			__func__, mpcb_meta_tp(mpcb)->mptcp_loc_token);

	write_lock_bh(&tk_hash_lock);
	list_add(&mpcb->collide_tk, &tk_hashtable[hash]);
	write_unlock_bh(&tk_hash_lock);
}

/**
 * This function increments the refcount of the mpcb struct.
 * It is the responsibility of the caller to decrement when releasing
 * the structure.
 */
struct multipath_pcb *mptcp_hash_find(u32 token)
{
	int hash = hash_tk(token);
	struct multipath_pcb *mpcb;

	read_lock(&tk_hash_lock);
	list_for_each_entry(mpcb, &tk_hashtable[hash], collide_tk) {
		if (token == loc_token(mpcb)) {
			sock_hold(mpcb->master_sk);
			read_unlock(&tk_hash_lock);
			return mpcb;
		}
	}
	read_unlock(&tk_hash_lock);
	return NULL;
}

void mptcp_hash_remove(struct multipath_pcb *mpcb)
{
	struct inet_connection_sock *meta_icsk =
	    (struct inet_connection_sock *)mpcb;
	struct listen_sock *lopt = meta_icsk->icsk_accept_queue.listen_opt;

	mptcp_debug("%s: remove mpcb from hash-table with loc_token %d\n",
			__func__, mpcb_meta_tp(mpcb)->mptcp_loc_token);

	/* remove from the token hashtable */
	write_lock_bh(&tk_hash_lock);
	list_del(&mpcb->collide_tk);
	write_unlock_bh(&tk_hash_lock);

	/* Remove all pending request socks.
	 */
	spin_lock_bh(&tuple_hash_lock);
	if (lopt->qlen != 0) {
		unsigned int i;
		for (i = 0; i < lopt->nr_table_entries; i++) {
			struct request_sock *cur_ref;
			cur_ref = lopt->syn_table[i];
			while (cur_ref) {
				/* Remove from global tuple hashtable
				 * We use list_del_init because that
				 * function supports multiple deletes, with
				 * only the first one actually deleting.
				 * This is useful since mptcp_check_req()
				 * might try to remove it as well
				 */
				list_del_init(&cur_ref->collide_tuple);
				/* next element in collision list.
				 * we don't remove yet the request_sock
				 * from the local hashtable. This will be done
				 * by mptcp_pm_release()
				 */
				cur_ref = cur_ref->dl_next;
			}
		}
	}
	spin_unlock_bh(&tuple_hash_lock);
}

void mptcp_hash_request_remove(struct request_sock *req)
{
	spin_lock(&tuple_hash_lock);
	/* list_del_init: see comment in mptcp_hash_remove() */
	list_del_init(&req->collide_tuple);
	spin_unlock(&tuple_hash_lock);
}

void mptcp_pm_release(struct multipath_pcb *mpcb)
{
	struct inet_connection_sock *meta_icsk =
	    (struct inet_connection_sock *)mpcb;
	struct listen_sock *lopt = meta_icsk->icsk_accept_queue.listen_opt;

	/* Remove all pending request socks. */
	if (lopt->qlen != 0) {
		unsigned int i;
		for (i = 0; i < lopt->nr_table_entries; i++) {
			struct request_sock **cur_ref;
			cur_ref = &lopt->syn_table[i];
			while (*cur_ref) {
				struct request_sock *todel;
				printk(KERN_ERR "Destroying request_sock\n");
				lopt->qlen--;
				todel = *cur_ref;
				/* Remove from local hashtable, it has
				 * been removed already from the global one by
				 * mptcp_hash_remove()
				 */
				*cur_ref = (*cur_ref)->dl_next;
				reqsk_free(todel);
			}
		}
	}

	/* Normally we should have
	 * accepted all the child socks in destroy_mpcb, after
	 * having removed the mpcb from the hashtable. So having this queue
	 * non-empty can only be a bug.
	 */
	BUG_ON(!reqsk_queue_empty(&meta_icsk->icsk_accept_queue));
}

/* Generates a token for a new MPTCP connection
 * Currently we assign sequential tokens to
 * successive MPTCP connections. In the future we
 * will need to define random tokens, while avoiding
 * collisions.
 */
u32 mptcp_new_token(void)
{
	static atomic_t latest_token = {.counter = 0 };
	return atomic_inc_return(&latest_token);
}

struct path4 *find_path_mapping4(struct mptcp_loc4 *loc, struct mptcp_loc4 *rem,
				 struct multipath_pcb *mpcb)
{
	int i;
	for (i = 0; i < mpcb->pa4_size; i++) {
		if (mpcb->pa4[i].loc_id != loc->id ||
		    mpcb->pa4[i].rem_id != rem->id)
			continue;

		/* Addresses are equal - now check the port numbers
		 * (0 means wildcard) */
		if (mpcb->pa4[i].loc.sin_port && loc->port &&
		    mpcb->pa4[i].loc.sin_port != loc->port)
			continue;

		if (mpcb->pa4[i].rem.sin_port && rem->port &&
		    mpcb->pa4[i].rem.sin_port != rem->port)
			continue;

		return &mpcb->pa4[i];
	}
	return NULL;
}

struct path4 *mptcp_get_path4(struct multipath_pcb *mpcb, int path_index)
{
	int i;
	for (i = 0; i < mpcb->pa4_size; i++)
		if (mpcb->pa4[i].path_index == path_index)
			return &mpcb->pa4[i];
	return NULL;
}

struct in_addr *mptcp_get_rem_addr4(struct multipath_pcb *mpcb, int path_index)
{
	int i;
	struct sock *meta_sk = (struct sock *)mpcb;
	if (path_index <= 1)
		return (struct in_addr *)&inet_sk(meta_sk)->inet_daddr;
	for (i = 0; i < mpcb->pa4_size; i++) {
		if (mpcb->pa4[i].path_index == path_index)
			return &mpcb->pa4[i].rem.sin_addr;
	}

	/* should not arrive here */
	printk(KERN_ERR "pa4_size:%d,pi:%d\n", mpcb->pa4_size, path_index);
	for (i = 0; i < mpcb->pa4_size; i++)
		printk(KERN_ERR "existing pi:%d\n", mpcb->pa4[i].path_index);

	BUG();
	return NULL;
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
struct path6 *mptcp_get_path6(struct multipath_pcb *mpcb, int path_index)
{
	int i;
	for (i = 0; i < mpcb->pa6_size; i++)
		if (mpcb->pa6[i].path_index == path_index)
			return &mpcb->pa6[i];
	return NULL;
}

struct path6 *find_path_mapping6(struct mptcp_loc6 *loc, struct mptcp_loc6 *rem,
				 struct multipath_pcb *mpcb)
{
	int i;
	for (i = 0; i < mpcb->pa6_size; i++) {
		if (mpcb->pa6[i].loc_id != loc->id ||
		    mpcb->pa6[i].rem_id != rem->id)
			continue;

		/* Addresses are equal - now check the port numbers
		 * (0 means wildcard) */
		if (mpcb->pa6[i].loc.sin6_port && loc->port &&
		    mpcb->pa6[i].loc.sin6_port != loc->port)
			continue;

		if (mpcb->pa6[i].rem.sin6_port && rem->port &&
		    mpcb->pa6[i].rem.sin6_port!= rem->port)
			continue;

		return &mpcb->pa6[i];
	}
	return NULL;
}

struct in6_addr *mptcp_get_rem_addr6(struct multipath_pcb *mpcb, int path_index)
{
	struct sock *meta_sk = (struct sock *)mpcb;
	int i;
	if (path_index <= 1)
		return (struct in6_addr *)&inet6_sk(meta_sk)->daddr;
	for (i = 0; i < mpcb->pa6_size; i++) {
		if (mpcb->pa6[i].path_index == path_index)
			return &mpcb->pa6[i].rem.sin6_addr;
	}

	/* should not arrive here */
	printk(KERN_ERR "pa6_size:%d,pi:%d\n",mpcb->pa6_size,path_index);
	for (i = 0; i < mpcb->pa6_size; i++)
		printk(KERN_ERR "existing pi:%d\n",mpcb->pa6[i].path_index);

	BUG();
	return NULL;
}
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

u8 mptcp_get_loc_addrid(struct multipath_pcb *mpcb, struct sock* sk)
{
	int i;

	if (sk->sk_family == AF_INET) {
		for (i = 0; i < mpcb->num_addr4; i++) {
			if (mpcb->addr4[i].addr.s_addr ==
					inet_sk(sk)->inet_saddr)
				return mpcb->addr4[i].id;
		}
		/* thus it must be the master-socket */
		if (mpcb->master_sk->sk_family != AF_INET ||
		    inet_sk(mpcb->master_sk)->inet_saddr !=
				    inet_sk(sk)->inet_saddr) {
			mptcp_debug("%s %pI4 not locally found\n", __func__,
					&inet_sk(sk)->inet_saddr);
			BUG();
		}

		return 0;
	}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	if (sk->sk_family == AF_INET6) {
		for (i = 0; i < mpcb->num_addr6; i++) {
			if (ipv6_addr_equal(&mpcb->addr6[i].addr,
					&inet6_sk(sk)->saddr))
				return mpcb->addr6[i].id;
		}
		/* thus it must be the master-socket - id = 0 */
		if (mpcb->master_sk->sk_family != AF_INET6 ||
		    ipv6_addr_equal(&inet6_sk(mpcb->master_sk)->saddr,
				&inet6_sk(sk)->saddr)) {
			mptcp_debug("%s %pI6 not locally found\n", __func__,
					&inet6_sk(sk)->saddr);
			BUG();
		}

		return 0;
	}
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

	BUG();
}

static void __mptcp_update_patharray_ports(struct multipath_pcb *mpcb)
{
	int pa4_size = sysctl_mptcp_ndiffports - 1; /* -1 because the initial
						     * flow counts for one.
						     */
	struct path4 *new_pa4;
	int newpa_idx = 0;
	struct sock *meta_sk = (struct sock *)mpcb;

	if (mpcb->pa4)
		return; /* path allocation already done */

	new_pa4 = kmalloc(pa4_size * sizeof(struct path4), GFP_ATOMIC);

	for (newpa_idx = 0; newpa_idx < pa4_size; newpa_idx++) {
		new_pa4[newpa_idx].loc.sin_family = AF_INET;
		new_pa4[newpa_idx].loc.sin_addr.s_addr =
				inet_sk(meta_sk)->inet_saddr;
		new_pa4[newpa_idx].loc.sin_port = 0;
		new_pa4[newpa_idx].loc_id = 0; /* ulid has id 0 */
		new_pa4[newpa_idx].rem.sin_family = AF_INET;
		new_pa4[newpa_idx].rem.sin_addr.s_addr =
			inet_sk(meta_sk)->inet_daddr;
		new_pa4[newpa_idx].rem.sin_port = inet_sk(meta_sk)->inet_dport;
		new_pa4[newpa_idx].rem_id = 0; /* ulid has id 0 */

		new_pa4[newpa_idx].path_index = mpcb->next_unused_pi++;
	}

	mpcb->pa4 = new_pa4;
	mpcb->pa4_size = pa4_size;
}

/* This is the MPTCP PM mapping table */
void mptcp_v4_update_patharray(struct multipath_pcb *mpcb)
{
	struct path4 *new_pa4, *old_pa4;
	int i, j, newpa_idx = 0;
	struct sock *meta_sk = (struct sock *)mpcb;
	/* Count how many paths are available
	 * We add 1 to size of local and remote set, to include the
	 * ULID
	 */
	int ulid_v4;
	int pa4_size;

	if (sysctl_mptcp_ndiffports > 1)
		return __mptcp_update_patharray_ports(mpcb);

	ulid_v4 = (meta_sk->sk_family == AF_INET ||
		   (meta_sk->sk_family == AF_INET6 &&
		    tcp_v6_is_v4_mapped(meta_sk))) ? 1 : 0;
	pa4_size = (mpcb->num_addr4 + ulid_v4) *
	    (mpcb->received_options.num_addr4 + ulid_v4) - ulid_v4;

	new_pa4 = kmalloc(pa4_size * sizeof(struct path4), GFP_ATOMIC);

	if (ulid_v4) {
		struct mptcp_loc4 loc_ulid, rem_ulid;
		loc_ulid.id = 0;
		loc_ulid.port = 0;
		rem_ulid.id = 0;
		rem_ulid.port = 0;
		/* ULID src with other dest */
		for (j = 0; j < mpcb->received_options.num_addr4; j++) {
			struct path4 *p = find_path_mapping4(&loc_ulid,
				&mpcb->received_options.addr4[j], mpcb);
			if (p) {
				memcpy(&new_pa4[newpa_idx++], p,
				       sizeof(struct path4));
			} else {
				p = &new_pa4[newpa_idx++];

				p->loc.sin_family = AF_INET;
				p->loc.sin_addr.s_addr =
						inet_sk(meta_sk)->inet_saddr;
				p->loc.sin_port = 0;
				p->loc_id = 0;

				p->rem.sin_family = AF_INET;
				p->rem.sin_addr = mpcb->received_options.addr4[j].addr;
				p->rem.sin_port = inet_sk(meta_sk)->inet_dport;
mptcp_debug("%s: ulid with dst %d\n", __func__, ntohs(p->rem.sin_port));
				p->rem_id = mpcb->received_options.addr4[j].id;

				p->path_index = mpcb->next_unused_pi++;
			}
		}

		/* ULID dest with other src */
		for (i = 0; i < mpcb->num_addr4; i++) {
			struct path4 *p = find_path_mapping4(&mpcb->addr4[i],
					&rem_ulid, mpcb);
			if (p) {
				memcpy(&new_pa4[newpa_idx++], p,
				       sizeof(struct path4));
			} else {
				p = &new_pa4[newpa_idx++];

				p->loc.sin_family = AF_INET;
				p->loc.sin_addr = mpcb->addr4[i].addr;
				p->loc.sin_port = 0;
				p->loc_id = mpcb->addr4[i].id;

				p->rem.sin_family = AF_INET;
				p->rem.sin_addr.s_addr =
						inet_sk(meta_sk)->inet_daddr;
				p->rem.sin_port = inet_sk(meta_sk)->inet_dport;
mptcp_debug("%s: ulid with src %d\n", __func__, ntohs(p->rem.sin_port));
				p->rem_id = 0;

				p->path_index = mpcb->next_unused_pi++;
			}
		}
	}

	/* Try all other combinations now */
	for (i = 0; i < mpcb->num_addr4; i++)
		for (j = 0; j < mpcb->received_options.num_addr4; j++) {
			struct path4 *p =
			    find_path_mapping4(&mpcb->addr4[i],
					    &mpcb->received_options.addr4[j],
					    mpcb);
			if (p) {
				memcpy(&new_pa4[newpa_idx++], p,
				       sizeof(struct path4));
			} else {
				p = &new_pa4[newpa_idx++];

				p->loc.sin_family = AF_INET;
				p->loc.sin_addr = mpcb->addr4[i].addr;
				p->loc.sin_port = 0;
				p->loc_id = mpcb->addr4[i].id;

				p->rem.sin_family = AF_INET;
				p->rem.sin_addr = mpcb->received_options.addr4[j].addr;
				p->rem.sin_port = inet_sk(meta_sk)->inet_dport;
mptcp_debug("%s: all other with port %d\n", __func__, ntohs(p->rem.sin_port));
				p->rem_id = mpcb->received_options.addr4[j].id;

				p->path_index = mpcb->next_unused_pi++;
			}
		}

	/* Replacing the mapping table */
	old_pa4 = mpcb->pa4;
	mpcb->pa4 = new_pa4;
	mpcb->pa4_size = pa4_size;
	kfree(old_pa4);
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
/*This is the MPTCP PM IPV6 mapping table*/
void mptcp_v6_update_patharray(struct multipath_pcb *mpcb)
{
	struct path6 *new_pa6, *old_pa6;
	int i, j, newpa_idx = 0;
	struct sock *meta_sk = (struct sock *)mpcb;

	/* Count how many paths are available
	 * We add 1 to size of local and remote set, to include the
	 * ULID */
	int ulid_v6 = (meta_sk->sk_family == AF_INET6) ? 1 : 0;
	int pa6_size = (mpcb->num_addr6 + ulid_v6) *
		(mpcb->received_options.num_addr6 + ulid_v6) - ulid_v6;

	new_pa6 = kmalloc(pa6_size * sizeof(struct path6), GFP_ATOMIC);

	if (ulid_v6) {
		struct mptcp_loc6 loc_ulid, rem_ulid;
		loc_ulid.id = 0;
		loc_ulid.port = 0;
		rem_ulid.id = 0;
		rem_ulid.port = 0;
		/* ULID src with other dest */
		for (j = 0; j < mpcb->received_options.num_addr6; j++) {
			struct path6 *p = find_path_mapping6(&loc_ulid,
				&mpcb->received_options.addr6[j], mpcb);
			if (p) {
				memcpy(&new_pa6[newpa_idx++], p,
				       sizeof(struct path6));
			} else {
				p = &new_pa6[newpa_idx++];

				p->loc.sin6_family = AF_INET6;
				ipv6_addr_copy(&p->loc.sin6_addr, &inet6_sk(meta_sk)->saddr);
				p->loc.sin6_port = 0;
				p->loc_id = 0;

				p->rem.sin6_family = AF_INET6;
				ipv6_addr_copy(&p->rem.sin6_addr, &mpcb->received_options.addr6[j].addr);
				p->rem.sin6_port = inet_sk(meta_sk)->inet_dport;
				p->rem_id = mpcb->received_options.addr6[j].id;

				p->path_index = mpcb->next_unused_pi++;
			}
		}
		/* ULID dest with other src */
		for (i = 0; i < mpcb->num_addr6; i++) {
			struct path6 *p = find_path_mapping6(&mpcb->addr6[i],
					&rem_ulid, mpcb);
			if (p) {
				memcpy(&new_pa6[newpa_idx++], p,
				       sizeof(struct path6));
			} else {
				p = &new_pa6[newpa_idx++];

				p->loc.sin6_family = AF_INET6;
				ipv6_addr_copy(&p->loc.sin6_addr, &mpcb->addr6[i].addr);
				p->loc.sin6_port = 0;
				p->loc_id = mpcb->addr6[i].id;

				p->rem.sin6_family = AF_INET6;
				ipv6_addr_copy(&p->rem.sin6_addr, &inet6_sk(meta_sk)->daddr);
				p->rem.sin6_port = inet_sk(meta_sk)->inet_dport;
				p->rem_id = 0;

				p->path_index = mpcb->next_unused_pi++;
			}
		}
	}
	/* Try all other combinations now */
	for (i = 0; i < mpcb->num_addr6; i++)
		for (j = 0; j < mpcb->received_options.num_addr6; j++) {
			struct path6 *p =
			    find_path_mapping6(&mpcb->addr6[i],
					    &mpcb->received_options.addr6[j],
					    mpcb);
			if (p) {
				memcpy(&new_pa6[newpa_idx++], p,
				       sizeof(struct path6));
			} else {
				p = &new_pa6[newpa_idx++];

				p->loc.sin6_family = AF_INET6;
				ipv6_addr_copy(&p->loc.sin6_addr, &mpcb->addr6[i].addr);
				p->loc.sin6_port = 0;
				p->loc_id = mpcb->addr6[i].id;

				p->rem.sin6_family = AF_INET6;
				ipv6_addr_copy(&p->rem.sin6_addr, &mpcb->received_options.addr6[j].addr);
				p->rem.sin6_port = inet_sk(meta_sk)->inet_dport;
				p->rem_id = mpcb->received_options.addr6[j].id;

				p->path_index = mpcb->next_unused_pi++;
			}
		}

	/* Replacing the mapping table */
	old_pa6 = mpcb->pa6;
	mpcb->pa6 = new_pa6;
	mpcb->pa6_size = pa6_size;
	kfree(old_pa6);
}

#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

void mptcp_update_patharray(struct multipath_pcb *mpcb)
{
	mptcp_v4_update_patharray(mpcb);

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	mptcp_v6_update_patharray(mpcb);
#endif

}

void mptcp_set_addresses(struct multipath_pcb *mpcb)
{
	struct net_device *dev;
	int id = 1;
	int num_addr4 = 0;

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	int num_addr6 = 0;
#endif

	/* if multiports is requested, we work with the main address
	 * and play only with the ports
	 */
	if (sysctl_mptcp_ndiffports != 1)
		return;

	read_lock_bh(&dev_base_lock);

	for_each_netdev(&init_net, dev) {
		if (netif_running(dev)) {
			struct in_device *in_dev = dev->ip_ptr;
			struct in_ifaddr *ifa;
			__be32 ifa_address;

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
			struct inet6_dev *in6_dev = dev->ip6_ptr;
			struct inet6_ifaddr *ifa6;
#endif

			if (dev->flags & IFF_LOOPBACK)
				continue;

			for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next) {
				ifa_address = ifa->ifa_local;

				if (num_addr4 == MPTCP_MAX_ADDR) {
					mptcp_debug
						("%s: At max num of local "
						 "addresses: "
						 "%d --- not adding address:"
						 " %pI4\n",
						 __func__, MPTCP_MAX_ADDR,
						 &ifa_address);
					goto out;
				}

				if (mpcb->master_sk->sk_family == AF_INET &&
					ifa->ifa_address ==
					inet_sk(mpcb->master_sk)->inet_saddr)
					continue;
				if (ifa->ifa_scope == RT_SCOPE_HOST)
					continue;
				mpcb->addr4[num_addr4].addr.s_addr =
				    ifa_address;
				mpcb->addr4[num_addr4].port = 0;
				mpcb->addr4[num_addr4++].id = id++;
			}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)

			list_for_each_entry(ifa6, &in6_dev->addr_list, if_list) {
				if (num_addr6 == MPTCP_MAX_ADDR) {
					mptcp_debug("%s: At max num of local addresses:"
						"%d --- not adding address: %pI6\n",
						__func__,
						MPTCP_MAX_ADDR, &ifa6->addr);
					goto out;
				}

				if (mpcb->master_sk->sk_family == AF_INET6 &&
					ipv6_addr_equal(&(ifa6->addr),
					&(inet6_sk(mpcb->master_sk)->saddr)))
					continue;
				if (ipv6_addr_scope(&ifa6->addr) == IPV6_ADDR_LINKLOCAL)
					continue;
				ipv6_addr_copy(&(mpcb->addr6[num_addr6].addr),
					&(ifa6->addr));
				mpcb->addr6[num_addr6].port = 0;
				mpcb->addr6[num_addr6++].id = id++;
			}
#endif
		}
	}

out:
	read_unlock_bh(&dev_base_lock);

	/* We update num_addr4 at the end to avoid racing with the ADDR option
	 * trigger (in tcp_established_options()),
	 * which can interrupt us in the middle of this function,
	 * and decide to already send the set of addresses, even though all
	 * addresses have not yet been read.
	 */
	mpcb->num_addr4 = mpcb->addr4_unsent = num_addr4;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	mpcb->num_addr6 = mpcb->addr6_unsent = num_addr6;
#endif
}

/**
 * Based on function tcp_v4_conn_request (tcp_ipv4.c)
 * Returns -1 if there is no space anymore to store an additional
 * address
 */
int mptcp_v4_add_raddress(struct multipath_options *mopt,
			struct in_addr *addr, __be16 port, u8 id)
{
	int i;
	int num_addr4 = mopt->num_addr4;
	struct mptcp_loc4 *loc4 = &mopt->addr4[0];

	/* If the id is zero, this is the ULID, do not add it. */
	if (!id)
		return 0;

	BUG_ON(num_addr4 > MPTCP_MAX_ADDR);

	for (i = 0; i < num_addr4; i++) {
		loc4 = &mopt->addr4[i];

		/* Address is already in the list --- continue */
		if (loc4->addr.s_addr == addr->s_addr && loc4->port == port)
			return 0;

		/* This may be the case, when the peer is behind a NAT. He is
		 * trying to JOIN, thus sending the JOIN with a certain ID.
		 * However the src_addr of the IP-packet has been changed. We
		 * update the addr in the list, because this is the address as
		 * OUR BOX sees it. */
		if (loc4->id == id && loc4->addr.s_addr != addr->s_addr) {
			/* update the address */
			mptcp_debug("%s: updating old addr:%pI4"
				   " to addr %pi4 with id:%d\n",
				   __FUNCTION__, &loc4->addr.s_addr,
				   &addr->s_addr, id);
			loc4->addr.s_addr = addr->s_addr;
			loc4->port = port;
			mopt->list_rcvd = 1;
			return 0;
		}
	}

	/* Do we have already the maximum number of local/remote addresses? */
	if (num_addr4 == MPTCP_MAX_ADDR) {
		mptcp_debug("%s: At max num of remote addresses: %d --- not "
			   "adding address: %pI4\n",
			   __func__, MPTCP_MAX_ADDR, &addr->s_addr);
		return -1;
	}

	/* Address is not known yet, store it */
	loc4->addr.s_addr = addr->s_addr;
	loc4->port = port;
	loc4->id = id;
	mopt->list_rcvd = 1;
	mopt->num_addr4++;

	return 0;
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
/**
 * Based on function tcp_v4_conn_request (tcp_ipv4.c)
 * Returns -1 if there is no space anymore to store an additional
 * address
 *
 */
int mptcp_v6_add_raddress(struct multipath_options *mopt,
			 struct in6_addr *addr, __be16 port, u8 id)
{
	int i;
	int num_addr6 = mopt->num_addr6;
	struct mptcp_loc6 *loc6 = &mopt->addr6[0];

	/* If the id is zero, this is the ULID, do not add it. */
	if (!id)
		return 0;

	BUG_ON(num_addr6 > MPTCP_MAX_ADDR);

	for (i = 0; i < num_addr6; i++) {
		loc6 = &mopt->addr6[i];

		/* Address is already in the list --- continue */
		if (ipv6_addr_equal(&loc6->addr, addr))
			return 0;

		/* This may be the case, when the peer is behind a NAT. He is
		 * trying to JOIN, thus sending the JOIN with a certain ID.
		 * However the src_addr of the IP-packet has been changed. We
		 * update the addr in the list, because this is the address as
		 * OUR BOX sees it. */
		if (loc6->id == id &&
			!ipv6_addr_equal(&loc6->addr, addr)) {
			/* update the address */
			mptcp_debug("%s: updating old addr: %pI6 \
					to addr %pI6 with id:%d\n",
					__func__, &loc6->addr,
					addr, id);
			ipv6_addr_copy(&loc6->addr, addr);
			loc6->port = port;
			mopt->list_rcvd = 1;
			return 0;
		}
	}

	/* Do we have already the maximum number of local/remote addresses? */
	if (num_addr6 == MPTCP_MAX_ADDR) {
		mptcp_debug("%s: At max num of remote addresses: %d --- not "
				"adding address: %pI6\n",
				__func__, MPTCP_MAX_ADDR, addr);
		return -1;
	}

	/* Address is not known yet, store it */
	ipv6_addr_copy(&loc6->addr, addr);
	loc6->port = port;
	loc6->id = id;
	mopt->list_rcvd = 1;
	mopt->num_addr6++;

	return 0;
}

#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

static struct dst_entry *mptcp_route_req(const struct request_sock *req)
{
	struct rtable *rt;
	const struct inet_request_sock *ireq = inet_rsk(req);
	struct ip_options *opt = inet_rsk(req)->opt;
	struct flowi fl = {.nl_u = {.ip4_u = {.daddr = ((opt && opt->srr) ?
					       opt->faddr : ireq->rmt_addr),
				     .saddr = ireq->loc_addr} },
	.proto = IPPROTO_TCP,
	.flags = 0,
	.uli_u = {.ports = {.sport = ireq->loc_port,
			    .dport = ireq->rmt_port} }
	};
	security_req_classify_flow(req, &fl);
	if (ip_route_output_flow(&init_net, &rt, &fl, NULL, 0)) {
		IP_INC_STATS_BH(&init_net, IPSTATS_MIB_OUTNOROUTES);
		return NULL;
	}
	if (opt && opt->is_strictroute && rt->rt_dst != rt->rt_gateway) {
		ip_rt_put(rt);
		IP_INC_STATS_BH(&init_net, IPSTATS_MIB_OUTNOROUTES);
		return NULL;
	}
	return &rt->dst;
}

static unsigned mptcp_synack_options(struct request_sock *req,
				    unsigned mss, struct sk_buff *skb,
				    struct tcp_out_options *opts,
				    struct tcp_md5sig_key **md5)
{
	struct inet_request_sock *ireq = inet_rsk(req);
	unsigned remaining = MAX_TCP_OPTION_SPACE;
	int i;

	*md5 = NULL;

	opts->mss = mss;
	remaining -= TCPOLEN_MSS_ALIGNED;

	if (likely(ireq->wscale_ok)) {
		opts->ws = ireq->rcv_wscale;
		opts->options |= OPTION_WSCALE;
		remaining -= TCPOLEN_WSCALE_ALIGNED;
	}
	if (likely(ireq->tstamp_ok)) {
		opts->options |= OPTION_TS;
		opts->tsval = TCP_SKB_CB(skb)->when;
		opts->tsecr = req->ts_recent;
		remaining -= TCPOLEN_TSTAMP_ALIGNED;
	}
	if (likely(ireq->sack_ok)) {
		opts->options |= OPTION_SACK_ADVERTISE;
		if (unlikely(!ireq->tstamp_ok))
			remaining -= TCPOLEN_SACKPERM_ALIGNED;
	}

	/* Send token in SYN/ACK */
	opts->options |= OPTION_MP_JOIN;
	opts->token = req->mptcp_rem_token;
#ifdef CONFIG_MPTCP_PM
	opts->addr_id = 0;

	/* Finding Address ID */
	if (req->rsk_ops->family == AF_INET)
		for (i = 0; i < req->mpcb->num_addr4; i++) {
			if (req->mpcb->addr4[i].addr.s_addr == ireq->loc_addr)
				opts->addr_id = req->mpcb->addr4[i].id;
		}
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else /* IPv6 */
		for (i = 0; i < req->mpcb->num_addr6; i++) {
			if (ipv6_addr_equal(&req->mpcb->addr6[i].addr, &inet6_rsk(req)->loc_addr))
				opts->addr_id = req->mpcb->addr6[i].id;
		}
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */
#endif /* CONFIG_MPTCP_PM */
	remaining -= MPTCP_SUB_LEN_JOIN_ALIGN;

	return MAX_TCP_OPTION_SPACE - remaining;
}

static __inline__ void
TCP_ECN_make_synack(struct request_sock *req, struct tcphdr *th)
{
	if (inet_rsk(req)->ecn_ok)
		th->ece = 1;
}

/*
 * Prepare a SYN-ACK, for JOINed subflows
 */
static struct sk_buff *mptcp_make_synack(struct sock *master_sk,
					struct dst_entry *dst,
					struct request_sock *req)
{
	struct inet_request_sock *ireq = inet_rsk(req);
	struct tcp_sock *master_tp = tcp_sk(master_sk);
	struct tcphdr *th;
	int tcp_header_size;
	struct tcp_out_options opts;
	struct sk_buff *skb;
	struct tcp_md5sig_key *md5;
	int mss;

	skb = alloc_skb(MAX_TCP_HEADER + 15, GFP_ATOMIC);
	if (skb == NULL)
		return NULL;

	/* Reserve space for headers. */
	skb_reserve(skb, MAX_TCP_HEADER);

	skb_dst_set(skb, dst_clone(dst));

	mss = dst_metric_advmss(dst);
	if (master_tp->rx_opt.user_mss && master_tp->rx_opt.user_mss < mss)
		mss = master_tp->rx_opt.user_mss;

	if (req->rcv_wnd == 0) {	/* ignored for retransmitted syns */
		__u8 rcv_wscale;
		/* Set this up on the first call only */
		req->window_clamp = dst_metric(dst, RTAX_WINDOW);
		/* tcp_full_space because it is guaranteed to be the first
		   packet */
		tcp_select_initial_window(tcp_win_from_space
					  (sysctl_rmem_default),
					  mss -
					  (ireq->tstamp_ok ?
					   TCPOLEN_TSTAMP_ALIGNED : 0),
					  &req->rcv_wnd, &req->window_clamp,
					  ireq->wscale_ok, &rcv_wscale,
					  dst_metric(dst, RTAX_INITRWND));
		ireq->rcv_wscale = rcv_wscale;
	}

	memset(&opts, 0, sizeof(opts));

	TCP_SKB_CB(skb)->when = tcp_time_stamp;
	tcp_header_size = mptcp_synack_options(req, mss, skb, &opts, &md5)
	    + sizeof(*th);

	skb_push(skb, tcp_header_size);
	skb_reset_transport_header(skb);

	th = tcp_hdr(skb);
	memset(th, 0, sizeof(struct tcphdr));
	th->syn = 1;
	th->ack = 1;
	TCP_ECN_make_synack(req, th);
	th->source = ireq->loc_port;
	th->dest = ireq->rmt_port;
	/* Setting of flags are superfluous here for callers (and ECE is
	 * not even correctly set)
	 */
	tcp_init_nondata_skb(skb, tcp_rsk(req)->snt_isn,
			     TCPHDR_SYN | TCPHDR_ACK);
	th->seq = htonl(TCP_SKB_CB(skb)->seq);
	th->ack_seq = htonl(tcp_rsk(req)->rcv_isn + 1);

	/* RFC1323: The window in SYN & SYN/ACK segments is never scaled. */
	th->window = htons(min(req->rcv_wnd, 65535U));
	tcp_options_write((__be32 *) (th + 1), NULL, &opts);
	th->doff = (tcp_header_size >> 2);

	return skb;
}

/**
 * Send a SYN-ACK after having received a SYN.
 * This is to be used for JOIN subflows only.
 * Initial subflows use the regular tcp_v4_rtx_synack() function.
 * This still operates on a request_sock only, not on a big
 * socket.
 */
int mptcp_v4_send_synack(struct sock *meta_sk,
			struct request_sock *req,
			struct request_values *rvp)
{
	const struct inet_request_sock *ireq = inet_rsk(req);
	struct sock *master_sk = ((struct multipath_pcb *)meta_sk)->master_sk;
	int err = -1;
	struct sk_buff *skb;
	struct dst_entry *dst;

	/* First, grab a route. */
	dst = mptcp_route_req(req);
	if (!dst)
		return -1;

	skb = mptcp_make_synack(master_sk, dst, req);

	if (skb) {
		__tcp_v4_send_check(skb, ireq->loc_addr, ireq->rmt_addr);

		err = ip_build_and_send_pkt(skb, meta_sk, ireq->loc_addr,
					    ireq->rmt_addr, ireq->opt);
		err = net_xmit_eval(err);
	}

	dst_release(dst);
	return err;
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)

int mptcp_v6_send_synack(struct sock *meta_sk,
				 struct request_sock *req)
{
	struct sock *master_sk = ((struct multipath_pcb *)meta_sk)->master_sk;
	struct inet6_request_sock *treq = inet6_rsk(req);
	struct ipv6_pinfo *np = inet6_sk(meta_sk);
	struct sk_buff *skb;
	struct ipv6_txoptions *opt = NULL;
	struct in6_addr *final_p, final;
	struct flowi fl;
	struct dst_entry *dst;
	int err = -1;

	memset(&fl, 0, sizeof(fl));
	fl.proto = IPPROTO_TCP;
	ipv6_addr_copy(&fl.fl6_dst, &treq->rmt_addr);
	ipv6_addr_copy(&fl.fl6_src, &treq->loc_addr);
	fl.fl6_flowlabel = 0;
	fl.oif = treq->iif;
	fl.mark = meta_sk->sk_mark;
	fl.fl_ip_dport = inet_rsk(req)->rmt_port;
	fl.fl_ip_sport = inet_rsk(req)->loc_port;
	security_req_classify_flow(req, &fl);

	opt = np->opt;
	final_p = fl6_update_dst(&fl, opt, &final);

	err = ip6_dst_lookup(meta_sk, &dst, &fl);
	if (err)
		goto done;
	if (final_p)
		ipv6_addr_copy(&fl.fl6_dst, final_p);
	err = xfrm_lookup(sock_net(meta_sk), &dst, &fl, meta_sk, 0);
	if (err < 0)
		goto done;

	skb = mptcp_make_synack(master_sk, dst, req);

	if (skb) {
		__tcp_v6_send_check(skb, &treq->loc_addr, &treq->rmt_addr);

		ipv6_addr_copy(&fl.fl6_dst, &treq->rmt_addr);
		err = ip6_xmit(meta_sk, skb, &fl, opt);
		err = net_xmit_eval(err);
	}

done:
	if (opt && opt != np->opt)
		sock_kfree_s(meta_sk, opt, opt->tot_len);
	dst_release(dst);
	return err;
}
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */


/*Copied from net/ipv4/inet_connection_sock.c*/
static inline u32 inet_synq_hash(const __be32 raddr, const __be16 rport,
				 const u32 rnd, const u32 synq_hsize)
{
	return jhash_2words((__force u32) raddr, (__force u32) rport,
			    rnd) & (synq_hsize - 1);
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
/*
 * Copied from net/ipv6/inet6_connection_sock.c
 */
static u32 inet6_synq_hash(const struct in6_addr *raddr, const __be16 rport,
			   const u32 rnd, const u16 synq_hsize)
{
	u32 c;

	c = jhash_3words((__force u32)raddr->s6_addr32[0],
			 (__force u32)raddr->s6_addr32[1],
			 (__force u32)raddr->s6_addr32[2],
			 rnd);

	c = jhash_2words((__force u32)raddr->s6_addr32[3],
			 (__force u32)rport,
			 c);

	return c & (synq_hsize - 1);
}
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

static void mptcp_v4_reqsk_queue_hash_add(struct request_sock *req,
				      unsigned long timeout)
{
	struct inet_connection_sock *meta_icsk =
	    (struct inet_connection_sock *)(req->mpcb);
	struct listen_sock *lopt = meta_icsk->icsk_accept_queue.listen_opt;
	const u32 h_local = inet_synq_hash(inet_rsk(req)->rmt_addr,
					   inet_rsk(req)->rmt_port,
					   lopt->hash_rnd,
					   lopt->nr_table_entries);
	const u32 h_global = inet_synq_hash(inet_rsk(req)->rmt_addr,
					    inet_rsk(req)->rmt_port,
					    0,
					    MPTCP_HASH_SIZE);
	spin_lock_bh(&tuple_hash_lock);
	reqsk_queue_hash_req(&meta_icsk->icsk_accept_queue,
			     h_local, req, timeout);
	list_add(&req->collide_tuple, &tuple_hashtable[h_global]);
	spin_unlock_bh(&tuple_hash_lock);
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)

static void mptcp_v6_reqsk_queue_hash_add(struct request_sock *req,
				      unsigned long timeout)
{

	struct inet_connection_sock *meta_icsk =
		(struct inet_connection_sock *)(req->mpcb);
	struct listen_sock *lopt = meta_icsk->icsk_accept_queue.listen_opt;
	const u32 h_local = inet6_synq_hash(&inet6_rsk(req)->rmt_addr,
					   inet_rsk(req)->rmt_port,
					   lopt->hash_rnd,
					   lopt->nr_table_entries);
	const u32 h_global = inet6_synq_hash(&inet6_rsk(req)->rmt_addr,
					    inet_rsk(req)->rmt_port,
					    0,
					    MPTCP_HASH_SIZE);
	spin_lock_bh(&tuple_hash_lock);
	reqsk_queue_hash_req(&meta_icsk->icsk_accept_queue,
			     h_local, req, timeout);
	list_add(&req->collide_tuple, &tuple_hashtable[h_global]);
	spin_unlock_bh(&tuple_hash_lock);
}

#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

/* Copied from tcp_ipv4.c */
static inline __u32 tcp_v4_init_sequence(struct sk_buff *skb)
{
	return secure_tcp_sequence_number(ip_hdr(skb)->daddr,
					  ip_hdr(skb)->saddr,
					  tcp_hdr(skb)->dest,
					  tcp_hdr(skb)->source);
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)

/* Copied from tcp_ipv6.c */
static __u32 tcp_v6_init_sequence(struct sk_buff *skb)
{
	return secure_tcpv6_sequence_number(ipv6_hdr(skb)->daddr.s6_addr32,
					    ipv6_hdr(skb)->saddr.s6_addr32,
					    tcp_hdr(skb)->dest,
					    tcp_hdr(skb)->source);
}

#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

/* from tcp_v4_conn_request() */
static int mptcp_v4_join_request(struct multipath_pcb *mpcb,
		struct sk_buff *skb)
{
	struct inet_request_sock *ireq;
	struct request_sock *req;
	struct tcp_options_received tmp_opt;
	u8 *hash_location;
	__be32 saddr = ip_hdr(skb)->saddr;
	__be32 daddr = ip_hdr(skb)->daddr;
	__u32 isn = TCP_SKB_CB(skb)->when;

	req = inet_reqsk_alloc(&tcp_request_sock_ops);
	if (!req)
		return -1;

	tcp_clear_options(&tmp_opt);
	tmp_opt.mss_clamp = TCP_MSS_DEFAULT;
	tmp_opt.user_mss = tcp_sk(mpcb->master_sk)->rx_opt.user_mss;

	tcp_parse_options(skb, &tmp_opt, &hash_location,
			  &mpcb->received_options, 0);

	tmp_opt.tstamp_ok = tmp_opt.saw_tstamp;

	req->mpcb = mpcb;
	req->rem_id = tmp_opt.rem_id;
	req->mptcp_loc_token = loc_token(mpcb);
	req->mptcp_rem_token = tcp_sk(mpcb->master_sk)->rx_opt.mptcp_rem_token;
	tcp_openreq_init(req, &tmp_opt, skb);

	ireq = inet_rsk(req);
	ireq->loc_addr = daddr;
	ireq->rmt_addr = saddr;
	ireq->opt = tcp_v4_save_options(NULL, skb);

	/* Todo: add the sanity checks here. See tcp_v4_conn_request */

	isn = tcp_v4_init_sequence(skb);

	tcp_rsk(req)->snt_isn = isn;

	if (mptcp_v4_send_synack((struct sock *)mpcb, req, NULL))
		goto drop_and_free;

	/*Adding to request queue in metasocket */
	mptcp_v4_reqsk_queue_hash_add(req, TCP_TIMEOUT_INIT);
	return 0;

drop_and_free:
	reqsk_free(req);
	return -1;
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)

static int mptcp_v6_join_request(struct multipath_pcb *mpcb,
		struct sk_buff *skb)
{
	struct inet6_request_sock *treq;
	struct request_sock *req;
	struct tcp_options_received tmp_opt;
	struct in6_addr saddr;
	struct in6_addr daddr;
	u8 *hash_location;
	__u32 isn = TCP_SKB_CB(skb)->when;

	ipv6_addr_copy(&saddr, &ipv6_hdr(skb)->saddr);
	ipv6_addr_copy(&daddr, &ipv6_hdr(skb)->daddr);

	req = inet6_reqsk_alloc(&tcp6_request_sock_ops);
	if (!req)
		return -1;

	tcp_clear_options(&tmp_opt);
	tmp_opt.mss_clamp = 536;
	tmp_opt.user_mss  = tcp_sk(mpcb->master_sk)->rx_opt.user_mss;

	tcp_parse_options(skb, &tmp_opt, &hash_location,
				  &mpcb->received_options, 0);

	tmp_opt.tstamp_ok = tmp_opt.saw_tstamp;

	req->mpcb = mpcb;
	req->mptcp_loc_token = loc_token(mpcb);
	req->mptcp_rem_token = tcp_sk(mpcb->master_sk)->rx_opt.mptcp_rem_token;
	tcp_openreq_init(req, &tmp_opt, skb);

	treq = inet6_rsk(req);
	ipv6_addr_copy(&treq->loc_addr, &daddr);
	ipv6_addr_copy(&treq->rmt_addr, &saddr);

	atomic_inc(&skb->users);
	treq->pktopts = skb;

	/*Todo: add the sanity checks here. See tcp_v6_conn_request*/


	treq->iif = inet6_iif(skb);
	isn = tcp_v6_init_sequence(skb);

	tcp_rsk(req)->snt_isn = isn;

	if (mptcp_v6_send_synack((struct sock *)mpcb, req))
		goto drop_and_free;

	/*Adding to request queue in metasocket*/
	mptcp_v6_reqsk_queue_hash_add(req, TCP_TIMEOUT_INIT);
	return 0;

drop_and_free:
	if (req)
		reqsk_free(req);
	return -1;
}

#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
#define AF_INET_FAMILY(fam) ((fam) == AF_INET)
#define AF_INET6_FAMILY(fam) ((fam) == AF_INET6)
#else
#define AF_INET_FAMILY(fam) 1
#define AF_INET6_FAMILY(fam) 0
#endif

/**
 * Inspired from inet_csk_search_req
 * After this, the ref count of the master_sk associated with the request_sock
 * is incremented. Thus it is the responsibility of the caller
 * to call sock_put() when the reference is not needed anymore.
 */
static struct request_sock *mptcp_v4_search_req(const __be16 rport,
					    const __be32 raddr,
					    const __be32 laddr)
{
	struct request_sock *req;
	int found = 0;

	spin_lock(&tuple_hash_lock);
	list_for_each_entry(req,
			    &tuple_hashtable[inet_synq_hash
					     (raddr, rport, 0, MPTCP_HASH_SIZE)],
			    collide_tuple) {
		const struct inet_request_sock *ireq = inet_rsk(req);

		if (!req->collide_tuple.next) {
			printk(KERN_ERR
			       "tuple hashtable corrupted! (bug 66)\n");
			printk("bad node %pI4:%d->%pI4:%d\n", &ireq->loc_addr,
			       ntohs(ireq->loc_port), &ireq->rmt_addr,
			       ntohs(ireq->rmt_port));
			BUG();
		}

		if (ireq->rmt_port == rport &&
		    ireq->rmt_addr == raddr &&
		    ireq->loc_addr == laddr &&
		    AF_INET_FAMILY(req->rsk_ops->family)) {
			WARN_ON(req->sk);
			found = 1;
			break;
		}
	}

	if (found)
		sock_hold(req->mpcb->master_sk);
	spin_unlock(&tuple_hash_lock);

	if (!found)
		return NULL;

	return req;
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)

/*inspired from inet_csk_search_req
 * After this, the kref count of the mpcb associated with the request_sock
 * is incremented. Thus it is the responsibility of the caller
 * to call mpcb_put() when the reference is not needed anymore.
 */

static struct request_sock *mptcp_v6_search_req(const __be16 rport,
					const struct in6_addr *raddr,
					const struct in6_addr *laddr)
{
	struct request_sock *req;
	int found = 0;

	spin_lock(&tuple_hash_lock);
	list_for_each_entry(req, &tuple_hashtable[
				inet6_synq_hash(raddr, rport, 0,
				MPTCP_HASH_SIZE)],
				collide_tuple) {
		const struct inet6_request_sock *treq = inet6_rsk(req);

		if (inet_rsk(req)->rmt_port == rport &&
			AF_INET6_FAMILY(req->rsk_ops->family) &&
			ipv6_addr_equal(&treq->rmt_addr, raddr) &&
			ipv6_addr_equal(&treq->loc_addr, laddr)) {
			WARN_ON(req->sk);
			found = 1;
			break;
		}
	}

	if (found)
		sock_hold(req->mpcb->master_sk);
	spin_unlock(&tuple_hash_lock);

	if (!found)
		return NULL;

	return req;
}

#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

/*copied from net/ipv4/tcp_minisocks.c*/
static __inline__ int tcp_in_window(u32 seq, u32 end_seq, u32 s_win, u32 e_win)
{
	if (seq == s_win)
		return 1;
	if (after(end_seq, s_win) && before(seq, e_win))
		return 1;
	return (seq == e_win && seq == end_seq);
}

int mptcp_syn_recv_sock(struct sk_buff *skb)
{
	struct tcphdr *th = tcp_hdr(skb);
	struct request_sock *req = NULL;
	struct sock *meta_sk, *master_sk;

	if (skb->protocol == htons(ETH_P_IP))
		req = mptcp_v4_search_req(th->source, ip_hdr(skb)->saddr,
						ip_hdr(skb)->daddr);
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else /* IPv6 */
		req = mptcp_v6_search_req(th->source, &ipv6_hdr(skb)->saddr,
						&ipv6_hdr(skb)->daddr);
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

	if (!req)
		return 0;
	meta_sk = (struct sock *)req->mpcb;
	master_sk = req->mpcb->master_sk;
	bh_lock_sock(master_sk);
	if (sock_owned_by_user(master_sk)) {
		if (unlikely(sk_add_backlog(meta_sk, skb))) {
			bh_unlock_sock(master_sk);
			NET_INC_STATS_BH(dev_net(skb->dev),
					LINUX_MIB_TCPBACKLOGDROP);
			sock_put(master_sk); /* Taken by
							 * mptcp_search_req */
			kfree_skb(skb);
			return 1;
		}
	} else if (skb->protocol == htons(ETH_P_IP))
		tcp_v4_do_rcv(meta_sk, skb);
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else /* IPv6 */
		tcp_v6_do_rcv(meta_sk, skb);
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */
	bh_unlock_sock(master_sk);
	sock_put(master_sk); /* Taken by mptcp_search_req */
	return 1;
}

static struct mp_join *mptcp_find_join(struct sk_buff *skb)
{
	struct tcphdr *th = tcp_hdr(skb);
	unsigned char *ptr;
	int length = (th->doff * 4) - sizeof(struct tcphdr);

	/* Jump through the options to check whether JOIN is there */
	ptr = (unsigned char *)(th + 1);
	while (length > 0) {
		int opcode = *ptr++;
		int opsize;

		switch (opcode) {
		case TCPOPT_EOL:
			return 0;
		case TCPOPT_NOP:	/* Ref: RFC 793 section 3.1 */
			length--;
			continue;
		default:
			opsize = *ptr++;
			if (opsize < 2)	/* "silly options" */
				return NULL;
			if (opsize > length)
				return NULL;   /* don't parse partial options */
			if (opcode == TCPOPT_MPTCP) {
				struct mptcp_option *mp_opt = (struct mptcp_option *) ptr;

				if (mp_opt->sub == MPTCP_SUB_JOIN)
					return (struct mp_join *) ptr; /* + 2 to get the token */
			}
			ptr += opsize - 2;
			length -= opsize;
		}
	}
	return NULL;
}

int mptcp_lookup_join(struct sk_buff *skb)
{
	struct multipath_pcb *mpcb;
	struct sock *meta_sk;
	u32 token;
	struct mp_join *join_opt = mptcp_find_join(skb);
	if (!join_opt)
		return 0;

	join_opt++; /* the token is at the end of struct mp_join */
	token = ntohl(*(u32 *) join_opt);
	mpcb = mptcp_hash_find(token);
	meta_sk = (struct sock *)mpcb;
	if (!mpcb) {
		printk(KERN_ERR
			"%s:mpcb not found:%x\n",
			__func__, token);
		/* Sending "Required key not available" error message meaning
		 * "mpcb with this token does not exist".
		 */
		return -ENOKEY;
	}
	/* OK, this is a new syn/join, let's create a new open request and
	 * send syn+ack
	 */
	bh_lock_sock(mpcb->master_sk);
	if (sock_owned_by_user(mpcb->master_sk)) {
		if (unlikely(sk_add_backlog(meta_sk, skb))) {
			bh_unlock_sock(mpcb->master_sk);
			NET_INC_STATS_BH(dev_net(skb->dev),
					LINUX_MIB_TCPBACKLOGDROP);
			sock_put(mpcb->master_sk); /* Taken by mptcp_hash_find */
			kfree_skb(skb);
			return 1;
		}
	} else if (skb->protocol == htons(ETH_P_IP))
		tcp_v4_do_rcv(meta_sk, skb);
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else /* IPv6 */
		tcp_v6_do_rcv(meta_sk, skb);
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */
	bh_unlock_sock(mpcb->master_sk);
	sock_put(mpcb->master_sk); /* Taken by mptcp_hash_find */
	return 1;
}

/**
 * Sends an update notification to the MPS
 * Since this particular PM works in the TCP layer, that is, the same
 * as the MPS, we "send" the notif through function call, not message
 * passing.
 * Warning: this can be called only from user context, not soft irq
 **/
static void __mptcp_send_updatenotif(struct multipath_pcb *mpcb)
{
	int i;
	u32 path_indices = 1;	/* Path index 1 is reserved for master sk. */
	for (i = 0; i < mpcb->pa4_size; i++)
		path_indices |= PI_TO_FLAG(mpcb->pa4[i].path_index);

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	for (i = 0; i < mpcb->pa6_size; i++)
		path_indices |= PI_TO_FLAG(mpcb->pa6[i].path_index);
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */
	mptcp_init_subsockets(mpcb, path_indices);
}

static void mptcp_send_updatenotif_wq(struct work_struct *work)
{
	struct multipath_pcb *mpcb = *(struct multipath_pcb **)(work + 1);
	lock_sock(mpcb->master_sk);
	__mptcp_send_updatenotif(mpcb);
	release_sock(mpcb->master_sk);
	sock_put(mpcb->master_sk);
	kfree(work);
}

void mptcp_send_updatenotif(struct multipath_pcb *mpcb)
{
	if (in_interrupt()) {
		struct work_struct *work = kmalloc(sizeof(*work) +
						sizeof(struct multipath_pcb *),
						GFP_ATOMIC);
		struct multipath_pcb **mpcbp = (struct multipath_pcb **)
			(work + 1);
		*mpcbp = mpcb;
		sock_hold(mpcb->master_sk); /* Needed to ensure we can take
					     * the lock
					     */
		INIT_WORK(work, mptcp_send_updatenotif_wq);
		schedule_work(work);
	} else {
		__mptcp_send_updatenotif(mpcb);
	}
}

static void mptcp_subflow_attach(struct multipath_pcb *mpcb, struct sock *subsk)
{
	struct path4 *p4 = NULL;
	struct path6 *p6 = NULL;
	struct mptcp_loc4 loc, rem;
	struct mptcp_loc6 loc6, rem6;
	loc.id = inet_sk(subsk)->loc_id;
	loc.port = inet_sk(subsk)->inet_sport;
	rem.id = inet_sk(subsk)->rem_id;
	rem.port = inet_sk(subsk)->inet_dport;
	loc6.id = inet_sk(subsk)->loc_id;
	loc6.port = inet_sk(subsk)->inet_sport;
	rem6.id = inet_sk(subsk)->rem_id;
	rem6.port = inet_sk(subsk)->inet_dport;
	/* Apply correct path index to that subflow
	 * (we bypass the patharray if in multiports mode)
	 */
	if (sysctl_mptcp_ndiffports > 1)
		goto diffPorts;

	if (subsk->sk_family == AF_INET)
		p4 = find_path_mapping4(&loc, &rem, mpcb);
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	else
		p6 = find_path_mapping6(&loc6, &rem6, mpcb);
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

	if (!p4 && !p6) {
		/* It is possible that we don't find the mapping,
		 * if we have not yet updated our set of local
		 * addresses.
		 */
		mptcp_set_addresses(mpcb);

		/* If this added new local addresses, build new paths
		 * with them
		 */
		if (mpcb->num_addr4 || mpcb->num_addr6)
			mptcp_update_patharray(mpcb);


		if (subsk->sk_family == AF_INET)
			p4 = find_path_mapping4(&loc, &rem, mpcb);
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		else
			p6 = find_path_mapping6(&loc6, &rem6, mpcb);
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */
	}

	if (p4 || p6) {
		if (subsk->sk_family == AF_INET) {
			tcp_sk(subsk)->path_index = p4->path_index;
			p4->loc.sin_port = loc.port;
			p4->rem.sin_port = rem.port;
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
		} else {
			tcp_sk(subsk)->path_index = p6->path_index;
			p6->loc.sin6_port = loc6.port;
			p6->rem.sin6_port = rem6.port;
		}
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */
	} else {
diffPorts:
		tcp_sk(subsk)->path_index = mpcb->next_unused_pi++;
	}

	/* Point it to the same struct socket and wq as the master */
	sk_set_socket(subsk, mpcb->master_sk->sk_socket);
	subsk->sk_wq = mpcb->master_sk->sk_wq;

	mptcp_add_sock(mpcb, tcp_sk(subsk));
}

/**
 * Currently we can only process join requests here.
 * (either the SYN or the final ACK)
 */
int mptcp_v4_do_rcv(struct sock *meta_sk, struct sk_buff *skb)
{
	const struct iphdr *iph = ip_hdr(skb);
	struct multipath_pcb *mpcb = (struct multipath_pcb *)meta_sk;
	if (tcp_hdr(skb)->syn) {
		struct mp_join *join_opt = mptcp_find_join(skb); /* Currently we make two
						       * calls to
						       * mptcp_find_join(). This
						       * can probably be
						       * optimized.
						       */
		if (mptcp_v4_add_raddress (&mpcb->received_options,
				(struct in_addr *)&iph->saddr, 0,
				join_opt->addr_id) < 0)
			goto discard;
		if (unlikely(mpcb->received_options.list_rcvd)) {
			mpcb->received_options.list_rcvd = 0;
			mptcp_update_patharray(mpcb);
		}
		mptcp_v4_join_request(mpcb, skb);
	} else { /* ack processing */
		struct request_sock **prev;
		struct tcphdr *th = tcp_hdr(skb);
		struct sock *child;
		struct request_sock *req =
			inet_csk_search_req(meta_sk, &prev, th->source,
					iph->saddr, iph->daddr);
		if (!req)
			goto discard;
		child = tcp_check_req(meta_sk, skb, req, prev);
		if (!child)
			goto discard;
		if (child != meta_sk) {
			mptcp_subflow_attach(mpcb, child);
			tcp_child_process(meta_sk, child, skb);
		} else {
			req->rsk_ops->send_reset(NULL, skb);
			goto discard;
		}
		return 0;
	}
discard:
	kfree_skb(skb);
	return 0;
}

/**
 * Currently we can only process join requests here.
 * (either the SYN or the final ACK)
 */
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
int mptcp_v6_do_rcv(struct sock *meta_sk, struct sk_buff *skb)
{
	const struct ipv6hdr *iph = ipv6_hdr(skb);
	struct multipath_pcb *mpcb = (struct multipath_pcb *)meta_sk;
	if (tcp_hdr(skb)->syn) {
		struct mp_join *join_opt = mptcp_find_join(skb); /* Currently we make two
						       * calls to
						       * mptcp_find_join(). This
						       * can probably be
						       * optimized.
						       */
		if (mptcp_v6_add_raddress(&mpcb->received_options,
				(struct in6_addr *)&iph->saddr, 0,
				join_opt->addr_id) < 0)
			goto discard;
		if (unlikely(mpcb->received_options.list_rcvd)) {
			mpcb->received_options.list_rcvd = 0;
			mptcp_update_patharray(mpcb);
		}
		mptcp_v6_join_request(mpcb, skb);
	} else { /* ack processing */
		struct request_sock **prev;
		struct tcphdr *th = tcp_hdr(skb);
		struct sock *child;
		struct request_sock *req =
			inet6_csk_search_req(meta_sk, &prev, th->source,
					&iph->saddr, &iph->daddr, skb->skb_iif);
		if (!req)
			goto discard;
		child = tcp_check_req(meta_sk, skb, req, prev);
		if (!child)
			goto discard;
		if (child != meta_sk) {
			mptcp_subflow_attach(mpcb, child);
			tcp_child_process(meta_sk, child, skb);
		} else {
			req->rsk_ops->send_reset(NULL, skb);
			goto discard;
		}
		return 0;
	}
discard:
	kfree_skb(skb);
	return 0;
}
#endif /* CONFIG_IPV6 || CONFIG_IPV6_MODULE */

/**
 * Reacts on Interface up/down events - scans all existing connections and
 * flags/de-flags unavailable paths, so that they are not considered for packet
 * scheduling. This will save us a couple of RTOs and helps to migrate traffic
 * faster
 */
static int mptcp_pm_inetaddr_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	unsigned int i;
	struct multipath_pcb *mpcb;
	struct in_ifaddr *ifa = (struct in_ifaddr *) ptr;

	if (! (ifa->ifa_scope < RT_SCOPE_HOST) )  /* local */
		return NOTIFY_DONE;

	if (! (event == NETDEV_UP || event == NETDEV_DOWN) )
		return NOTIFY_DONE;

	read_lock_bh(&tk_hash_lock);

	/* go through all mpcbs and check */
	for (i = 0; i < MPTCP_HASH_SIZE; i++) {
		list_for_each_entry(mpcb, &tk_hashtable[i], collide_tk) {
			int found = 0;
			struct tcp_sock *tp;

			if (!tcp_sk(mpcb->master_sk)->mpc)
				continue;

			bh_lock_sock(mpcb->master_sk);

			/* do we have this address already ? */
			mptcp_for_each_tp(mpcb, tp) {
				if (tp->inet_conn.icsk_inet.inet_saddr != ifa->ifa_local)
					continue;

				found = 1;
				if (event == NETDEV_DOWN) {
					printk(KERN_DEBUG "MPTCP_PM: NETDEV_DOWN %x\n",
							ifa->ifa_local);
					tp->pf = 1;
				} else if (event == NETDEV_UP) {
					printk(KERN_DEBUG "MPTCP_PM: NETDEV_UP %x\n",
							ifa->ifa_local);
					tp->pf = 0;
				}
			}

			if (!found && event == NETDEV_UP) {
				if (mpcb->num_addr4 >= MPTCP_MAX_ADDR) {
					printk(KERN_DEBUG "MPTCP_PM: NETDEV_UP "
						"Reached max number of local IPv4 addresses: %d\n",
						MPTCP_MAX_ADDR);
					goto next;
				}

				printk(KERN_DEBUG "MPTCP_PM: NETDEV_UP adding "
					"address %pI4 to existing connection with mpcb: %d\n",
					&ifa->ifa_local, loc_token(mpcb));
				/* update this mpcb */
				mpcb->addr4[mpcb->num_addr4].addr.s_addr = ifa->ifa_local;
				mpcb->addr4[mpcb->num_addr4].id = mpcb->num_addr4 + 1;
				smp_wmb();
				mpcb->num_addr4++;
				/* re-send addresses */
				mpcb->addr4_unsent++;
				/* re-evaluate paths eventually */
				mpcb->received_options.list_rcvd = 1;
			}
next:
			bh_unlock_sock(mpcb->master_sk);
		}
	}

	read_unlock_bh(&tk_hash_lock);

	return NOTIFY_DONE;
}

/*
 *	Output /proc/net/mptcp_pm
 */
static int mptcp_pm_seq_show(struct seq_file *seq, void *v)
{
	struct multipath_pcb *mpcb;
	int i;

	seq_puts(seq, "Multipath TCP (path manager):");
	seq_putc(seq, '\n');

	for (i = 0; i < MPTCP_HASH_SIZE; i++) {
		read_lock_bh(&tk_hash_lock);
		list_for_each_entry(mpcb, &tk_hashtable[i], collide_tk) {
			seq_printf(seq, "[%d] %d (%d): %d", loc_token(mpcb),
				   mpcb->num_addr4, mpcb->pa4_size,
				   mpcb->cnt_subflows);
			seq_putc(seq, '\n');
		}
		read_unlock_bh(&tk_hash_lock);
	}

	return 0;
}

static int mptcp_pm_seq_open(struct inode *inode, struct file *file)
{
	return single_open_net(inode, file, mptcp_pm_seq_show);
}

static const struct file_operations mptcp_pm_seq_fops = {
	.owner = THIS_MODULE,
	.open = mptcp_pm_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release_net,
};

static __net_init int mptcp_pm_proc_init_net(struct net *net)
{
	if (!proc_net_fops_create(net, "mptcp_pm", S_IRUGO, &mptcp_pm_seq_fops))
		return -ENOMEM;

	return 0;
}

static __net_exit void mptcp_pm_proc_exit_net(struct net *net)
{
	proc_net_remove(net, "mptcp_pm");
}

static __net_initdata struct pernet_operations mptcp_pm_proc_ops = {
	.init = mptcp_pm_proc_init_net,
	.exit = mptcp_pm_proc_exit_net,
};

/* General initialization of MPTCP_PM
 */
static int __init mptcp_pm_init(void)
{
	int i;
	for (i = 0; i < MPTCP_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&tk_hashtable[i]);
		INIT_LIST_HEAD(&tuple_hashtable[i]);
	}

	rwlock_init(&tk_hash_lock);
	spin_lock_init(&tuple_hash_lock);

        /* setup notification chain for interfaces */
        register_inetaddr_notifier(&mptcp_pm_inetaddr_notifier);

	return register_pernet_subsys(&mptcp_pm_proc_ops);
}

module_init(mptcp_pm_init);

MODULE_LICENSE("GPL");
