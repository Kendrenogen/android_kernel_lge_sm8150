/*
 *	MTCP PM implementation
 *
 *	Authors:
 *      Sébastien Barré		<sebastien.barre@uclouvain.be>
 *
 *
 *
 *      date : March 10
 *
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <net/mtcp.h>
#include <net/mtcp_pm.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/list.h>
#include <linux/tcp.h>
#include <net/inet_sock.h>
#include <net/tcp.h>

#define MTCP_HASH_SIZE                16
#define hash_tk(token) \
	jhash_1word(token,0)%MTCP_HASH_SIZE


extern struct ip_options *tcp_v4_save_options(struct sock *sk,
					      struct sk_buff *skb);
extern void tcp_init_nondata_skb(struct sk_buff *skb, u32 seq, u8 flags);
extern void tcp_options_write(__be32 *ptr, struct tcp_sock *tp,
		       const struct tcp_out_options *opts,
		       __u8 **md5_hash);
extern void tcp_v4_send_ack(struct sk_buff *skb, u32 seq, u32 ack,
			    u32 win, u32 ts, int oif,
			    struct tcp_md5sig_key *key,
			    int reply_flags);


static struct list_head tk_hashtable[MTCP_HASH_SIZE];
static rwlock_t tk_hash_lock; /*hashtable protection*/

struct request_sock_queue mtcp_accept_queue; /*To handle incoming
					       syns+join*/


/* General initialization of MTCP_PM
 */
static int __init mtcp_pm_init(void) 
{
	int i;
	for (i=0;i<MTCP_HASH_SIZE;i++)
		INIT_LIST_HEAD(&tk_hashtable[i]);		
	rwlock_init(&tk_hash_lock);

	/*Init the accept_queue structure, we support a queue of 4 pending
	  connections, it does not need to be huge, since we only store 
	  here pending subflow creations*/
	reqsk_queue_alloc(&mtcp_accept_queue,32);	
	return 0;
}

static void __exit mtcp_pm_exit(void)
{
	/*Destroy the accept queue*/
	reqsk_queue_destroy(&mtcp_accept_queue);
}

void mtcp_hash_insert(struct multipath_pcb *mpcb,u32 token)
{
	int hash=hash_tk(token);
	write_lock_bh(&tk_hash_lock);
	list_add(&mpcb->collide_tk,&tk_hashtable[hash]);
	write_unlock_bh(&tk_hash_lock);
}

struct multipath_pcb* mtcp_hash_find(u32 token)
{
	int hash=hash_tk(token);
	struct multipath_pcb *mpcb;
	read_lock(&tk_hash_lock);
	list_for_each_entry(mpcb,&tk_hashtable[hash],collide_tk) {
		if (token==loc_token(mpcb)) {
			read_unlock(&tk_hash_lock);
			return mpcb;
		}
	}
	read_unlock(&tk_hash_lock);
	return NULL;
}

void mtcp_hash_remove(struct multipath_pcb *mpcb)
{
	/*Remove all request_socks pointing to this mpcb*/
	struct listen_sock *lopt = mtcp_accept_queue.listen_opt;

	if (lopt->qlen != 0) {
		unsigned int i;

		for (i = 0; i < lopt->nr_table_entries; i++) {
			struct request_sock **cur_ref;
			
			cur_ref = &lopt->syn_table[i];
			while (*cur_ref) {
				if ((*cur_ref)->mpcb==mpcb) {
					struct request_sock *todel;
					printk(KERN_ERR 
					       "Destroying request_sock\n");
					lopt->qlen--;
					todel=*cur_ref;
					*cur_ref=(*cur_ref)->dl_next;
					reqsk_free(todel);
				}
				else cur_ref=&(*cur_ref)->dl_next;
			}
		}
	}

	write_lock_bh(&tk_hash_lock);
	list_del(&mpcb->collide_tk);
	write_unlock_bh(&tk_hash_lock);
}


/* Generates a token for a new MPTCP connection
 * Currently we assign sequential tokens to
 * successive MPTCP connections. In the future we
 * will need to define random tokens, while avoiding
 * collisions.
 */
u32 mtcp_new_token(void)
{
	static u32 latest_token=0;
	latest_token++;
	return latest_token;
}



