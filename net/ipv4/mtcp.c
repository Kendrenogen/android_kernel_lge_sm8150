/*
 *	MTCP implementation
 *
 *	Authors:
 *      Sébastien Barré		<sebastien.barre@uclouvain.be>
 *
 *      Partially inspired from initial user space MTCP stack by Costin Raiciu.
 *
 *      date : June 09
 *
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */


#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/mtcp.h>
#include <net/netevent.h>
#include <net/ipv6.h>
#include <net/tcp.h>
#include <linux/list.h>
#include <linux/jhash.h>
#include <linux/tcp.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/random.h>
#include <asm/atomic.h>

inline void mtcp_reset_options(struct multipath_options* mopt){
#ifdef CONFIG_MTCP_PM
	mopt->remote_token = -1;
	mopt->local_token = -1;
	if (mopt->ip_count>0){
		if (mopt->ip_list){
			mopt->ip_list = NULL;
		}
	}
	mopt->ip_count = 0;
	mopt->first = 0;
#endif
}

/**
 * Creates as many sockets as path indices announced by the Path Manager.
 * The first path indices are (re)allocated to existing sockets.
 * New sockets are created if needed.
 *
 * WARNING: We make the assumption that this function is run in user context
 *      (we use sock_create_kern, that reserves ressources with GFP_KERNEL)
 *      AND only one user process can trigger the sending of a PATH_UPDATE
 *      notification. This is in conformance with the fact that only one PM
 *      can send messages to the MPS, according to our multipath arch.
 *      (further PMs are cascaded and use the depth attribute).
 */
static int mtcp_init_subsockets(struct multipath_pcb *mpcb, 
				uint32_t path_indices)
{
	int i;
	int retval;
	struct socket *sock;
	struct tcp_sock *tp=mpcb->connection_list;
	struct tcp_sock *newtp;

	PDEBUG("Entering %s, path_indices:%x\n",__FUNCTION__,path_indices);
	for (i=0;i<sizeof(path_indices)*8;i++) {
		if (!((1<<i) & path_indices))
			continue;
		if (tp) {
			/*realloc path index*/
			tp->path_index=i+1;
			tp=tp->next;
		}
		else {
			struct sockaddr *loculid,*remulid;
			int ulid_size;
			struct sockaddr_in loculid_in,remulid_in;
			struct sockaddr_in6 loculid_in6,remulid_in6;
			/*a new socket must be created*/
			retval = sock_create_kern(mpcb->sa_family, SOCK_STREAM, 
						  IPPROTO_MTCPSUB, &sock);
			
			if (retval<0) {
				printk(KERN_ERR "%s:sock_create failed\n",
				       __FUNCTION__);
				return retval;
			}
			newtp=tcp_sk(sock->sk);

			/*Binding the new socket to the local ulid*/
			switch(mpcb->sa_family) {
			case AF_INET:
				memset(&loculid,0,sizeof(loculid));
				loculid_in.sin_family=mpcb->sa_family;
				
				memcpy(&remulid_in,&loculid_in,
				       sizeof(remulid_in));
				
				loculid_in.sin_port=mpcb->local_port;
				remulid_in.sin_port=mpcb->remote_port;
				memcpy(&loculid_in.sin_addr,
				       (struct in_addr*)&mpcb->local_ulid.a4,
				       sizeof(struct in_addr));
				memcpy(&remulid_in.sin_addr,
				       (struct in_addr*)&mpcb->remote_ulid.a4,
				       sizeof(struct in_addr));
				loculid=(struct sockaddr *)&loculid_in;
				remulid=(struct sockaddr *)&remulid_in;
				ulid_size=sizeof(loculid_in);
				break;
			case AF_INET6:
				memset(&loculid,0,sizeof(loculid));
				loculid_in6.sin6_family=mpcb->sa_family;
				
				memcpy(&remulid_in6,&loculid_in6,
				       sizeof(remulid_in6));

				loculid_in6.sin6_port=mpcb->local_port;
				remulid_in6.sin6_port=mpcb->remote_port;
				ipv6_addr_copy(&loculid_in6.sin6_addr,
					       (struct in6_addr*)&mpcb->
					       local_ulid.a6);
				ipv6_addr_copy(&remulid_in6.sin6_addr,
					       (struct in6_addr*)&mpcb->
					       remote_ulid.a6);
				
				loculid=(struct sockaddr *)&loculid_in6;
				remulid=(struct sockaddr *)&remulid_in6;
				ulid_size=sizeof(loculid_in6);
				break;
			}
			newtp->path_index=i+1;
			newtp->mpcb = mpcb;
			newtp->mtcp_flags=0;
			mtcp_add_sock(mpcb,newtp);			
       
			retval = sock->ops->bind(sock, loculid, ulid_size);
			if (retval<0) goto fail_bind;
			
			retval = sock->ops->connect(sock,remulid,
						    ulid_size,0);
			if (retval<0) goto fail_connect;
						
			PDEBUG("New MTCP subsocket created, pi %d\n",i+1);
		}
	}
	return 0;
fail_bind:
	printk(KERN_ERR "MTCP subsocket bind() failed\n");
fail_connect:
	printk(KERN_ERR "MTCP subsocket connect() failed\n");
	mtcp_del_sock(mpcb,newtp);
	sock_release(sock);
	return -1;
}

