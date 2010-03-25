/*
 * tcpprobe - Observe the TCP flow with kprobes.
 *
 * The idea for this came from Werner Almesberger's umlsim
 * Copyright (C) 2004, Stephen Hemminger <shemminger@osdl.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/kernel.h>
#include <linux/socket.h>
#include <linux/tcp.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <net/net_namespace.h>

#ifdef CONFIG_KPROBES
#include <linux/kprobes.h>
#else
#include <linux/tcp_probe.h>
#endif

#include <net/tcp.h>

MODULE_AUTHOR("Stephen Hemminger <shemminger@linux-foundation.org>");
MODULE_DESCRIPTION("TCP cwnd snooper");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1");

static int port __read_mostly = 0;
MODULE_PARM_DESC(port, "Port to match (0=all)");
module_param(port, int, 0);

static int bufsize __read_mostly = 4096;
MODULE_PARM_DESC(bufsize, "Log buffer size in packets (4096)");
module_param(bufsize, int, 0);

static int full __read_mostly=1;
MODULE_PARM_DESC(full, "Full log (1=every ack packet received,  0=only cwnd changes)");
module_param(full, int, 0);

static const char procname[] = "tcpprobe";

struct tcp_log {
	ktime_t tstamp;
	__be32	saddr, daddr;
	__be16	sport, dport;
	int     path_index;
	u16	length;
	u32	snd_nxt;
	u32	snd_una;
	u32	snd_wnd;
	u32	snd_cwnd;
	u32	ssthresh;
	u32	srtt;
	u32     rcv_nxt;
	u32     copied_seq;
	u32     rcv_wnd;
	u32     rcv_buf;  /*N*/
	u32     rcv_ssthresh; /*N*/
	u32     window_clamp; /*N*/
	char    send; /*1 if sending side, 0 if receive*/
	int     space;
	u32     rtt_est;
	u32     in_flight;
	u32     mss_cache;
	int     snd_buf;
};

static struct {
	spinlock_t	lock;
	wait_queue_head_t wait;
	ktime_t		start;
	u32		lastcwnd;

	unsigned long	head, tail;
	struct tcp_log	*log;
} tcp_probe;


static inline int tcp_probe_used(void)
{
	return (tcp_probe.head - tcp_probe.tail) % bufsize;
}

static inline int tcp_probe_avail(void)
{
	if (!(bufsize-tcp_probe_used()))
		printk(KERN_ERR "No log space anymore\n");
	return bufsize - tcp_probe_used();
}

/*
 * Hook inserted to be called before each receive packet.
 * Note: arguments must match tcp_rcv_established()!
 */
static int jtcp_rcv_established(struct sock *sk, struct sk_buff *skb,
				struct tcphdr *th, unsigned len)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	const struct inet_sock *inet = inet_sk(sk);

	/* Only update if port matches */
	if ((port == 0 || ntohs(inet->dport) == port 
	     || ntohs(inet->sport) == port)
	    && ntohs(inet->sport) != 22
	    && ntohs(inet->dport) != 22
	    && (full || tp->snd_cwnd != tcp_probe.lastcwnd)) {

		spin_lock(&tcp_probe.lock);
		/* If log fills, just silently drop */
		if (tcp_probe_avail() > 1) {
			struct tcp_log *p = tcp_probe.log + tcp_probe.head;

			p->tstamp = ktime_get();
			p->saddr = inet->saddr;
			p->sport = inet->sport;
			p->daddr = inet->daddr;
			p->dport = inet->dport;
			p->path_index = sk?tcp_sk(sk)->path_index:0;
			p->length = skb->len;
			p->snd_nxt = tp->snd_nxt;
			p->snd_una = tp->snd_una;
			p->snd_cwnd = tp->snd_cwnd;
			p->snd_wnd = tp->snd_wnd;
			p->ssthresh = tcp_current_ssthresh(sk);
			p->srtt = tp->srtt >> 3;
			p->rcv_nxt=tp->rcv_nxt;
			p->copied_seq=tp->copied_seq;
			p->rcv_wnd=tp->rcv_wnd;
			p->rcv_buf=sk->sk_rcvbuf;
			p->rcv_ssthresh=tp->rcv_ssthresh;
			p->window_clamp=tp->window_clamp;
			p->send=0;
			p->space=tp->rcvq_space.space;
			p->rtt_est=tp->rcv_rtt_est.rtt;
			p->in_flight=tp->packets_out;
			p->mss_cache=tp->mss_cache;
			p->snd_buf=sk->sk_sndbuf;
			tcp_probe.head = (tcp_probe.head + 1) % bufsize;
		}
		tcp_probe.lastcwnd = tp->snd_cwnd;
		spin_unlock(&tcp_probe.lock);

		wake_up(&tcp_probe.wait);
	}