struct path4 *find_path_mapping4(struct in_addr *loc,struct in_addr *rem,
				 struct multipath_pcb *mpcb)
{
	int i;
	for (i=0;i<mpcb->pa4_size;i++)
		if (mpcb->pa4[i].loc.addr.s_addr == loc->s_addr &&
		    mpcb->pa4[i].rem.addr.s_addr == rem->s_addr)
			return &mpcb->pa4[i];
	return NULL;
}

struct in_addr *mtcp_get_loc_addr(struct multipath_pcb *mpcb, int path_index)
{
	int i;
 	if (path_index<=1)
		return (struct in_addr*)&mpcb->local_ulid.a4;
	for (i=0;i<mpcb->pa4_size;i++) {
		if (mpcb->pa4[i].path_index==path_index)
			return &mpcb->pa4[i].loc.addr;
	}
	BUG();
	return NULL;
}

struct in_addr *mtcp_get_rem_addr(struct multipath_pcb *mpcb, int path_index)
{
	int i;
 	if (path_index<=1)
		return (struct in_addr*)&mpcb->remote_ulid.a4;
	for (i=0;i<mpcb->pa4_size;i++) {
		if (mpcb->pa4[i].path_index==path_index)
			return &mpcb->pa4[i].rem.addr;
	}
	BUG();
	return NULL;
}

u8 mtcp_get_loc_addrid(struct multipath_pcb *mpcb, int path_index)
{
	int i;
	/*master subsocket has both addresses with id 0*/
	if (path_index<=1) return 0;
	for (i=0;i<mpcb->pa4_size;i++) {
		if (mpcb->pa4[i].path_index==path_index)
			return mpcb->pa4[i].loc.id;
	}
	BUG();
	return -1;
}


/*For debugging*/
void print_patharray(struct path4 *pa, int size)
{
	int i;
	printk(KERN_ERR "==================\n");
	for (i=0;i<size;i++) {
		printk(KERN_ERR NIPQUAD_FMT "/%d->"
		       NIPQUAD_FMT "/%d, pi %d\n",
		       NIPQUAD(pa[i].loc.addr),pa[i].loc.id,
		       NIPQUAD(pa[i].rem.addr),pa[i].rem.id,
		       pa[i].path_index);
	}
}



/*This is the MPTCP PM mapping table*/
void mtcp_update_patharray(struct multipath_pcb *mpcb)
{
	struct path4 *new_pa4, *old_pa4;
	int i,j,newpa_idx=0;
	/*Count how many paths are available
	  We add 1 to size of local and remote set, to include the 
	  ULID*/
	int ulid_v4=(mpcb->sa_family==AF_INET)?1:0;
	int pa4_size=(mpcb->num_addr4+ulid_v4)*
		(mpcb->received_options.num_addr4+ulid_v4)-ulid_v4;
	
	new_pa4=kmalloc(pa4_size*sizeof(struct path4),GFP_ATOMIC);
	
	if (ulid_v4) {
		/*ULID src with other dest*/
		for (j=0;j<mpcb->received_options.num_addr4;j++) {
			struct path4 *p=find_path_mapping4(
				(struct in_addr*)&mpcb->local_ulid.a4,
				&mpcb->received_options.addr4[j].addr,mpcb);
			if (p)
				memcpy(&new_pa4[newpa_idx++],p,
				       sizeof(struct path4));
			else {
				/*local addr*/
				new_pa4[newpa_idx].loc.addr.s_addr=
					mpcb->local_ulid.a4;
				new_pa4[newpa_idx].loc.id=0; /*ulid has id 0*/
				/*remote addr*/
				memcpy(&new_pa4[newpa_idx].rem,
				       &mpcb->received_options.addr4[j],
				       sizeof(struct mtcp_loc4));
				/*new path index to be given*/
				new_pa4[newpa_idx++].path_index=
					mpcb->next_unused_pi++;
			}			
		}
		/*ULID dest with other src*/
		for (i=0;i<mpcb->num_addr4;i++) {
			struct path4 *p=find_path_mapping4(
				&mpcb->addr4[i].addr,
				(struct in_addr*)&mpcb->remote_ulid.a4,mpcb);
			if (p)
				memcpy(&new_pa4[newpa_idx++],p,
				       sizeof(struct path4));
			else {
				/*local addr*/
				memcpy(&new_pa4[newpa_idx].loc,
				       &mpcb->addr4[i],
				       sizeof(struct mtcp_loc4));
				
				/*remote addr*/
				new_pa4[newpa_idx].rem.addr.s_addr=
					mpcb->remote_ulid.a4;
				new_pa4[newpa_idx].rem.id=0; /*ulid has id 0*/
				/*new path index to be given*/
				new_pa4[newpa_idx++].path_index=
					mpcb->next_unused_pi++;
			}
		}
	}
	/*Try all other combinations now*/
	for (i=0;i<mpcb->num_addr4;i++)
		for (j=0;j<mpcb->received_options.num_addr4;j++) {
			struct path4 *p=find_path_mapping4(
				&mpcb->addr4[i].addr,
				&mpcb->received_options.addr4[j].addr,mpcb);
			if (p)
				memcpy(&new_pa4[newpa_idx++],p,
				       sizeof(struct path4));	
			else {
				/*local addr*/
				memcpy(&new_pa4[newpa_idx].loc,
				       &mpcb->addr4[i],
				       sizeof(struct mtcp_loc4));
				/*remote addr*/
				memcpy(&new_pa4[newpa_idx].rem,
				       &mpcb->received_options.addr4[j],
				       sizeof(struct mtcp_loc4));
				
				/*new path index to be given*/
				new_pa4[newpa_idx++].path_index=
					mpcb->next_unused_pi++;
			}
		}
	
	
	/*Replacing the mapping table*/
	old_pa4=mpcb->pa4;
	mpcb->pa4=new_pa4;
	mpcb->pa4_size=pa4_size;
	if (old_pa4) kfree(old_pa4);
}