static int netevent_callback(struct notifier_block *self, unsigned long event,
			     void *ctx)
{	
	struct multipath_pcb *mpcb;
	struct ulid_pair *up;
	PDEBUG("Received path update event\n");
	switch(event) {
	case NETEVENT_PATH_UPDATEV6:
		mpcb=container_of(self,struct multipath_pcb,nb);
		up=ctx;
		PDEBUG("mpcb is %p\n",mpcb);
		if (mpcb->sa_family!=AF_INET6) break;
		
		PDEBUG("ev loc ulid:" NIP6_FMT "\n",NIP6(*up->local));
		PDEBUG("ev loc ulid:" NIP6_FMT "\n",NIP6(*up->remote));
		PDEBUG("ev loc ulid:" NIP6_FMT "\n",NIP6(*(struct in6_addr*)mpcb->local_ulid.a6));
		PDEBUG("ev loc ulid:" NIP6_FMT "\n",NIP6(*(struct in6_addr*)mpcb->remote_ulid.a6));
		if (ipv6_addr_equal(up->local,
				    (struct in6_addr*)&mpcb->local_ulid) &&
		    ipv6_addr_equal(up->remote,
				    (struct in6_addr*)&mpcb->remote_ulid))
			mtcp_init_subsockets(mpcb,
					     up->path_indices);
		break;
        }
        return 0;
}

struct multipath_pcb* mtcp_alloc_mpcb(struct sock *master_sk)
{
	struct multipath_pcb * mpcb = kmalloc(
		sizeof(struct multipath_pcb),GFP_KERNEL);
	
	memset(mpcb,0,sizeof(struct multipath_pcb));
	
	skb_queue_head_init(&mpcb->receive_queue);
	skb_queue_head_init(&mpcb->write_queue);
	skb_queue_head_init(&mpcb->retransmit_queue);
	skb_queue_head_init(&mpcb->error_queue);
	skb_queue_head_init(&mpcb->out_of_order_queue);
	
	mpcb->rcvbuf = sysctl_rmem_default;
	mpcb->sndbuf = sysctl_wmem_default;
	
	mpcb->state = TCPF_CLOSE;

	mpcb->master_sk=master_sk;

	kref_init(&mpcb->kref);
	spin_lock_init(&mpcb->lock);
	
	mpcb->nb.notifier_call=netevent_callback;
	register_netevent_notifier(&mpcb->nb);
	
	return mpcb;
}

static void mpcb_release(struct kref* kref)
{
	struct multipath_pcb *mpcb;
	mpcb=container_of(kref,struct multipath_pcb,kref);
	printk(KERN_ERR "about to kfree\n");
	kfree(mpcb);
}

/*Warning: can only be called in user conext
  (due to unregister_netevent_notifier)*/
void mtcp_destroy_mpcb(struct multipath_pcb *mpcb)
{
	unregister_netevent_notifier(&mpcb->nb);
	kref_put(&mpcb->kref,mpcb_release);
}