#ifdef CONFIG_KPROBES
	jprobe_return();
#endif
	return 0;
}

/*
 * Hook inserted to be called before each packet transmission.
 * Note: arguments must match tcp_transmit_skb()!
 */
static int jtcp_transmit_skb(struct sock *sk, struct sk_buff *skb, int clone_it,
			     gfp_t gfp_mask)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	const struct inet_sock *inet = inet_sk(sk);

	/* Only update if port matches and state is established*/
	if (sk->sk_state == TCP_ESTABLISHED && 
	    (port == 0 || ntohs(inet->dport) == port || 
	     ntohs(inet->sport) == port)
	    && ntohs(inet->sport) != 22
	    && ntohs(inet->dport) != 22
	    && (full || tp->snd_cwnd != tcp_probe.lastcwnd)) {

		spin_lock_bh(&tcp_probe.lock);
		/* If log fills, just silently drop */
		if (tcp_probe_avail() > 1) {
			struct tcp_log *p = tcp_probe.log + tcp_probe.head;

			p->tstamp = ktime_get();
			p->saddr = inet->saddr;
			p->sport = inet->sport;
			p->daddr = inet->daddr;
			p->dport = inet->dport;
			p->path_index = tp->path_index;
			p->length = skb->len;
			p->snd_nxt = tp->snd_nxt;
			p->snd_una = tp->snd_una;
			p->snd_cwnd = tp->snd_cwnd;
			p->snd_wnd = tp->snd_wnd;
			p->ssthresh = tcp_current_ssthresh(sk);
			p->srtt = tp->srtt >> 3;
			p->rcv_nxt=tp->rcv_nxt;
			p->copied_seq=tp->copied_seq;
			p->rcv_wnd=tp->rcv_wnd;
			p->rcv_buf=sk->sk_rcvbuf;
			p->rcv_ssthresh=tp->rcv_ssthresh;
			p->window_clamp=tp->window_clamp;		
			p->send=1;
			p->space=tp->rcvq_space.space;
			p->rtt_est=tp->rcv_rtt_est.rtt;
			p->in_flight=tp->packets_out;
			p->mss_cache=tp->mss_cache;
			p->snd_buf=sk->sk_sndbuf;

			tcp_probe.head = (tcp_probe.head + 1) % bufsize;
		}
		tcp_probe.lastcwnd = tp->snd_cwnd;
		spin_unlock_bh(&tcp_probe.lock);

		wake_up(&tcp_probe.wait);
	}

#ifdef CONFIG_KPROBES
	jprobe_return();
#endif
	return 0;
}

#ifdef CONFIG_KPROBES
static struct jprobe tcp_jprobe_rcv = {
	.kp = {
		.symbol_name	= "tcp_rcv_established",
	},
	.entry	= jtcp_rcv_established,
	};
static struct jprobe tcp_jprobe_send = {
	.kp = {
		.symbol_name	= "tcp_transmit_skb",		
	},
	.entry	= jtcp_transmit_skb,
	};
#else
static struct tcpprobe_ops tcpprobe_fcts = {
	.rcv_established=jtcp_rcv_established,
	.transmit_skb=jtcp_transmit_skb,
};
#endif


static int tcpprobe_open(struct inode * inode, struct file * file)
{
	/* Reset (empty) log */
	spin_lock_bh(&tcp_probe.lock);
	tcp_probe.head = tcp_probe.tail = 0;
	tcp_probe.start = ktime_get();
	spin_unlock_bh(&tcp_probe.lock);

	return 0;
}