void mtcp_set_addresses(struct multipath_pcb *mpcb)
{
	struct net_device *dev;
	int id=1;

	mpcb->num_addr4=0;

	read_lock(&dev_base_lock); 

	for_each_netdev(&init_net,dev) {
		if(netif_running(dev)) {
			struct in_device *in_dev=dev->ip_ptr;
			struct in_ifaddr *ifa;
			
			if (!strcmp(dev->name,"lo"))
				continue;

			if (mpcb->num_addr4==MTCP_MAX_ADDR) {
				printk(KERN_ERR "Reached max number of local"
				       "IPv4 addresses : %d\n", MTCP_MAX_ADDR);
				break;
			}
			
			for (ifa = in_dev->ifa_list; ifa; 
			     ifa = ifa->ifa_next) {
				if (ifa->ifa_address==
				    inet_sk(mpcb->master_sk)->saddr)
					continue;
				mpcb->addr4[mpcb->num_addr4].addr.s_addr=
					ifa->ifa_address;
				mpcb->addr4[mpcb->num_addr4++].id=id++;
			}
		}
	}
	
	read_unlock(&dev_base_lock); 
}

/**
 * Based on function tcp_v4_conn_request (tcp_ipv4.c)
 * Returns -1 if there is no space anymore to store an additional 
 * address
 */
static int mtcp_v4_add_raddress(struct multipath_pcb *mpcb, 
				struct in_addr *addr, u8 id)
{
	int i;
	int num_addr4=mpcb->received_options.num_addr4;
	for (i=0;i<mpcb->received_options.num_addr4;i++) {
		if (mpcb->received_options.addr4[i].addr.s_addr==
		    addr->s_addr) {
			mpcb->received_options.addr4[i].id=id; /*update the 
								 id*/
			return 0;
		}
	}
	if (mpcb->received_options.num_addr4==MTCP_MAX_ADDR)
		return -1;

	/*Address is not known yet, store it*/
	mpcb->received_options.addr4[num_addr4].addr.s_addr=
		addr->s_addr;
	mpcb->received_options.addr4[num_addr4].id=id;
	mpcb->received_options.num_addr4++;
	return 0;
}


static struct dst_entry* mtcp_route_req(const struct request_sock *req)
{
	struct rtable *rt;
	const struct inet_request_sock *ireq = inet_rsk(req);
	struct ip_options *opt = inet_rsk(req)->opt;
	struct flowi fl = { .nl_u = { .ip4_u =
				      { .daddr = ((opt && opt->srr) ?
						  opt->faddr :
						  ireq->rmt_addr),
					.saddr = ireq->loc_addr } },
			    .proto = IPPROTO_TCP,
			    .flags = 0,
			    .uli_u = { .ports =
				       { .sport = ireq->loc_port,
					 .dport = ireq->rmt_port } } };
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
	return &rt->u.dst;
}