void mtcp_add_sock(struct multipath_pcb *mpcb,struct tcp_sock *tp)
{
	/*Adding new node to head of connection_list*/
	spin_lock_bh(&mpcb->lock);
	tp->mpcb = mpcb;
	tp->next=mpcb->connection_list;
	mpcb->connection_list=tp;
	
	mpcb->cnt_subflows++;
	kref_get(&mpcb->kref);	
	spin_unlock_bh(&mpcb->lock);
	printk(KERN_ERR "Added subsocket, cnt_subflows now %d\n",
	       mpcb->cnt_subflows);
}

void mtcp_del_sock(struct multipath_pcb *mpcb, struct tcp_sock *tp)
{
	struct tcp_sock *tp_prev=mpcb->connection_list;
	int done=0;
	spin_lock_bh(&mpcb->lock);
	if (tp_prev==tp) {
		mpcb->connection_list=tp->next;
		mpcb->cnt_subflows--;
		done=1;
	}
	else for (;tp_prev && tp_prev->next;tp_prev=tp_prev->next) {
			if (tp_prev->next==tp) {
				tp_prev->next=tp->next;
				mpcb->cnt_subflows--;
				done=1;
				break;
			}
		}
	spin_unlock_bh(&mpcb->lock);
	tp->mpcb=NULL; tp->next=NULL;
	kref_put(&mpcb->kref,mpcb_release);
	BUG_ON(!done);
}

/**
 * Updates the metasocket ULID/port data, based on the given sock.
 * The argument sock must be the sock accessible to the application.
 * In this function, we update the meta socket info, based on the changes 
 * in the application socket (bind, address allocation, ...)
 */
void mtcp_update_metasocket(struct sock *sk)
{
	struct tcp_sock *tp;
	struct multipath_pcb *mpcb;
	if (sk->sk_protocol != IPPROTO_TCP) return;
	tp=tcp_sk(sk);
	mpcb=mpcb_from_tcpsock(tp);

	PDEBUG("Entering %s, mpcb %p\n",__FUNCTION__,mpcb);

	mpcb->sa_family=sk->sk_family;
	mpcb->remote_port=inet_sk(sk)->dport;
	mpcb->local_port=inet_sk(sk)->sport;
	
	switch (sk->sk_family) {
	case AF_INET:
		mpcb->remote_ulid.a4=inet_sk(sk)->daddr;
		mpcb->local_ulid.a4=inet_sk(sk)->saddr;
		break;
	case AF_INET6:
		ipv6_addr_copy((struct in6_addr*)&mpcb->remote_ulid,
			       &inet6_sk(sk)->daddr);
		ipv6_addr_copy((struct in6_addr*)&mpcb->local_ulid,
			       &inet6_sk(sk)->saddr);

		PDEBUG("mum loc ulid:" NIP6_FMT "\n",NIP6(*(struct in6_addr*)mpcb->local_ulid.a6));
		PDEBUG("mum loc ulid:" NIP6_FMT "\n",NIP6(*(struct in6_addr*)mpcb->remote_ulid.a6));

		break;
	}
}

/*This is the scheduler. This function decides on which flow to send
  a given MSS. Currently we choose a simple round-robin policy.*/
static struct tcp_sock* get_available_subflow(struct multipath_pcb *mpcb) 
{
	struct tcp_sock *tp, *sel_tp;
	mtcp_for_each_tp(mpcb,tp)
		if (tp->mtcp_flags & MTCP_CURRENT_SUBFLOW) {
			/*Find the next tcp sock to round robin*/
			sel_tp=(tp->next)?tp->next:mpcb->connection_list;
			/*Move the flag to it*/
			tp->mtcp_flags&=~MTCP_CURRENT_SUBFLOW;
			sel_tp->mtcp_flags|=MTCP_CURRENT_SUBFLOW;
			return sel_tp;
		}
	/*No socket has the flag yet, take the first one available*/
	sel_tp=mpcb->connection_list;
	sel_tp->mtcp_flags|=MTCP_CURRENT_SUBFLOW;	
	return sel_tp;
}