static int tcpprobe_sprint(char *tbuf, int n)
{
	const struct tcp_log *p
		= tcp_probe.log + tcp_probe.tail % bufsize;
	struct timespec tv
		= ktime_to_timespec(ktime_sub(p->tstamp, tcp_probe.start));

	return snprintf(tbuf, n,
			"%lu.%09lu " NIPQUAD_FMT ":%u " NIPQUAD_FMT ":%u"
			" %d %d %#x %#x %u %u %u %u %#x %#x %u %u %u %u %d"
			" %d %u %u %u %d\n",
			(unsigned long) tv.tv_sec,
			(unsigned long) tv.tv_nsec,
			NIPQUAD(p->saddr), ntohs(p->sport),
			NIPQUAD(p->daddr), ntohs(p->dport),
			p->path_index,p->length, p->snd_nxt, p->snd_una,
			p->snd_cwnd, p->ssthresh, p->snd_wnd, p->srtt,
			p->rcv_nxt,p->copied_seq,p->rcv_wnd,p->rcv_buf,
			p->window_clamp,p->rcv_ssthresh, p->send,
			p->space,p->rtt_est*1000/HZ,p->in_flight,p->mss_cache,
			p->snd_buf);
}

static ssize_t tcpprobe_read(struct file *file, char __user *buf,
			     size_t len, loff_t *ppos)
{
	int error = 0, cnt = 0;

	if (!buf || len < 0)
		return -EINVAL;

	while (cnt < len) {
		char tbuf[512];
		int width;

		/* Wait for data in buffer */
		error = wait_event_interruptible(tcp_probe.wait,
						 tcp_probe_used() > 0);
		if (error)
			break;

		spin_lock_bh(&tcp_probe.lock);
		if (tcp_probe.head == tcp_probe.tail) {
			/* multiple readers race? */
			spin_unlock_bh(&tcp_probe.lock);
			continue;
		}

		width = tcpprobe_sprint(tbuf, sizeof(tbuf));

		if (cnt + width < len)
			tcp_probe.tail = (tcp_probe.tail + 1) % bufsize;

		spin_unlock_bh(&tcp_probe.lock);

		/* if record greater than space available
		   return partial buffer (so far) */
		if (cnt + width >= len)
			break;

		if (copy_to_user(buf + cnt, tbuf, width))
			return -EFAULT;
		cnt += width;
	}

	return cnt == 0 ? error : cnt;
}

static const struct file_operations tcpprobe_fops = {
	.owner	 = THIS_MODULE,
	.open	 = tcpprobe_open,
	.read    = tcpprobe_read,
};

static __init int tcpprobe_init(void)
{
	int ret = -ENOMEM;

	init_waitqueue_head(&tcp_probe.wait);
	spin_lock_init(&tcp_probe.lock);

	if (bufsize < 0)
		return -EINVAL;

	tcp_probe.log = kcalloc(bufsize, sizeof(struct tcp_log), GFP_KERNEL);
	if (!tcp_probe.log)
		goto err0;

	if (!proc_net_fops_create(&init_net, procname, S_IRUSR, &tcpprobe_fops))
		goto err0;

#ifdef CONFIG_KPROBES
	ret = register_jprobe(&tcp_jprobe_rcv);
	if (!ret) ret = register_jprobe(&tcp_jprobe_send);
#else
	ret=register_probe(&tcpprobe_fcts, 4);
#endif
	if (ret)
		goto err1;

	pr_info("TCP probe registered (port=%d)\n", port);
	return 0;
 err1:
	proc_net_remove(&init_net, procname);
 err0:
	kfree(tcp_probe.log);
	return ret;
}
module_init(tcpprobe_init);

static __exit void tcpprobe_exit(void)
{
	proc_net_remove(&init_net, procname);
#ifdef CONFIG_KPROBES
	unregister_jprobe(&tcp_jprobe_rcv);
	unregister_jprobe(&tcp_jprobe_send);
#else
	unregister_probe(&tcpprobe_fcts,4);
#endif
	kfree(tcp_probe.log);
}
module_exit(tcpprobe_exit);