static unsigned mtcp_synack_options(struct request_sock *req,
				    unsigned mss, struct sk_buff *skb,
				    struct tcp_out_options *opts,
				    struct tcp_md5sig_key **md5)
{
	unsigned size = 0;
	struct inet_request_sock *ireq = inet_rsk(req);
	char doing_ts;

	*md5 = NULL;

	/* we can't fit any SACK blocks in a packet with MD5 + TS
	   options. There was discussion about disabling SACK rather than TS in
	   order to fit in better with old, buggy kernels, but that was deemed
	   to be unnecessary. */
	doing_ts = ireq->tstamp_ok && !(*md5 && ireq->sack_ok);

	opts->mss = mss;
	size += TCPOLEN_MSS_ALIGNED;

	if (likely(ireq->wscale_ok)) {
		opts->ws = ireq->rcv_wscale;
		if(likely(opts->ws))
			size += TCPOLEN_WSCALE_ALIGNED;
	}
	if (likely(doing_ts)) {
		opts->options |= OPTION_TS;
		opts->tsval = TCP_SKB_CB(skb)->when;
		opts->tsecr = req->ts_recent;
		size += TCPOLEN_TSTAMP_ALIGNED;
	}
	if (likely(ireq->sack_ok)) {
		opts->options |= OPTION_SACK_ADVERTISE;
		if (unlikely(!doing_ts))
			size += TCPOLEN_SACKPERM_ALIGNED;
	}

	return size;
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
static struct sk_buff *mtcp_make_synack(struct sock *master_sk, 
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
	__u8 *md5_hash_location;
	int mss;

	skb = alloc_skb(MAX_TCP_HEADER + 15, GFP_ATOMIC);
	if (skb == NULL)
		return NULL;

	/* Reserve space for headers. */
	skb_reserve(skb, MAX_TCP_HEADER);

	skb->dst = dst_clone(dst);

	mss = dst_metric(dst, RTAX_ADVMSS);
	if (master_tp->rx_opt.user_mss && master_tp->rx_opt.user_mss < mss)
		mss = master_tp->rx_opt.user_mss;

	if (req->rcv_wnd == 0) { /* ignored for retransmitted syns */
		__u8 rcv_wscale;
		/* Set this up on the first call only */
		req->window_clamp = dst_metric(dst, RTAX_WINDOW);
		/* tcp_full_space because it is guaranteed to be the first 
		   packet */
		tcp_select_initial_window(
			tcp_win_from_space(sysctl_rmem_default),
			mss - (ireq->tstamp_ok ? TCPOLEN_TSTAMP_ALIGNED : 0),
			&req->rcv_wnd,
			&req->window_clamp,
			ireq->wscale_ok,
			&rcv_wscale);
		ireq->rcv_wscale = rcv_wscale;
	}

	memset(&opts, 0, sizeof(opts));

	TCP_SKB_CB(skb)->when = tcp_time_stamp;
	tcp_header_size = mtcp_synack_options(req, mss,
					      skb, &opts, &md5) +
		sizeof(struct tcphdr);       
	
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
			     TCPCB_FLAG_SYN | TCPCB_FLAG_ACK);
	th->seq = htonl(TCP_SKB_CB(skb)->seq);
	th->ack_seq = htonl(tcp_rsk(req)->rcv_isn + 1);
	
	/* RFC1323: The window in SYN & SYN/ACK segments is never scaled. */
	th->window = htons(min(req->rcv_wnd, 65535U));
	tcp_options_write((__be32 *)(th + 1), NULL, &opts, &md5_hash_location);
	th->doff = (tcp_header_size >> 2);

	return skb;
}

/*
 *	Send a SYN-ACK after having received a SYN.
 *	This still operates on a request_sock only, not on a big
 *	socket.
 */
static int __mtcp_v4_send_synack(struct sock *master_sk,
				 struct request_sock *req,
				 struct dst_entry *dst)
{
	const struct inet_request_sock *ireq = inet_rsk(req);
	int err = -1;
	struct sk_buff * skb;

	/* First, grab a route. */
	if (!dst && (dst = mtcp_route_req(req)) == NULL)
		return -1;

	skb = mtcp_make_synack(master_sk, dst, req);

	if (skb) {
		struct tcphdr *th = tcp_hdr(skb);

		th->check = tcp_v4_check(skb->len,
					 ireq->loc_addr,
					 ireq->rmt_addr,
					 csum_partial((char *)th, skb->len,
						      skb->csum));

		err = ip_build_and_send_pkt(skb, master_sk, ireq->loc_addr,
					    ireq->rmt_addr,
					    ireq->opt);
		err = net_xmit_eval(err);
	}