int mtcp_sendmsg(struct kiocb *iocb, struct socket *sock, struct msghdr *msg,
		 size_t size)
{
	struct sock *master_sk = sock->sk;
	struct tcp_sock *tp;
	struct iovec *iov;
	struct multipath_pcb *mpcb;
	int iovlen,copied,msg_size;
	
	if (!tp->mpc)
		return tcp_sendmsg(iocb,sock, msg, size);
	
	mpcb=mpcb_from_tcpsock(tcp_sk(master_sk));
	if (mpcb==NULL){
		printk(KERN_ERR "MPCB null in %s\n",__FUNCTION__);
		BUG();
	}
	
	/* Compute the total number of bytes stored in the message*/
	iovlen=msg->msg_iovlen;
	iov=msg->msg_iov;
	msg_size=0;
	while(--iovlen>=0) {
		msg_size+=iov->iov_len;
		iov++;
	}
	
	/*Until everything is sent, we round-robin on the subsockets
	  TODO: This part MUST be able to sleep.(to avoid looping forever)
	  Currently it sleeps inside tcp_sendmsg, but it is not the most
	  efficient, since during that time, we could try sending on other
	  subsockets*/
	copied=0;
	while (copied<msg_size) {
		/*Find a candidate socket for eating data*/
		tp=get_available_subflow(mpcb);
		/*Let the selected socket eat*/
		copied+=tcp_sendmsg(NULL,((struct sock*)tp)->sk_socket, 
				    msg, copied);
	}

	return copied;
}

/**
 * sk_wait_data - wait for data to arrive at sk_receive_queue
 * on any of the subsockets attached to the mpcb
 * @mpcb:  the mpcb to wait on
 * @sk:    its master socket
 * @timeo: for how long
 *
 * Now socket state including sk->sk_err is changed only under lock,
 * hence we may omit checks after joining wait queue.
 * We check receive queue before schedule() only as optimization;
 * it is very likely that release_sock() added new data.
 */
int mtcp_wait_data(struct multipath_pcb *mpcb, struct sock *master_sk,
		   long *timeo)
{
	int rc; struct sock *sk; struct tcp_sock *tp;
	DEFINE_WAIT(wait);

	prepare_to_wait(master_sk->sk_sleep, &wait, TASK_INTERRUPTIBLE);
	mtcp_for_each_sk(mpcb,sk,tp)
		set_bit(SOCK_ASYNC_WAITDATA, &sk->sk_socket->flags);
	rc = mtcp_wait_event_any_sk(mpcb, sk, timeo, 
				    !skb_queue_empty(&sk->sk_receive_queue));
	mtcp_for_each_sk(mpcb,sk,tp)
		clear_bit(SOCK_ASYNC_WAITDATA, &sk->sk_socket->flags);
	finish_wait(master_sk->sk_sleep, &wait);
	return rc;
}

static void mtcp_ofo_queue(struct sock *sk, struct msghdr *msg, size_t *len,
			   u32 *data_seq, int *copied)
{
	struct multipath_pcb *mpcb=mpcb_from_tcpsock(tcp_sk(sk));
	struct sk_buff *skb;
	int err;
	u32 data_offset;
	unsigned long used;
	
	while ((skb = skb_peek(&mpcb->out_of_order_queue)) != NULL) {
		if (after(TCP_SKB_CB(skb)->data_seq, *data_seq))
			break;
				
		if (!after(TCP_SKB_CB(skb)->end_data_seq, *data_seq)) {
			printk(KERN_ERR "ofo packet was already received \n");
			__skb_unlink(skb, &mpcb->out_of_order_queue);
			__kfree_skb(skb);
			continue;
		}
		printk(KERN_ERR "ofo delivery : "
		       "nxt_data_seq %X data_seq %X - %X\n",
		       *data_seq, TCP_SKB_CB(skb)->data_seq,
		       TCP_SKB_CB(skb)->end_data_seq);
		
		__skb_unlink(skb, &mpcb->out_of_order_queue);
		
		/*The skb can be read by the app*/
		data_offset= *data_seq - TCP_SKB_CB(skb)->data_seq;
		if (tcp_hdr(skb)->syn)
			data_offset--;
		used = skb->len - data_offset;
		if (*len < used)
			used = *len;
				
		err=skb_copy_datagram_iovec(skb, data_offset,
					    msg->msg_iov, used);
		*copied+=used;
		__kfree_skb(skb);
		
		*data_seq = TCP_SKB_CB(skb)->end_data_seq;
	}
}

