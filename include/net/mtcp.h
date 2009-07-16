/*
 *	MTCP implementation
 *
 *	Authors:
 *      Sébastien Barré		<sebastien.barre@uclouvain.be>
 *      Costin Raiciu           <c.raiciu@cs.ucl.ac.uk>
 *
 *
 *      date : June 09
 *
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#ifndef _MTCP_H
#define _MTCP_H

#include <linux/tcp_options.h>
#include <linux/notifier.h>
#include <linux/xfrm.h>
#include <linux/aio.h>
#include <linux/net.h>
#include <linux/socket.h>

/*Macro for activation/deactivation of debug messages*/

#undef PDEBUG
#ifdef CONFIG_MTCP_DEBUG
# define PDEBUG(fmt,args...) printk( KERN_DEBUG __FILE__ ": " fmt,##args)
#else
# define PDEBUG(fmt,args...)
#endif

/*hashtable Not used currently -- To delete ?*/
#define MTCP_HASH_SIZE                16
#define hash_fd(fd) \
	jhash_1word(fd,0)%MTCP_HASH_SIZE

struct multipath_options {	
#ifdef CONFIG_MTCP_PM
	u32    remote_token;
	u32    local_token;
	u8     ip_count;
	u32*   ip_list;
	u8     list_rcvd:1; /*1 if IP list has been received*/
#endif
	u32    data_seq;
	u8     saw_mpc:1,
	       saw_dsn:1;	
};

extern struct proto mtcpsub_prot;

struct tcp_sock;

struct multipath_pcb {
	struct list_head          collide_sd;
	
	/*receive and send buffer sizing*/
	int                       rcvbuf, sndbuf;
	atomic_t                  rmem_alloc;       
	
	/*connection identifier*/
	sa_family_t               sa_family;
	xfrm_address_t            remote_ulid, local_ulid;
	__be16                    remote_port,local_port;
	
	/*list of sockets in this multipath connection*/
	struct tcp_sock*          connection_list;
	/*Master socket, also part of the connection_list, this
	  socket is the one that the application sees.*/
	struct sock*              master_sk;
	/*socket count in this connection*/
	int                       cnt_subflows;    
	int                       syn_sent;
	int                       cnt_established;
	
	/*state, for faster tests in code*/
	int                       state;
	int                       err;
	
	char                      done;
	unsigned short            shutdown;

	struct {
		struct task_struct	*task;
		struct iovec		*iov;
/*The length field is initialized by mtcp_recvmsg, and decremented by 
  each subsocket separately, upon data reception. That's why each subsocket
  must do the copies with appropriate locks.
  Whenever a subsocket decrements this field, it must increment its 
  tp->copied field, so that we can track later how many bytes have been
  eaten by which subsocket.*/
		int                     len;
	} ucopy; /*Fields moved from tcp_sock struct to this one*/

	struct multipath_options  received_options;
	struct tcp_options_received tcp_opt;

	u32    write_seq;  /*data sequence number, counts the number of 
			     bytes the user has written so far */
	u32    copied_seq; /* Head of yet unread data		*/
	
	/*user data, unpacketized
	  This is a circular buffer, data is stored in the "subbuffer"
	  starting at byte index wb_start with the write_buffer,
	  with length wb_length. Uppon mpcb init, the size
	  of the write buffer is stored in wb_size */
	char*                     write_buffer;
	/*wb_size: size of the circular sending buffer
	  wb_start: index of the first byte of pending data in the buffer
	  wb_length: number of bytes occupied by the pending data.
	             of course, it never exceeds wb_size*/
	int                       wb_size,wb_start,wb_length;
	