	dst_release(dst);
	return err;
}

/*Copied from net/ipv4/inet_connection_sock.c*/
static inline u32 inet_synq_hash(const __be32 raddr, const __be16 rport,
				 const u32 rnd, const u32 synq_hsize)
{
	return jhash_2words((__force u32)raddr, (__force u32)rport, rnd) & (synq_hsize - 1);
}

static void mtcp_reqsk_queue_hash_add(struct request_sock *req,
				      unsigned long timeout)
{
	struct listen_sock *lopt = mtcp_accept_queue.listen_opt;
	const u32 h = inet_synq_hash(inet_rsk(req)->rmt_addr, 
				     inet_rsk(req)->rmt_port,
				     lopt->hash_rnd, lopt->nr_table_entries);

	reqsk_queue_hash_req(&mtcp_accept_queue, h, req, timeout);
}

/*Copied from tcp_ipv4.c*/
static inline __u32 tcp_v4_init_sequence(struct sk_buff *skb)
{
	return secure_tcp_sequence_number(ip_hdr(skb)->daddr,
					  ip_hdr(skb)->saddr,
					  tcp_hdr(skb)->dest,
					  tcp_hdr(skb)->source);
}

static int mtcp_v4_join_request(struct multipath_pcb *mpcb, struct sk_buff *skb)
{
	struct inet_request_sock *ireq;
	struct request_sock *req;
	struct tcp_options_received tmp_opt;
	__be32 saddr = ip_hdr(skb)->saddr;
	__be32 daddr = ip_hdr(skb)->daddr;
	__u32 isn = TCP_SKB_CB(skb)->when;	

	req = inet_reqsk_alloc(mpcb->master_sk->sk_prot->rsk_prot);
	if (!req)
		return -1;
	
	tcp_clear_options(&tmp_opt);
	tmp_opt.mss_clamp = 536;
	tmp_opt.user_mss  = tcp_sk(mpcb->master_sk)->rx_opt.user_mss;
	
	tcp_parse_options(skb, &tmp_opt, &mpcb->received_options, 0);
	
	if (tmp_opt.saw_tstamp && !tmp_opt.rcv_tsval) {
		/* Some OSes (unknown ones, but I see them on web server, which
		 * contains information interesting only for windows'
		 * users) do not send their stamp in SYN. It is easy case.
		 * We simply do not advertise TS support.
		 */
		tmp_opt.saw_tstamp = 0;
		tmp_opt.tstamp_ok  = 0;
	}
	tmp_opt.tstamp_ok = tmp_opt.saw_tstamp;
	tcp_openreq_init(req, &tmp_opt, skb);

	ireq = inet_rsk(req);
	ireq->loc_addr = daddr;
	ireq->rmt_addr = saddr;
	ireq->opt = tcp_v4_save_options(NULL, skb);

	req->mpcb=mpcb;

	/*Todo: add the sanity checks here. See tcp_v4_conn_request*/

	isn = tcp_v4_init_sequence(skb);

	tcp_rsk(req)->snt_isn = isn;

 	if (__mtcp_v4_send_synack(mpcb->master_sk, req, NULL))
		goto drop_and_free;

	/*Adding to request queue in metasocket*/
	mtcp_reqsk_queue_hash_add(req, TCP_TIMEOUT_INIT);
	return 0;

drop_and_free:
	reqsk_free(req);
	return -1;
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
#define AF_INET_FAMILY(fam) ((fam) == AF_INET)
#else
#define AF_INET_FAMILY(fam) 1
#endif
/*inspired from inet_csk_search_req*/
static struct request_sock *mtcp_search_req(struct request_sock ***prevp,
					    const __be16 rport, 
					    const __be32 raddr,
					    const __be32 laddr)
{
	struct listen_sock *lopt = mtcp_accept_queue.listen_opt;
	struct request_sock *req, **prev;

	for (prev = &lopt->syn_table[inet_synq_hash(raddr, rport, 
						    lopt->hash_rnd,
						    lopt->nr_table_entries)];
	     (req = *prev) != NULL;
	     prev = &req->dl_next) {
		const struct inet_request_sock *ireq = inet_rsk(req);
		
		if (ireq->rmt_port == rport &&
		    ireq->rmt_addr == raddr &&
		    ireq->loc_addr == laddr &&
		    AF_INET_FAMILY(req->rsk_ops->family)) {
			WARN_ON(req->sk);
			if (prevp) *prevp = prev;
			break;
		}
	}
	
	return req;
}

/*copied from net/ipv4/tcp_minisocks.c*/
static __inline__ int tcp_in_window(u32 seq, u32 end_seq, u32 s_win, u32 e_win)
{
	if (seq == s_win)
		return 1;
	if (after(end_seq, s_win) && before(seq, e_win))
		return 1;
	return (seq == e_win && seq == end_seq);
}

static inline void mtcp_reqsk_queue_add(struct request_sock_queue *queue,
					struct request_sock *req,
					struct sock *child)
{
	req->sk = child;

	if (queue->rskq_accept_head == NULL)
		queue->rskq_accept_head = req;
	else
		queue->rskq_accept_tail->dl_next = req;

	queue->rskq_accept_tail = req;
	req->dl_next = NULL;
}

/*
 *      (adapted from tcp_check_req)    
 *	Process an incoming packet for SYN_RECV sockets represented
 *	as a request_sock.
 */

static struct sock *mtcp_check_req(struct sk_buff *skb,
				   struct request_sock *req,
				   struct request_sock **prev)
{
	const struct tcphdr *th = tcp_hdr(skb);
	__be32 flg = tcp_flag_word(th) & 
		(TCP_FLAG_RST|TCP_FLAG_SYN|TCP_FLAG_ACK);
	int paws_reject = 0;
	struct tcp_options_received tmp_opt;
	struct multipath_options mtp;
	struct sock *child;
	struct multipath_pcb *mpcb=req->mpcb;

	tmp_opt.saw_tstamp = 0;
	if (th->doff > (sizeof(struct tcphdr)>>2)) {
		tcp_parse_options(skb, &tmp_opt, &mtp, 0);

		if (tmp_opt.saw_tstamp) {
			tmp_opt.ts_recent = req->ts_recent;
			/* We do not store true stamp, but it is not required,
			 * it can be estimated (approximately)
			 * from another data.
			 */
			tmp_opt.ts_recent_stamp = get_seconds() - ((TCP_TIMEOUT_INIT/HZ)<<req->retrans);
			paws_reject = tcp_paws_check(&tmp_opt, th->rst);
		}
	}

	/* Check for pure retransmitted SYN. */
	if (TCP_SKB_CB(skb)->seq == tcp_rsk(req)->rcv_isn &&
	    flg == TCP_FLAG_SYN &&
	    !paws_reject) {
		/*
		 * RFC793 draws (Incorrectly! It was fixed in RFC1122)
		 * this case on figure 6 and figure 8, but formal
		 * protocol description says NOTHING.
		 * To be more exact, it says that we should send ACK,
		 * because this segment (at least, if it has no data)
		 * is out of window.
		 *
		 *  CONCLUSION: RFC793 (even with RFC1122) DOES NOT
		 *  describe SYN-RECV state. All the description
		 *  is wrong, we cannot believe to it and should
		 *  rely only on common sense and implementation
		 *  experience.
		 *
		 * Enforce "SYN-ACK" according to figure 8, figure 6
		 * of RFC793, fixed by RFC1122.
		 */
		__mtcp_v4_send_synack(mpcb->master_sk, req, NULL);
		return NULL;
	}

	/* Further reproduces section "SEGMENT ARRIVES"
	   for state SYN-RECEIVED of RFC793.
	   It is broken, however, it does not work only
	   when SYNs are crossed.

	   You would think that SYN crossing is impossible here, since
	   we should have a SYN_SENT socket (from connect()) on our end,
	   but this is not true if the crossed SYNs were sent to both
	   ends by a malicious third party.  We must defend against this,
	   and to do that we first verify the ACK (as per RFC793, page
	   36) and reset if it is invalid.  Is this a true full defense?
	   To convince ourselves, let us consider a way in which the ACK
	   test can still pass in this 'malicious crossed SYNs' case.
	   Malicious sender sends identical SYNs (and thus identical sequence
	   numbers) to both A and B:

		A: gets SYN, seq=7
		B: gets SYN, seq=7

	   By our good fortune, both A and B select the same initial
	   send sequence number of seven :-)

		A: sends SYN|ACK, seq=7, ack_seq=8
		B: sends SYN|ACK, seq=7, ack_seq=8

	   So we are now A eating this SYN|ACK, ACK test passes.  So
	   does sequence test, SYN is truncated, and thus we consider
	   it a bare ACK.

	   If icsk->icsk_accept_queue.rskq_defer_accept, we silently drop this
	   bare ACK.  Otherwise, we create an established connection.  Both
	   ends (listening sockets) accept the new incoming connection and try
	   to talk to each other. 8-)

	   Note: This case is both harmless, and rare.  Possibility is about the
	   same as us discovering intelligent life on another plant tomorrow.

	   But generally, we should (RFC lies!) to accept ACK
	   from SYNACK both here and in tcp_rcv_state_process().
	   tcp_rcv_state_process() does not, hence, we do not too.

	   Note that the case is absolutely generic:
	   we cannot optimize anything here without
	   violating protocol. All the checks must be made
	   before attempt to create socket.
	 */

	/* RFC793 page 36: "If the connection is in any non-synchronized 
	 *                  state ... and the incoming segment acknowledges 
	 *                  something not yet sent (the segment carries an 
	 *                  unacceptable ACK) ... a reset is sent."
	 *
	 * Invalid ACK: reset will be sent by listening socket
	 */
	if ((flg & TCP_FLAG_ACK) &&
	    (TCP_SKB_CB(skb)->ack_seq != tcp_rsk(req)->snt_isn + 1))
		req->rsk_ops->send_reset(NULL,skb);

	/* Also, it would be not so bad idea to check rcv_tsecr, which
	 * is essentially ACK extension and too early or too late values
	 * should cause reset in unsynchronized states.
	 */

	/* RFC793: "first check sequence number". */

	if (paws_reject || !tcp_in_window(TCP_SKB_CB(skb)->seq, TCP_SKB_CB(skb)->end_seq,
					  tcp_rsk(req)->rcv_isn + 1, tcp_rsk(req)->rcv_isn + 1 + req->rcv_wnd)) {
		/* Out of window: send ACK and drop. */
		if (!(flg & TCP_FLAG_RST))
			tcp_v4_send_ack(skb, tcp_rsk(req)->snt_isn + 1,
					tcp_rsk(req)->rcv_isn + 1, req->rcv_wnd,
					req->ts_recent, 0, NULL,
					inet_rsk(req)->no_srccheck ? 
					IP_REPLY_ARG_NOSRCCHECK : 0);
		return NULL;
	}

	/* In sequence, PAWS is OK. */

	if (tmp_opt.saw_tstamp && !after(TCP_SKB_CB(skb)->seq, tcp_rsk(req)->rcv_isn + 1))
		req->ts_recent = tmp_opt.rcv_tsval;

	if (TCP_SKB_CB(skb)->seq == tcp_rsk(req)->rcv_isn) {
		/* Truncate SYN, it is out of window starting
		   at tcp_rsk(req)->rcv_isn + 1. */
		flg &= ~TCP_FLAG_SYN;
	}

	/* RFC793: "second check the RST bit" and
	 *	   "fourth, check the SYN bit"
	 */
	if (flg & (TCP_FLAG_RST|TCP_FLAG_SYN))
		goto embryonic_reset;

	/* ACK sequence verified above, just make sure ACK is
	 * set.  If ACK not set, just silently drop the packet.
	 */
	if (!(flg & TCP_FLAG_ACK))
		return NULL;

	/* OK, ACK is valid, create big socket and
	 * feed this segment to it. It will repeat all
	 * the tests. THIS SEGMENT MUST MOVE SOCKET TO
	 * ESTABLISHED STATE. If it will be dropped after
	 * socket is created, wait for troubles.
	 */
	child = inet_csk(mpcb->master_sk)->icsk_af_ops->
		syn_recv_sock(mpcb->master_sk, 
			      skb, req, 
			      NULL);

	if (child == NULL)
		goto listen_overflow;

	/*The child is a clone of the master socket, we must now reset
	  some of the fields*/
	tcp_sk(child)->bytes_eaten=0;

	tcp_sk(child)->mpc=1;
	tcp_sk(child)->rx_opt.mtcp_rem_token=req->mtcp_rem_token;
	tcp_sk(child)->mtcp_loc_token=req->mtcp_loc_token;
	
	reqsk_queue_unlink(&mtcp_accept_queue, req, prev);
	reqsk_queue_removed(&mtcp_accept_queue, req);
	mtcp_reqsk_queue_add(&mtcp_accept_queue, req, child);
	return child;
	
listen_overflow:
	if (!sysctl_tcp_abort_on_overflow) {
		inet_rsk(req)->acked = 1;
		return NULL;
	}
	
embryonic_reset:
	if (!(flg & TCP_FLAG_RST))
		req->rsk_ops->send_reset(NULL, skb);

	reqsk_queue_unlink(&mtcp_accept_queue, req, prev);
	reqsk_queue_removed(&mtcp_accept_queue, req);
	reqsk_free(req);
	return NULL;
}

int mtcp_syn_recv_sock(struct sk_buff *skb)
{
	struct tcphdr *th=tcp_hdr(skb);
	struct iphdr *iph=ip_hdr(skb);
	struct request_sock **prev;
	struct request_sock *req=mtcp_search_req(
		&prev,th->source,iph->saddr,iph->daddr);
	struct sock *child;
	if (!req) return 0;

	/*If this is a valid ack, we can build a full socket*/
	child=mtcp_check_req(skb,req,prev);
	if (child)
		tcp_child_process(req->mpcb->master_sk,
				  child,skb);
	return 1;
}

/**
 *
 * Returns 1 if a join option has been found, and a new request_sock has been 
 * created. Else returns 0.
 */
int mtcp_lookup_join(struct sk_buff *skb)
{
	struct tcphdr *th=tcp_hdr(skb);
	const struct iphdr *iph = ip_hdr(skb);
	unsigned char *ptr;
	int length = (th->doff * 4) - sizeof(struct tcphdr);
	u32 token;
	struct multipath_pcb *mpcb;
	int ans;

	/*Jump through the options to check whether JOIN is there*/
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
			if (opsize < 2) /* "silly options" */
				return 0;
			if (opsize > length)
				return 0; /* don't parse partial options */
			if (opcode==TCPOPT_JOIN) {
				token=ntohl(*(u32*)ptr);
				mpcb=mtcp_hash_find(token);			
				if (!mpcb) {
					printk(KERN_ERR 
					       "%s:mpcb not found:%x\n",
					       __FUNCTION__,token);
					return 0;
				}
				/*OK, this is a new syn/join, let's 
				  create a new open request and 
				  send syn+ack*/
				ans=mtcp_v4_add_raddress(mpcb, 
							 (struct in_addr*)
							 &iph->saddr, *(ptr+4));
				if (ans<0) goto finished;
				mtcp_v4_join_request(mpcb, skb);		
				goto finished;
			}
			ptr += opsize-2;
			length -= opsize;
		}
	}

	return 0;