int mtcp_queue_skb(struct sock *sk,struct sk_buff *skb, u32 offset,
		   unsigned long *used, struct msghdr *msg, size_t *len,
		   u32 *data_seq, int *copied)
{
	struct tcp_sock *tp=tcp_sk(sk);
	struct multipath_pcb *mpcb=mpcb_from_tcpsock(tp);
	u32 data_offset;
	int err;      
	
	/*Is this a duplicate segment ?*/
	if (after(*data_seq,TCP_SKB_CB(skb)->data_seq+skb->len)) {
		return MTCP_EATEN;
	}

	if (before(*data_seq,TCP_SKB_CB(skb)->data_seq)) {		
		/*the skb must be queued in the ofo queue*/
		__skb_unlink(skb, &sk->sk_receive_queue);
		
		/*Since the skb is removed from the receive queue
		  we must advance the seq num in the corresponding
		  tp*/
		*tp->seq += skb->len;
		
		/*TODEL*/
		printk(KERN_ERR "exp. data_seq:%x, skb->data_seq:%x\n",
		       *data_seq,TCP_SKB_CB(skb)->data_seq);
		
		if (!skb_peek(&mpcb->out_of_order_queue)) {
			/* Initial out of order segment */
			__skb_queue_head(&mpcb->out_of_order_queue, skb);
			return MTCP_QUEUED;
		}
		else {	
			struct sk_buff *skb1 = mpcb->out_of_order_queue.prev;
			/* Find place to insert this segment. */
			do {
				if (!after(TCP_SKB_CB(skb1)->data_seq, 
					   TCP_SKB_CB(skb)->data_seq))
					break;
			} while ((skb1 = skb1->prev) !=
				 (struct sk_buff *)&tp->out_of_order_queue);

			/* Do skb overlap to previous one? */
			if (skb1 != 
			    (struct sk_buff *)&mpcb->out_of_order_queue &&
			    before(TCP_SKB_CB(skb)->data_seq, 
				   TCP_SKB_CB(skb1)->end_data_seq)) {
				if (!after(TCP_SKB_CB(skb)->end_data_seq, 
					   TCP_SKB_CB(skb1)->end_data_seq)) {
					/* All the bits are present. Drop. */
					return MTCP_EATEN;
				}
				if (!after(TCP_SKB_CB(skb)->data_seq, 
					   TCP_SKB_CB(skb1)->data_seq)) {
					/*skb and skb1 have the same starting 
					  point, but skb terminates after skb1*/
					skb1 = skb1->prev;
				}
			}
			__skb_insert(skb, skb1, skb1->next, 
				     &mpcb->out_of_order_queue);
			/* And clean segments covered by new one as whole. */
			while ((skb1 = skb->next) !=
			       (struct sk_buff *)&mpcb->out_of_order_queue &&
			       after(TCP_SKB_CB(skb)->end_data_seq, 
				     TCP_SKB_CB(skb1)->data_seq)) {
				if (!before(TCP_SKB_CB(skb)->end_data_seq, 
					    TCP_SKB_CB(skb1)->end_data_seq)) {
					__skb_unlink(skb1, 
						     &mpcb->out_of_order_queue);
					__kfree_skb(skb1);
				}
			}
			return MTCP_QUEUED;
		}
	}

	else {
		/*The skb can be read by the app*/
		data_offset= *data_seq - TCP_SKB_CB(skb)->data_seq;
		if (tcp_hdr(skb)->syn)
			data_offset--;
		*used = skb->len - data_offset;
		if (*len < *used)
			*used = *len;
		
		err=skb_copy_datagram_iovec(skb, data_offset,
					    msg->msg_iov, *used);
		if (err) return err;
		
		*tp->seq += *used;
		*data_seq += *used;
		*len -= *used;
		*copied+=*used;
		tp->copied+=*used;
		
		/*Check if this fills a gap in the ofo queue*/
		if (!skb_queue_empty(&mpcb->out_of_order_queue))
			mtcp_ofo_queue(sk,msg,len,data_seq,copied);
		/*If the skb has been partially eaten, it will tcp_recvmsg
		  will see it anyway thanks to the @used pointer.*/
		return MTCP_EATEN;
	}
}

MODULE_LICENSE("GPL");

EXPORT_SYMBOL(mtcp_sendmsg);