	/*remember user flags*/
	struct flag_stack*        flags;
	uint8_t                   mtcp_flags;
#define MTCP_ACCEPT 0x1  /*the user socket is in accept mode
			   keep accept mode for subsockets
			   (that is, we don't make a connect)*/
	
	struct sk_buff_head       receive_queue;/*received data*/
	struct sk_buff_head       write_queue;/*sent stuff, waiting for ack*/
	struct sk_buff_head       retransmit_queue;/*need to rexmit*/
	struct sk_buff_head       error_queue;
	struct sk_buff_head       out_of_order_queue; /* Out of order segments 
							 go here */
	
	spinlock_t                lock;
	struct kref               kref;
	struct notifier_block     nb; /*For listening to PM events*/
};

#define mpcb_from_tcpsock(tp) ((tp)->mpcb)
#define is_master_sk(tp) ((tp)->mpcb && tcp_sk((tp)->mpcb->master_sk)==tp)

/*Iterates overs all subflows*/
#define mtcp_for_each_tp(mpcb,tp)			\
	for (tp=mpcb->connection_list;tp;tp=tp->next)

#define mtcp_for_each_sk(mpcb,sk,tp)					\
	for (sk=(struct sock*)mpcb->connection_list,tp=tcp_sk(sk);	\
	     sk;							\
	     sk=(struct sock*)tcp_sk(sk)->next,tp=tcp_sk(sk))

/*Returns 1 if any subflow meets the condition @cond
  Else return 0. Moreover, if 1 is returned, sk points to the
  first subsocket that verified the condition*/
#define mtcp_test_any_sk(mpcb,sk,cond)			\
	({int __ans=0; struct tcp_sock *__tp;		\
		mtcp_for_each_sk(mpcb,sk,__tp) {	\
			if (cond) __ans=1;		\
			break;				\
		}					\
		__ans;})				\

/*Idem here with tp in lieu of sk*/	
#define mtcp_test_any_tp(mpcb,tp,cond)			\
	({      int __ans=0;				\
		mtcp_for_each_tp(mpcb,tp) {		\
			if (cond) __ans=1;		\
			break;				\
		}					\
		__ans;					\
	})						\


/*Wait for event @__condition to happen ony subsocket, 
  or __timeo to expire
  This is the MTCP equivalent of sk_wait_event */
#define mtcp_wait_event_any_sk(__mpcb,__sk, __timeo, __condition)	\
	({	int __rc; struct tcp_sock *__tp;			\
		mtcp_for_each_sk(__mpcb,__sk,__tp)			\
			release_sock(__sk);				\
		__rc = mtcp_test_any_sk(__mpcb,__sk,__condition);	\
		if (!__rc) {						\
			if (__mpcb->master_sk->sk_protocol==IPPROTO_TCP && \
			    __mpcb->master_sk->sk_family==AF_INET6)	\
				printk("will really sleep\n");		\
			*(__timeo) = schedule_timeout(*(__timeo));	\
			if (__mpcb->master_sk->sk_protocol==IPPROTO_TCP && \
			    __mpcb->master_sk->sk_family==AF_INET6)	\
				printk("woken up\n");			\
		}							\
		mtcp_for_each_sk(__mpcb,__sk,__tp)			\
			lock_sock(__sk);				\
		if (__mpcb->master_sk->sk_protocol==IPPROTO_TCP &&	\
		    __mpcb->master_sk->sk_family==AF_INET6)		\
			printk("will really sleep\n");			\
		__rc = mtcp_test_any_sk(__mpcb,__sk,__condition);	\
		__rc;							\
	})

int mtcp_wait_data(struct multipath_pcb *mpcb, struct sock *master_sk, 
			  long *timeo);
int mtcp_queue_skb(struct sock *sk,struct sk_buff *skb, u32 offset,
		   unsigned long *used, struct msghdr *msg, size_t *len,   
		   u32 *data_seq, int *copied);
/*Possible return values from mtcp_queue_skb*/
#define MTCP_EATEN 1 /*The skb has been (fully or partially) eaten by the app*/
#define MTCP_QUEUED 2 /*The skb has been queued in the mpcb ofo queue*/

struct multipath_pcb* mtcp_alloc_mpcb(struct sock *master_sk);
void mtcp_add_sock(struct multipath_pcb *mpcb,struct tcp_sock *tp);
struct multipath_pcb* mtcp_lookup_mpcb(int sd);
void mtcp_reset_options(struct multipath_options* mopt);
void mtcp_update_metasocket(struct sock *sock);
int mtcp_sendmsg(struct kiocb *iocb, struct socket *sock, struct msghdr *msg,
		 size_t size);
int mtcpv6_init(void);


#endif /*_MTCP_H*/