finished:
	kfree_skb(skb);
	return 1;
}

/*checks whether a new established subflow has appeared,
  in which case that subflow is added to the path set. This should
  be run by a control daemon in the future*/
void mtcp_check_new_subflow(void)
{
	struct sock *child;
	struct request_sock *req;
	struct inet_request_sock *ireq;
	struct path4 *p;
	while (!reqsk_queue_empty(&mtcp_accept_queue)) {
		req = reqsk_queue_remove(&mtcp_accept_queue);
		ireq =  inet_rsk(req);
		child = req->sk;
		BUG_ON(!child);
		mtcp_update_patharray(req->mpcb);
		/*Apply correct path index to that subflow*/
		p=find_path_mapping4((struct in_addr*)&ireq->loc_addr,
				     (struct in_addr*)&ireq->rmt_addr,
				     req->mpcb);
		BUG_ON(!p);
		tcp_sk(child)->path_index=p->path_index;
		/*Point it to the same struct socket as the master*/
		sk_set_socket(child,req->mpcb->master_sk->sk_socket);
		
		mtcp_add_sock(req->mpcb,tcp_sk(child));
		__reqsk_free(req);
	}
}

/**
 *Sends an update notification to the MPS
 *Since this particular PM works in the TCP layer, that is, the same
 *as the MPS, we "send" the notif through function call, not message
 *passing.
 * Warning: this can be called only from user context, not soft irq
 **/
void mtcp_send_updatenotif(struct multipath_pcb *mpcb)
{
	int i;
	u32 path_indices=1; /*Path index 1 is reserved for master sk.*/
	for (i=0;i<mpcb->pa4_size;i++) {
		path_indices|=PI_TO_FLAG(mpcb->pa4[i].path_index);
	}
	mtcp_init_subsockets(mpcb,path_indices);
}

module_init(mtcp_pm_init);

MODULE_LICENSE("GPL");

