/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the TCP module.
 *
 * Version:	@(#)tcp.h	1.0.5	05/23/93
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *              S. Barré: Added this file, to recuperate a portion
 *              of the previous tcp.h file, in order to support mptcp
 *              includes interdependence.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#ifndef _TCP_OPTIONS_H
#define _TCP_OPTIONS_H

#include <linux/types.h>
#include <net/mptcp_pm.h>

#define OPTION_SACK_ADVERTISE	(1 << 0)
#define OPTION_TS		(1 << 1)
#define OPTION_MD5		(1 << 2)
#define OPTION_WSCALE		(1 << 3)
#define OPTION_COOKIE_EXTENSION	(1 << 4)
#define OPTION_MP_CAPABLE       (1 << 5)
#define OPTION_DSN_MAP          (1 << 6)
#define OPTION_DATA_FIN         (1 << 7)
#define OPTION_DATA_ACK         (1 << 8)
#define OPTION_ADD_ADDR         (1 << 9)
#define OPTION_MP_JOIN          (1 << 10)
#define OPTION_MP_FAIL		(1 << 11)

struct tcp_out_options {
	u16	options;	/* bit field of OPTION_* */
	u8	ws;		/* window scale, 0 to disable */
	u8	num_sack_blocks;/* number of SACK blocks to include */
	u8	hash_size;	/* bytes in hash_location */
	u16	mss;		/* 0 to disable */
	__u32	tsval, tsecr;	/* need to include OPTION_TS */
	__u8	*hash_location;	/* temporary pointer, overloaded */
#ifdef CONFIG_MPTCP
	__u32	data_seq;	/* data sequence number, for MPTCP */
	__u32	data_ack;	/* data ack, for MPTCP */
	__u32	sub_seq;	/* subflow seqnum, for MPTCP */
	__u16	data_len;	/* data level length, for MPTCP */
	__sum16	dss_csum;	/* Overloaded field: dss-checksum required
				 * (for SYN-packets)? Or dss-csum itself */
	__u64	sender_key;	/* sender's key for mptcp */
	__u64	receiver_key;	/* receiver's key for mptcp */
	__u64	sender_truncated_mac;
	__u32	sender_random_number;	/* random number of the sender */
	__u32	receiver_random_number;	/* random number of the receiver */
	__u32	token;		/* token for mptcp */
	char	sender_mac[20];
#ifdef CONFIG_MPTCP_PM
	struct mptcp_loc4 *addr4;/* v4 addresses for MPTCP */
	struct mptcp_loc6 *addr6;/* v6 addresses for MPTCP */
	u8	addr_id;	/* address id */
#endif /* CONFIG_MPTCP_PM */
	__u8	mp_join_type;	/* SYN/SYNACK/ACK */
#endif /* CONFIG_MPTCP */
};

struct tcp_options_received {
/*	PAWS/RTTM data	*/
	long	ts_recent_stamp;/* Time we stored ts_recent (for aging) */
	u32	ts_recent;	/* Time stamp to echo next		*/
	u32	rcv_tsval;	/* Time stamp value			*/
	u32	rcv_tsecr;	/* Time stamp echo reply		*/
	u16	saw_tstamp:1,	/* Saw TIMESTAMP on last packet		*/
		tstamp_ok:1,	/* TIMESTAMP seen on SYN packet		*/
		dsack:1,	/* D-SACK is scheduled			*/
		wscale_ok:1,	/* Wscale seen on SYN packet		*/
		sack_ok:4,	/* SACK seen on SYN packet		*/
		snd_wscale:4,	/* Window scaling received from sender	*/
		rcv_wscale:4,	/* Window scaling to send to receiver	*/
		saw_mpc:1,    /* MPC option seen, for MPTCP */
		saw_dfin:1;    /* DFIN option seen, for MPTCP */
	u8	cookie_plus:6,	/* bytes in authenticator/cookie option	*/
		cookie_out_never:1,
		cookie_in_always:1;
	u8	num_sacks;	/* Number of SACK blocks		*/
	u16	user_mss;	/* mss requested by user in ioctl	*/
	u16	mss_clamp;	/* Maximal mss, negotiated at
				 * connection setup
				 */
#ifdef CONFIG_MPTCP
	__u8	rem_id;			/* Address-id in the MP_JOIN */
	u32	rcv_isn; /* Needed to retrieve abs subflow seqnum from the
			  * relative version.
			  */
#endif /* CONFIG_MPTCP */
};

static inline void tcp_clear_options(struct tcp_options_received *rx_opt)
{
	rx_opt->tstamp_ok = rx_opt->sack_ok = 0;
	rx_opt->wscale_ok = rx_opt->snd_wscale = 0;
	rx_opt->cookie_plus = rx_opt->saw_mpc = 0;
}

struct multipath_options {
#ifdef CONFIG_MPTCP_PM
	int	num_addr4;
	int	num_addr6;
	struct	mptcp_loc4 addr4[MPTCP_MAX_ADDR];
	struct	mptcp_loc6 addr6[MPTCP_MAX_ADDR];
#endif
	__u32	mptcp_rem_token;	/* Received token */
	__u32	mptcp_recv_random_number;
	__u64	mptcp_rem_key;	/* Remote key */
	__u64	mptcp_recv_tmac;
	u32	fin_dsn; /* DSN of the byte  FOLLOWING the Data FIN */
	__u8	mptcp_recv_mac[20];
	__u8	mptcp_opt_type;
	u8	list_rcvd:1, /* 1 if IP list has been received (MPTCP_PM) */
		dfin_rcvd:1,
		mp_fail:1,
		dss_csum:1;
};

#endif /*_TCP_OPTIONS_H*/
