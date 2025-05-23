/*
 * Copyright (c) 2000-2024 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1982, 1986, 1988, 1990, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)tcp_subr.c	8.2 (Berkeley) 5/24/95
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

#include "tcp_includes.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/domain.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/protosw.h>
#include <sys/random.h>
#include <sys/syslog.h>
#include <sys/mcache.h>
#include <kern/locks.h>
#include <kern/zalloc.h>

#include <dev/random/randomdev.h>

#include <net/route.h>
#include <net/if.h>
#include <net/content_filter.h>
#include <net/ntstat.h>
#include <net/multi_layer_pkt_log.h>

#define tcp_minmssoverload fring
#define _IP_VHL
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <netinet/in_pcb.h>
#include <netinet6/in6_pcb.h>
#include <netinet/in_var.h>
#include <netinet/ip_var.h>
#include <netinet/icmp_var.h>
#include <netinet6/ip6_var.h>
#include <netinet/mptcp_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_seq.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>
#include <netinet/tcp_cc.h>
#include <netinet/tcp_cache.h>
#include <kern/thread_call.h>

#include <netinet6/tcp6_var.h>
#include <netinet/tcpip.h>
#include <netinet/tcp_log.h>

#include <netinet6/ip6protosw.h>
#include <netinet6/esp.h>

#if IPSEC
#include <netinet6/ipsec.h>
#include <netinet6/ipsec6.h>
#endif /* IPSEC */

#if NECP
#include <net/necp.h>
#endif /* NECP */

#undef tcp_minmssoverload

#include <net/sockaddr_utils.h>

#include <corecrypto/ccaes.h>
#include <libkern/crypto/aes.h>
#include <libkern/crypto/md5.h>
#include <sys/kdebug.h>
#include <mach/sdt.h>
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>

#define DBG_FNC_TCP_CLOSE       NETDBG_CODE(DBG_NETTCP, ((5 << 8) | 2))

static tcp_cc tcp_ccgen;

extern struct tcptimerlist tcp_timer_list;
extern struct tcptailq tcp_tw_tailq;

extern int tcp_awdl_rtobase;

SYSCTL_SKMEM_TCP_INT(TCPCTL_MSSDFLT, mssdflt, CTLFLAG_RW | CTLFLAG_LOCKED,
    int, tcp_mssdflt, TCP_MSS, "Default TCP Maximum Segment Size");

SYSCTL_SKMEM_TCP_INT(TCPCTL_V6MSSDFLT, v6mssdflt,
    CTLFLAG_RW | CTLFLAG_LOCKED, int, tcp_v6mssdflt, TCP6_MSS,
    "Default TCP Maximum Segment Size for IPv6");

int tcp_sysctl_fastopenkey(struct sysctl_oid *, void *, int,
    struct sysctl_req *);
SYSCTL_PROC(_net_inet_tcp, OID_AUTO, fastopen_key, CTLTYPE_STRING | CTLFLAG_WR,
    0, 0, tcp_sysctl_fastopenkey, "S", "TCP Fastopen key");

/* Current count of half-open TFO connections */
int     tcp_tfo_halfcnt = 0;

/* Maximum of half-open TFO connection backlog */
SYSCTL_SKMEM_TCP_INT(OID_AUTO, fastopen_backlog,
    CTLFLAG_RW | CTLFLAG_LOCKED, int, tcp_tfo_backlog, 10,
    "Backlog queue for half-open TFO connections");

SYSCTL_SKMEM_TCP_INT(OID_AUTO, fastopen, CTLFLAG_RW | CTLFLAG_LOCKED,
    int, tcp_fastopen, TCP_FASTOPEN_CLIENT | TCP_FASTOPEN_SERVER,
    "Enable TCP Fastopen (RFC 7413)");

SYSCTL_SKMEM_TCP_INT(OID_AUTO, now_init, CTLFLAG_RD | CTLFLAG_LOCKED,
    uint32_t, tcp_now_init, 0, "Initial tcp now value");

SYSCTL_SKMEM_TCP_INT(OID_AUTO, microuptime_init, CTLFLAG_RD | CTLFLAG_LOCKED,
    uint32_t, tcp_microuptime_init, 0, "Initial tcp uptime value in micro seconds");

/*
 * Minimum MSS we accept and use. This prevents DoS attacks where
 * we are forced to a ridiculous low MSS like 20 and send hundreds
 * of packets instead of one. The effect scales with the available
 * bandwidth and quickly saturates the CPU and network interface
 * with packet generation and sending. Set to zero to disable MINMSS
 * checking. This setting prevents us from sending too small packets.
 */
SYSCTL_SKMEM_TCP_INT(OID_AUTO, minmss, CTLFLAG_RW | CTLFLAG_LOCKED,
    int, tcp_minmss, TCP_MINMSS, "Minmum TCP Maximum Segment Size");

SYSCTL_UINT(_net_inet_tcp, OID_AUTO, pcbcount, CTLFLAG_RD | CTLFLAG_LOCKED,
    &tcbinfo.ipi_count, 0, "Number of active PCBs");

SYSCTL_SKMEM_TCP_INT(OID_AUTO, icmp_may_rst, CTLFLAG_RW | CTLFLAG_LOCKED,
    static int, icmp_may_rst, 1,
    "Certain ICMP unreachable messages may abort connections in SYN_SENT");

int             tcp_do_timestamps = 1;
#if (DEVELOPMENT || DEBUG)
SYSCTL_INT(_net_inet_tcp, OID_AUTO, do_timestamps,
    CTLFLAG_RW | CTLFLAG_LOCKED, &tcp_do_timestamps, 0, "enable TCP timestamps");
#endif /* (DEVELOPMENT || DEBUG) */

SYSCTL_SKMEM_TCP_INT(OID_AUTO, rtt_min, CTLFLAG_RW | CTLFLAG_LOCKED,
    int, tcp_TCPTV_MIN, 100, "min rtt value allowed");

SYSCTL_SKMEM_TCP_INT(OID_AUTO, rexmt_slop, CTLFLAG_RW,
    int, tcp_rexmt_slop, TCPTV_REXMTSLOP, "Slop added to retransmit timeout");

SYSCTL_SKMEM_TCP_INT(OID_AUTO, randomize_ports, CTLFLAG_RW | CTLFLAG_LOCKED,
    __private_extern__ int, tcp_use_randomport, 0,
    "Randomize TCP port numbers");

SYSCTL_SKMEM_TCP_INT(OID_AUTO, win_scale_factor, CTLFLAG_RW | CTLFLAG_LOCKED,
    __private_extern__ int, tcp_win_scale, 3, "Window scaling factor");

#if (DEVELOPMENT || DEBUG)
SYSCTL_SKMEM_TCP_INT(OID_AUTO, init_rtt_from_cache,
    CTLFLAG_RW | CTLFLAG_LOCKED, static int, tcp_init_rtt_from_cache, 1,
    "Initalize RTT from route cache");
#else
SYSCTL_SKMEM_TCP_INT(OID_AUTO, init_rtt_from_cache,
    CTLFLAG_RD | CTLFLAG_LOCKED, static int, tcp_init_rtt_from_cache, 1,
    "Initalize RTT from route cache");
#endif /* (DEVELOPMENT || DEBUG) */

static int tso_debug = 0;
SYSCTL_INT(_net_inet_tcp, OID_AUTO, tso_debug, CTLFLAG_RW | CTLFLAG_LOCKED,
    &tso_debug, 0, "TSO verbosity");

static int tcp_rxt_seg_max = 1024;
SYSCTL_INT(_net_inet_tcp, OID_AUTO, rxt_seg_max, CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcp_rxt_seg_max, 0, "");

static unsigned long tcp_rxt_seg_drop = 0;
SYSCTL_ULONG(_net_inet_tcp, OID_AUTO, rxt_seg_drop, CTLFLAG_RD | CTLFLAG_LOCKED,
    &tcp_rxt_seg_drop, "");

static void     tcp_notify(struct inpcb *, int);

static KALLOC_TYPE_DEFINE(tcp_bwmeas_zone, struct bwmeas, NET_KT_DEFAULT);
KALLOC_TYPE_DEFINE(tcp_reass_zone, struct tseg_qent, NET_KT_DEFAULT);
KALLOC_TYPE_DEFINE(tcp_rxt_seg_zone, struct tcp_rxt_seg, NET_KT_DEFAULT);
KALLOC_TYPE_DEFINE(tcp_seg_sent_zone, struct tcp_seg_sent, NET_KT_DEFAULT);

extern int slowlink_wsize;      /* window correction for slow links */
extern int path_mtu_discovery;

uint32_t tcp_now_remainder_us = 0;  /* remaining micro seconds for tcp_now */

static void tcp_sbrcv_grow_rwin(struct tcpcb *tp, struct sockbuf *sb);

#define TCP_BWMEAS_BURST_MINSIZE 6
#define TCP_BWMEAS_BURST_MAXSIZE 25

/*
 * Target size of TCP PCB hash tables. Must be a power of two.
 *
 * Note that this can be overridden by the kernel environment
 * variable net.inet.tcp.tcbhashsize
 */
#ifndef TCBHASHSIZE
#define TCBHASHSIZE     CONFIG_TCBHASHSIZE
#endif

__private_extern__ int  tcp_tcbhashsize = TCBHASHSIZE;
SYSCTL_INT(_net_inet_tcp, OID_AUTO, tcbhashsize, CTLFLAG_RD | CTLFLAG_LOCKED,
    &tcp_tcbhashsize, 0, "Size of TCP control-block hashtable");

/*
 * This is the actual shape of what we allocate using the zone
 * allocator.  Doing it this way allows us to protect both structures
 * using the same generation count, and also eliminates the overhead
 * of allocating tcpcbs separately.  By hiding the structure here,
 * we avoid changing most of the rest of the code (although it needs
 * to be changed, eventually, for greater efficiency).
 */
#define ALIGNMENT       32
struct  inp_tp {
	struct  inpcb   inp;
	struct  tcpcb   tcb __attribute__((aligned(ALIGNMENT)));
};
#undef ALIGNMENT

static KALLOC_TYPE_DEFINE(tcpcbzone, struct inp_tp, NET_KT_DEFAULT);

int  get_inpcb_str_size(void);
int  get_tcp_str_size(void);

os_log_t tcp_mpkl_log_object = NULL;

static void tcpcb_to_otcpcb(struct tcpcb *, struct otcpcb *);

int tcp_notsent_lowat_check(struct socket *so);
static void tcp_flow_lim_stats(struct ifnet_stats_per_flow *ifs,
    struct if_lim_perf_stat *stat);
static void tcp_flow_ecn_perf_stats(struct ifnet_stats_per_flow *ifs,
    struct if_tcp_ecn_perf_stat *stat);

static aes_encrypt_ctx tfo_ctx; /* Crypto-context for TFO */

void
tcp_tfo_gen_cookie(struct inpcb *inp, u_char *out __sized_by(blk_size), size_t blk_size)
{
	u_char in[CCAES_BLOCK_SIZE];
	int isipv6 = inp->inp_vflag & INP_IPV6;

	VERIFY(blk_size == CCAES_BLOCK_SIZE);

	bzero(&in[0], CCAES_BLOCK_SIZE);
	bzero(&out[0], CCAES_BLOCK_SIZE);

	if (isipv6) {
		memcpy(in, &inp->in6p_faddr, sizeof(struct in6_addr));
	} else {
		memcpy(in, &inp->inp_faddr, sizeof(struct in_addr));
	}

	aes_encrypt_cbc(in, NULL, 1, out, &tfo_ctx);
}

__private_extern__ int
tcp_sysctl_fastopenkey(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int error = 0;
	/*
	 * TFO-key is expressed as a string in hex format
	 *  +1 to account for the \0 char
	 *  +1 because sysctl_io_string() expects a string length but the sysctl command
	 *     now includes the terminating \0 in newlen -- see rdar://77205344
	 */
	char keystring[TCP_FASTOPEN_KEYLEN * 2 + 2];
	u_int32_t key[TCP_FASTOPEN_KEYLEN / sizeof(u_int32_t)];
	int i;
	size_t ks_len;

	/*
	 * sysctl_io_string copies keystring into the oldptr of the sysctl_req.
	 * Make sure everything is zero, to avoid putting garbage in there or
	 * leaking the stack.
	 */
	bzero(keystring, sizeof(keystring));

	error = sysctl_io_string(req, keystring, sizeof(keystring), 0, NULL);
	if (error) {
		os_log(OS_LOG_DEFAULT,
		    "%s: sysctl_io_string() error %d, req->newlen %lu, sizeof(keystring) %lu",
		    __func__, error, req->newlen, sizeof(keystring));
		goto exit;
	}
	if (req->newptr == USER_ADDR_NULL) {
		goto exit;
	}

	ks_len = strbuflen(keystring, sizeof(keystring));
	if (ks_len != TCP_FASTOPEN_KEYLEN * 2) {
		os_log(OS_LOG_DEFAULT,
		    "%s: strlen(keystring) %lu != TCP_FASTOPEN_KEYLEN * 2 %u, newlen %lu",
		    __func__, ks_len, TCP_FASTOPEN_KEYLEN * 2, req->newlen);
		error = EINVAL;
		goto exit;
	}

	for (i = 0; i < (TCP_FASTOPEN_KEYLEN / sizeof(u_int32_t)); i++) {
		/*
		 * We jump over the keystring in 8-character (4 byte in hex)
		 * steps
		 */
		if (sscanf(__unsafe_null_terminated_from_indexable(&keystring[i * 8]), "%8x", &key[i]) != 1) {
			error = EINVAL;
			os_log(OS_LOG_DEFAULT,
			    "%s: sscanf() != 1, error EINVAL", __func__);
			goto exit;
		}
	}

	aes_encrypt_key128((u_char *)key, &tfo_ctx);

exit:
	return error;
}

int
get_inpcb_str_size(void)
{
	return sizeof(struct inpcb);
}

int
get_tcp_str_size(void)
{
	return sizeof(struct tcpcb);
}

static int scale_to_powerof2(int size);

/*
 * This helper routine returns one of the following scaled value of size:
 * 1. Rounded down power of two value of size if the size value passed as
 *    argument is not a power of two and the rounded up value overflows.
 * OR
 * 2. Rounded up power of two value of size if the size value passed as
 *    argument is not a power of two and the rounded up value does not overflow
 * OR
 * 3. Same value as argument size if it is already a power of two.
 */
static int
scale_to_powerof2(int size)
{
	/* Handle special case of size = 0 */
	int ret = size ? size : 1;

	if (!powerof2(ret)) {
		while (!powerof2(size)) {
			/*
			 * Clear out least significant
			 * set bit till size is left with
			 * its highest set bit at which point
			 * it is rounded down power of two.
			 */
			size = size & (size - 1);
		}

		/* Check for overflow when rounding up */
		if (0 == (size << 1)) {
			ret = size;
		} else {
			ret = size << 1;
		}
	}

	return ret;
}

/*
 * Round the floating point to the next integer
 * Eg. 1.3 will round up to 2.
 */
uint32_t
tcp_ceil(double a)
{
	double res = (uint32_t) a;
	return (uint32_t)(res + (res < a));
}

uint32_t
tcp_round_to(uint32_t val, uint32_t round)
{
	/*
	 * Round up or down based on the middle. Meaning, if we round upon a
	 * multiple of 10, 16 will round to 20 and 14 will round to 10.
	 */
	return ((val + (round / 2)) / round) * round;
}

/*
 * Round up to the next multiple of base.
 * Eg. for a base of 64, 65 will become 128,
 * 2896 will become 2944.
 */
uint32_t
tcp_round_up(uint32_t val, uint32_t base)
{
	if (base == 1 || val % base == 0) {
		return val;
	}

	return ((val + base) / base) * base;
}

uint32_t
ntoh24(u_char *p __sized_by(3))
{
	uint32_t v;

	v  = (uint32_t)(p[0] << 16);
	v |= (uint32_t)(p[1] << 8);
	v |= (uint32_t)(p[2] << 0);
	return v;
}

uint32_t
tcp_packets_this_ack(struct tcpcb *tp, uint32_t acked)
{
	return acked / tp->t_maxseg +
	       (((acked % tp->t_maxseg) != 0) ? 1 : 0);
}

static void
tcp_tfo_init(void)
{
	u_char key[TCP_FASTOPEN_KEYLEN];

	read_frandom(key, sizeof(key));
	aes_encrypt_key128(key, &tfo_ctx);
}

static u_char isn_secret[32];

/*
 * Tcp initialization
 */
void
tcp_init(struct protosw *pp, struct domain *dp)
{
#pragma unused(dp)
	static int tcp_initialized = 0;
	struct inpcbinfo *pcbinfo;

	VERIFY((pp->pr_flags & (PR_INITIALIZED | PR_ATTACHED)) == PR_ATTACHED);

	if (tcp_initialized) {
		return;
	}
	tcp_initialized = 1;

#if DEBUG || DEVELOPMENT
	(void) PE_parse_boot_argn("tcp_rxt_seg_max", &tcp_rxt_seg_max,
	    sizeof(tcp_rxt_seg_max));
#endif /* DEBUG || DEVELOPMENT */

	tcp_ccgen = 1;
	tcp_keepinit = TCPTV_KEEP_INIT;
	tcp_keepidle = TCPTV_KEEP_IDLE;
	tcp_keepintvl = TCPTV_KEEPINTVL;
	tcp_keepcnt = TCPTV_KEEPCNT;
	tcp_maxpersistidle = TCPTV_KEEP_IDLE;
	tcp_msl = TCPTV_MSL;

	microuptime(&tcp_uptime);
	read_frandom(&tcp_now, sizeof(tcp_now));

	/* Starts tcp internal clock at a random value */
	tcp_now = tcp_now & 0x3fffffff;

	/* expose initial uptime/now via systcl for utcp to keep time sync */
	tcp_now_init = tcp_now;
	tcp_microuptime_init =
	    (uint32_t)(tcp_uptime.tv_usec + (tcp_uptime.tv_sec * USEC_PER_SEC));
	SYSCTL_SKMEM_UPDATE_FIELD(tcp.microuptime_init, tcp_microuptime_init);
	SYSCTL_SKMEM_UPDATE_FIELD(tcp.now_init, tcp_now_init);

	tcp_tfo_init();

	LIST_INIT(&tcb);
	tcbinfo.ipi_listhead = &tcb;

	pcbinfo = &tcbinfo;

	/*
	 * allocate group, lock attributes and lock for tcp pcb mutexes
	 */
	pcbinfo->ipi_lock_grp = lck_grp_alloc_init("tcppcb",
	    LCK_GRP_ATTR_NULL);
	lck_attr_setdefault(&pcbinfo->ipi_lock_attr);
	lck_rw_init(&pcbinfo->ipi_lock, pcbinfo->ipi_lock_grp,
	    &pcbinfo->ipi_lock_attr);

	if (tcp_tcbhashsize == 0) {
		/* Set to default */
		tcp_tcbhashsize = 512;
	}

	if (!powerof2(tcp_tcbhashsize)) {
		int old_hash_size = tcp_tcbhashsize;
		tcp_tcbhashsize = scale_to_powerof2(tcp_tcbhashsize);
		/* Lower limit of 16  */
		if (tcp_tcbhashsize < 16) {
			tcp_tcbhashsize = 16;
		}
		printf("WARNING: TCB hash size not a power of 2, "
		    "scaled from %d to %d.\n",
		    old_hash_size,
		    tcp_tcbhashsize);
	}

	hashinit_counted_by(tcp_tcbhashsize, tcbinfo.ipi_hashbase,
	    tcbinfo.ipi_hashbase_count);
	tcbinfo.ipi_hashmask = tcbinfo.ipi_hashbase_count - 1;
	hashinit_counted_by(tcp_tcbhashsize, tcbinfo.ipi_porthashbase,
	    tcbinfo.ipi_porthashbase_count);
	tcbinfo.ipi_porthashmask = tcbinfo.ipi_porthashbase_count - 1;
	tcbinfo.ipi_zone = tcpcbzone;

	tcbinfo.ipi_gc = tcp_gc;
	tcbinfo.ipi_timer = tcp_itimer;
	in_pcbinfo_attach(&tcbinfo);

#define TCP_MINPROTOHDR (sizeof(struct ip6_hdr) + sizeof(struct tcphdr))
	if (max_protohdr < TCP_MINPROTOHDR) {
		max_protohdr = (int)P2ROUNDUP(TCP_MINPROTOHDR, sizeof(uint32_t));
	}
	if (max_linkhdr + max_protohdr > MCLBYTES) {
		panic("tcp_init");
	}
#undef TCP_MINPROTOHDR

	/* Initialize time wait and timer lists */
	TAILQ_INIT(&tcp_tw_tailq);

	bzero(&tcp_timer_list, sizeof(tcp_timer_list));
	LIST_INIT(&tcp_timer_list.lhead);
	/*
	 * allocate group and attribute for the tcp timer list
	 */
	tcp_timer_list.mtx_grp = lck_grp_alloc_init("tcptimerlist",
	    LCK_GRP_ATTR_NULL);
	lck_mtx_init(&tcp_timer_list.mtx, tcp_timer_list.mtx_grp,
	    LCK_ATTR_NULL);

	tcp_timer_list.call = thread_call_allocate(tcp_run_timerlist, NULL);
	if (tcp_timer_list.call == NULL) {
		panic("failed to allocate call entry 1 in tcp_init");
	}

	/* Initialize TCP Cache */
	tcp_cache_init();

	tcp_mpkl_log_object = MPKL_CREATE_LOGOBJECT("com.apple.xnu.tcp");
	if (tcp_mpkl_log_object == NULL) {
		panic("MPKL_CREATE_LOGOBJECT failed");
	}

	if (PE_parse_boot_argn("tcp_log", &tcp_log_enable_flags, sizeof(tcp_log_enable_flags))) {
		os_log(OS_LOG_DEFAULT, "tcp_init: set tcp_log_enable_flags to 0x%x", tcp_log_enable_flags);
	}

	if (PE_parse_boot_argn("tcp_link_heuristics", &tcp_link_heuristics_flags, sizeof(tcp_link_heuristics_flags))) {
		os_log(OS_LOG_DEFAULT, "tcp_init: set tcp_link_heuristics_flags to 0x%x", tcp_link_heuristics_flags);
	}

	/*
	 * If more than 4GB of actual memory is available, increase the
	 * maximum allowed receive and send socket buffer size.
	 */
	if (mem_actual >= (1ULL << (GBSHIFT + 2))) {
		if (serverperfmode) {
			tcp_autorcvbuf_max = 8 * 1024 * 1024;
			tcp_autosndbuf_max = 8 * 1024 * 1024;
		} else {
			tcp_autorcvbuf_max = 4 * 1024 * 1024;
			tcp_autosndbuf_max = 4 * 1024 * 1024;
		}

		SYSCTL_SKMEM_UPDATE_FIELD(tcp.autorcvbufmax, tcp_autorcvbuf_max);
		SYSCTL_SKMEM_UPDATE_FIELD(tcp.autosndbufmax, tcp_autosndbuf_max);
	}

	/* Initialize the TCP CCA array */
	tcp_cc_init();

	read_frandom(&isn_secret, sizeof(isn_secret));
}

/*
 * Fill in the IP and TCP headers for an outgoing packet, given the tcpcb.
 * tcp_template used to store this data in mbufs, but we now recopy it out
 * of the tcpcb each time to conserve mbufs.
 */
void
tcp_fillheaders(struct mbuf *m, struct tcpcb *tp, void *ip_ptr, void *tcp_ptr)
{
	struct inpcb *inp = tp->t_inpcb;
	struct tcphdr *tcp_hdr = (struct tcphdr *)tcp_ptr;

	if ((inp->inp_vflag & INP_IPV6) != 0) {
		struct ip6_hdr *ip6;

		ip6 = (struct ip6_hdr *)ip_ptr;
		ip6->ip6_flow = (ip6->ip6_flow & ~IPV6_FLOWINFO_MASK) |
		    (inp->inp_flow & IPV6_FLOWINFO_MASK);
		ip6->ip6_vfc = (ip6->ip6_vfc & ~IPV6_VERSION_MASK) |
		    (IPV6_VERSION & IPV6_VERSION_MASK);
		ip6->ip6_plen = htons(sizeof(struct tcphdr));
		ip6->ip6_nxt = IPPROTO_TCP;
		ip6->ip6_hlim = 0;
		ip6->ip6_src = inp->in6p_laddr;
		ip6->ip6_dst = inp->in6p_faddr;
		if (m->m_flags & M_PKTHDR) {
			uint32_t lifscope = inp->inp_lifscope != 0 ? inp->inp_lifscope : inp->inp_fifscope;
			uint32_t fifscope = inp->inp_fifscope != 0 ? inp->inp_fifscope : inp->inp_lifscope;
			ip6_output_setsrcifscope(m, lifscope, NULL);
			ip6_output_setdstifscope(m, fifscope, NULL);
		}
		tcp_hdr->th_sum = in6_pseudo(&inp->in6p_laddr, &inp->in6p_faddr,
		    htonl(sizeof(struct tcphdr) + IPPROTO_TCP));
	} else {
		struct ip *ip = (struct ip *) ip_ptr;

		ip->ip_vhl = IP_VHL_BORING;
		ip->ip_tos = 0;
		ip->ip_len = 0;
		ip->ip_id = 0;
		ip->ip_off = 0;
		ip->ip_ttl = 0;
		ip->ip_sum = 0;
		ip->ip_p = IPPROTO_TCP;
		ip->ip_src = inp->inp_laddr;
		ip->ip_dst = inp->inp_faddr;
		tcp_hdr->th_sum =
		    in_pseudo(ip->ip_src.s_addr, ip->ip_dst.s_addr,
		    htons(sizeof(struct tcphdr) + IPPROTO_TCP));
	}

	tcp_hdr->th_sport = inp->inp_lport;
	tcp_hdr->th_dport = inp->inp_fport;
	tcp_hdr->th_seq = 0;
	tcp_hdr->th_ack = 0;
	tcp_hdr->th_x2 = 0;
	tcp_hdr->th_off = 5;
	tcp_hdr->th_flags = 0;
	tcp_hdr->th_win = 0;
	tcp_hdr->th_urp = 0;
}

/*
 * Create template to be used to send tcp packets on a connection.
 * Allocates an mbuf and fills in a skeletal tcp/ip header.  The only
 * use for this function is in keepalives, which use tcp_respond.
 */
struct tcptemp *
tcp_maketemplate(struct tcpcb *tp, struct mbuf **mp)
{
	struct mbuf *m;
	struct tcptemp *n;

	*mp = m = m_get(M_DONTWAIT, MT_HEADER);
	if (m == NULL) {
		return NULL;
	}
	m->m_len = sizeof(struct tcptemp);
	n = mtod(m, struct tcptemp *);

	tcp_fillheaders(m, tp, (void *)&n->tt_ipgen, (void *)&n->tt_t);
	return n;
}

/*
 * Send a single message to the TCP at address specified by
 * the given TCP/IP header.  If m == 0, then we make a copy
 * of the tcpiphdr at ti and send directly to the addressed host.
 * This is used to force keep alive messages out using the TCP
 * template for a connection.  If flags are given then we send
 * a message back to the TCP which originated the * segment ti,
 * and discard the mbuf containing it and any other attached mbufs.
 *
 * In any case the ack and sequence number of the transmitted
 * segment are as specified by the parameters.
 *
 * NOTE: If m != NULL, then ti must point to *inside* the mbuf.
 */
void
tcp_respond(struct tcpcb *tp, void *ipgen __sized_by(ipgen_size), size_t ipgen_size __unused, struct tcphdr *th, struct mbuf *m,
    tcp_seq ack, tcp_seq seq, uint8_t flags, struct tcp_respond_args *tra)
{
	uint16_t tlen;
	int win = 0;
	struct route *ro = 0;
	struct route sro;
	struct ip *ip;
	struct tcphdr *nth;
	struct route_in6 *ro6 = 0;
	struct route_in6 sro6;
	struct ip6_hdr *ip6;
	int isipv6;
	struct ifnet *outif;
	int sotc = SO_TC_UNSPEC;
	bool check_qos_marking_again = FALSE;
	uint32_t sifscope = IFSCOPE_NONE, fifscope = IFSCOPE_NONE;

	isipv6 = IP_VHL_V(((struct ip *)ipgen)->ip_vhl) == 6;
	ip6 = ipgen;
	ip = ipgen;

	if (tp) {
		check_qos_marking_again = tp->t_inpcb->inp_socket->so_flags1 & SOF1_QOSMARKING_POLICY_OVERRIDE ? FALSE : TRUE;
		sifscope = tp->t_inpcb->inp_lifscope;
		fifscope = tp->t_inpcb->inp_fifscope;
		if (!(flags & TH_RST)) {
			win = tcp_sbspace(tp);
			if (win > (int32_t)TCP_MAXWIN << tp->rcv_scale) {
				win = (int32_t)TCP_MAXWIN << tp->rcv_scale;
			}
		}
		if (isipv6) {
			ro6 = &tp->t_inpcb->in6p_route;
		} else {
			ro = &tp->t_inpcb->inp_route;
		}
	} else {
		if (isipv6) {
			ro6 = &sro6;
			bzero(ro6, sizeof(*ro6));
		} else {
			ro = &sro;
			bzero(ro, sizeof(*ro));
		}
	}
	if (m == 0) {
		m = m_gethdr(M_DONTWAIT, MT_HEADER);    /* MAC-OK */
		if (m == NULL) {
			return;
		}
		tlen = 0;
		m->m_data += max_linkhdr;
		if (isipv6) {
			VERIFY((MHLEN - max_linkhdr) >=
			    (sizeof(*ip6) + sizeof(*nth)));
			bcopy((caddr_t)ip6, mtod(m, caddr_t),
			    sizeof(struct ip6_hdr));
			ip6 = mtod(m, struct ip6_hdr *);
			nth = (struct tcphdr *)(void *)(ip6 + 1);
		} else {
			VERIFY((MHLEN - max_linkhdr) >=
			    (sizeof(*ip) + sizeof(*nth)));
			bcopy((caddr_t)ip, mtod(m, caddr_t), sizeof(struct ip));
			ip = mtod(m, struct ip *);
			nth = (struct tcphdr *)(void *)(ip + 1);
		}
		bcopy(th, nth, sizeof(struct tcphdr));
#if MPTCP
		if ((tp) && (tp->t_mpflags & TMPF_RESET)) {
			flags = (TH_RST | TH_ACK);
		} else
#endif
		flags = TH_ACK;
	} else {
		m_freem(m->m_next);
		m->m_next = 0;
		m->m_data = (uintptr_t)ipgen;
		/* m_len is set later */
		tlen = 0;
#define xchg(a, b, type) { type t; t = a; a = b; b = t; }
		if (isipv6) {
			ip6_getsrcifaddr_info(m, &sifscope, NULL);
			ip6_getdstifaddr_info(m, &fifscope, NULL);
			if (!in6_embedded_scope) {
				m->m_pkthdr.pkt_flags &= ~PKTF_IFAINFO;
			}
			/* Expect 32-bit aligned IP on strict-align platforms */
			IP6_HDR_STRICT_ALIGNMENT_CHECK(ip6);
			xchg(ip6->ip6_dst, ip6->ip6_src, struct in6_addr);
			nth = (struct tcphdr *)(void *)(ip6 + 1);
		} else {
			/* Expect 32-bit aligned IP on strict-align platforms */
			IP_HDR_STRICT_ALIGNMENT_CHECK(ip);
			xchg(ip->ip_dst.s_addr, ip->ip_src.s_addr, n_long);
			nth = (struct tcphdr *)(void *)(ip + 1);
		}
		if (th != nth) {
			/*
			 * this is usually a case when an extension header
			 * exists between the IPv6 header and the
			 * TCP header.
			 */
			nth->th_sport = th->th_sport;
			nth->th_dport = th->th_dport;
		}
		xchg(nth->th_dport, nth->th_sport, n_short);
#undef xchg
	}
	if (isipv6) {
		ip6->ip6_plen = htons((u_short)(sizeof(struct tcphdr) +
		    tlen));
		tlen += sizeof(struct ip6_hdr) + sizeof(struct tcphdr);
		ip6_output_setsrcifscope(m, sifscope, NULL);
		ip6_output_setdstifscope(m, fifscope, NULL);
	} else {
		tlen += sizeof(struct tcpiphdr);
		ip->ip_len = tlen;
		ip->ip_ttl = (uint8_t)ip_defttl;
	}
	m->m_len = tlen;
	m->m_pkthdr.len = tlen;
	m->m_pkthdr.rcvif = 0;
	if (tra->keep_alive) {
		m->m_pkthdr.pkt_flags |= PKTF_KEEPALIVE;
	}

	nth->th_seq = htonl(seq);
	nth->th_ack = htonl(ack);
	nth->th_x2 = 0;
	nth->th_off = sizeof(struct tcphdr) >> 2;
	nth->th_flags = flags;
	if (tp) {
		nth->th_win = htons((u_short) (win >> tp->rcv_scale));
	} else {
		nth->th_win = htons((u_short)win);
	}
	nth->th_urp = 0;
	if (isipv6) {
		nth->th_sum = 0;
		nth->th_sum = in6_pseudo(&ip6->ip6_src, &ip6->ip6_dst,
		    htonl((tlen - sizeof(struct ip6_hdr)) + IPPROTO_TCP));
		m->m_pkthdr.csum_flags = CSUM_TCPIPV6;
		m->m_pkthdr.csum_data = offsetof(struct tcphdr, th_sum);
		ip6->ip6_hlim = in6_selecthlim(tp ? tp->t_inpcb : NULL,
		    ro6 && ro6->ro_rt ? ro6->ro_rt->rt_ifp : NULL);
	} else {
		nth->th_sum = in_pseudo(ip->ip_src.s_addr, ip->ip_dst.s_addr,
		    htons((u_short)(tlen - sizeof(struct ip) + ip->ip_p)));
		m->m_pkthdr.csum_flags = CSUM_TCP;
		m->m_pkthdr.csum_data = offsetof(struct tcphdr, th_sum);
	}
#if NECP
	necp_mark_packet_from_socket(m, tp ? tp->t_inpcb : NULL, 0, 0, 0, 0);
#endif /* NECP */

#if IPSEC
	if (tp != NULL && tp->t_inpcb->inp_sp != NULL &&
	    ipsec_setsocket(m, tp ? tp->t_inpcb->inp_socket : NULL) != 0) {
		m_freem(m);
		return;
	}
#endif

	if (tp != NULL) {
		u_int32_t svc_flags = 0;
		if (isipv6) {
			svc_flags |= PKT_SCF_IPV6;
		}
		sotc = tp->t_inpcb->inp_socket->so_traffic_class;
		if ((flags & TH_RST) == 0) {
			set_packet_service_class(m, tp->t_inpcb->inp_socket,
			    sotc, svc_flags);
		} else {
			m_set_service_class(m, MBUF_SC_BK_SYS);
		}

		/* Embed flowhash and flow control flags */
		m->m_pkthdr.pkt_flowsrc = FLOWSRC_INPCB;
		m->m_pkthdr.pkt_flowid = tp->t_inpcb->inp_flowhash;
		m->m_pkthdr.pkt_flags |= (PKTF_FLOW_ID | PKTF_FLOW_LOCALSRC | PKTF_FLOW_ADV);
		m->m_pkthdr.pkt_proto = IPPROTO_TCP;
		m->m_pkthdr.tx_tcp_pid = tp->t_inpcb->inp_socket->last_pid;
		m->m_pkthdr.tx_tcp_e_pid = tp->t_inpcb->inp_socket->e_pid;

		if (flags & TH_RST) {
			m->m_pkthdr.comp_gencnt = tp->t_comp_ack_gencnt;
		}
	} else {
		if (flags & TH_RST) {
			m->m_pkthdr.comp_gencnt = TCP_ACK_COMPRESSION_DUMMY;
			m_set_service_class(m, MBUF_SC_BK_SYS);
		}
	}

	if (isipv6) {
		struct ip6_out_args ip6oa;
		bzero(&ip6oa, sizeof(ip6oa));
		ip6oa.ip6oa_boundif = tra->ifscope;
		ip6oa.ip6oa_flags = IP6OAF_SELECT_SRCIF | IP6OAF_BOUND_SRCADDR;
		ip6oa.ip6oa_sotc = SO_TC_UNSPEC;
		ip6oa.ip6oa_netsvctype = _NET_SERVICE_TYPE_UNSPEC;

		if (tra->ifscope != IFSCOPE_NONE) {
			ip6oa.ip6oa_flags |= IP6OAF_BOUND_IF;
		}
		if (tra->nocell) {
			ip6oa.ip6oa_flags |= IP6OAF_NO_CELLULAR;
		}
		if (tra->noexpensive) {
			ip6oa.ip6oa_flags |= IP6OAF_NO_EXPENSIVE;
		}
		if (tra->noconstrained) {
			ip6oa.ip6oa_flags |= IP6OAF_NO_CONSTRAINED;
		}
		if (tra->awdl_unrestricted) {
			ip6oa.ip6oa_flags |= IP6OAF_AWDL_UNRESTRICTED;
		}
		if (tra->intcoproc_allowed) {
			ip6oa.ip6oa_flags |= IP6OAF_INTCOPROC_ALLOWED;
		}
		if (tra->management_allowed) {
			ip6oa.ip6oa_flags |= IP6OAF_MANAGEMENT_ALLOWED;
		}
		if (tra->ultra_constrained_allowed) {
			ip6oa.ip6oa_flags |= IP6OAF_ULTRA_CONSTRAINED_ALLOWED;
		}
		ip6oa.ip6oa_sotc = sotc;
		if (tp != NULL) {
			if ((tp->t_inpcb->inp_socket->so_flags1 & SOF1_QOSMARKING_ALLOWED)) {
				ip6oa.ip6oa_flags |= IP6OAF_QOSMARKING_ALLOWED;
			}
			ip6oa.qos_marking_gencount = tp->t_inpcb->inp_policyresult.results.qos_marking_gencount;
			if (check_qos_marking_again) {
				ip6oa.ip6oa_flags |= IP6OAF_REDO_QOSMARKING_POLICY;
			}
			ip6oa.ip6oa_netsvctype = tp->t_inpcb->inp_socket->so_netsvctype;
		}
		(void) ip6_output(m, NULL, ro6, IPV6_OUTARGS, NULL,
		    NULL, &ip6oa);

		if (check_qos_marking_again) {
			struct inpcb *inp = tp->t_inpcb;
			inp->inp_policyresult.results.qos_marking_gencount = ip6oa.qos_marking_gencount;
			if (ip6oa.ip6oa_flags & IP6OAF_QOSMARKING_ALLOWED) {
				inp->inp_socket->so_flags1 |= SOF1_QOSMARKING_ALLOWED;
			} else {
				inp->inp_socket->so_flags1 &= ~SOF1_QOSMARKING_ALLOWED;
			}
		}

		if (tp != NULL && ro6 != NULL && ro6->ro_rt != NULL &&
		    (outif = ro6->ro_rt->rt_ifp) !=
		    tp->t_inpcb->in6p_last_outifp) {
			tp->t_inpcb->in6p_last_outifp = outif;
#if SKYWALK
			if (NETNS_TOKEN_VALID(&tp->t_inpcb->inp_netns_token)) {
				netns_set_ifnet(&tp->t_inpcb->inp_netns_token,
				    tp->t_inpcb->in6p_last_outifp);
			}
#endif /* SKYWALK */
		}

		if (ro6 == &sro6) {
			ROUTE_RELEASE(ro6);
		}
	} else {
		struct ip_out_args ipoa;
		bzero(&ipoa, sizeof(ipoa));
		ipoa.ipoa_boundif = tra->ifscope;
		ipoa.ipoa_flags = IPOAF_SELECT_SRCIF | IPOAF_BOUND_SRCADDR;
		ipoa.ipoa_sotc = SO_TC_UNSPEC;
		ipoa.ipoa_netsvctype = _NET_SERVICE_TYPE_UNSPEC;

		if (tra->ifscope != IFSCOPE_NONE) {
			ipoa.ipoa_flags |= IPOAF_BOUND_IF;
		}
		if (tra->nocell) {
			ipoa.ipoa_flags |= IPOAF_NO_CELLULAR;
		}
		if (tra->noexpensive) {
			ipoa.ipoa_flags |= IPOAF_NO_EXPENSIVE;
		}
		if (tra->noconstrained) {
			ipoa.ipoa_flags |= IPOAF_NO_CONSTRAINED;
		}
		if (tra->awdl_unrestricted) {
			ipoa.ipoa_flags |= IPOAF_AWDL_UNRESTRICTED;
		}
		if (tra->management_allowed) {
			ipoa.ipoa_flags |= IPOAF_MANAGEMENT_ALLOWED;
		}
		ipoa.ipoa_sotc = sotc;
		if (tp != NULL) {
			if ((tp->t_inpcb->inp_socket->so_flags1 & SOF1_QOSMARKING_ALLOWED)) {
				ipoa.ipoa_flags |= IPOAF_QOSMARKING_ALLOWED;
			}
			if (!(tp->t_inpcb->inp_socket->so_flags1 & SOF1_QOSMARKING_POLICY_OVERRIDE)) {
				ipoa.ipoa_flags |= IPOAF_REDO_QOSMARKING_POLICY;
			}
			ipoa.qos_marking_gencount = tp->t_inpcb->inp_policyresult.results.qos_marking_gencount;
			ipoa.ipoa_netsvctype = tp->t_inpcb->inp_socket->so_netsvctype;
		}
		if (ro != &sro) {
			/* Copy the cached route and take an extra reference */
			inp_route_copyout(tp->t_inpcb, &sro);
		}
		/*
		 * For consistency, pass a local route copy.
		 */
		(void) ip_output(m, NULL, &sro, IP_OUTARGS, NULL, &ipoa);

		if (check_qos_marking_again) {
			struct inpcb *inp = tp->t_inpcb;
			inp->inp_policyresult.results.qos_marking_gencount = ipoa.qos_marking_gencount;
			if (ipoa.ipoa_flags & IPOAF_QOSMARKING_ALLOWED) {
				inp->inp_socket->so_flags1 |= SOF1_QOSMARKING_ALLOWED;
			} else {
				inp->inp_socket->so_flags1 &= ~SOF1_QOSMARKING_ALLOWED;
			}
		}
		if (tp != NULL && sro.ro_rt != NULL &&
		    (outif = sro.ro_rt->rt_ifp) !=
		    tp->t_inpcb->inp_last_outifp) {
			tp->t_inpcb->inp_last_outifp = outif;
#if SKYWALK
			if (NETNS_TOKEN_VALID(&tp->t_inpcb->inp_netns_token)) {
				netns_set_ifnet(&tp->t_inpcb->inp_netns_token, outif);
			}
#endif /* SKYWALK */
		}
		if (ro != &sro) {
			/* Synchronize cached PCB route */
			inp_route_copyin(tp->t_inpcb, &sro);
		} else {
			ROUTE_RELEASE(&sro);
		}
	}
}

/*
 * Create a new TCP control block, making an
 * empty reassembly queue and hooking it to the argument
 * protocol control block.  The `inp' parameter must have
 * come from the zone allocator set up in tcp_init().
 */
struct tcpcb *
tcp_newtcpcb(struct inpcb *inp)
{
	struct inp_tp *it;
	struct tcpcb *tp;
	struct socket *so = inp->inp_socket;
	int isipv6 = (inp->inp_vflag & INP_IPV6) != 0;
	uint32_t random_32;

	calculate_tcp_clock();

	if ((so->so_flags1 & SOF1_CACHED_IN_SOCK_LAYER) == 0) {
		it = (struct inp_tp *)(void *)inp;
		tp = &it->tcb;
	} else {
		tp = (struct tcpcb *)(void *)inp->inp_saved_ppcb;
	}

	bzero((char *) tp, sizeof(struct tcpcb));
	LIST_INIT(&tp->t_segq);
	tp->t_maxseg = tp->t_maxopd = isipv6 ? tcp_v6mssdflt : tcp_mssdflt;

	tp->t_flags = TF_REQ_SCALE | (tcp_do_timestamps ? TF_REQ_TSTMP : 0);
	tp->t_flagsext |= TF_SACK_ENABLE;

	if (tcp_rack) {
		tp->t_flagsext |= TF_RACK_ENABLED;
	}

	TAILQ_INIT(&tp->snd_holes);
	SLIST_INIT(&tp->t_rxt_segments);
	TAILQ_INIT(&tp->t_segs_sent);
	RB_INIT(&tp->t_segs_sent_tree);
	TAILQ_INIT(&tp->t_segs_acked);
	TAILQ_INIT(&tp->seg_pool.free_segs);
	SLIST_INIT(&tp->t_notify_ack);
	tp->t_inpcb = inp;
	/*
	 * Init srtt to TCPTV_SRTTBASE (0), so we can tell that we have no
	 * rtt estimate.  Set rttvar so that srtt + 4 * rttvar gives
	 * reasonable initial retransmit time.
	 */
	tp->t_srtt = TCPTV_SRTTBASE;
	tp->t_rttvar =
	    ((TCPTV_RTOBASE - TCPTV_SRTTBASE) << TCP_RTTVAR_SHIFT) / 4;
	tp->t_rttmin = tcp_TCPTV_MIN;
	tp->t_rxtcur = TCPTV_RTOBASE;

	if (tcp_use_newreno) {
		/* use newreno by default */
		tp->tcp_cc_index = TCP_CC_ALGO_NEWRENO_INDEX;
#if (DEVELOPMENT || DEBUG)
	} else if (tcp_use_ledbat) {
		/* use ledbat for testing */
		tp->tcp_cc_index = TCP_CC_ALGO_BACKGROUND_INDEX;
#endif
	} else {
		if (TCP_L4S_ENABLED(tp)) {
			tp->tcp_cc_index = TCP_CC_ALGO_PRAGUE_INDEX;
		} else {
			tp->tcp_cc_index = TCP_CC_ALGO_CUBIC_INDEX;
		}
	}

	tcp_cc_allocate_state(tp);

	if (CC_ALGO(tp)->init != NULL) {
		CC_ALGO(tp)->init(tp);
	}

	/* Initialize rledbat if we are using recv_bg */
	if (tcp_rledbat == 1 && TCP_RECV_BG(inp->inp_socket) &&
	    tcp_cc_rledbat.init != NULL) {
		tcp_cc_rledbat.init(tp);
	}

	tp->snd_cwnd = tcp_initial_cwnd(tp);
	tp->snd_ssthresh = TCP_MAXWIN << TCP_MAX_WINSHIFT;
	tp->snd_ssthresh_prev = TCP_MAXWIN << TCP_MAX_WINSHIFT;
	tp->t_rcvtime = tcp_now;
	tp->tentry.timer_start = tcp_now;
	tp->rcv_unackwin = tcp_now;
	tp->t_persist_timeout = tcp_max_persist_timeout;
	tp->t_persist_stop = 0;
	tp->t_flagsext |= TF_RCVUNACK_WAITSS;
	tp->t_rexmtthresh = (uint8_t)tcprexmtthresh;
	tp->rack.reo_wnd_multi = 1;
	tp->rfbuf_ts = tcp_now;
	tp->rfbuf_space = tcp_initial_cwnd(tp);
	tp->t_forced_acks = TCP_FORCED_ACKS_COUNT;
	tp->bytes_lost = tp->bytes_sacked = tp->bytes_retransmitted = 0;

	/* Enable bandwidth measurement on this connection */
	tp->t_flagsext |= TF_MEASURESNDBW;
	if (tp->t_bwmeas == NULL) {
		tp->t_bwmeas = tcp_bwmeas_alloc(tp);
		if (tp->t_bwmeas == NULL) {
			tp->t_flagsext &= ~TF_MEASURESNDBW;
		}
	}

	/* Clear time wait tailq entry */
	tp->t_twentry.tqe_next = NULL;
	tp->t_twentry.tqe_prev = NULL;

	read_frandom(&random_32, sizeof(random_32));
	tp->t_comp_ack_gencnt = random_32;
	if (tp->t_comp_ack_gencnt <= TCP_ACK_COMPRESSION_DUMMY ||
	    tp->t_comp_ack_gencnt > INT_MAX) {
		tp->t_comp_ack_gencnt = TCP_ACK_COMPRESSION_DUMMY + 1;
	}
	tp->t_comp_ack_lastinc = tcp_now;

	/* Initialize Accurate ECN state */
	tp->t_client_accecn_state = tcp_connection_client_accurate_ecn_feature_disabled;
	tp->t_server_accecn_state = tcp_connection_server_accurate_ecn_feature_disabled;

	/*
	 * IPv4 TTL initialization is necessary for an IPv6 socket as well,
	 * because the socket may be bound to an IPv6 wildcard address,
	 * which may match an IPv4-mapped IPv6 address.
	 */
	inp->inp_ip_ttl = (uint8_t)ip_defttl;
	inp->inp_ppcb = (caddr_t)tp;
	return tp;            /* XXX */
}

/*
 * Drop a TCP connection, reporting
 * the specified error.  If connection is synchronized,
 * then send a RST to peer.
 */
struct tcpcb *
tcp_drop(struct tcpcb *tp, int errno)
{
	struct socket *so = tp->t_inpcb->inp_socket;
#if CONFIG_DTRACE
	struct inpcb *inp = tp->t_inpcb;
#endif

	if (TCPS_HAVERCVDSYN(tp->t_state)) {
		DTRACE_TCP4(state__change, void, NULL, struct inpcb *, inp,
		    struct tcpcb *, tp, int32_t, TCPS_CLOSED);
		TCP_LOG_STATE(tp, TCPS_CLOSED);
		tp->t_state = TCPS_CLOSED;
		(void) tcp_output(tp);
		tcpstat.tcps_drops++;
	} else {
		tcpstat.tcps_conndrops++;
	}
	if (errno == ETIMEDOUT && tp->t_softerror) {
		errno = tp->t_softerror;
	}
	so->so_error = (u_short)errno;

	TCP_LOG_CONNECTION_SUMMARY(tp);

	return tcp_close(tp);
}

void
tcp_getrt_rtt(struct tcpcb *tp, struct rtentry *rt)
{
	uint32_t rtt = rt->rt_rmx.rmx_rtt;

	TCP_LOG_RTM_RTT(tp, rt);

	if (rtt != 0 && tcp_init_rtt_from_cache != 0) {
		/*
		 * XXX the lock bit for RTT indicates that the value
		 * is also a minimum value; this is subject to time.
		 */
		if (rt->rt_rmx.rmx_locks & RTV_RTT) {
			tp->t_rttmin = rtt / (RTM_RTTUNIT / TCP_RETRANSHZ);
		} else {
			tp->t_rttmin = TCPTV_REXMTMIN;
		}

		tp->t_srtt =
		    rtt / (RTM_RTTUNIT / (TCP_RETRANSHZ * TCP_RTT_SCALE));
		tcpstat.tcps_usedrtt++;

		if (rt->rt_rmx.rmx_rttvar) {
			tp->t_rttvar = rt->rt_rmx.rmx_rttvar /
			    (RTM_RTTUNIT / (TCP_RETRANSHZ * TCP_RTTVAR_SCALE));
			tcpstat.tcps_usedrttvar++;
		} else {
			/* default variation is +- 1 rtt */
			tp->t_rttvar =
			    tp->t_srtt * TCP_RTTVAR_SCALE / TCP_RTT_SCALE;
		}

		/*
		 * The RTO formula in the route metric case is based on:
		 *     srtt + 4 * rttvar
		 * modulo the min, max and slop
		 */
		TCPT_RANGESET(tp->t_rxtcur,
		    TCP_REXMTVAL(tp),
		    tp->t_rttmin, TCPTV_REXMTMAX,
		    TCP_ADD_REXMTSLOP(tp));
	} else if (tp->t_state < TCPS_ESTABLISHED && tp->t_srtt == 0 &&
	    tp->t_rxtshift == 0) {
		struct ifnet *ifp = rt->rt_ifp;

		if (ifp != NULL && (ifp->if_eflags & IFEF_AWDL) != 0) {
			/*
			 * AWDL needs a special value for the default initial retransmission timeout
			 */
			if (tcp_awdl_rtobase > tcp_TCPTV_MIN) {
				tp->t_rttvar = ((tcp_awdl_rtobase - TCPTV_SRTTBASE) << TCP_RTTVAR_SHIFT) / 4;
			} else {
				tp->t_rttvar = ((tcp_TCPTV_MIN - TCPTV_SRTTBASE) << TCP_RTTVAR_SHIFT) / 4;
			}
			TCPT_RANGESET(tp->t_rxtcur,
			    TCP_REXMTVAL(tp),
			    tp->t_rttmin, TCPTV_REXMTMAX,
			    TCP_ADD_REXMTSLOP(tp));
		}
	}

	TCP_LOG_RTT_INFO(tp);
}

static inline void
tcp_create_ifnet_stats_per_flow(struct tcpcb *tp,
    struct ifnet_stats_per_flow *ifs)
{
	struct inpcb *inp;
	struct socket *so;
	if (tp == NULL || ifs == NULL) {
		return;
	}

	bzero(ifs, sizeof(*ifs));
	inp = tp->t_inpcb;
	so = inp->inp_socket;

	ifs->ipv4 = (inp->inp_vflag & INP_IPV6) ? 0 : 1;
	ifs->local = (tp->t_flags & TF_LOCAL) ? 1 : 0;
	ifs->connreset = (so->so_error == ECONNRESET) ? 1 : 0;
	ifs->conntimeout = (so->so_error == ETIMEDOUT) ? 1 : 0;
	ifs->ecn_flags = tp->ecn_flags;
	ifs->txretransmitbytes = tp->t_stat.txretransmitbytes;
	ifs->rxoutoforderbytes = tp->t_stat.rxoutoforderbytes;
	ifs->rxmitpkts = tp->t_stat.rxmitpkts;
	ifs->rcvoopack = tp->t_rcvoopack;
	ifs->pawsdrop = tp->t_pawsdrop;
	ifs->sack_recovery_episodes = tp->t_sack_recovery_episode;
	ifs->reordered_pkts = tp->t_reordered_pkts;
	ifs->dsack_sent = tp->t_dsack_sent;
	ifs->dsack_recvd = tp->t_dsack_recvd;
	ifs->srtt = tp->t_srtt;
	ifs->rttupdated = tp->t_rttupdated;
	ifs->rttvar = tp->t_rttvar;
	ifs->rttmin = get_base_rtt(tp);
	if (tp->t_bwmeas != NULL && tp->t_bwmeas->bw_sndbw_max > 0) {
		ifs->bw_sndbw_max = tp->t_bwmeas->bw_sndbw_max;
	} else {
		ifs->bw_sndbw_max = 0;
	}
	if (tp->t_bwmeas != NULL && tp->t_bwmeas->bw_rcvbw_max > 0) {
		ifs->bw_rcvbw_max = tp->t_bwmeas->bw_rcvbw_max;
	} else {
		ifs->bw_rcvbw_max = 0;
	}
	ifs->bk_txpackets = so->so_tc_stats[MBUF_TC_BK].txpackets;
	ifs->txpackets = inp->inp_stat->txpackets;
	ifs->rxpackets = inp->inp_stat->rxpackets;
}

static inline void
tcp_flow_ecn_perf_stats(struct ifnet_stats_per_flow *ifs,
    struct if_tcp_ecn_perf_stat *stat)
{
	u_int64_t curval, oldval;
	stat->total_txpkts += ifs->txpackets;
	stat->total_rxpkts += ifs->rxpackets;
	stat->total_rxmitpkts += ifs->rxmitpkts;
	stat->total_oopkts += ifs->rcvoopack;
	stat->total_reorderpkts += (ifs->reordered_pkts +
	    ifs->pawsdrop + ifs->dsack_sent + ifs->dsack_recvd);

	/* Average RTT */
	curval = ifs->srtt >> TCP_RTT_SHIFT;
	if (curval > 0 && ifs->rttupdated >= 16) {
		if (stat->rtt_avg == 0) {
			stat->rtt_avg = curval;
		} else {
			oldval = stat->rtt_avg;
			stat->rtt_avg = ((oldval << 4) - oldval + curval) >> 4;
		}
	}

	/* RTT variance */
	curval = ifs->rttvar >> TCP_RTTVAR_SHIFT;
	if (curval > 0 && ifs->rttupdated >= 16) {
		if (stat->rtt_var == 0) {
			stat->rtt_var = curval;
		} else {
			oldval = stat->rtt_var;
			stat->rtt_var =
			    ((oldval << 4) - oldval + curval) >> 4;
		}
	}

	/* SACK episodes */
	stat->sack_episodes += ifs->sack_recovery_episodes;
	if (ifs->connreset) {
		stat->rst_drop++;
	}
}

static inline void
tcp_flow_lim_stats(struct ifnet_stats_per_flow *ifs,
    struct if_lim_perf_stat *stat)
{
	u_int64_t curval, oldval;

	stat->lim_total_txpkts += ifs->txpackets;
	stat->lim_total_rxpkts += ifs->rxpackets;
	stat->lim_total_retxpkts += ifs->rxmitpkts;
	stat->lim_total_oopkts += ifs->rcvoopack;

	if (ifs->bw_sndbw_max > 0) {
		/* convert from bytes per ms to bits per second */
		ifs->bw_sndbw_max *= 8000;
		stat->lim_ul_max_bandwidth = MAX(stat->lim_ul_max_bandwidth,
		    ifs->bw_sndbw_max);
	}

	if (ifs->bw_rcvbw_max > 0) {
		/* convert from bytes per ms to bits per second */
		ifs->bw_rcvbw_max *= 8000;
		stat->lim_dl_max_bandwidth = MAX(stat->lim_dl_max_bandwidth,
		    ifs->bw_rcvbw_max);
	}

	/* Average RTT */
	curval = ifs->srtt >> TCP_RTT_SHIFT;
	if (curval > 0 && ifs->rttupdated >= 16) {
		if (stat->lim_rtt_average == 0) {
			stat->lim_rtt_average = curval;
		} else {
			oldval = stat->lim_rtt_average;
			stat->lim_rtt_average =
			    ((oldval << 4) - oldval + curval) >> 4;
		}
	}

	/* RTT variance */
	curval = ifs->rttvar >> TCP_RTTVAR_SHIFT;
	if (curval > 0 && ifs->rttupdated >= 16) {
		if (stat->lim_rtt_variance == 0) {
			stat->lim_rtt_variance = curval;
		} else {
			oldval = stat->lim_rtt_variance;
			stat->lim_rtt_variance =
			    ((oldval << 4) - oldval + curval) >> 4;
		}
	}

	if (stat->lim_rtt_min == 0) {
		stat->lim_rtt_min = ifs->rttmin;
	} else {
		stat->lim_rtt_min = MIN(stat->lim_rtt_min, ifs->rttmin);
	}

	/* connection timeouts */
	stat->lim_conn_attempts++;
	if (ifs->conntimeout) {
		stat->lim_conn_timeouts++;
	}

	/* bytes sent using background delay-based algorithms */
	stat->lim_bk_txpkts += ifs->bk_txpackets;
}

/*
 * Close a TCP control block:
 *	discard all space held by the tcp
 *	discard internet protocol block
 *	wake up any sleepers
 */
struct tcpcb *
tcp_close(struct tcpcb *tp)
{
	struct inpcb *inp = tp->t_inpcb;
	struct socket *so = inp->inp_socket;
	int isipv6 = (inp->inp_vflag & INP_IPV6) != 0;
	struct route *ro;
	struct rtentry *rt;
	int dosavessthresh;
	struct ifnet_stats_per_flow ifs;

	/* tcp_close was called previously, bail */
	if (inp->inp_ppcb == NULL) {
		return NULL;
	}

	tcp_del_fsw_flow(tp);

	tcp_canceltimers(tp);
	KERNEL_DEBUG(DBG_FNC_TCP_CLOSE | DBG_FUNC_START, tp, 0, 0, 0, 0);

	/*
	 * If another thread for this tcp is currently in ip (indicated by
	 * the TF_SENDINPROG flag), defer the cleanup until after it returns
	 * back to tcp.  This is done to serialize the close until after all
	 * pending output is finished, in order to avoid having the PCB be
	 * detached and the cached route cleaned, only for ip to cache the
	 * route back into the PCB again.  Note that we've cleared all the
	 * timers at this point.  Set TF_CLOSING to indicate to tcp_output()
	 * that is should call us again once it returns from ip; at that
	 * point both flags should be cleared and we can proceed further
	 * with the cleanup.
	 */
	if ((tp->t_flags & TF_CLOSING) ||
	    inp->inp_sndinprog_cnt > 0) {
		tp->t_flags |= TF_CLOSING;
		return NULL;
	}

	TCP_LOG_CONNECTION_SUMMARY(tp);

	DTRACE_TCP4(state__change, void, NULL, struct inpcb *, inp,
	    struct tcpcb *, tp, int32_t, TCPS_CLOSED);

	ro = (isipv6 ? (struct route *)&inp->in6p_route : &inp->inp_route);
	rt = ro->ro_rt;
	if (rt != NULL) {
		RT_LOCK_SPIN(rt);
	}

	/*
	 * If we got enough samples through the srtt filter,
	 * save the rtt and rttvar in the routing entry.
	 * 'Enough' is arbitrarily defined as the 16 samples.
	 * 16 samples is enough for the srtt filter to converge
	 * to within 5% of the correct value; fewer samples and
	 * we could save a very bogus rtt.
	 *
	 * Don't update the default route's characteristics and don't
	 * update anything that the user "locked".
	 */
	if (tp->t_rttupdated >= 16) {
		u_int32_t i = 0;
		bool log_rtt = false;

		if (isipv6) {
			struct sockaddr_in6 *sin6;

			if (rt == NULL) {
				goto no_valid_rt;
			}
			sin6 = SIN6(rt_key(rt));
			if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr)) {
				goto no_valid_rt;
			}
		} else if (ROUTE_UNUSABLE(ro) ||
		    SIN(rt_key(rt))->sin_addr.s_addr == INADDR_ANY) {
			DTRACE_TCP4(state__change, void, NULL,
			    struct inpcb *, inp, struct tcpcb *, tp,
			    int32_t, TCPS_CLOSED);
			TCP_LOG_STATE(tp, TCPS_CLOSED);
			tp->t_state = TCPS_CLOSED;
			goto no_valid_rt;
		}

		RT_LOCK_ASSERT_HELD(rt);
		if ((rt->rt_rmx.rmx_locks & RTV_RTT) == 0) {
			i = tp->t_srtt *
			    (RTM_RTTUNIT / (TCP_RETRANSHZ * TCP_RTT_SCALE));
			if (rt->rt_rmx.rmx_rtt && i) {
				/*
				 * filter this update to half the old & half
				 * the new values, converting scale.
				 * See route.h and tcp_var.h for a
				 * description of the scaling constants.
				 */
				rt->rt_rmx.rmx_rtt =
				    (rt->rt_rmx.rmx_rtt + i) / 2;
			} else {
				rt->rt_rmx.rmx_rtt = i;
			}
			tcpstat.tcps_cachedrtt++;
			log_rtt = true;
		}
		if ((rt->rt_rmx.rmx_locks & RTV_RTTVAR) == 0) {
			i = tp->t_rttvar *
			    (RTM_RTTUNIT / (TCP_RETRANSHZ * TCP_RTTVAR_SCALE));
			if (rt->rt_rmx.rmx_rttvar && i) {
				rt->rt_rmx.rmx_rttvar =
				    (rt->rt_rmx.rmx_rttvar + i) / 2;
			} else {
				rt->rt_rmx.rmx_rttvar = i;
			}
			tcpstat.tcps_cachedrttvar++;
			log_rtt = true;
		}
		if (log_rtt) {
			TCP_LOG_RTM_RTT(tp, rt);
			TCP_LOG_RTT_INFO(tp);
		}
		/*
		 * The old comment here said:
		 * update the pipelimit (ssthresh) if it has been updated
		 * already or if a pipesize was specified & the threshhold
		 * got below half the pipesize.  I.e., wait for bad news
		 * before we start updating, then update on both good
		 * and bad news.
		 *
		 * But we want to save the ssthresh even if no pipesize is
		 * specified explicitly in the route, because such
		 * connections still have an implicit pipesize specified
		 * by the global tcp_sendspace.  In the absence of a reliable
		 * way to calculate the pipesize, it will have to do.
		 */
		i = tp->snd_ssthresh;
		if (rt->rt_rmx.rmx_sendpipe != 0) {
			dosavessthresh = (i < rt->rt_rmx.rmx_sendpipe / 2);
		} else {
			dosavessthresh = (i < so->so_snd.sb_hiwat / 2);
		}
		if (((rt->rt_rmx.rmx_locks & RTV_SSTHRESH) == 0 &&
		    i != 0 && rt->rt_rmx.rmx_ssthresh != 0) ||
		    dosavessthresh) {
			/*
			 * convert the limit from user data bytes to
			 * packets then to packet data bytes.
			 */
			i = (i + tp->t_maxseg / 2) / tp->t_maxseg;
			if (i < 2) {
				i = 2;
			}
			i *= (u_int32_t)(tp->t_maxseg +
			    isipv6 ? sizeof(struct ip6_hdr) +
			    sizeof(struct tcphdr) :
			    sizeof(struct tcpiphdr));
			if (rt->rt_rmx.rmx_ssthresh) {
				rt->rt_rmx.rmx_ssthresh =
				    (rt->rt_rmx.rmx_ssthresh + i) / 2;
			} else {
				rt->rt_rmx.rmx_ssthresh = i;
			}
			tcpstat.tcps_cachedssthresh++;
		}
	}

	/*
	 * Mark route for deletion if no information is cached.
	 */
	if (rt != NULL && (so->so_flags & SOF_OVERFLOW)) {
		if (!(rt->rt_rmx.rmx_locks & RTV_RTT) &&
		    rt->rt_rmx.rmx_rtt == 0) {
			rt->rt_flags |= RTF_DELCLONE;
		}
	}

no_valid_rt:
	if (rt != NULL) {
		RT_UNLOCK(rt);
	}

	/* free the reassembly queue, if any */
	(void) tcp_freeq(tp);

	/* performance stats per interface */
	tcp_create_ifnet_stats_per_flow(tp, &ifs);
	tcp_update_stats_per_flow(&ifs, inp->inp_last_outifp);

	tcp_free_sackholes(tp);
	tcp_notify_ack_free(tp);

	inp_decr_sndbytes_allunsent(so, tp->snd_una);

	if (tp->t_bwmeas != NULL) {
		tcp_bwmeas_free(tp);
	}
	tcp_rxtseg_clean(tp);
	tcp_segs_sent_clean(tp, true);

	/* Free the packet list */
	if (tp->t_pktlist_head != NULL) {
		m_freem_list(tp->t_pktlist_head);
	}
	TCP_PKTLIST_CLEAR(tp);

	if (so->so_flags1 & SOF1_CACHED_IN_SOCK_LAYER) {
		inp->inp_saved_ppcb = (caddr_t) tp;
	}

	TCP_LOG_STATE(tp, TCPS_CLOSED);
	tp->t_state = TCPS_CLOSED;

	/*
	 * Issue a wakeup before detach so that we don't miss
	 * a wakeup
	 */
	sodisconnectwakeup(so);

	/*
	 * Make sure to clear the TCP Keep Alive Offload as it is
	 * ref counted on the interface
	 */
	tcp_clear_keep_alive_offload(so);

	/*
	 * If this is a socket that does not want to wakeup the device
	 * for it's traffic, the application might need to know that the
	 * socket is closed, send a notification.
	 */
	if ((so->so_options & SO_NOWAKEFROMSLEEP) &&
	    inp->inp_state != INPCB_STATE_DEAD &&
	    !(inp->inp_flags2 & INP2_TIMEWAIT)) {
		socket_post_kev_msg_closed(so);
	}

	if (CC_ALGO(tp)->cleanup != NULL) {
		CC_ALGO(tp)->cleanup(tp);
	}

	tp->tcp_cc_index = TCP_CC_ALGO_NONE;

	if (TCP_USE_RLEDBAT(tp, so) && tcp_cc_rledbat.cleanup != NULL) {
		tcp_cc_rledbat.cleanup(tp);
	}

	/* Can happen if we close the socket before receiving the third ACK */
	if ((tp->t_tfo_flags & TFO_F_COOKIE_VALID)) {
		OSDecrementAtomic(&tcp_tfo_halfcnt);

		/* Panic if something has gone terribly wrong. */
		VERIFY(tcp_tfo_halfcnt >= 0);

		tp->t_tfo_flags &= ~TFO_F_COOKIE_VALID;
	}

	if (SOCK_CHECK_DOM(so, PF_INET6)) {
		in6_pcbdetach(inp);
	} else {
		in_pcbdetach(inp);
	}

	/*
	 * Call soisdisconnected after detach because it might unlock the socket
	 */
	soisdisconnected(so);
	tcpstat.tcps_closed++;
	KERNEL_DEBUG(DBG_FNC_TCP_CLOSE | DBG_FUNC_END,
	    tcpstat.tcps_closed, 0, 0, 0, 0);
	return NULL;
}

int
tcp_freeq(struct tcpcb *tp)
{
	struct tseg_qent *q;
	int rv = 0;
	int count = 0;

	while ((q = LIST_FIRST(&tp->t_segq)) != NULL) {
		LIST_REMOVE(q, tqe_q);
		tp->t_reassq_mbcnt -= _MSIZE + (q->tqe_m->m_flags & M_EXT) ?
		    q->tqe_m->m_ext.ext_size : 0;
		m_freem(q->tqe_m);
		zfree(tcp_reass_zone, q);
		rv = 1;
		count++;
	}
	tp->t_reassqlen = 0;
	if (count > 0) {
		OSAddAtomic(-count, &tcp_reass_total_qlen);
	}
	return rv;
}


void
tcp_drain(void)
{
	struct inpcb *inp;
	struct tcpcb *tp;

	if (!lck_rw_try_lock_exclusive(&tcbinfo.ipi_lock)) {
		return;
	}

	LIST_FOREACH(inp, tcbinfo.ipi_listhead, inp_list) {
		if (in_pcb_checkstate(inp, WNT_ACQUIRE, 0) !=
		    WNT_STOPUSING) {
			socket_lock(inp->inp_socket, 1);
			if (in_pcb_checkstate(inp, WNT_RELEASE, 1)
			    == WNT_STOPUSING) {
				/* lost a race, try the next one */
				socket_unlock(inp->inp_socket, 1);
				continue;
			}
			tp = intotcpcb(inp);

			so_drain_extended_bk_idle(inp->inp_socket);

			socket_unlock(inp->inp_socket, 1);
		}
	}
	lck_rw_done(&tcbinfo.ipi_lock);
}

/*
 * Notify a tcp user of an asynchronous error;
 * store error as soft error, but wake up user
 * (for now, won't do anything until can select for soft error).
 *
 * Do not wake up user since there currently is no mechanism for
 * reporting soft errors (yet - a kqueue filter may be added).
 */
static void
tcp_notify(struct inpcb *inp, int error)
{
	struct tcpcb *tp;

	if (inp == NULL || (inp->inp_state == INPCB_STATE_DEAD)) {
		return; /* pcb is gone already */
	}
	tp = (struct tcpcb *)inp->inp_ppcb;

	VERIFY(tp != NULL);
	/*
	 * Ignore some errors if we are hooked up.
	 * If connection hasn't completed, has retransmitted several times,
	 * and receives a second error, give up now.  This is better
	 * than waiting a long time to establish a connection that
	 * can never complete.
	 */
	if (tp->t_state == TCPS_ESTABLISHED &&
	    (error == EHOSTUNREACH || error == ENETUNREACH ||
	    error == EHOSTDOWN)) {
		if (inp->inp_route.ro_rt) {
			rtfree(inp->inp_route.ro_rt);
			inp->inp_route.ro_rt = (struct rtentry *)NULL;
		}
	} else if (tp->t_state < TCPS_ESTABLISHED && tp->t_rxtshift > 3 &&
	    tp->t_softerror) {
		tcp_drop(tp, error);
	} else {
		tp->t_softerror = error;
	}
}

struct bwmeas *
tcp_bwmeas_alloc(struct tcpcb *tp)
{
	struct bwmeas *elm;
	elm = zalloc_flags(tcp_bwmeas_zone, Z_ZERO | Z_WAITOK);
	elm->bw_minsizepkts = TCP_BWMEAS_BURST_MINSIZE;
	elm->bw_minsize = elm->bw_minsizepkts * tp->t_maxseg;
	return elm;
}

void
tcp_bwmeas_free(struct tcpcb *tp)
{
	zfree(tcp_bwmeas_zone, tp->t_bwmeas);
	tp->t_bwmeas = NULL;
	tp->t_flagsext &= ~(TF_MEASURESNDBW);
}

int
get_tcp_inp_list(struct inpcb * __single *inp_list __counted_by(n), size_t n, inp_gen_t gencnt)
{
	struct tcpcb *tp;
	struct inpcb *inp;
	int i = 0;

	LIST_FOREACH(inp, tcbinfo.ipi_listhead, inp_list) {
		if (i >= n) {
			break;
		}
		if (inp->inp_gencnt <= gencnt &&
		    inp->inp_state != INPCB_STATE_DEAD) {
			inp_list[i++] = inp;
		}
	}

	TAILQ_FOREACH(tp, &tcp_tw_tailq, t_twentry) {
		if (i >= n) {
			break;
		}
		inp = tp->t_inpcb;
		if (inp->inp_gencnt <= gencnt &&
		    inp->inp_state != INPCB_STATE_DEAD) {
			inp_list[i++] = inp;
		}
	}
	return i;
}

/*
 * tcpcb_to_otcpcb copies specific bits of a tcpcb to a otcpcb format.
 * The otcpcb data structure is passed to user space and must not change.
 */
static void
tcpcb_to_otcpcb(struct tcpcb *tp, struct otcpcb *otp)
{
	otp->t_segq = (uint32_t)VM_KERNEL_ADDRHASH(tp->t_segq.lh_first);
	otp->t_dupacks = tp->t_dupacks;
	otp->t_timer[TCPT_REXMT_EXT] = tp->t_timer[TCPT_REXMT];
	otp->t_timer[TCPT_PERSIST_EXT] = tp->t_timer[TCPT_PERSIST];
	otp->t_timer[TCPT_KEEP_EXT] = tp->t_timer[TCPT_KEEP];
	otp->t_timer[TCPT_2MSL_EXT] = tp->t_timer[TCPT_2MSL];
	otp->t_inpcb =
	    (_TCPCB_PTR(struct inpcb *))VM_KERNEL_ADDRHASH(tp->t_inpcb);
	otp->t_state = tp->t_state;
	otp->t_flags = tp->t_flags;
	otp->t_force = (tp->t_flagsext & TF_FORCE) ? 1 : 0;
	otp->snd_una = tp->snd_una;
	otp->snd_max = tp->snd_max;
	otp->snd_nxt = tp->snd_nxt;
	otp->snd_up = tp->snd_up;
	otp->snd_wl1 = tp->snd_wl1;
	otp->snd_wl2 = tp->snd_wl2;
	otp->iss = tp->iss;
	otp->irs = tp->irs;
	otp->rcv_nxt = tp->rcv_nxt;
	otp->rcv_adv = tp->rcv_adv;
	otp->rcv_wnd = tp->rcv_wnd;
	otp->rcv_up = tp->rcv_up;
	otp->snd_wnd = tp->snd_wnd;
	otp->snd_cwnd = tp->snd_cwnd;
	otp->snd_ssthresh = tp->snd_ssthresh;
	otp->t_maxopd = tp->t_maxopd;
	otp->t_rcvtime = tp->t_rcvtime;
	otp->t_starttime = tp->t_starttime;
	otp->t_rtttime = tp->t_rtttime;
	otp->t_rtseq = tp->t_rtseq;
	otp->t_rxtcur = tp->t_rxtcur;
	otp->t_maxseg = tp->t_maxseg;
	otp->t_srtt = tp->t_srtt;
	otp->t_rttvar = tp->t_rttvar;
	otp->t_rxtshift = tp->t_rxtshift;
	otp->t_rttmin = tp->t_rttmin;
	otp->t_rttupdated = tp->t_rttupdated;
	otp->max_sndwnd = tp->max_sndwnd;
	otp->t_softerror = tp->t_softerror;
	otp->t_oobflags = tp->t_oobflags;
	otp->t_iobc = tp->t_iobc;
	otp->snd_scale = tp->snd_scale;
	otp->rcv_scale = tp->rcv_scale;
	otp->request_r_scale = tp->request_r_scale;
	otp->requested_s_scale = tp->requested_s_scale;
	otp->ts_recent = tp->ts_recent;
	otp->ts_recent_age = tp->ts_recent_age;
	otp->last_ack_sent = tp->last_ack_sent;
	otp->cc_send = 0;
	otp->cc_recv = 0;
	otp->snd_recover = tp->snd_recover;
	otp->snd_cwnd_prev = tp->snd_cwnd_prev;
	otp->snd_ssthresh_prev = tp->snd_ssthresh_prev;
	otp->t_badrxtwin = 0;
}

static int
tcp_pcblist SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int error, i = 0, n, sz;
	struct inpcb **inp_list;
	inp_gen_t gencnt;
	struct xinpgen xig;

	/*
	 * The process of preparing the TCB list is too time-consuming and
	 * resource-intensive to repeat twice on every request.
	 */
	lck_rw_lock_shared(&tcbinfo.ipi_lock);
	if (req->oldptr == USER_ADDR_NULL) {
		n = tcbinfo.ipi_count;
		req->oldidx = 2 * (sizeof(xig))
		    + (n + n / 8) * sizeof(struct xtcpcb);
		lck_rw_done(&tcbinfo.ipi_lock);
		return 0;
	}

	if (req->newptr != USER_ADDR_NULL) {
		lck_rw_done(&tcbinfo.ipi_lock);
		return EPERM;
	}

	/*
	 * OK, now we're committed to doing something.
	 */
	gencnt = tcbinfo.ipi_gencnt;
	sz = n = tcbinfo.ipi_count;

	bzero(&xig, sizeof(xig));
	xig.xig_len = sizeof(xig);
	xig.xig_count = n;
	xig.xig_gen = gencnt;
	xig.xig_sogen = so_gencnt;
	error = SYSCTL_OUT(req, &xig, sizeof(xig));
	if (error) {
		lck_rw_done(&tcbinfo.ipi_lock);
		return error;
	}
	/*
	 * We are done if there is no pcb
	 */
	if (n == 0) {
		lck_rw_done(&tcbinfo.ipi_lock);
		return 0;
	}

	inp_list = kalloc_type(struct inpcb *, n, Z_WAITOK);
	if (inp_list == NULL) {
		lck_rw_done(&tcbinfo.ipi_lock);
		return ENOMEM;
	}

	n = get_tcp_inp_list(inp_list, n, gencnt);

	error = 0;
	for (i = 0; i < n; i++) {
		struct xtcpcb xt;
		caddr_t inp_ppcb __single;
		struct inpcb *inp;

		inp = inp_list[i];

		if (in_pcb_checkstate(inp, WNT_ACQUIRE, 0) == WNT_STOPUSING) {
			continue;
		}
		socket_lock(inp->inp_socket, 1);
		if (in_pcb_checkstate(inp, WNT_RELEASE, 1) == WNT_STOPUSING) {
			socket_unlock(inp->inp_socket, 1);
			continue;
		}
		if (inp->inp_gencnt > gencnt) {
			socket_unlock(inp->inp_socket, 1);
			continue;
		}

		bzero(&xt, sizeof(xt));
		xt.xt_len = sizeof(xt);
		/* XXX should avoid extra copy */
		inpcb_to_compat(inp, &xt.xt_inp);
		inp_ppcb = inp->inp_ppcb;
		if (inp_ppcb != NULL) {
			tcpcb_to_otcpcb((struct tcpcb *)(void *)inp_ppcb,
			    &xt.xt_tp);
		} else {
			bzero((char *) &xt.xt_tp, sizeof(xt.xt_tp));
		}
		if (inp->inp_socket) {
			sotoxsocket(inp->inp_socket, &xt.xt_socket);
		}

		socket_unlock(inp->inp_socket, 1);

		error = SYSCTL_OUT(req, &xt, sizeof(xt));
	}
	if (!error) {
		/*
		 * Give the user an updated idea of our state.
		 * If the generation differs from what we told
		 * her before, she knows that something happened
		 * while we were processing this request, and it
		 * might be necessary to retry.
		 */
		bzero(&xig, sizeof(xig));
		xig.xig_len = sizeof(xig);
		xig.xig_gen = tcbinfo.ipi_gencnt;
		xig.xig_sogen = so_gencnt;
		xig.xig_count = tcbinfo.ipi_count;
		error = SYSCTL_OUT(req, &xig, sizeof(xig));
	}

	lck_rw_done(&tcbinfo.ipi_lock);
	kfree_type(struct inpcb *, sz, inp_list);
	return error;
}

SYSCTL_PROC(_net_inet_tcp, TCPCTL_PCBLIST, pcblist,
    CTLTYPE_STRUCT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    tcp_pcblist, "S,xtcpcb", "List of active TCP connections");

#if XNU_TARGET_OS_OSX

static void
tcpcb_to_xtcpcb64(struct tcpcb *tp, struct xtcpcb64 *otp)
{
	otp->t_segq = (uint32_t)VM_KERNEL_ADDRHASH(tp->t_segq.lh_first);
	otp->t_dupacks = tp->t_dupacks;
	otp->t_timer[TCPT_REXMT_EXT] = tp->t_timer[TCPT_REXMT];
	otp->t_timer[TCPT_PERSIST_EXT] = tp->t_timer[TCPT_PERSIST];
	otp->t_timer[TCPT_KEEP_EXT] = tp->t_timer[TCPT_KEEP];
	otp->t_timer[TCPT_2MSL_EXT] = tp->t_timer[TCPT_2MSL];
	otp->t_state = tp->t_state;
	otp->t_flags = tp->t_flags;
	otp->t_force = (tp->t_flagsext & TF_FORCE) ? 1 : 0;
	otp->snd_una = tp->snd_una;
	otp->snd_max = tp->snd_max;
	otp->snd_nxt = tp->snd_nxt;
	otp->snd_up = tp->snd_up;
	otp->snd_wl1 = tp->snd_wl1;
	otp->snd_wl2 = tp->snd_wl2;
	otp->iss = tp->iss;
	otp->irs = tp->irs;
	otp->rcv_nxt = tp->rcv_nxt;
	otp->rcv_adv = tp->rcv_adv;
	otp->rcv_wnd = tp->rcv_wnd;
	otp->rcv_up = tp->rcv_up;
	otp->snd_wnd = tp->snd_wnd;
	otp->snd_cwnd = tp->snd_cwnd;
	otp->snd_ssthresh = tp->snd_ssthresh;
	otp->t_maxopd = tp->t_maxopd;
	otp->t_rcvtime = tp->t_rcvtime;
	otp->t_starttime = tp->t_starttime;
	otp->t_rtttime = tp->t_rtttime;
	otp->t_rtseq = tp->t_rtseq;
	otp->t_rxtcur = tp->t_rxtcur;
	otp->t_maxseg = tp->t_maxseg;
	otp->t_srtt = tp->t_srtt;
	otp->t_rttvar = tp->t_rttvar;
	otp->t_rxtshift = tp->t_rxtshift;
	otp->t_rttmin = tp->t_rttmin;
	otp->t_rttupdated = tp->t_rttupdated;
	otp->max_sndwnd = tp->max_sndwnd;
	otp->t_softerror = tp->t_softerror;
	otp->t_oobflags = tp->t_oobflags;
	otp->t_iobc = tp->t_iobc;
	otp->snd_scale = tp->snd_scale;
	otp->rcv_scale = tp->rcv_scale;
	otp->request_r_scale = tp->request_r_scale;
	otp->requested_s_scale = tp->requested_s_scale;
	otp->ts_recent = tp->ts_recent;
	otp->ts_recent_age = tp->ts_recent_age;
	otp->last_ack_sent = tp->last_ack_sent;
	otp->cc_send = 0;
	otp->cc_recv = 0;
	otp->snd_recover = tp->snd_recover;
	otp->snd_cwnd_prev = tp->snd_cwnd_prev;
	otp->snd_ssthresh_prev = tp->snd_ssthresh_prev;
	otp->t_badrxtwin = 0;
}


static int
tcp_pcblist64 SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int error, i = 0, n, sz;
	struct inpcb **inp_list;
	inp_gen_t gencnt;
	struct xinpgen xig;

	/*
	 * The process of preparing the TCB list is too time-consuming and
	 * resource-intensive to repeat twice on every request.
	 */
	lck_rw_lock_shared(&tcbinfo.ipi_lock);
	if (req->oldptr == USER_ADDR_NULL) {
		n = tcbinfo.ipi_count;
		req->oldidx = 2 * (sizeof(xig))
		    + (n + n / 8) * sizeof(struct xtcpcb64);
		lck_rw_done(&tcbinfo.ipi_lock);
		return 0;
	}

	if (req->newptr != USER_ADDR_NULL) {
		lck_rw_done(&tcbinfo.ipi_lock);
		return EPERM;
	}

	/*
	 * OK, now we're committed to doing something.
	 */
	gencnt = tcbinfo.ipi_gencnt;
	sz = n = tcbinfo.ipi_count;

	bzero(&xig, sizeof(xig));
	xig.xig_len = sizeof(xig);
	xig.xig_count = n;
	xig.xig_gen = gencnt;
	xig.xig_sogen = so_gencnt;
	error = SYSCTL_OUT(req, &xig, sizeof(xig));
	if (error) {
		lck_rw_done(&tcbinfo.ipi_lock);
		return error;
	}
	/*
	 * We are done if there is no pcb
	 */
	if (n == 0) {
		lck_rw_done(&tcbinfo.ipi_lock);
		return 0;
	}

	inp_list = kalloc_type(struct inpcb *, n, Z_WAITOK);
	if (inp_list == NULL) {
		lck_rw_done(&tcbinfo.ipi_lock);
		return ENOMEM;
	}

	n = get_tcp_inp_list(inp_list, n, gencnt);

	error = 0;
	for (i = 0; i < n; i++) {
		struct xtcpcb64 xt;
		struct inpcb *inp;

		inp = inp_list[i];

		if (in_pcb_checkstate(inp, WNT_ACQUIRE, 0) == WNT_STOPUSING) {
			continue;
		}
		socket_lock(inp->inp_socket, 1);
		if (in_pcb_checkstate(inp, WNT_RELEASE, 1) == WNT_STOPUSING) {
			socket_unlock(inp->inp_socket, 1);
			continue;
		}
		if (inp->inp_gencnt > gencnt) {
			socket_unlock(inp->inp_socket, 1);
			continue;
		}

		bzero(&xt, sizeof(xt));
		xt.xt_len = sizeof(xt);
		inpcb_to_xinpcb64(inp, &xt.xt_inpcb);
		xt.xt_inpcb.inp_ppcb =
		    (uint64_t)VM_KERNEL_ADDRHASH(inp->inp_ppcb);
		if (inp->inp_ppcb != NULL) {
			tcpcb_to_xtcpcb64((struct tcpcb *)inp->inp_ppcb,
			    &xt);
		}
		if (inp->inp_socket) {
			sotoxsocket64(inp->inp_socket,
			    &xt.xt_inpcb.xi_socket);
		}

		socket_unlock(inp->inp_socket, 1);

		error = SYSCTL_OUT(req, &xt, sizeof(xt));
	}
	if (!error) {
		/*
		 * Give the user an updated idea of our state.
		 * If the generation differs from what we told
		 * her before, she knows that something happened
		 * while we were processing this request, and it
		 * might be necessary to retry.
		 */
		bzero(&xig, sizeof(xig));
		xig.xig_len = sizeof(xig);
		xig.xig_gen = tcbinfo.ipi_gencnt;
		xig.xig_sogen = so_gencnt;
		xig.xig_count = tcbinfo.ipi_count;
		error = SYSCTL_OUT(req, &xig, sizeof(xig));
	}

	lck_rw_done(&tcbinfo.ipi_lock);
	kfree_type(struct inpcb *, sz, inp_list);
	return error;
}

SYSCTL_PROC(_net_inet_tcp, OID_AUTO, pcblist64,
    CTLTYPE_STRUCT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    tcp_pcblist64, "S,xtcpcb64", "List of active TCP connections");

#endif /* XNU_TARGET_OS_OSX */

static int
tcp_pcblist_n SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int error = 0;

	error = get_pcblist_n(IPPROTO_TCP, req, &tcbinfo);

	return error;
}


SYSCTL_PROC(_net_inet_tcp, OID_AUTO, pcblist_n,
    CTLTYPE_STRUCT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    tcp_pcblist_n, "S,xtcpcb_n", "List of active TCP connections");

static int
tcp_progress_probe_enable SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)

	return ntstat_tcp_progress_enable(req);
}

SYSCTL_PROC(_net_inet_tcp, OID_AUTO, progress_enable,
    CTLTYPE_STRUCT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, 0, 0,
    tcp_progress_probe_enable, "S", "Enable/disable TCP keepalive probing on the specified link(s)");


__private_extern__ void
tcp_get_ports_used(ifnet_t ifp, int protocol, uint32_t flags,
    bitstr_t *__counted_by(bitstr_size(IP_PORTRANGE_SIZE)) bitfield)
{
	inpcb_get_ports_used(ifp, protocol, flags, bitfield,
	    &tcbinfo);
}

__private_extern__ uint32_t
tcp_count_opportunistic(unsigned int ifindex, u_int32_t flags)
{
	return inpcb_count_opportunistic(ifindex, &tcbinfo, flags);
}

__private_extern__ uint32_t
tcp_find_anypcb_byaddr(struct ifaddr *ifa)
{
#if SKYWALK
	if (netns_is_enabled()) {
		return netns_find_anyres_byaddr(ifa, IPPROTO_TCP);
	} else
#endif /* SKYWALK */
	return inpcb_find_anypcb_byaddr(ifa, &tcbinfo);
}

static void
tcp_handle_msgsize(struct ip *ip, struct inpcb *inp)
{
	struct rtentry *rt = NULL;
	u_short ifscope = IFSCOPE_NONE;
	int mtu;
	struct sockaddr_in icmpsrc = {
		.sin_len = sizeof(struct sockaddr_in),
		.sin_family = AF_INET, .sin_port = 0, .sin_addr = { .s_addr = 0 },
		.sin_zero = { 0, 0, 0, 0, 0, 0, 0, 0 }
	};
	struct icmp *icp = NULL;

	icp = __container_of(ip, struct icmp, icmp_ip);
	icmpsrc.sin_addr = icp->icmp_ip.ip_dst;

	/*
	 * MTU discovery:
	 * If we got a needfrag and there is a host route to the
	 * original destination, and the MTU is not locked, then
	 * set the MTU in the route to the suggested new value
	 * (if given) and then notify as usual.  The ULPs will
	 * notice that the MTU has changed and adapt accordingly.
	 * If no new MTU was suggested, then we guess a new one
	 * less than the current value.  If the new MTU is
	 * unreasonably small (defined by sysctl tcp_minmss), then
	 * we reset the MTU to the interface value and enable the
	 * lock bit, indicating that we are no longer doing MTU
	 * discovery.
	 */
	if (ROUTE_UNUSABLE(&(inp->inp_route)) == false) {
		rt = inp->inp_route.ro_rt;
	}

	/*
	 * icmp6_mtudisc_update scopes the routing lookup
	 * to the incoming interface (delivered from mbuf
	 * packet header.
	 * That is mostly ok but for asymmetric networks
	 * that may be an issue.
	 * Frag needed OR Packet too big really communicates
	 * MTU for the out data path.
	 * Take the interface scope from cached route or
	 * the last outgoing interface from inp
	 */
	if (rt != NULL) {
		ifscope = (rt->rt_ifp != NULL) ?
		    rt->rt_ifp->if_index : IFSCOPE_NONE;
	} else {
		ifscope = (inp->inp_last_outifp != NULL) ?
		    inp->inp_last_outifp->if_index : IFSCOPE_NONE;
	}

	if ((rt == NULL) ||
	    !(rt->rt_flags & RTF_HOST) ||
	    (rt->rt_flags & (RTF_CLONING | RTF_PRCLONING))) {
		rt = rtalloc1_scoped(SA(&icmpsrc), 0, RTF_CLONING | RTF_PRCLONING, ifscope);
	} else if (rt) {
		RT_LOCK(rt);
		rtref(rt);
		RT_UNLOCK(rt);
	}

	if (rt != NULL) {
		RT_LOCK(rt);
		if ((rt->rt_flags & RTF_HOST) &&
		    !(rt->rt_rmx.rmx_locks & RTV_MTU)) {
			mtu = ntohs(icp->icmp_nextmtu);
			/*
			 * XXX Stock BSD has changed the following
			 * to compare with icp->icmp_ip.ip_len
			 * to converge faster when sent packet
			 * < route's MTU. We may want to adopt
			 * that change.
			 */
			if (mtu == 0) {
				mtu = ip_next_mtu(rt->rt_rmx.
				    rmx_mtu, 1);
			}
#if DEBUG_MTUDISC
			printf("MTU for %s reduced to %d\n",
			    inet_ntop(AF_INET,
			    &icmpsrc.sin_addr, ipv4str,
			    sizeof(ipv4str)), mtu);
#endif
			if (mtu < max(296, (tcp_minmss +
			    sizeof(struct tcpiphdr)))) {
				rt->rt_rmx.rmx_locks |= RTV_MTU;
			} else if (rt->rt_rmx.rmx_mtu > mtu) {
				rt->rt_rmx.rmx_mtu = mtu;
			}
		}
		RT_UNLOCK(rt);
		rtfree(rt);
	}
}

void
tcp_ctlinput(int cmd, struct sockaddr *sa, void *vip, __unused struct ifnet *ifp)
{
	tcp_seq icmp_tcp_seq;
	struct ipctlparam *ctl_param __single = vip;
	struct ip *ip = NULL;
	struct mbuf *m = NULL;
	struct in_addr faddr;
	struct inpcb *inp;
	struct tcpcb *tp;
	struct tcphdr *th;
	struct icmp *icp;
	size_t off;
#if SKYWALK
	union sockaddr_in_4_6 sock_laddr;
	struct protoctl_ev_val prctl_ev_val;
#endif /* SKYWALK */
	void (*notify)(struct inpcb *, int) = tcp_notify;

	if (ctl_param != NULL) {
		ip = ctl_param->ipc_icmp_ip;
		icp = ctl_param->ipc_icmp;
		m = ctl_param->ipc_m;
		off = ctl_param->ipc_off;
	} else {
		ip = NULL;
		icp = NULL;
		m = NULL;
		off = 0;
	}

	faddr = SIN(sa)->sin_addr;
	if (sa->sa_family != AF_INET || faddr.s_addr == INADDR_ANY) {
		return;
	}

	if ((unsigned)cmd >= PRC_NCMDS) {
		return;
	}

	/* Source quench is deprecated */
	if (cmd == PRC_QUENCH) {
		return;
	}

	if (cmd == PRC_MSGSIZE) {
		notify = tcp_mtudisc;
	} else if (icmp_may_rst && (cmd == PRC_UNREACH_ADMIN_PROHIB ||
	    cmd == PRC_UNREACH_PORT || cmd == PRC_UNREACH_PROTOCOL ||
	    cmd == PRC_TIMXCEED_INTRANS) && ip) {
		notify = tcp_drop_syn_sent;
	}
	/*
	 * Hostdead is ugly because it goes linearly through all PCBs.
	 * XXX: We never get this from ICMP, otherwise it makes an
	 * excellent DoS attack on machines with many connections.
	 */
	else if (cmd == PRC_HOSTDEAD) {
		ip = NULL;
	} else if (inetctlerrmap[cmd] == 0 && !PRC_IS_REDIRECT(cmd)) {
		return;
	}

#if SKYWALK
	bzero(&prctl_ev_val, sizeof(prctl_ev_val));
	bzero(&sock_laddr, sizeof(sock_laddr));
#endif /* SKYWALK */

	if (ip == NULL) {
		in_pcbnotifyall(&tcbinfo, faddr, inetctlerrmap[cmd], notify);
#if SKYWALK
		protoctl_event_enqueue_nwk_wq_entry(ifp, NULL,
		    sa, 0, 0, IPPROTO_TCP, cmd, NULL);
#endif /* SKYWALK */
		return;
	}

	/* Check if we can safely get the sport, dport and the sequence number from the tcp header. */
	if (m == NULL ||
	    (m->m_len < off + (sizeof(unsigned short) + sizeof(unsigned short) + sizeof(tcp_seq)))) {
		/* Insufficient length */
		return;
	}

	th = (struct tcphdr*)(void*)(mtod(m, uint8_t*) + off);
	icmp_tcp_seq = ntohl(th->th_seq);

	inp = in_pcblookup_hash(&tcbinfo, faddr, th->th_dport,
	    ip->ip_src, th->th_sport, 0, NULL);

	if (inp == NULL ||
	    inp->inp_socket == NULL) {
#if SKYWALK
		if (cmd == PRC_MSGSIZE) {
			prctl_ev_val.val = ntohs(icp->icmp_nextmtu);
		}
		prctl_ev_val.tcp_seq_number = icmp_tcp_seq;

		sock_laddr.sin.sin_family = AF_INET;
		sock_laddr.sin.sin_len = sizeof(sock_laddr.sin);
		sock_laddr.sin.sin_addr = ip->ip_src;

		protoctl_event_enqueue_nwk_wq_entry(ifp,
		    SA(&sock_laddr), sa,
		    th->th_sport, th->th_dport, IPPROTO_TCP,
		    cmd, &prctl_ev_val);
#endif /* SKYWALK */
		return;
	}

	socket_lock(inp->inp_socket, 1);
	if (in_pcb_checkstate(inp, WNT_RELEASE, 1) ==
	    WNT_STOPUSING) {
		socket_unlock(inp->inp_socket, 1);
		return;
	}

	if (PRC_IS_REDIRECT(cmd)) {
		/* signal EHOSTDOWN, as it flushes the cached route */
		(*notify)(inp, EHOSTDOWN);
	} else {
		tp = intotcpcb(inp);
		if (SEQ_GEQ(icmp_tcp_seq, tp->snd_una) &&
		    SEQ_LT(icmp_tcp_seq, tp->snd_max)) {
			if (cmd == PRC_MSGSIZE) {
				tcp_handle_msgsize(ip, inp);
			}

			(*notify)(inp, inetctlerrmap[cmd]);
		}
	}
	socket_unlock(inp->inp_socket, 1);
}

void
tcp6_ctlinput(int cmd, struct sockaddr *sa, void *d, __unused struct ifnet *ifp)
{
	tcp_seq icmp_tcp_seq;
	struct in6_addr *dst;
	void (*notify)(struct inpcb *, int) = tcp_notify;
	struct ip6_hdr *ip6;
	struct mbuf *m;
	struct inpcb *inp;
	struct tcpcb *tp;
	struct icmp6_hdr *icmp6;
	struct ip6ctlparam *ip6cp = NULL;
	const struct sockaddr_in6 *sa6_src = NULL;
	unsigned int mtu;
	unsigned int off;

	struct tcp_ports {
		uint16_t th_sport;
		uint16_t th_dport;
	} t_ports;
#if SKYWALK
	union sockaddr_in_4_6 sock_laddr;
	struct protoctl_ev_val prctl_ev_val;
#endif /* SKYWALK */

	if (sa->sa_family != AF_INET6 ||
	    sa->sa_len != sizeof(struct sockaddr_in6)) {
		return;
	}

	/* Source quench is deprecated */
	if (cmd == PRC_QUENCH) {
		return;
	}

	if ((unsigned)cmd >= PRC_NCMDS) {
		return;
	}

	/* if the parameter is from icmp6, decode it. */
	if (d != NULL) {
		ip6cp = (struct ip6ctlparam *)d;
		icmp6 = ip6cp->ip6c_icmp6;
		m = ip6cp->ip6c_m;
		ip6 = ip6cp->ip6c_ip6;
		off = ip6cp->ip6c_off;
		sa6_src = ip6cp->ip6c_src;
		dst = ip6cp->ip6c_finaldst;
	} else {
		m = NULL;
		ip6 = NULL;
		off = 0;        /* fool gcc */
		sa6_src = &sa6_any;
		dst = NULL;
	}

	if (cmd == PRC_MSGSIZE) {
		notify = tcp_mtudisc;
	} else if (icmp_may_rst && (cmd == PRC_UNREACH_ADMIN_PROHIB ||
	    cmd == PRC_UNREACH_PORT || cmd == PRC_TIMXCEED_INTRANS) &&
	    ip6 != NULL) {
		notify = tcp_drop_syn_sent;
	}
	/*
	 * Hostdead is ugly because it goes linearly through all PCBs.
	 * XXX: We never get this from ICMP, otherwise it makes an
	 * excellent DoS attack on machines with many connections.
	 */
	else if (cmd == PRC_HOSTDEAD) {
		ip6 = NULL;
	} else if (inet6ctlerrmap[cmd] == 0 && !PRC_IS_REDIRECT(cmd)) {
		return;
	}

#if SKYWALK
	bzero(&prctl_ev_val, sizeof(prctl_ev_val));
	bzero(&sock_laddr, sizeof(sock_laddr));
#endif /* SKYWALK */

	if (ip6 == NULL) {
		in6_pcbnotify(&tcbinfo, sa, 0, SA(sa6_src), 0, cmd, NULL, notify);
#if SKYWALK
		protoctl_event_enqueue_nwk_wq_entry(ifp, NULL, sa,
		    0, 0, IPPROTO_TCP, cmd, NULL);
#endif /* SKYWALK */
		return;
	}

	/* Check if we can safely get the ports from the tcp hdr */
	if (m == NULL ||
	    (m->m_pkthdr.len <
	    (int32_t) (off + sizeof(struct tcp_ports)))) {
		return;
	}
	bzero(&t_ports, sizeof(struct tcp_ports));
	m_copydata(m, off, sizeof(struct tcp_ports), (caddr_t)&t_ports);

	off += sizeof(struct tcp_ports);
	if (m->m_pkthdr.len < (int32_t) (off + sizeof(tcp_seq))) {
		return;
	}
	m_copydata(m, off, sizeof(tcp_seq), (caddr_t)&icmp_tcp_seq);
	icmp_tcp_seq = ntohl(icmp_tcp_seq);

	if (cmd == PRC_MSGSIZE) {
		mtu = ntohl(icmp6->icmp6_mtu);
		/*
		 * If no alternative MTU was proposed, or the proposed
		 * MTU was too small, set to the min.
		 */
		if (mtu < IPV6_MMTU) {
			mtu = IPV6_MMTU - 8;
		}
	}

	inp = in6_pcblookup_hash(&tcbinfo, &ip6->ip6_dst, t_ports.th_dport, ip6_input_getdstifscope(m),
	    &ip6->ip6_src, t_ports.th_sport, ip6_input_getsrcifscope(m), 0, NULL);

	if (inp == NULL ||
	    inp->inp_socket == NULL) {
#if SKYWALK
		if (cmd == PRC_MSGSIZE) {
			prctl_ev_val.val = mtu;
		}
		prctl_ev_val.tcp_seq_number = icmp_tcp_seq;

		sock_laddr.sin6.sin6_family = AF_INET6;
		sock_laddr.sin6.sin6_len = sizeof(sock_laddr.sin6);
		sock_laddr.sin6.sin6_addr = ip6->ip6_src;

		protoctl_event_enqueue_nwk_wq_entry(ifp,
		    SA(&sock_laddr), sa,
		    t_ports.th_sport, t_ports.th_dport, IPPROTO_TCP,
		    cmd, &prctl_ev_val);
#endif /* SKYWALK */
		return;
	}

	socket_lock(inp->inp_socket, 1);
	if (in_pcb_checkstate(inp, WNT_RELEASE, 1) ==
	    WNT_STOPUSING) {
		socket_unlock(inp->inp_socket, 1);
		return;
	}

	if (PRC_IS_REDIRECT(cmd)) {
		/* signal EHOSTDOWN, as it flushes the cached route */
		(*notify)(inp, EHOSTDOWN);
	} else {
		tp = intotcpcb(inp);
		if (SEQ_GEQ(icmp_tcp_seq, tp->snd_una) &&
		    SEQ_LT(icmp_tcp_seq, tp->snd_max)) {
			if (cmd == PRC_MSGSIZE) {
				/*
				 * Only process the offered MTU if it
				 * is smaller than the current one.
				 */
				if (mtu < tp->t_maxseg +
				    (sizeof(struct tcphdr) + sizeof(struct ip6_hdr))) {
					(*notify)(inp, inetctlerrmap[cmd]);
				}
			} else {
				(*notify)(inp, inetctlerrmap[cmd]);
			}
		}
	}
	socket_unlock(inp->inp_socket, 1);
}


/*
 * Following is where TCP initial sequence number generation occurs.
 *
 * There are two places where we must use initial sequence numbers:
 * 1.  In SYN-ACK packets.
 * 2.  In SYN packets.
 *
 * The ISNs in SYN-ACK packets have no monotonicity requirement,
 * and should be as unpredictable as possible to avoid the possibility
 * of spoofing and/or connection hijacking.  To satisfy this
 * requirement, SYN-ACK ISNs are generated via the arc4random()
 * function.  If exact RFC 1948 compliance is requested via sysctl,
 * these ISNs will be generated just like those in SYN packets.
 *
 * The ISNs in SYN packets must be monotonic; TIME_WAIT recycling
 * depends on this property.  In addition, these ISNs should be
 * unguessable so as to prevent connection hijacking.  To satisfy
 * the requirements of this situation, the algorithm outlined in
 * RFC 9293 is used to generate sequence numbers.
 *
 * For more information on the theory of operation, please see
 * RFC 9293.
 *
 * Implementation details:
 *
 * Time is based off the system timer, and is corrected so that it
 * increases by one megabyte per second.  This allows for proper
 * recycling on high speed LANs while still leaving over an hour
 * before rollover.
 *
 */

#define ISN_BYTES_PER_SECOND 1048576

tcp_seq
tcp_new_isn(struct tcpcb *tp)
{
	uint32_t md5_buffer[4];
	tcp_seq new_isn;
	struct timespec timenow;
	MD5_CTX isn_ctx;

	nanouptime(&timenow);

	/* Compute the md5 hash and return the ISN. */
	MD5Init(&isn_ctx);
	MD5Update(&isn_ctx, (u_char *) &tp->t_inpcb->inp_fport,
	    sizeof(u_short));
	MD5Update(&isn_ctx, (u_char *) &tp->t_inpcb->inp_lport,
	    sizeof(u_short));
	if ((tp->t_inpcb->inp_vflag & INP_IPV6) != 0) {
		MD5Update(&isn_ctx, (u_char *) &tp->t_inpcb->in6p_faddr,
		    sizeof(struct in6_addr));
		MD5Update(&isn_ctx, (u_char *) &tp->t_inpcb->in6p_laddr,
		    sizeof(struct in6_addr));
	} else {
		MD5Update(&isn_ctx, (u_char *) &tp->t_inpcb->inp_faddr,
		    sizeof(struct in_addr));
		MD5Update(&isn_ctx, (u_char *) &tp->t_inpcb->inp_laddr,
		    sizeof(struct in_addr));
	}
	MD5Update(&isn_ctx, (u_char *) &isn_secret, sizeof(isn_secret));
	MD5Final((u_char *) &md5_buffer, &isn_ctx);

	new_isn = (tcp_seq) md5_buffer[0];

	/*
	 * We use a 128ns clock, which is equivalent to 600 Mbps and wraps at
	 * 549 seconds, thus safe for 2 MSL lifetime of TIME-WAIT-state.
	 */
	new_isn += (timenow.tv_sec * NSEC_PER_SEC + timenow.tv_nsec) >> 7;

	if (__probable(tcp_randomize_timestamps)) {
		tp->t_ts_offset = md5_buffer[1];
	}

	return new_isn;
}


/*
 * When a specific ICMP unreachable message is received and the
 * connection state is SYN-SENT, drop the connection.  This behavior
 * is controlled by the icmp_may_rst sysctl.
 */
void
tcp_drop_syn_sent(struct inpcb *inp, int errno)
{
	struct tcpcb *tp = intotcpcb(inp);

	if (tp && tp->t_state == TCPS_SYN_SENT) {
		tcp_drop(tp, errno);
	}
}

/*
 * Get effective MTU for redirect virtual interface. Redirect
 * virtual interface switches between multiple delegated interfaces.
 * For cases, where redirect forwards packets to an ipsec interface,
 * MTU should be adjusted to consider ESP encapsulation overhead.
 */
uint32_t
tcp_get_effective_mtu(struct rtentry *rt, uint32_t current_mtu)
{
	ifnet_t ifp = NULL;
	ifnet_t delegated_ifp = NULL;
	ifnet_t outgoing_ifp = NULL;
	uint32_t min_mtu = 0;
	uint32_t outgoing_mtu = 0;
	uint32_t tunnel_overhead = 0;

	if (rt == NULL || rt->rt_ifp == NULL) {
		return current_mtu;
	}

	ifp = rt->rt_ifp;
	if (ifp->if_subfamily != IFNET_SUBFAMILY_REDIRECT) {
		return current_mtu;
	}

	delegated_ifp = ifp->if_delegated.ifp;
	if (delegated_ifp == NULL || delegated_ifp->if_family != IFNET_FAMILY_IPSEC) {
		return current_mtu;
	}

	min_mtu = MIN(delegated_ifp->if_mtu, current_mtu);

	outgoing_ifp = delegated_ifp->if_delegated.ifp;
	if (outgoing_ifp == NULL) {
		return min_mtu;
	}

	outgoing_mtu = outgoing_ifp->if_mtu;
	if (outgoing_mtu > 0) {
		tunnel_overhead = (u_int32_t)(esp_hdrsiz(NULL) + sizeof(struct ip6_hdr));
		if (outgoing_mtu > tunnel_overhead) {
			outgoing_mtu -= tunnel_overhead;
		}
		if (outgoing_mtu < min_mtu) {
			return outgoing_mtu;
		}
	}

	return min_mtu;
}

/*
 * When `need fragmentation' ICMP is received, update our idea of the MSS
 * based on the new value in the route.  Also nudge TCP to send something,
 * since we know the packet we just sent was dropped.
 * This duplicates some code in the tcp_mss() function in tcp_input.c.
 */
void
tcp_mtudisc(struct inpcb *inp, __unused int errno)
{
	struct tcpcb *tp = intotcpcb(inp);
	struct rtentry *rt;
	struct socket *so = inp->inp_socket;
	int mss;
	u_int32_t mtu;
	u_int32_t protoHdrOverhead = sizeof(struct tcpiphdr);
	int isipv6 = (tp->t_inpcb->inp_vflag & INP_IPV6) != 0;

	/*
	 * Nothing left to send after the socket is defunct or TCP is in the closed state
	 */
	if ((so->so_state & SS_DEFUNCT) || (tp != NULL && tp->t_state == TCPS_CLOSED)) {
		return;
	}

	if (isipv6) {
		protoHdrOverhead = sizeof(struct ip6_hdr) +
		    sizeof(struct tcphdr);
	}

	if (tp != NULL) {
		if (isipv6) {
			rt = tcp_rtlookup6(inp, IFSCOPE_NONE);
		} else {
			rt = tcp_rtlookup(inp, IFSCOPE_NONE);
		}
		if (!rt || !rt->rt_rmx.rmx_mtu) {
			tp->t_maxopd = tp->t_maxseg =
			    isipv6 ? tcp_v6mssdflt :
			    tcp_mssdflt;

			/* Route locked during lookup above */
			if (rt != NULL) {
				RT_UNLOCK(rt);
			}
			return;
		}
		mtu = rt->rt_rmx.rmx_mtu;

		mtu = tcp_get_effective_mtu(rt, mtu);

		/* Route locked during lookup above */
		RT_UNLOCK(rt);

#if NECP
		// Adjust MTU if necessary.
		mtu = necp_socket_get_effective_mtu(inp, mtu);
#endif /* NECP */
		mss = mtu - protoHdrOverhead;

		if (tp->t_maxopd) {
			mss = min(mss, tp->t_maxopd);
		}
		/*
		 * XXX - The above conditional probably violates the TCP
		 * spec.  The problem is that, since we don't know the
		 * other end's MSS, we are supposed to use a conservative
		 * default.  But, if we do that, then MTU discovery will
		 * never actually take place, because the conservative
		 * default is much less than the MTUs typically seen
		 * on the Internet today.  For the moment, we'll sweep
		 * this under the carpet.
		 *
		 * The conservative default might not actually be a problem
		 * if the only case this occurs is when sending an initial
		 * SYN with options and data to a host we've never talked
		 * to before.  Then, they will reply with an MSS value which
		 * will get recorded and the new parameters should get
		 * recomputed.  For Further Study.
		 */
		if (tp->t_maxopd <= mss) {
			return;
		}
		tp->t_maxopd = mss;

		if ((tp->t_flags & (TF_REQ_TSTMP | TF_NOOPT)) == TF_REQ_TSTMP &&
		    (tp->t_flags & TF_RCVD_TSTMP) == TF_RCVD_TSTMP) {
			mss -= TCPOLEN_TSTAMP_APPA;
		}

#if MPTCP
		mss -= mptcp_adj_mss(tp, TRUE);
#endif
		if (so->so_snd.sb_hiwat < mss) {
			mss = so->so_snd.sb_hiwat;
		}

		tp->t_maxseg = mss;

		ASSERT(tp->t_maxseg);

		/*
		 * Reset the slow-start flight size as it may depends on the
		 * new MSS
		 */
		if (CC_ALGO(tp)->cwnd_init != NULL) {
			CC_ALGO(tp)->cwnd_init(tp);
		}

		if (TCP_USE_RLEDBAT(tp, so) && tcp_cc_rledbat.rwnd_init != NULL) {
			tcp_cc_rledbat.rwnd_init(tp);
		}

		tcpstat.tcps_mturesent++;
		tp->t_rtttime = 0;
		tp->snd_nxt = tp->snd_una;
		tcp_output(tp);
	}
}

/*
 * Look-up the routing entry to the peer of this inpcb.  If no route
 * is found and it cannot be allocated the return NULL.  This routine
 * is called by TCP routines that access the rmx structure and by tcp_mss
 * to get the interface MTU.  If a route is found, this routine will
 * hold the rtentry lock; the caller is responsible for unlocking.
 */
struct rtentry *
tcp_rtlookup(struct inpcb *inp, unsigned int input_ifscope)
{
	struct route *ro;
	struct rtentry *rt;
	struct tcpcb *tp;

	LCK_MTX_ASSERT(rnh_lock, LCK_MTX_ASSERT_NOTOWNED);

	ro = &inp->inp_route;
	if ((rt = ro->ro_rt) != NULL) {
		RT_LOCK(rt);
	}

	if (ROUTE_UNUSABLE(ro)) {
		if (rt != NULL) {
			RT_UNLOCK(rt);
			rt = NULL;
		}
		ROUTE_RELEASE(ro);
		/* No route yet, so try to acquire one */
		if (inp->inp_faddr.s_addr != INADDR_ANY) {
			unsigned int ifscope;

			ro->ro_dst.sa_family = AF_INET;
			ro->ro_dst.sa_len = sizeof(struct sockaddr_in);
			SIN(&ro->ro_dst)->sin_addr = inp->inp_faddr;

			/*
			 * If the socket was bound to an interface, then
			 * the bound-to-interface takes precedence over
			 * the inbound interface passed in by the caller
			 * (if we get here as part of the output path then
			 * input_ifscope is IFSCOPE_NONE).
			 */
			ifscope = (inp->inp_flags & INP_BOUND_IF) ?
			    inp->inp_boundifp->if_index : input_ifscope;

			rtalloc_scoped(ro, ifscope);
			if ((rt = ro->ro_rt) != NULL) {
				RT_LOCK(rt);
			}
		}
	}
	if (rt != NULL) {
		RT_LOCK_ASSERT_HELD(rt);
	}

	/*
	 * Update MTU discovery determination. Don't do it if:
	 *	1) it is disabled via the sysctl
	 *	2) the route isn't up
	 *	3) the MTU is locked (if it is, then discovery has been
	 *	   disabled)
	 */

	tp = intotcpcb(inp);

	if (!path_mtu_discovery || ((rt != NULL) &&
	    (!(rt->rt_flags & RTF_UP) || (rt->rt_rmx.rmx_locks & RTV_MTU)))) {
		tp->t_flags &= ~TF_PMTUD;
	} else {
		tp->t_flags |= TF_PMTUD;
	}

	if (rt != NULL && rt->rt_ifp != NULL) {
		somultipages(inp->inp_socket,
		    (rt->rt_ifp->if_hwassist & IFNET_MULTIPAGES));
		tcp_set_tso(tp, rt->rt_ifp);
		soif2kcl(inp->inp_socket,
		    (rt->rt_ifp->if_eflags & IFEF_2KCL));
		tcp_set_ecn(tp, rt->rt_ifp);
		if (inp->inp_last_outifp == NULL) {
			inp->inp_last_outifp = rt->rt_ifp;
#if SKYWALK
			if (NETNS_TOKEN_VALID(&inp->inp_netns_token)) {
				netns_set_ifnet(&inp->inp_netns_token,
				    inp->inp_last_outifp);
			}
#endif /* SKYWALK */
		}
	}

	/* Note if the peer is local */
	if (rt != NULL && !(rt->rt_ifp->if_flags & IFF_POINTOPOINT) &&
	    (rt->rt_gateway->sa_family == AF_LINK ||
	    rt->rt_ifp->if_flags & IFF_LOOPBACK ||
	    in_localaddr(inp->inp_faddr))) {
		tp->t_flags |= TF_LOCAL;
	}

	/*
	 * Caller needs to call RT_UNLOCK(rt).
	 */
	return rt;
}

struct rtentry *
tcp_rtlookup6(struct inpcb *inp, unsigned int input_ifscope)
{
	struct route_in6 *ro6;
	struct rtentry *rt;
	struct tcpcb *tp;

	LCK_MTX_ASSERT(rnh_lock, LCK_MTX_ASSERT_NOTOWNED);

	ro6 = &inp->in6p_route;
	if ((rt = ro6->ro_rt) != NULL) {
		RT_LOCK(rt);
	}

	if (ROUTE_UNUSABLE(ro6)) {
		if (rt != NULL) {
			RT_UNLOCK(rt);
			rt = NULL;
		}
		ROUTE_RELEASE(ro6);
		/* No route yet, so try to acquire one */
		if (!IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_faddr)) {
			struct sockaddr_in6 *dst6;
			unsigned int ifscope;

			dst6 = SIN6(&ro6->ro_dst);
			dst6->sin6_family = AF_INET6;
			dst6->sin6_len = sizeof(*dst6);
			dst6->sin6_addr = inp->in6p_faddr;

			/*
			 * If the socket was bound to an interface, then
			 * the bound-to-interface takes precedence over
			 * the inbound interface passed in by the caller
			 * (if we get here as part of the output path then
			 * input_ifscope is IFSCOPE_NONE).
			 */
			ifscope = (inp->inp_flags & INP_BOUND_IF) ?
			    inp->inp_boundifp->if_index : input_ifscope;

			rtalloc_scoped((struct route *)ro6, ifscope);
			if ((rt = ro6->ro_rt) != NULL) {
				RT_LOCK(rt);
			}
		}
	}
	if (rt != NULL) {
		RT_LOCK_ASSERT_HELD(rt);
	}

	/*
	 * Update path MTU Discovery determination
	 * while looking up the route:
	 *  1) we have a valid route to the destination
	 *  2) the MTU is not locked (if it is, then discovery has been
	 *    disabled)
	 */


	tp = intotcpcb(inp);

	/*
	 * Update MTU discovery determination. Don't do it if:
	 *	1) it is disabled via the sysctl
	 *	2) the route isn't up
	 *	3) the MTU is locked (if it is, then discovery has been
	 *	   disabled)
	 */

	if (!path_mtu_discovery || ((rt != NULL) &&
	    (!(rt->rt_flags & RTF_UP) || (rt->rt_rmx.rmx_locks & RTV_MTU)))) {
		tp->t_flags &= ~TF_PMTUD;
	} else {
		tp->t_flags |= TF_PMTUD;
	}

	if (rt != NULL && rt->rt_ifp != NULL) {
		somultipages(inp->inp_socket,
		    (rt->rt_ifp->if_hwassist & IFNET_MULTIPAGES));
		tcp_set_tso(tp, rt->rt_ifp);
		soif2kcl(inp->inp_socket,
		    (rt->rt_ifp->if_eflags & IFEF_2KCL));
		tcp_set_ecn(tp, rt->rt_ifp);
		if (inp->inp_last_outifp == NULL) {
			inp->inp_last_outifp = rt->rt_ifp;
#if SKYWALK
			if (NETNS_TOKEN_VALID(&inp->inp_netns_token)) {
				netns_set_ifnet(&inp->inp_netns_token,
				    inp->inp_last_outifp);
			}
#endif /* SKYWALK */
		}

		/* Note if the peer is local */
		if (!(rt->rt_ifp->if_flags & IFF_POINTOPOINT) &&
		    (IN6_IS_ADDR_LOOPBACK(&inp->in6p_faddr) ||
		    IN6_IS_ADDR_LINKLOCAL(&inp->in6p_faddr) ||
		    rt->rt_gateway->sa_family == AF_LINK ||
		    in6_localaddr(&inp->in6p_faddr))) {
			tp->t_flags |= TF_LOCAL;
		}
	}

	/*
	 * Caller needs to call RT_UNLOCK(rt).
	 */
	return rt;
}

#if IPSEC
/* compute ESP/AH header size for TCP, including outer IP header. */
size_t
ipsec_hdrsiz_tcp(struct tcpcb *tp)
{
	struct inpcb *inp;
	struct mbuf *m;
	size_t hdrsiz;
	struct ip *ip;
	struct ip6_hdr *ip6 = NULL;
	struct tcphdr *th;

	if ((tp == NULL) || ((inp = tp->t_inpcb) == NULL)) {
		return 0;
	}
	MGETHDR(m, M_DONTWAIT, MT_DATA);        /* MAC-OK */
	if (!m) {
		return 0;
	}

	if ((inp->inp_vflag & INP_IPV6) != 0) {
		ip6 = mtod(m, struct ip6_hdr *);
		th = (struct tcphdr *)(void *)(ip6 + 1);
		m->m_pkthdr.len = m->m_len =
		    sizeof(struct ip6_hdr) + sizeof(struct tcphdr);
		tcp_fillheaders(m, tp, ip6, th);
		hdrsiz = ipsec6_hdrsiz(m, IPSEC_DIR_OUTBOUND, inp);
	} else {
		ip = mtod(m, struct ip *);
		th = (struct tcphdr *)(ip + 1);
		m->m_pkthdr.len = m->m_len = sizeof(struct tcpiphdr);
		tcp_fillheaders(m, tp, ip, th);
		hdrsiz = ipsec4_hdrsiz(m, IPSEC_DIR_OUTBOUND, inp);
	}
	m_free(m);
	return hdrsiz;
}
#endif /* IPSEC */

int
tcp_lock(struct socket *so, int refcount, void *lr)
{
	lr_ref_t lr_saved = TCP_INIT_LR_SAVED(lr);

retry:
	if (so->so_pcb != NULL) {
		if (so->so_flags & SOF_MP_SUBFLOW) {
			struct mptcb *mp_tp = tptomptp(sototcpcb(so));
			struct socket *mp_so = mptetoso(mp_tp->mpt_mpte);

			socket_lock(mp_so, refcount);

			/*
			 * Check if we became non-MPTCP while waiting for the lock.
			 * If yes, we have to retry to grab the right lock.
			 */
			if (!(so->so_flags & SOF_MP_SUBFLOW)) {
				socket_unlock(mp_so, refcount);
				goto retry;
			}
		} else {
			lck_mtx_lock(&((struct inpcb *)so->so_pcb)->inpcb_mtx);

			if (so->so_flags & SOF_MP_SUBFLOW) {
				/*
				 * While waiting for the lock, we might have
				 * become MPTCP-enabled (see mptcp_subflow_socreate).
				 */
				lck_mtx_unlock(&((struct inpcb *)so->so_pcb)->inpcb_mtx);
				goto retry;
			}
		}
	} else {
		panic("tcp_lock: so=%p NO PCB! lr=%p lrh= %s",
		    so, lr_saved, solockhistory_nr(so));
		/* NOTREACHED */
	}

	if (so->so_usecount < 0) {
		panic("tcp_lock: so=%p so_pcb=%p lr=%p ref=%x lrh= %s",
		    so, so->so_pcb, lr_saved, so->so_usecount,
		    solockhistory_nr(so));
		/* NOTREACHED */
	}
	if (refcount) {
		so->so_usecount++;
	}
	so->lock_lr[so->next_lock_lr] = lr_saved;
	so->next_lock_lr = (so->next_lock_lr + 1) % SO_LCKDBG_MAX;
	return 0;
}

int
tcp_unlock(struct socket *so, int refcount, void *lr)
{
	lr_ref_t lr_saved = TCP_INIT_LR_SAVED(lr);


#ifdef MORE_TCPLOCK_DEBUG
	printf("tcp_unlock: so=0x%llx sopcb=0x%llx lock=0x%llx ref=%x "
	    "lr=0x%llx\n", (uint64_t)VM_KERNEL_ADDRPERM(so),
	    (uint64_t)VM_KERNEL_ADDRPERM(so->so_pcb),
	    (uint64_t)VM_KERNEL_ADDRPERM(&(sotoinpcb(so)->inpcb_mtx)),
	    so->so_usecount, (uint64_t)VM_KERNEL_ADDRPERM(lr_saved));
#endif
	if (refcount) {
		so->so_usecount--;
	}

	if (so->so_usecount < 0) {
		panic("tcp_unlock: so=%p usecount=%x lrh= %s",
		    so, so->so_usecount, solockhistory_nr(so));
		/* NOTREACHED */
	}
	if (so->so_pcb == NULL) {
		panic("tcp_unlock: so=%p NO PCB usecount=%x lr=%p lrh= %s",
		    so, so->so_usecount, lr_saved, solockhistory_nr(so));
		/* NOTREACHED */
	} else {
		so->unlock_lr[so->next_unlock_lr] = lr_saved;
		so->next_unlock_lr = (so->next_unlock_lr + 1) % SO_LCKDBG_MAX;

		if (so->so_flags & SOF_MP_SUBFLOW) {
			struct mptcb *mp_tp = tptomptp(sototcpcb(so));
			struct socket *mp_so = mptetoso(mp_tp->mpt_mpte);

			socket_lock_assert_owned(mp_so);

			socket_unlock(mp_so, refcount);
		} else {
			LCK_MTX_ASSERT(&((struct inpcb *)so->so_pcb)->inpcb_mtx,
			    LCK_MTX_ASSERT_OWNED);
			lck_mtx_unlock(&((struct inpcb *)so->so_pcb)->inpcb_mtx);
		}
	}
	return 0;
}

lck_mtx_t *
tcp_getlock(struct socket *so, int flags)
{
	struct inpcb *inp = sotoinpcb(so);

	if (so->so_pcb) {
		if (so->so_usecount < 0) {
			panic("tcp_getlock: so=%p usecount=%x lrh= %s",
			    so, so->so_usecount, solockhistory_nr(so));
		}

		if (so->so_flags & SOF_MP_SUBFLOW) {
			struct mptcb *mp_tp = tptomptp(sototcpcb(so));
			struct socket *mp_so = mptetoso(mp_tp->mpt_mpte);

			return mp_so->so_proto->pr_getlock(mp_so, flags);
		} else {
			return &inp->inpcb_mtx;
		}
	} else {
		panic("tcp_getlock: so=%p NULL so_pcb %s",
		    so, solockhistory_nr(so));
		return so->so_proto->pr_domain->dom_mtx;
	}
}

/*
 * Determine if we can grow the recieve socket buffer to avoid sending
 * a zero window update to the peer. We allow even socket buffers that
 * have fixed size (set by the application) to grow if the resource
 * constraints are met. They will also be trimmed after the application
 * reads data.
 */
static void
tcp_sbrcv_grow_rwin(struct tcpcb *tp, struct sockbuf *sb)
{
	u_int32_t rcvbufinc = tp->t_maxseg << 4;
	u_int32_t rcvbuf = sb->sb_hiwat;
	struct socket *so = tp->t_inpcb->inp_socket;

	if (tcp_recv_bg == 1 || IS_TCP_RECV_BG(so)) {
		return;
	}

	if (tcp_do_autorcvbuf == 1 &&
	    (tp->t_flags & TF_SLOWLINK) == 0 &&
	    (so->so_flags1 & SOF1_EXTEND_BK_IDLE_WANTED) == 0 &&
	    (rcvbuf - sb->sb_cc) < rcvbufinc &&
	    rcvbuf < tcp_autorcvbuf_max &&
	    (sb->sb_idealsize > 0 &&
	    sb->sb_hiwat <= (sb->sb_idealsize + rcvbufinc))) {
		sbreserve(sb,
		    min((sb->sb_hiwat + rcvbufinc), tcp_autorcvbuf_max));
	}
}

int32_t
tcp_sbspace(struct tcpcb *tp)
{
	struct socket *so = tp->t_inpcb->inp_socket;
	struct sockbuf *sb = &so->so_rcv;
	u_int32_t rcvbuf;
	int32_t space;
	int32_t pending = 0;

	if (so->so_flags & SOF_MP_SUBFLOW) {
		/* We still need to grow TCP's buffer to have a BDP-estimate */
		tcp_sbrcv_grow_rwin(tp, sb);

		return mptcp_sbspace(tptomptp(tp));
	}

	tcp_sbrcv_grow_rwin(tp, sb);

	/* hiwat might have changed */
	rcvbuf = sb->sb_hiwat;

	space =  ((int32_t) imin((rcvbuf - sb->sb_cc),
	    (sb->sb_mbmax - sb->sb_mbcnt)));
	if (space < 0) {
		space = 0;
	}

#if CONTENT_FILTER
	/* Compensate for data being processed by content filters */
	pending = cfil_sock_data_space(sb);
#endif /* CONTENT_FILTER */
	if (pending > space) {
		space = 0;
	} else {
		space -= pending;
	}

	/*
	 * Avoid increasing window size if the current window
	 * is already very low, we could be in "persist" mode and
	 * we could break some apps (see rdar://5409343)
	 */

	if (space < tp->t_maxseg) {
		return space;
	}

	/* Clip window size for slower link */

	if (((tp->t_flags & TF_SLOWLINK) != 0) && slowlink_wsize > 0) {
		return imin(space, slowlink_wsize);
	}

	return space;
}
/*
 * Checks TCP Segment Offloading capability for a given connection
 * and interface pair.
 */
void
tcp_set_tso(struct tcpcb *tp, struct ifnet *ifp)
{
	struct inpcb *inp;
	int isipv6;
	struct ifnet *tunnel_ifp = NULL;
#define IFNET_TSO_MASK (IFNET_TSO_IPV6 | IFNET_TSO_IPV4)

	tp->t_flags &= ~TF_TSO;

	/*
	 * Bail if there's a non-TSO-capable filter on the interface.
	 */
	if (ifp == NULL || ifp->if_flt_no_tso_count > 0) {
		return;
	}

	inp = tp->t_inpcb;
	isipv6 = (inp->inp_vflag & INP_IPV6) != 0;

#if MPTCP
	/*
	 * We can't use TSO if this tcpcb belongs to an MPTCP session.
	 */
	if (inp->inp_socket->so_flags & SOF_MP_SUBFLOW) {
		return;
	}
#endif
	/*
	 * We can't use TSO if the TSO capability of the tunnel interface does
	 * not match the capability of another interface known by TCP
	 */
	if (inp->inp_policyresult.results.result == NECP_KERNEL_POLICY_RESULT_IP_TUNNEL) {
		u_int tunnel_if_index = inp->inp_policyresult.results.result_parameter.tunnel_interface_index;

		if (tunnel_if_index != 0) {
			ifnet_head_lock_shared();
			tunnel_ifp = ifindex2ifnet[tunnel_if_index];
			ifnet_head_done();
		}

		if (tunnel_ifp == NULL) {
			return;
		}

		if ((ifp->if_hwassist & IFNET_TSO_MASK) != (tunnel_ifp->if_hwassist & IFNET_TSO_MASK)) {
			if (tso_debug > 0) {
				os_log(OS_LOG_DEFAULT,
				    "%s: %u > %u TSO 0 tunnel_ifp %s hwassist mismatch with ifp %s",
				    __func__,
				    ntohs(tp->t_inpcb->inp_lport), ntohs(tp->t_inpcb->inp_fport),
				    tunnel_ifp->if_xname, ifp->if_xname);
			}
			return;
		}
		if (inp->inp_last_outifp != NULL &&
		    (inp->inp_last_outifp->if_hwassist & IFNET_TSO_MASK) != (tunnel_ifp->if_hwassist & IFNET_TSO_MASK)) {
			if (tso_debug > 0) {
				os_log(OS_LOG_DEFAULT,
				    "%s: %u > %u TSO 0 tunnel_ifp %s hwassist mismatch with inp_last_outifp %s",
				    __func__,
				    ntohs(tp->t_inpcb->inp_lport), ntohs(tp->t_inpcb->inp_fport),
				    tunnel_ifp->if_xname, inp->inp_last_outifp->if_xname);
			}
			return;
		}
		if ((inp->inp_flags & INP_BOUND_IF) && inp->inp_boundifp != NULL &&
		    (inp->inp_boundifp->if_hwassist & IFNET_TSO_MASK) != (tunnel_ifp->if_hwassist & IFNET_TSO_MASK)) {
			if (tso_debug > 0) {
				os_log(OS_LOG_DEFAULT,
				    "%s: %u > %u TSO 0 tunnel_ifp %s hwassist mismatch with inp_boundifp %s",
				    __func__,
				    ntohs(tp->t_inpcb->inp_lport), ntohs(tp->t_inpcb->inp_fport),
				    tunnel_ifp->if_xname, inp->inp_boundifp->if_xname);
			}
			return;
		}
	}

	if (isipv6) {
		if (ifp->if_hwassist & IFNET_TSO_IPV6) {
			tp->t_flags |= TF_TSO;
			if (ifp->if_tso_v6_mtu != 0) {
				tp->tso_max_segment_size = ifp->if_tso_v6_mtu;
			} else {
				tp->tso_max_segment_size = TCP_MAXWIN;
			}
		}
	} else {
		if (ifp->if_hwassist & IFNET_TSO_IPV4) {
			tp->t_flags |= TF_TSO;
			if (ifp->if_tso_v4_mtu != 0) {
				tp->tso_max_segment_size = ifp->if_tso_v4_mtu;
			} else {
				tp->tso_max_segment_size = TCP_MAXWIN;
			}
			if (INTF_ADJUST_MTU_FOR_CLAT46(ifp)) {
				tp->tso_max_segment_size -=
				    CLAT46_HDR_EXPANSION_OVERHD;
			}
		}
	}

	if (tso_debug > 1) {
		os_log(OS_LOG_DEFAULT, "%s: %u > %u TSO %d ifp %s",
		    __func__,
		    ntohs(tp->t_inpcb->inp_lport),
		    ntohs(tp->t_inpcb->inp_fport),
		    (tp->t_flags & TF_TSO) != 0,
		    ifp != NULL ? ifp->if_xname : "<NULL>");
	}
}

#define TIMEVAL_TO_TCPHZ(_tv_) ((uint32_t)((_tv_).tv_sec * TCP_RETRANSHZ + \
	(_tv_).tv_usec / TCP_RETRANSHZ_TO_USEC))

/*
 * Function to calculate the tcp clock. The tcp clock will get updated
 * at the boundaries of the tcp layer. This is done at 3 places:
 * 1. Right before processing an input tcp packet
 * 2. Whenever a connection wants to access the network using tcp_usrreqs
 * 3. When a tcp timer fires or before tcp slow timeout
 *
 */

void
calculate_tcp_clock(void)
{
	struct timeval tv = tcp_uptime;
	struct timeval interval = {.tv_sec = 0, .tv_usec = TCP_RETRANSHZ_TO_USEC};
	struct timeval now, hold_now;
	uint32_t incr = 0;

	microuptime(&now);

	/*
	 * Update coarse-grained networking timestamp (in sec.); the idea
	 * is to update the counter returnable via net_uptime() when
	 * we read time.
	 */
	net_update_uptime_with_time(&now);

	timevaladd(&tv, &interval);
	if (timevalcmp(&now, &tv, >)) {
		/* time to update the clock */
		lck_spin_lock(&tcp_uptime_lock);
		if (timevalcmp(&tcp_uptime, &now, >=)) {
			/* clock got updated while waiting for the lock */
			lck_spin_unlock(&tcp_uptime_lock);
			return;
		}

		microuptime(&now);
		hold_now = now;
		tv = tcp_uptime;
		timevalsub(&now, &tv);

		incr = TIMEVAL_TO_TCPHZ(now);

		/* Account for the previous remainder */
		uint32_t remaining_us = (now.tv_usec % TCP_RETRANSHZ_TO_USEC) +
		    tcp_now_remainder_us;
		if (remaining_us >= TCP_RETRANSHZ_TO_USEC) {
			incr += (remaining_us / TCP_RETRANSHZ_TO_USEC);
		}

		if (incr > 0) {
			tcp_uptime = hold_now;
			tcp_now_remainder_us = remaining_us % TCP_RETRANSHZ_TO_USEC;
			tcp_now += incr;
		}

		lck_spin_unlock(&tcp_uptime_lock);
	}
}

uint64_t
microuptime_ns(void)
{
	uint64_t abstime = mach_absolute_time();
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(abstime, &ns);

	return ns;
}

#define MAX_BURST_INTERVAL_KERNEL_PACING_NSEC                                  \
	(10 * NSEC_PER_MSEC) // Don't delay more than 10ms between two bursts
static uint64_t
tcp_pacer_get_packet_interval(struct tcpcb *tp, uint32_t size)
{
	if (tp->t_pacer.rate == 0) {
		os_log_error(OS_LOG_DEFAULT,
		    "pacer rate shouldn't be 0, CCA is %s (cwnd=%u, smoothed rtt=%u ms)",
		    CC_ALGO(tp)->name, tp->snd_cwnd, tp->t_srtt >> TCP_RTT_SHIFT);

		return MAX_BURST_INTERVAL_KERNEL_PACING_NSEC;
	}

	uint64_t interval = (uint64_t)size * NSEC_PER_SEC / tp->t_pacer.rate;
	if (interval > MAX_BURST_INTERVAL_KERNEL_PACING_NSEC) {
		interval = MAX_BURST_INTERVAL_KERNEL_PACING_NSEC;
	}

	return interval;
}

/* Return packet tx_time in nanoseconds (absolute as well as continuous) */
uint64_t
tcp_pacer_get_packet_tx_time(struct tcpcb *tp, uint16_t pkt_len)
{
	/*
	 * This function is called multiple times for mss-sized packets
	 * and for high-speeds, we'd want to send multiple packets
	 * that add up to burst_size at the same time.
	 */
	uint64_t now = microuptime_ns();

	if (pkt_len == 0 || now == 0) {
		return now;
	}

	if (tp->t_pacer.packet_tx_time == 0) {
		tp->t_pacer.packet_tx_time = now;
		tp->t_pacer.current_size = pkt_len;
	} else {
		tp->t_pacer.current_size += pkt_len;
		if (tp->t_pacer.current_size > tp->t_pacer.tso_burst_size) {
			/*
			 * Increment tx_time by packet_interval and
			 * reset size to this packet's len
			 */
			tp->t_pacer.packet_tx_time +=
			    tcp_pacer_get_packet_interval(tp, tp->t_pacer.current_size);
			tp->t_pacer.current_size = 0;
			if (now > tp->t_pacer.packet_tx_time) {
				/*
				 * If current time is bigger, then application
				 * has already paced the packet. Also, we can't
				 * set tx_time in the past.
				 */
				tp->t_pacer.packet_tx_time = now;
			}
		}
	}

	return tp->t_pacer.packet_tx_time;
}

void
tcp_set_mbuf_tx_time(struct mbuf *m, uint64_t tx_time)
{
	struct m_tag *tag = NULL;
	tag = m_tag_create(KERNEL_MODULE_TAG_ID, KERNEL_TAG_TYPE_AQM,
	    sizeof(uint64_t), M_WAITOK, m);
	if (tag != NULL) {
		m_tag_prepend(m, tag);
		*(uint64_t *)tag->m_tag_data = tx_time;
	}
}

/*
 * Compute receive window scaling that we are going to request
 * for this connection based on  sb_hiwat. Try to leave some
 * room to potentially increase the window size upto a maximum
 * defined by the constant tcp_autorcvbuf_max.
 */
void
tcp_set_max_rwinscale(struct tcpcb *tp, struct socket *so)
{
	uint32_t maxsockbufsize;

	tp->request_r_scale = MAX((uint8_t)tcp_win_scale, tp->request_r_scale);
	maxsockbufsize = ((so->so_rcv.sb_flags & SB_USRSIZE) != 0) ?
	    so->so_rcv.sb_hiwat : tcp_autorcvbuf_max;

	/*
	 * Window scale should not exceed what is needed
	 * to send the max receive window size; adding 1 to TCP_MAXWIN
	 * ensures that.
	 */
	while (tp->request_r_scale < TCP_MAX_WINSHIFT &&
	    ((TCP_MAXWIN + 1) << tp->request_r_scale) < maxsockbufsize) {
		tp->request_r_scale++;
	}
	tp->request_r_scale = MIN(tp->request_r_scale, TCP_MAX_WINSHIFT);
}

int
tcp_notsent_lowat_check(struct socket *so)
{
	struct inpcb *inp = sotoinpcb(so);
	struct tcpcb *tp = NULL;
	int notsent = 0;

	if (inp != NULL) {
		tp = intotcpcb(inp);
	}

	if (tp == NULL) {
		return 0;
	}

	notsent = so->so_snd.sb_cc -
	    (tp->snd_nxt - tp->snd_una);

	/*
	 * When we send a FIN or SYN, not_sent can be negative.
	 * In that case also we need to send a write event to the
	 * process if it is waiting. In the FIN case, it will
	 * get an error from send because cantsendmore will be set.
	 */
	if (notsent <= tp->t_notsent_lowat) {
		return 1;
	}

	/*
	 * When Nagle's algorithm is not disabled, it is better
	 * to wakeup the client until there is atleast one
	 * maxseg of data to write.
	 */
	if ((tp->t_flags & TF_NODELAY) == 0 &&
	    notsent > 0 && notsent < tp->t_maxseg) {
		return 1;
	}
	return 0;
}

void
tcp_rxtseg_insert(struct tcpcb *tp, tcp_seq start, tcp_seq end)
{
	struct tcp_rxt_seg *rxseg = NULL, *prev = NULL, *next = NULL;
	uint16_t rxcount = 0;

	if (SLIST_EMPTY(&tp->t_rxt_segments)) {
		tp->t_dsack_lastuna = tp->snd_una;
	}
	/*
	 * First check if there is a segment already existing for this
	 * sequence space.
	 */

	SLIST_FOREACH(rxseg, &tp->t_rxt_segments, rx_link) {
		if (SEQ_GT(rxseg->rx_start, start)) {
			break;
		}
		prev = rxseg;
	}
	next = rxseg;

	/* check if prev seg is for this sequence */
	if (prev != NULL && SEQ_LEQ(prev->rx_start, start) &&
	    SEQ_GEQ(prev->rx_end, end)) {
		prev->rx_count++;
		return;
	}

	/*
	 * There are a couple of possibilities at this point.
	 * 1. prev overlaps with the beginning of this sequence
	 * 2. next overlaps with the end of this sequence
	 * 3. there is no overlap.
	 */

	if (prev != NULL && SEQ_GT(prev->rx_end, start)) {
		if (prev->rx_start == start && SEQ_GT(end, prev->rx_end)) {
			start = prev->rx_end + 1;
			prev->rx_count++;
		} else {
			prev->rx_end = (start - 1);
			rxcount = prev->rx_count;
		}
	}

	if (next != NULL && SEQ_LT(next->rx_start, end)) {
		if (SEQ_LEQ(next->rx_end, end)) {
			end = next->rx_start - 1;
			next->rx_count++;
		} else {
			next->rx_start = end + 1;
			rxcount = next->rx_count;
		}
	}
	if (!SEQ_LT(start, end)) {
		return;
	}

	if (tcp_rxt_seg_max > 0 && tp->t_rxt_seg_count >= tcp_rxt_seg_max) {
		rxseg = SLIST_FIRST(&tp->t_rxt_segments);
		if (prev == rxseg) {
			prev = NULL;
		}
		SLIST_REMOVE(&tp->t_rxt_segments, rxseg,
		    tcp_rxt_seg, rx_link);

		tcp_rxt_seg_drop++;
		tp->t_rxt_seg_drop++;
		zfree(tcp_rxt_seg_zone, rxseg);

		tp->t_rxt_seg_count -= 1;
	}

	rxseg = zalloc_flags(tcp_rxt_seg_zone, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	rxseg->rx_start = start;
	rxseg->rx_end = end;
	rxseg->rx_count = rxcount + 1;

	if (prev != NULL) {
		SLIST_INSERT_AFTER(prev, rxseg, rx_link);
	} else {
		SLIST_INSERT_HEAD(&tp->t_rxt_segments, rxseg, rx_link);
	}
	tp->t_rxt_seg_count += 1;
}

struct tcp_rxt_seg *
tcp_rxtseg_find(struct tcpcb *tp, tcp_seq start, tcp_seq end)
{
	struct tcp_rxt_seg *rxseg;

	if (SLIST_EMPTY(&tp->t_rxt_segments)) {
		return NULL;
	}

	SLIST_FOREACH(rxseg, &tp->t_rxt_segments, rx_link) {
		if (SEQ_LEQ(rxseg->rx_start, start) &&
		    SEQ_GEQ(rxseg->rx_end, end)) {
			return rxseg;
		}
		if (SEQ_GT(rxseg->rx_start, start)) {
			break;
		}
	}
	return NULL;
}

void
tcp_rxtseg_set_spurious(struct tcpcb *tp, tcp_seq start, tcp_seq end)
{
	struct tcp_rxt_seg *rxseg;

	if (SLIST_EMPTY(&tp->t_rxt_segments)) {
		return;
	}

	SLIST_FOREACH(rxseg, &tp->t_rxt_segments, rx_link) {
		if (SEQ_GEQ(rxseg->rx_start, start) &&
		    SEQ_LEQ(rxseg->rx_end, end)) {
			/*
			 * If the segment was retransmitted only once, mark it as
			 * spurious.
			 */
			if (rxseg->rx_count == 1) {
				rxseg->rx_flags |= TCP_RXT_SPURIOUS;
			}
		}

		if (SEQ_GEQ(rxseg->rx_start, end)) {
			break;
		}
	}
	return;
}

void
tcp_rxtseg_clean(struct tcpcb *tp)
{
	struct tcp_rxt_seg *rxseg, *next;

	SLIST_FOREACH_SAFE(rxseg, &tp->t_rxt_segments, rx_link, next) {
		SLIST_REMOVE(&tp->t_rxt_segments, rxseg,
		    tcp_rxt_seg, rx_link);
		zfree(tcp_rxt_seg_zone, rxseg);
	}
	tp->t_rxt_seg_count = 0;
	tp->t_dsack_lastuna = tp->snd_max;
}

boolean_t
tcp_rxtseg_detect_bad_rexmt(struct tcpcb *tp, tcp_seq th_ack)
{
	boolean_t bad_rexmt;
	struct tcp_rxt_seg *rxseg;

	if (SLIST_EMPTY(&tp->t_rxt_segments)) {
		return FALSE;
	}

	/*
	 * If all of the segments in this window are not cumulatively
	 * acknowledged, then there can still be undetected packet loss.
	 * Do not restore congestion window in that case.
	 */
	if (SEQ_LT(th_ack, tp->snd_recover)) {
		return FALSE;
	}

	bad_rexmt = TRUE;
	SLIST_FOREACH(rxseg, &tp->t_rxt_segments, rx_link) {
		if (!(rxseg->rx_flags & TCP_RXT_SPURIOUS)) {
			bad_rexmt = FALSE;
			break;
		}
	}
	return bad_rexmt;
}

u_int32_t
tcp_rxtseg_total_size(struct tcpcb *tp)
{
	struct tcp_rxt_seg *rxseg;
	u_int32_t total_size = 0;

	SLIST_FOREACH(rxseg, &tp->t_rxt_segments, rx_link) {
		total_size += (rxseg->rx_end - rxseg->rx_start) + 1;
	}
	return total_size;
}

int
tcp_seg_cmp(const struct tcp_seg_sent *seg1, const struct tcp_seg_sent *seg2)
{
	return (int)(seg1->end_seq - seg2->end_seq);
}

RB_GENERATE(tcp_seg_sent_tree_head, tcp_seg_sent, seg_link, tcp_seg_cmp)

uint32_t
tcp_seg_len(struct tcp_seg_sent *seg)
{
	if (SEQ_LT(seg->end_seq, seg->start_seq)) {
		os_log_error(OS_LOG_DEFAULT, "segment end(%u) can't be smaller "
		    "than segment start(%u)", seg->end_seq, seg->start_seq);
	}

	return seg->end_seq - seg->start_seq;
}

static struct tcp_seg_sent *
tcp_seg_alloc_init(struct tcpcb *tp)
{
	struct tcp_seg_sent *seg = TAILQ_FIRST(&tp->seg_pool.free_segs);
	if (seg != NULL) {
		TAILQ_REMOVE(&tp->seg_pool.free_segs, seg, free_link);
		tp->seg_pool.free_segs_count--;
	} else {
		// TODO: remove Z_WAITOK and Z_NOFAIL?
		seg = zalloc_flags(tcp_seg_sent_zone, Z_WAITOK | Z_ZERO | Z_NOFAIL);
		if (seg == NULL) {
			return NULL;
		}
	}
	bzero(seg, sizeof(*seg));

	return seg;
}

static void
tcp_update_seg_after_rto(struct tcpcb *tp, struct tcp_seg_sent *found_seg,
    uint32_t xmit_ts, uint8_t flags)
{
	tcp_rack_transmit_seg(tp, found_seg, found_seg->start_seq, found_seg->end_seq,
	    xmit_ts, flags);
	struct tcp_seg_sent *seg = TAILQ_FIRST(&tp->t_segs_sent);
	if (found_seg == seg) {
		// Move this segment to the end of time-ordered list.
		TAILQ_REMOVE(&tp->t_segs_sent, seg, tx_link);
		TAILQ_INSERT_TAIL(&tp->t_segs_sent, seg, tx_link);
	}
}

static void
tcp_process_rxmt_segs_after_rto(struct tcpcb *tp, struct tcp_seg_sent *seg, tcp_seq start,
    uint32_t xmit_ts, uint8_t flags)
{
	struct tcp_seg_sent segment = {};

	while (seg != NULL) {
		if (SEQ_LEQ(seg->start_seq, start)) {
			tcp_update_seg_after_rto(tp, seg, xmit_ts, flags);
			break;
		} else {
			/* The segment is a part of the total RTO retransmission */
			tcp_update_seg_after_rto(tp, seg, xmit_ts, flags);

			/* Find the next segment ending at the start of current segment */
			segment.end_seq = seg->start_seq;
			seg = RB_FIND(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, &segment);
		}
	}
}

static struct tcp_seg_sent *
tcp_seg_sent_insert_before(struct tcpcb *tp, struct tcp_seg_sent *before, tcp_seq start, tcp_seq end,
    uint32_t xmit_ts, uint8_t flags)
{
	struct tcp_seg_sent *seg = tcp_seg_alloc_init(tp);
	/* segment MUST be allocated, there is no other fail-safe here */
	tcp_rack_transmit_seg(tp, seg, start, end, xmit_ts, flags);
	struct tcp_seg_sent *not_inserted = RB_INSERT(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, seg);
	if (not_inserted) {
		os_log(OS_LOG_DEFAULT, "segment %p[%u %u) was not inserted in the RB tree", not_inserted,
		    not_inserted->start_seq, not_inserted->end_seq);
	}
	TAILQ_INSERT_BEFORE(before, seg, tx_link);

	return seg;
}

static struct tcp_seg_sent *
tcp_seg_rto_insert_end(struct tcpcb *tp, tcp_seq start, tcp_seq end,
    uint32_t xmit_ts, uint8_t flags)
{
	struct tcp_seg_sent *seg = tcp_seg_alloc_init(tp);
	/* segment MUST be allocated, there is no other fail-safe here */
	tcp_rack_transmit_seg(tp, seg, start, end, xmit_ts, flags);
	struct tcp_seg_sent *not_inserted = RB_INSERT(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, seg);
	if (not_inserted) {
		os_log(OS_LOG_DEFAULT, "segment %p[%u %u) was not inserted in the RB tree", not_inserted,
		    not_inserted->start_seq, not_inserted->end_seq);
	}
	TAILQ_INSERT_TAIL(&tp->t_segs_sent, seg, tx_link);

	return seg;
}

void
tcp_seg_sent_insert(struct tcpcb *tp, struct tcp_seg_sent *seg, tcp_seq start, tcp_seq end,
    uint32_t xmit_ts, uint8_t flags)
{
	if (seg != NULL) {
		uint8_t seg_flags = seg->flags | flags;
		if (seg->end_seq == end) {
			/* Entire seg retransmitted in RACK recovery, start and end sequence doesn't change */
			if (seg->start_seq != start) {
				os_log_error(OS_LOG_DEFAULT, "Segment start (%u) is not same as retransmitted "
				    "start sequence number (%u)", seg->start_seq, start);
			}
			tcp_rack_transmit_seg(tp, seg, seg->start_seq, seg->end_seq, xmit_ts, seg_flags);
			TAILQ_REMOVE(&tp->t_segs_sent, seg, tx_link);
			TAILQ_INSERT_TAIL(&tp->t_segs_sent, seg, tx_link);
		} else {
			/*
			 * Original segment is retransmitted partially, update start_seq by len
			 * and create new segment for retransmitted part
			 */
			struct tcp_seg_sent *partial_seg = tcp_seg_alloc_init(tp);
			if (partial_seg == NULL) {
				return;
			}
			seg->start_seq += (end - start);
			tcp_rack_transmit_seg(tp, partial_seg, start, end, xmit_ts, seg_flags);
			struct tcp_seg_sent *not_inserted = RB_INSERT(tcp_seg_sent_tree_head,
			    &tp->t_segs_sent_tree, partial_seg);
			if (not_inserted) {
				os_log(OS_LOG_DEFAULT, "segment %p[%u %u) was not inserted in the RB tree", not_inserted,
				    not_inserted->start_seq, not_inserted->end_seq);
			}
			TAILQ_INSERT_TAIL(&tp->t_segs_sent, partial_seg, tx_link);
		}

		return;
	}

	if ((flags & TCP_SEGMENT_RETRANSMITTED_ATLEAST_ONCE) == 0) {
		/* This is a new segment */
		seg = tcp_seg_alloc_init(tp);
		if (seg == NULL) {
			return;
		}

		tcp_rack_transmit_seg(tp, seg, start, end, xmit_ts, flags);
		struct tcp_seg_sent *not_inserted = RB_INSERT(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, seg);
		if (not_inserted) {
			os_log(OS_LOG_DEFAULT, "segment %p[%u %u) was not inserted in the RB tree", not_inserted,
			    not_inserted->start_seq, not_inserted->end_seq);
		}
		TAILQ_INSERT_TAIL(&tp->t_segs_sent, seg, tx_link);

		return;
	}
	/*
	 * Either retransmitted after an RTO or PTO.
	 * During RTO, time-ordered list may lose its order.
	 * If retransmitted after RTO, check if the segment
	 * already exists in RB tree and update its xmit_ts. Also,
	 * if this seg is at the top of ordered list, then move it
	 * to the end.
	 */
	struct tcp_seg_sent segment = {};
	struct tcp_seg_sent *found_seg = NULL, *rxmt_seg = NULL;

	/* Set the end sequence to search for existing segment */
	segment.end_seq = end;
	found_seg = RB_FIND(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, &segment);
	if (found_seg != NULL) {
		/* Found an exact match for retransmitted end sequence */
		tcp_process_rxmt_segs_after_rto(tp, found_seg, start, xmit_ts, flags);
		return;
	}
	/*
	 * We come here when we don't find an exact match and end of segment
	 * retransmitted after RTO lies within a segment.
	 */
	RB_FOREACH(found_seg, tcp_seg_sent_tree_head, &tp->t_segs_sent_tree) {
		if (SEQ_LT(end, found_seg->end_seq) && SEQ_GT(end, found_seg->start_seq)) {
			/*
			 * This segment is partially retransmitted. We split this segment at the boundary of end
			 * sequence. First insert the part being retransmitted at the end of time-ordered list.
			 */
			tcp_seg_rto_insert_end(tp, found_seg->start_seq, end, xmit_ts,
			    found_seg->flags | flags);

			if (SEQ_LEQ(found_seg->start_seq, start)) {
				/*
				 * We are done with the retransmitted part.
				 * Move the start of existing segment
				 */
				found_seg->start_seq = end;
			} else {
				/*
				 * This retransmitted sequence covers more than one segment
				 * Look for segments covered by this retransmission below this segment
				 */
				segment.end_seq = found_seg->start_seq;
				rxmt_seg = RB_FIND(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, &segment);

				if (rxmt_seg != NULL) {
					/* rxmt_seg is just before the current segment */
					tcp_process_rxmt_segs_after_rto(tp, rxmt_seg, start, xmit_ts, flags);
				}

				/* Move the start of existing segment */
				found_seg->start_seq = end;
			}
			return;
		}
	}
}

static void
tcp_seg_collect_acked_subtree(struct tcpcb *tp, struct tcp_seg_sent *seg,
    uint32_t acked_xmit_ts, uint32_t tsecr)
{
	if (seg != NULL) {
		tcp_seg_collect_acked_subtree(tp, RB_LEFT(seg, seg_link), acked_xmit_ts, tsecr);
		tcp_seg_collect_acked_subtree(tp, RB_RIGHT(seg, seg_link), acked_xmit_ts, tsecr);
		TAILQ_INSERT_TAIL(&tp->t_segs_acked, seg, ack_link);
	}
}

/* Call this function with root of the rb tree */
static void
tcp_seg_collect_acked(struct tcpcb *tp, struct tcp_seg_sent *seg, tcp_seq th_ack,
    uint32_t acked_xmit_ts, uint32_t tsecr)
{
	if (seg == NULL) {
		return;
	}

	if (SEQ_GEQ(th_ack, seg->end_seq)) {
		/* Delete the entire left sub-tree */
		tcp_seg_collect_acked_subtree(tp, RB_LEFT(seg, seg_link), acked_xmit_ts, tsecr);
		/* Evaluate the right sub-tree */
		tcp_seg_collect_acked(tp, RB_RIGHT(seg, seg_link), th_ack, acked_xmit_ts, tsecr);
		TAILQ_INSERT_TAIL(&tp->t_segs_acked, seg, ack_link);
	} else {
		/*
		 * This ACK doesn't acknowledge the current root and its right sub-tree.
		 * Evaluate the left sub-tree
		 */
		tcp_seg_collect_acked(tp, RB_LEFT(seg, seg_link), th_ack, acked_xmit_ts, tsecr);
	}
}

static void
tcp_seg_delete_acked(struct tcpcb *tp, uint32_t acked_xmit_ts, uint32_t tsecr)
{
	struct tcp_seg_sent *acked_seg = NULL, *next = NULL;

	TAILQ_FOREACH_SAFE(acked_seg, &tp->t_segs_acked, ack_link, next) {
		/* Advance RACK state if applicable */
		if (acked_seg->xmit_ts > acked_xmit_ts) {
			tcp_rack_update_segment_acked(tp, tsecr, acked_seg->xmit_ts, acked_seg->end_seq,
			    !!(acked_seg->flags & TCP_SEGMENT_RETRANSMITTED_ATLEAST_ONCE));
		}
		/* Check for reordering */
		tcp_rack_detect_reordering_acked(tp, acked_seg);

		const uint32_t seg_len = tcp_seg_len(acked_seg);
		if (acked_seg->flags & TCP_SEGMENT_LOST) {
			if (tp->bytes_lost < seg_len) {
				os_log_error(OS_LOG_DEFAULT, "bytes_lost (%u) can't be smaller than already "
				    "lost segment length (%u)", tp->bytes_lost, seg_len);
			}
			tp->bytes_lost -= seg_len;
		}
		if (acked_seg->flags & TCP_RACK_RETRANSMITTED) {
			if (tp->bytes_retransmitted < seg_len) {
				os_log_error(OS_LOG_DEFAULT, "bytes_retransmitted (%u) can't be smaller "
				    "than already retransmited segment length (%u)",
				    tp->bytes_retransmitted, seg_len);
			}
			tp->bytes_retransmitted -= seg_len;
		}
		if (acked_seg->flags & TCP_SEGMENT_SACKED) {
			if (tp->bytes_sacked < seg_len) {
				os_log_error(OS_LOG_DEFAULT, "bytes_sacked (%u) can't be smaller than already "
				    "SACKed segment length (%u)", tp->bytes_sacked, seg_len);
			}
			tp->bytes_sacked -= seg_len;
		}
		TAILQ_REMOVE(&tp->t_segs_acked, acked_seg, ack_link);
		TAILQ_REMOVE(&tp->t_segs_sent, acked_seg, tx_link);
		RB_REMOVE(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, acked_seg);
		tcp_seg_delete(tp, acked_seg);
	}
}

void
tcp_segs_doack(struct tcpcb *tp, tcp_seq th_ack, struct tcpopt *to)
{
	uint32_t tsecr = 0, acked_xmit_ts = 0;
	tcp_seq acked_seq = th_ack;
	bool was_retransmitted = false;

	if (TAILQ_EMPTY(&tp->t_segs_sent)) {
		return;
	}

	if (((to->to_flags & TOF_TS) != 0) && (to->to_tsecr != 0)) {
		tsecr = to->to_tsecr;
	}

	struct tcp_seg_sent seg = {};
	struct tcp_seg_sent *found_seg = NULL, *next = NULL;

	found_seg = TAILQ_LAST(&tp->t_segs_sent, tcp_seg_sent_head);

	if (tp->rack.segs_retransmitted == false) {
		if (SEQ_GEQ(th_ack, found_seg->end_seq)) {
			/*
			 * ACK acknowledges the last sent segment completely (snd_max),
			 * we can remove all segments from time ordered list.
			 */
			acked_seq = found_seg->end_seq;
			acked_xmit_ts = found_seg->xmit_ts;
			was_retransmitted = !!(found_seg->flags & TCP_SEGMENT_RETRANSMITTED_ATLEAST_ONCE);
			tcp_segs_sent_clean(tp, false);

			/* Advance RACK state */
			tcp_rack_update_segment_acked(tp, tsecr, acked_xmit_ts, acked_seq, was_retransmitted);
			return;
		}
	}
	/*
	 * If either not all segments are ACKed OR the time-ordered list contains retransmitted
	 * segments, do a RB tree search for largest (completely) ACKed segment and remove the ACKed
	 * segment and all segments left of it from both RB tree and time-ordered list.
	 *
	 * Set the end sequence to search for ACKed segment.
	 */
	seg.end_seq = th_ack;

	if ((found_seg = RB_FIND(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, &seg)) != NULL) {
		acked_seq = found_seg->end_seq;
		acked_xmit_ts = found_seg->xmit_ts;
		was_retransmitted = !!(found_seg->flags & TCP_SEGMENT_RETRANSMITTED_ATLEAST_ONCE);

		/*
		 * Remove all segments that are ACKed by this ACK.
		 * We defer self-balancing of RB tree to the end
		 * by calling RB_REMOVE after collecting all ACKed segments.
		 */
		tcp_seg_collect_acked(tp, RB_ROOT(&tp->t_segs_sent_tree), th_ack, acked_xmit_ts, tsecr);
		tcp_seg_delete_acked(tp, acked_xmit_ts, tsecr);

		/* Advance RACK state */
		tcp_rack_update_segment_acked(tp, tsecr, acked_xmit_ts, acked_seq, was_retransmitted);

		return;
	}
	/*
	 * When TSO is enabled, it is possible that th_ack is less
	 * than segment->end, hence we search the tree
	 * until we find the largest (partially) ACKed segment.
	 */
	RB_FOREACH_SAFE(found_seg, tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, next) {
		if (SEQ_LT(th_ack, found_seg->end_seq) && SEQ_GT(th_ack, found_seg->start_seq)) {
			acked_seq = th_ack;
			acked_xmit_ts = found_seg->xmit_ts;
			was_retransmitted = !!(found_seg->flags & TCP_SEGMENT_RETRANSMITTED_ATLEAST_ONCE);

			/* Remove all segments completely ACKed by this ack */
			tcp_seg_collect_acked(tp, RB_ROOT(&tp->t_segs_sent_tree), th_ack, acked_xmit_ts, tsecr);
			tcp_seg_delete_acked(tp, acked_xmit_ts, tsecr);
			found_seg->start_seq = th_ack;

			/* Advance RACK state */
			tcp_rack_update_segment_acked(tp, tsecr, acked_xmit_ts, acked_seq, was_retransmitted);
			break;
		}
	}
}

static bool
tcp_seg_mark_sacked(struct tcpcb *tp, struct tcp_seg_sent *seg, uint32_t *newbytes_sacked)
{
	if (seg->flags & TCP_SEGMENT_SACKED) {
		return false;
	}

	const uint32_t seg_len = tcp_seg_len(seg);

	/* Check for reordering */
	tcp_rack_detect_reordering_acked(tp, seg);

	if (seg->flags & TCP_RACK_RETRANSMITTED) {
		if (seg->flags & TCP_SEGMENT_LOST) {
			/*
			 * If the segment is not considered lost, we don't clear
			 * retransmitted as it might still be in flight. The ONLY time
			 * this can happen is when RTO happens and segment is retransmitted
			 * and SACKed before RACK detects segment was lost.
			 */
			seg->flags &= ~(TCP_SEGMENT_LOST | TCP_RACK_RETRANSMITTED);
			if (tp->bytes_lost < seg_len || tp->bytes_retransmitted < seg_len) {
				os_log_error(OS_LOG_DEFAULT, "bytes_lost (%u) and/or bytes_retransmitted (%u) "
				    "can't be smaller than already lost/retransmitted segment length (%u)", tp->bytes_lost,
				    tp->bytes_retransmitted, seg_len);
			}
			tp->bytes_lost -= seg_len;
			tp->bytes_retransmitted -= seg_len;
		}
	} else {
		if (seg->flags & TCP_SEGMENT_LOST) {
			seg->flags &= ~(TCP_SEGMENT_LOST);
			if (tp->bytes_lost < seg_len) {
				os_log_error(OS_LOG_DEFAULT, "bytes_lost (%u) can't be smaller "
				    "than already lost segment length (%u)", tp->bytes_lost, seg_len);
			}
			tp->bytes_lost -= seg_len;
		}
	}
	*newbytes_sacked += seg_len;
	seg->flags |= TCP_SEGMENT_SACKED;
	tp->bytes_sacked += seg_len;

	return true;
}

static void
tcp_segs_dosack_matched(struct tcpcb *tp, struct tcp_seg_sent *found_seg,
    tcp_seq sblk_start, uint32_t tsecr,
    uint32_t *newbytes_sacked)
{
	struct tcp_seg_sent seg = {};

	while (found_seg != NULL) {
		if (sblk_start == found_seg->start_seq) {
			/*
			 * Covered the entire SACK block.
			 * Record segment flags before they get erased.
			 */
			uint8_t seg_flags = found_seg->flags;
			bool newly_marked = tcp_seg_mark_sacked(tp, found_seg, newbytes_sacked);
			if (newly_marked) {
				/* Advance RACK state */
				tcp_rack_update_segment_acked(tp, tsecr, found_seg->xmit_ts,
				    found_seg->end_seq,
				    !!(seg_flags & TCP_SEGMENT_RETRANSMITTED_ATLEAST_ONCE));
			}
			break;
		} else if (SEQ_GT(sblk_start, found_seg->start_seq)) {
			if ((found_seg->flags & TCP_SEGMENT_SACKED) != 0) {
				/* No need to process an already SACKED segment */
				break;
			}
			/*
			 * This segment is partially ACKed by SACK block
			 * as sblk_start > segment start. Since it is
			 * partially SACKed, we should split the unSACKed and
			 * SACKed parts.
			 */
			/* First create a new segment for unSACKed part */
			tcp_seg_sent_insert_before(tp, found_seg, found_seg->start_seq, sblk_start,
			    found_seg->xmit_ts, found_seg->flags);
			/* Now, update the SACKed part */
			found_seg->start_seq = sblk_start;
			/* Record seg flags before they get erased. */
			uint8_t seg_flags = found_seg->flags;
			bool newly_marked = tcp_seg_mark_sacked(tp, found_seg, newbytes_sacked);
			if (newly_marked) {
				/* Advance RACK state */
				tcp_rack_update_segment_acked(tp, tsecr, found_seg->xmit_ts,
				    found_seg->end_seq,
				    !!(seg_flags & TCP_SEGMENT_RETRANSMITTED_ATLEAST_ONCE));
			}
			break;
		} else {
			/*
			 * This segment lies within the SACK block
			 * Record segment flags before they get erased.
			 */
			uint8_t seg_flags = found_seg->flags;
			bool newly_marked = tcp_seg_mark_sacked(tp, found_seg, newbytes_sacked);
			if (newly_marked) {
				/* Advance RACK state */
				tcp_rack_update_segment_acked(tp, tsecr, found_seg->xmit_ts,
				    found_seg->end_seq,
				    !!(seg_flags & TCP_SEGMENT_RETRANSMITTED_ATLEAST_ONCE));
			}
			/* Find the next segment ending at the start of current segment */
			seg.end_seq = found_seg->start_seq;
			found_seg = RB_FIND(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, &seg);
		}
	}
}

void
tcp_segs_dosack(struct tcpcb *tp, tcp_seq sblk_start, tcp_seq sblk_end,
    uint32_t tsecr, uint32_t *newbytes_sacked)
{
	/*
	 * When we receive SACK, min RTT is computed after SACK processing which
	 * means we are using min RTT from the previous ACK to advance RACK state
	 * This is ok as we track a windowed min-filtered estimate over a period.
	 */
	struct tcp_seg_sent seg = {};
	struct tcp_seg_sent *found_seg = NULL, *sacked_seg = NULL;

	/* Set the end sequence to search for SACKed segment */
	seg.end_seq = sblk_end;
	found_seg = RB_FIND(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, &seg);

	if (found_seg != NULL) {
		/* We found an exact match for sblk_end */
		tcp_segs_dosack_matched(tp, found_seg, sblk_start, tsecr, newbytes_sacked);
		return;
	}
	/*
	 * We come here when we don't find an exact match and sblk_end
	 * lies within a segment. This would happen only when TSO is used.
	 */
	RB_FOREACH(found_seg, tcp_seg_sent_tree_head, &tp->t_segs_sent_tree) {
		if (SEQ_LT(sblk_end, found_seg->end_seq) && SEQ_GT(sblk_end, found_seg->start_seq)) {
			/*
			 * This segment is partially SACKed. We split this segment at the boundary
			 * of SACK block. First insert the newly SACKed part
			 */
			tcp_seq start = SEQ_LEQ(sblk_start, found_seg->start_seq) ? found_seg->start_seq : sblk_start;
			struct tcp_seg_sent *inserted = tcp_seg_sent_insert_before(tp, found_seg, start,
			    sblk_end, found_seg->xmit_ts, found_seg->flags);
			/* Record seg flags before they get erased. */
			uint8_t seg_flags = inserted->flags;
			/* Mark the SACKed segment */
			tcp_seg_mark_sacked(tp, inserted, newbytes_sacked);

			/* Advance RACK state */
			tcp_rack_update_segment_acked(tp, tsecr, inserted->xmit_ts,
			    inserted->end_seq, !!(seg_flags & TCP_SEGMENT_RETRANSMITTED_ATLEAST_ONCE));

			if (sblk_start == found_seg->start_seq) {
				/*
				 * We are done with this SACK block.
				 * Move the start of existing segment
				 */
				found_seg->start_seq = sblk_end;
				break;
			}

			if (SEQ_GT(sblk_start, found_seg->start_seq)) {
				/* Insert the remaining unSACKed part before the SACKED segment inserted above */
				tcp_seg_sent_insert_before(tp, inserted, found_seg->start_seq,
				    sblk_start, found_seg->xmit_ts, found_seg->flags);
				/* Move the start of existing segment */
				found_seg->start_seq = sblk_end;
				break;
			} else {
				/*
				 * This SACK block covers more than one segment
				 * Look for segments SACKed below this segment
				 */
				seg.end_seq = found_seg->start_seq;
				sacked_seg = RB_FIND(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, &seg);

				if (sacked_seg != NULL) {
					/* We found an exact match for sblk_end */
					tcp_segs_dosack_matched(tp, sacked_seg, sblk_start, tsecr, newbytes_sacked);
				}

				/* Move the start of existing segment */
				found_seg->start_seq = sblk_end;
			}
			break;
		}
	}
}

void
tcp_segs_clear_sacked(struct tcpcb *tp)
{
	struct tcp_seg_sent *seg = NULL;

	TAILQ_FOREACH(seg, &tp->t_segs_sent, tx_link)
	{
		const uint32_t seg_len = tcp_seg_len(seg);

		if (seg->flags & TCP_SEGMENT_SACKED) {
			seg->flags &= ~(TCP_SEGMENT_SACKED);
			if (tp->bytes_sacked < seg_len) {
				os_log_error(OS_LOG_DEFAULT, "bytes_sacked (%u) can't be smaller "
				    "than already SACKed segment length (%u)", tp->bytes_sacked, seg_len);
			}
			tp->bytes_sacked -= seg_len;
		}
	}
}

void
tcp_mark_seg_lost(struct tcpcb *tp, struct tcp_seg_sent *seg)
{
	const uint32_t seg_len = tcp_seg_len(seg);

	if (seg->flags & TCP_SEGMENT_LOST) {
		if (seg->flags & TCP_RACK_RETRANSMITTED) {
			/* Retransmission was lost */
			seg->flags &= ~TCP_RACK_RETRANSMITTED;
			if (tp->bytes_retransmitted < seg_len) {
				os_log_error(OS_LOG_DEFAULT, "bytes_retransmitted (%u) can't be "
				    "smaller than retransmited segment length (%u)",
				    tp->bytes_retransmitted, seg_len);
				return;
			}
			tp->bytes_retransmitted -= seg_len;
		}
	} else {
		seg->flags |= TCP_SEGMENT_LOST;
		tp->bytes_lost += seg_len;
	}
}

void
tcp_seg_delete(struct tcpcb *tp, struct tcp_seg_sent *seg)
{
	if (tp->seg_pool.free_segs_count >= TCP_SEG_POOL_MAX_ITEM_COUNT) {
		zfree(tcp_seg_sent_zone, seg);
	} else {
		bzero(seg, sizeof(*seg));
		TAILQ_INSERT_TAIL(&tp->seg_pool.free_segs, seg, free_link);
		tp->seg_pool.free_segs_count++;
	}
}

void
tcp_segs_sent_clean(struct tcpcb *tp, bool free_segs)
{
	struct tcp_seg_sent *seg = NULL, *next = NULL;

	TAILQ_FOREACH_SAFE(seg, &tp->t_segs_sent, tx_link, next) {
		/* Check for reordering */
		tcp_rack_detect_reordering_acked(tp, seg);

		TAILQ_REMOVE(&tp->t_segs_sent, seg, tx_link);
		RB_REMOVE(tcp_seg_sent_tree_head, &tp->t_segs_sent_tree, seg);
		tcp_seg_delete(tp, seg);
	}
	if (__improbable(!RB_EMPTY(&tp->t_segs_sent_tree))) {
		os_log_error(OS_LOG_DEFAULT, "RB tree still contains segments while "
		    "time ordered list is already empty");
	}
	if (__improbable(!TAILQ_EMPTY(&tp->t_segs_acked))) {
		os_log_error(OS_LOG_DEFAULT, "Segment ACKed list shouldn't contain "
		    "any segments as they are removed immediately after being ACKed");
	}
	/* Reset seg_retransmitted as we emptied the list */
	tcp_rack_reset_segs_retransmitted(tp);
	tp->bytes_lost = tp->bytes_sacked = tp->bytes_retransmitted = 0;

	/* Empty the free segments pool */
	if (free_segs) {
		TAILQ_FOREACH_SAFE(seg, &tp->seg_pool.free_segs, free_link, next) {
			TAILQ_REMOVE(&tp->seg_pool.free_segs, seg, free_link);
			zfree(tcp_seg_sent_zone, seg);
		}
		tp->seg_pool.free_segs_count = 0;
	}
}

void
tcp_get_connectivity_status(struct tcpcb *tp,
    struct tcp_conn_status *connstatus)
{
	if (tp == NULL || connstatus == NULL) {
		return;
	}
	bzero(connstatus, sizeof(*connstatus));
	if (tp->t_rxtshift >= TCP_CONNECTIVITY_PROBES_MAX) {
		if (TCPS_HAVEESTABLISHED(tp->t_state)) {
			connstatus->write_probe_failed = 1;
		} else {
			connstatus->conn_probe_failed = 1;
		}
	}
	if (tp->t_rtimo_probes >= TCP_CONNECTIVITY_PROBES_MAX) {
		connstatus->read_probe_failed = 1;
	}
	if (tp->t_inpcb != NULL && tp->t_inpcb->inp_last_outifp != NULL &&
	    (tp->t_inpcb->inp_last_outifp->if_eflags & IFEF_PROBE_CONNECTIVITY)) {
		connstatus->probe_activated = 1;
	}
}

void
tcp_disable_tfo(struct tcpcb *tp)
{
	tp->t_flagsext &= ~TF_FASTOPEN;
}

static struct mbuf *
tcp_make_keepalive_frame(struct tcpcb *tp, struct ifnet *ifp,
    boolean_t is_probe)
{
	struct inpcb *inp = tp->t_inpcb;
	struct tcphdr *th;
	caddr_t data;
	int win = 0;
	struct mbuf *m;

	/*
	 * The code assumes the IP + TCP headers fit in an mbuf packet header
	 */
	_CASSERT(sizeof(struct ip) + sizeof(struct tcphdr) <= _MHLEN);
	_CASSERT(sizeof(struct ip6_hdr) + sizeof(struct tcphdr) <= _MHLEN);

	MGETHDR(m, M_WAIT, MT_HEADER);
	if (m == NULL) {
		return NULL;
	}
	m->m_pkthdr.pkt_proto = IPPROTO_TCP;

	data = m_mtod_lower_bound(m);

	if (inp->inp_vflag & INP_IPV4) {
		bzero(data, sizeof(struct ip) + sizeof(struct tcphdr));
		th = (struct tcphdr *)(void *) (data + sizeof(struct ip));
		m->m_len = sizeof(struct ip) + sizeof(struct tcphdr);
		m->m_pkthdr.len = m->m_len;
	} else {
		VERIFY(inp->inp_vflag & INP_IPV6);

		bzero(data, sizeof(struct ip6_hdr)
		    + sizeof(struct tcphdr));
		th = (struct tcphdr *)(void *)(data + sizeof(struct ip6_hdr));
		m->m_len = sizeof(struct ip6_hdr) +
		    sizeof(struct tcphdr);
		m->m_pkthdr.len = m->m_len;
	}

	tcp_fillheaders(m, tp, data, th);

	if (inp->inp_vflag & INP_IPV4) {
		struct ip *ip;

		ip = (__typeof__(ip))(void *)data;

		ip->ip_id = rfc6864 ? 0 : ip_randomid((uint64_t)m);
		ip->ip_off = htons(IP_DF);
		ip->ip_len = htons(sizeof(struct ip) + sizeof(struct tcphdr));
		ip->ip_ttl = inp->inp_ip_ttl;
		ip->ip_tos |= (inp->inp_ip_tos & ~IPTOS_ECN_MASK);
		ip->ip_sum = in_cksum_hdr(ip);
	} else {
		struct ip6_hdr *ip6;

		ip6 = (__typeof__(ip6))(void *)data;

		ip6->ip6_plen = htons(sizeof(struct tcphdr));
		ip6->ip6_hlim = in6_selecthlim(inp, ifp);
		ip6->ip6_flow = ip6->ip6_flow & ~IPV6_FLOW_ECN_MASK;

		if (IN6_IS_SCOPE_EMBED(&ip6->ip6_src)) {
			ip6->ip6_src.s6_addr16[1] = 0;
		}
		if (IN6_IS_SCOPE_EMBED(&ip6->ip6_dst)) {
			ip6->ip6_dst.s6_addr16[1] = 0;
		}
	}
	th->th_flags = TH_ACK;

	win = tcp_sbspace(tp);
	if (win > ((int32_t)TCP_MAXWIN << tp->rcv_scale)) {
		win = (int32_t)TCP_MAXWIN << tp->rcv_scale;
	}
	th->th_win = htons((u_short) (win >> tp->rcv_scale));

	if (is_probe) {
		th->th_seq = htonl(tp->snd_una - 1);
	} else {
		th->th_seq = htonl(tp->snd_una);
	}
	th->th_ack = htonl(tp->rcv_nxt);

	/* Force recompute TCP checksum to be the final value */
	th->th_sum = 0;
	if (inp->inp_vflag & INP_IPV4) {
		th->th_sum = inet_cksum(m, IPPROTO_TCP,
		    sizeof(struct ip), sizeof(struct tcphdr));
	} else {
		th->th_sum = inet6_cksum(m, IPPROTO_TCP,
		    sizeof(struct ip6_hdr), sizeof(struct tcphdr));
	}

	return m;
}

void
tcp_fill_keepalive_offload_frames(ifnet_t ifp,
    struct ifnet_keepalive_offload_frame *frames_array __counted_by(frames_array_count),
    u_int32_t frames_array_count, size_t frame_data_offset,
    u_int32_t *used_frames_count)
{
	struct inpcb *inp;
	inp_gen_t gencnt;
	u_int32_t frame_index = *used_frames_count;

	/* Validation of the parameters */
	if (ifp == NULL || frames_array == NULL ||
	    frames_array_count == 0 ||
	    frame_index >= frames_array_count ||
	    frame_data_offset >= IFNET_KEEPALIVE_OFFLOAD_FRAME_DATA_SIZE) {
		return;
	}

	/* Fast exit when no process is using the socket option TCP_KEEPALIVE_OFFLOAD */
	if (ifp->if_tcp_kao_cnt == 0) {
		return;
	}

	/*
	 * This function is called outside the regular TCP processing
	 * so we need to update the TCP clock.
	 */
	calculate_tcp_clock();

	lck_rw_lock_shared(&tcbinfo.ipi_lock);
	gencnt = tcbinfo.ipi_gencnt;
	LIST_FOREACH(inp, tcbinfo.ipi_listhead, inp_list) {
		struct socket *so;
		struct ifnet_keepalive_offload_frame *frame;
		struct mbuf *m = NULL;
		struct tcpcb *tp = intotcpcb(inp);

		if (frame_index >= frames_array_count) {
			break;
		}

		if (inp->inp_gencnt > gencnt ||
		    inp->inp_state == INPCB_STATE_DEAD) {
			continue;
		}

		if ((so = inp->inp_socket) == NULL ||
		    (so->so_state & SS_DEFUNCT)) {
			continue;
		}
		/*
		 * check for keepalive offload flag without socket
		 * lock to avoid a deadlock
		 */
		if (!(inp->inp_flags2 & INP2_KEEPALIVE_OFFLOAD)) {
			continue;
		}

		if (!(inp->inp_vflag & (INP_IPV4 | INP_IPV6))) {
			continue;
		}
		if (inp->inp_ppcb == NULL ||
		    in_pcb_checkstate(inp, WNT_ACQUIRE, 0) == WNT_STOPUSING) {
			continue;
		}
		socket_lock(so, 1);
		/* Release the want count */
		if (inp->inp_ppcb == NULL ||
		    (in_pcb_checkstate(inp, WNT_RELEASE, 1) == WNT_STOPUSING)) {
			socket_unlock(so, 1);
			continue;
		}
		if ((inp->inp_vflag & INP_IPV4) &&
		    (inp->inp_laddr.s_addr == INADDR_ANY ||
		    inp->inp_faddr.s_addr == INADDR_ANY)) {
			socket_unlock(so, 1);
			continue;
		}
		if ((inp->inp_vflag & INP_IPV6) &&
		    (IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_laddr) ||
		    IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_faddr))) {
			socket_unlock(so, 1);
			continue;
		}
		if (inp->inp_lport == 0 || inp->inp_fport == 0) {
			socket_unlock(so, 1);
			continue;
		}
		if (inp->inp_last_outifp == NULL ||
		    inp->inp_last_outifp->if_index != ifp->if_index) {
			socket_unlock(so, 1);
			continue;
		}
		if ((inp->inp_vflag & INP_IPV4) && frame_data_offset +
		    sizeof(struct ip) + sizeof(struct tcphdr) >
		    IFNET_KEEPALIVE_OFFLOAD_FRAME_DATA_SIZE) {
			socket_unlock(so, 1);
			continue;
		} else if (!(inp->inp_vflag & INP_IPV4) && frame_data_offset +
		    sizeof(struct ip6_hdr) + sizeof(struct tcphdr) >
		    IFNET_KEEPALIVE_OFFLOAD_FRAME_DATA_SIZE) {
			socket_unlock(so, 1);
			continue;
		}
		/*
		 * There is no point in waking up the device for connections
		 * that are not established. Long lived connection are meant
		 * for processes that will sent and receive data
		 */
		if (tp->t_state != TCPS_ESTABLISHED) {
			socket_unlock(so, 1);
			continue;
		}
		/*
		 * This inp has all the information that is needed to
		 * generate an offload frame.
		 */
		frame = &frames_array[frame_index];
		frame->type = IFNET_KEEPALIVE_OFFLOAD_FRAME_TCP;
		frame->ether_type = (inp->inp_vflag & INP_IPV4) ?
		    IFNET_KEEPALIVE_OFFLOAD_FRAME_ETHERTYPE_IPV4 :
		    IFNET_KEEPALIVE_OFFLOAD_FRAME_ETHERTYPE_IPV6;
		frame->interval = (uint16_t)(tp->t_keepidle > 0 ? tp->t_keepidle :
		    tcp_keepidle);
		frame->keep_cnt = (uint8_t)TCP_CONN_KEEPCNT(tp);
		frame->keep_retry = (uint16_t)TCP_CONN_KEEPINTVL(tp);
		if (so->so_options & SO_NOWAKEFROMSLEEP) {
			frame->flags |=
			    IFNET_KEEPALIVE_OFFLOAD_FLAG_NOWAKEFROMSLEEP;
		}
		frame->local_port = ntohs(inp->inp_lport);
		frame->remote_port = ntohs(inp->inp_fport);
		frame->local_seq = tp->snd_nxt;
		frame->remote_seq = tp->rcv_nxt;
		if (inp->inp_vflag & INP_IPV4) {
			ASSERT(frame_data_offset + sizeof(struct ip) + sizeof(struct tcphdr) <= UINT8_MAX);
			frame->length = (uint8_t)(frame_data_offset +
			    sizeof(struct ip) + sizeof(struct tcphdr));
			frame->reply_length =  frame->length;

			frame->addr_length = sizeof(struct in_addr);
			bcopy(&inp->inp_laddr, frame->local_addr,
			    sizeof(struct in_addr));
			bcopy(&inp->inp_faddr, frame->remote_addr,
			    sizeof(struct in_addr));
		} else {
			struct in6_addr *ip6;

			ASSERT(frame_data_offset + sizeof(struct ip6_hdr) + sizeof(struct tcphdr) <= UINT8_MAX);
			frame->length = (uint8_t)(frame_data_offset +
			    sizeof(struct ip6_hdr) + sizeof(struct tcphdr));
			frame->reply_length =  frame->length;

			frame->addr_length = sizeof(struct in6_addr);
			ip6 = (struct in6_addr *)(void *)frame->local_addr;
			bcopy(&inp->in6p_laddr, ip6, sizeof(struct in6_addr));
			if (IN6_IS_SCOPE_EMBED(ip6)) {
				ip6->s6_addr16[1] = 0;
			}

			ip6 = (struct in6_addr *)(void *)frame->remote_addr;
			bcopy(&inp->in6p_faddr, ip6, sizeof(struct in6_addr));
			if (IN6_IS_SCOPE_EMBED(ip6)) {
				ip6->s6_addr16[1] = 0;
			}
		}

		/*
		 * First the probe
		 */
		m = tcp_make_keepalive_frame(tp, ifp, TRUE);
		if (m == NULL) {
			socket_unlock(so, 1);
			continue;
		}
		bcopy(m_mtod_current(m), frame->data + frame_data_offset, m->m_len);
		m_freem(m);

		/*
		 * Now the response packet to incoming probes
		 */
		m = tcp_make_keepalive_frame(tp, ifp, FALSE);
		if (m == NULL) {
			socket_unlock(so, 1);
			continue;
		}
		bcopy(m_mtod_current(m), frame->reply_data + frame_data_offset,
		    m->m_len);
		m_freem(m);

		frame_index++;
		socket_unlock(so, 1);
	}
	lck_rw_done(&tcbinfo.ipi_lock);
	*used_frames_count = frame_index;
}

static bool
inp_matches_kao_frame(ifnet_t ifp, struct ifnet_keepalive_offload_frame *frame,
    struct inpcb *inp)
{
	if (inp->inp_ppcb == NULL) {
		return false;
	}
	/* Release the want count */
	if (in_pcb_checkstate(inp, WNT_RELEASE, 1) == WNT_STOPUSING) {
		return false;
	}
	if (inp->inp_last_outifp == NULL ||
	    inp->inp_last_outifp->if_index != ifp->if_index) {
		return false;
	}
	if (frame->local_port != ntohs(inp->inp_lport) ||
	    frame->remote_port != ntohs(inp->inp_fport)) {
		return false;
	}
	if (inp->inp_vflag & INP_IPV4) {
		if (memcmp(&inp->inp_laddr, frame->local_addr,
		    sizeof(struct in_addr)) != 0 ||
		    memcmp(&inp->inp_faddr, frame->remote_addr,
		    sizeof(struct in_addr)) != 0) {
			return false;
		}
	} else if (inp->inp_vflag & INP_IPV6) {
		if (memcmp(&inp->inp_laddr, frame->local_addr,
		    sizeof(struct in6_addr)) != 0 ||
		    memcmp(&inp->inp_faddr, frame->remote_addr,
		    sizeof(struct in6_addr)) != 0) {
			return false;
		}
	} else {
		return false;
	}
	return true;
}

int
tcp_notify_kao_timeout(ifnet_t ifp,
    struct ifnet_keepalive_offload_frame *frame)
{
	struct inpcb *inp = NULL;
	struct socket *so = NULL;
	bool found = false;

	/*
	 *  Unlock the list before posting event on the matching socket
	 */
	lck_rw_lock_shared(&tcbinfo.ipi_lock);

	LIST_FOREACH(inp, tcbinfo.ipi_listhead, inp_list) {
		if ((so = inp->inp_socket) == NULL ||
		    (so->so_state & SS_DEFUNCT)) {
			continue;
		}
		if (!(inp->inp_flags2 & INP2_KEEPALIVE_OFFLOAD)) {
			continue;
		}
		if (!(inp->inp_vflag & (INP_IPV4 | INP_IPV6))) {
			continue;
		}
		if (inp->inp_ppcb == NULL ||
		    in_pcb_checkstate(inp, WNT_ACQUIRE, 0) == WNT_STOPUSING) {
			continue;
		}
		socket_lock(so, 1);
		if (inp_matches_kao_frame(ifp, frame, inp)) {
			/*
			 * Keep the matching socket locked
			 */
			found = true;
			break;
		}
		socket_unlock(so, 1);
	}
	lck_rw_done(&tcbinfo.ipi_lock);

	if (found) {
		ASSERT(inp != NULL);
		ASSERT(so != NULL);
		ASSERT(so == inp->inp_socket);
		/*
		 * Drop the TCP connection like tcptimers() does
		 */
		tcpcb_ref_t tp = inp->inp_ppcb;

		tcpstat.tcps_keepdrops++;
		soevent(so,
		    (SO_FILT_HINT_LOCKED | SO_FILT_HINT_TIMEOUT));
		tp = tcp_drop(tp, ETIMEDOUT);

		tcpstat.tcps_ka_offload_drops++;
		os_log_info(OS_LOG_DEFAULT, "%s: dropped lport %u fport %u\n",
		    __func__, frame->local_port, frame->remote_port);

		socket_unlock(so, 1);
	}

	return 0;
}

errno_t
tcp_notify_ack_id_valid(struct tcpcb *tp, struct socket *so,
    u_int32_t notify_id)
{
	struct tcp_notify_ack_marker *elm;

	if (so->so_snd.sb_cc == 0) {
		return ENOBUFS;
	}

	SLIST_FOREACH(elm, &tp->t_notify_ack, notify_next) {
		/* Duplicate id is not allowed */
		if (elm->notify_id == notify_id) {
			return EINVAL;
		}
		/* Duplicate position is not allowed */
		if (elm->notify_snd_una == tp->snd_una + so->so_snd.sb_cc) {
			return EINVAL;
		}
	}
	return 0;
}

errno_t
tcp_add_notify_ack_marker(struct tcpcb *tp, u_int32_t notify_id)
{
	struct tcp_notify_ack_marker *nm, *elm = NULL;
	struct socket *so = tp->t_inpcb->inp_socket;

	nm = kalloc_type(struct tcp_notify_ack_marker, M_WAIT | Z_ZERO);
	if (nm == NULL) {
		return ENOMEM;
	}
	nm->notify_id = notify_id;
	nm->notify_snd_una = tp->snd_una + so->so_snd.sb_cc;

	SLIST_FOREACH(elm, &tp->t_notify_ack, notify_next) {
		if (SEQ_GT(nm->notify_snd_una, elm->notify_snd_una)) {
			break;
		}
	}

	if (elm == NULL) {
		VERIFY(SLIST_EMPTY(&tp->t_notify_ack));
		SLIST_INSERT_HEAD(&tp->t_notify_ack, nm, notify_next);
	} else {
		SLIST_INSERT_AFTER(elm, nm, notify_next);
	}
	tp->t_notify_ack_count++;
	return 0;
}

void
tcp_notify_ack_free(struct tcpcb *tp)
{
	struct tcp_notify_ack_marker *elm, *next;
	if (SLIST_EMPTY(&tp->t_notify_ack)) {
		return;
	}

	SLIST_FOREACH_SAFE(elm, &tp->t_notify_ack, notify_next, next) {
		SLIST_REMOVE(&tp->t_notify_ack, elm, tcp_notify_ack_marker,
		    notify_next);
		kfree_type(struct tcp_notify_ack_marker, elm);
	}
	SLIST_INIT(&tp->t_notify_ack);
	tp->t_notify_ack_count = 0;
}

inline void
tcp_notify_acknowledgement(struct tcpcb *tp, struct socket *so)
{
	struct tcp_notify_ack_marker *elm;

	elm = SLIST_FIRST(&tp->t_notify_ack);
	if (SEQ_GEQ(tp->snd_una, elm->notify_snd_una)) {
		soevent(so, SO_FILT_HINT_LOCKED | SO_FILT_HINT_NOTIFY_ACK);
	}
}

void
tcp_get_notify_ack_count(struct tcpcb *tp,
    struct tcp_notify_ack_complete *retid)
{
	struct tcp_notify_ack_marker *elm;
	uint32_t  complete = 0;

	SLIST_FOREACH(elm, &tp->t_notify_ack, notify_next) {
		if (SEQ_GEQ(tp->snd_una, elm->notify_snd_una)) {
			ASSERT(complete < UINT32_MAX);
			complete++;
		} else {
			break;
		}
	}
	retid->notify_pending = tp->t_notify_ack_count - complete;
	retid->notify_complete_count = min(TCP_MAX_NOTIFY_ACK, complete);
}

void
tcp_get_notify_ack_ids(struct tcpcb *tp,
    struct tcp_notify_ack_complete *retid)
{
	size_t i = 0;
	struct tcp_notify_ack_marker *elm, *next;

	SLIST_FOREACH_SAFE(elm, &tp->t_notify_ack, notify_next, next) {
		if (i >= retid->notify_complete_count) {
			break;
		}
		if (SEQ_GEQ(tp->snd_una, elm->notify_snd_una)) {
			retid->notify_complete_id[i++] = elm->notify_id;
			SLIST_REMOVE(&tp->t_notify_ack, elm,
			    tcp_notify_ack_marker, notify_next);
			kfree_type(struct tcp_notify_ack_marker, elm);
			tp->t_notify_ack_count--;
		} else {
			break;
		}
	}
}

bool
tcp_notify_ack_active(struct socket *so)
{
	if ((SOCK_DOM(so) == PF_INET || SOCK_DOM(so) == PF_INET6) &&
	    SOCK_TYPE(so) == SOCK_STREAM) {
		struct tcpcb *tp = intotcpcb(sotoinpcb(so));

		if (!SLIST_EMPTY(&tp->t_notify_ack)) {
			struct tcp_notify_ack_marker *elm;
			elm = SLIST_FIRST(&tp->t_notify_ack);
			if (SEQ_GEQ(tp->snd_una, elm->notify_snd_una)) {
				return true;
			}
		}
	}
	return false;
}

inline int32_t
inp_get_sndbytes_allunsent(struct socket *so, u_int32_t th_ack)
{
	struct inpcb *inp = sotoinpcb(so);
	struct tcpcb *tp = intotcpcb(inp);

	if ((so->so_snd.sb_flags & SB_SNDBYTE_CNT) &&
	    so->so_snd.sb_cc > 0) {
		int32_t unsent, sent;
		sent = tp->snd_max - th_ack;
		if (tp->t_flags & TF_SENTFIN) {
			sent--;
		}
		unsent = so->so_snd.sb_cc - sent;
		return unsent;
	}
	return 0;
}

uint8_t
tcp_get_ace(struct tcphdr *th)
{
	uint8_t ace = 0;
	if (th->th_flags & TH_ECE) {
		ace += 1;
	}
	if (th->th_flags & TH_CWR) {
		ace += 2;
	}
	if (th->th_x2 & (TH_AE >> 8)) {
		ace += 4;
	}

	return ace;
}

#define IFP_PER_FLOW_STAT(_ipv4_, _stat_) { \
	if (_ipv4_) { \
	        ifp->if_ipv4_stat->_stat_++; \
	} else { \
	        ifp->if_ipv6_stat->_stat_++; \
	} \
}

#define FLOW_ECN_ENABLED(_flags_) \
    ((_flags_ & (TE_ECN_ON)) == (TE_ECN_ON))

void
tcp_update_stats_per_flow(struct ifnet_stats_per_flow *ifs,
    struct ifnet *ifp)
{
	if (ifp == NULL || !IF_FULLY_ATTACHED(ifp)) {
		return;
	}

	ifnet_lock_shared(ifp);
	if (ifs->ecn_flags & TE_SETUPSENT) {
		if (ifs->ecn_flags & TE_CLIENT_SETUP) {
			IFP_PER_FLOW_STAT(ifs->ipv4, ecn_client_setup);
			if (FLOW_ECN_ENABLED(ifs->ecn_flags)) {
				IFP_PER_FLOW_STAT(ifs->ipv4,
				    ecn_client_success);
			} else if (ifs->ecn_flags & TE_LOST_SYN) {
				IFP_PER_FLOW_STAT(ifs->ipv4,
				    ecn_syn_lost);
			} else {
				IFP_PER_FLOW_STAT(ifs->ipv4,
				    ecn_peer_nosupport);
			}
		} else {
			IFP_PER_FLOW_STAT(ifs->ipv4, ecn_server_setup);
			if (FLOW_ECN_ENABLED(ifs->ecn_flags)) {
				IFP_PER_FLOW_STAT(ifs->ipv4,
				    ecn_server_success);
			} else if (ifs->ecn_flags & TE_LOST_SYN) {
				IFP_PER_FLOW_STAT(ifs->ipv4,
				    ecn_synack_lost);
			} else {
				IFP_PER_FLOW_STAT(ifs->ipv4,
				    ecn_peer_nosupport);
			}
		}
	} else {
		IFP_PER_FLOW_STAT(ifs->ipv4, ecn_off_conn);
	}
	if (FLOW_ECN_ENABLED(ifs->ecn_flags)) {
		if (ifs->ecn_flags & TE_RECV_ECN_CE) {
			tcpstat.tcps_ecn_conn_recv_ce++;
			IFP_PER_FLOW_STAT(ifs->ipv4, ecn_conn_recv_ce);
		}
		if (ifs->ecn_flags & TE_RECV_ECN_ECE) {
			tcpstat.tcps_ecn_conn_recv_ece++;
			IFP_PER_FLOW_STAT(ifs->ipv4, ecn_conn_recv_ece);
		}
		if (ifs->ecn_flags & (TE_RECV_ECN_CE | TE_RECV_ECN_ECE)) {
			if (ifs->txretransmitbytes > 0 ||
			    ifs->rxoutoforderbytes > 0) {
				tcpstat.tcps_ecn_conn_pl_ce++;
				IFP_PER_FLOW_STAT(ifs->ipv4, ecn_conn_plce);
			} else {
				tcpstat.tcps_ecn_conn_nopl_ce++;
				IFP_PER_FLOW_STAT(ifs->ipv4, ecn_conn_noplce);
			}
		} else {
			if (ifs->txretransmitbytes > 0 ||
			    ifs->rxoutoforderbytes > 0) {
				tcpstat.tcps_ecn_conn_plnoce++;
				IFP_PER_FLOW_STAT(ifs->ipv4, ecn_conn_plnoce);
			}
		}
	}

	/* Other stats are interesting for non-local connections only */
	if (ifs->local) {
		ifnet_lock_done(ifp);
		return;
	}

	if (ifs->ipv4) {
		ifp->if_ipv4_stat->timestamp = net_uptime();
		if (FLOW_ECN_ENABLED(ifs->ecn_flags)) {
			tcp_flow_ecn_perf_stats(ifs, &ifp->if_ipv4_stat->ecn_on);
		} else {
			tcp_flow_ecn_perf_stats(ifs, &ifp->if_ipv4_stat->ecn_off);
		}
	} else {
		ifp->if_ipv6_stat->timestamp = net_uptime();
		if (FLOW_ECN_ENABLED(ifs->ecn_flags)) {
			tcp_flow_ecn_perf_stats(ifs, &ifp->if_ipv6_stat->ecn_on);
		} else {
			tcp_flow_ecn_perf_stats(ifs, &ifp->if_ipv6_stat->ecn_off);
		}
	}

	if (ifs->rxmit_drop) {
		if (FLOW_ECN_ENABLED(ifs->ecn_flags)) {
			IFP_PER_FLOW_STAT(ifs->ipv4, ecn_on.rxmit_drop);
		} else {
			IFP_PER_FLOW_STAT(ifs->ipv4, ecn_off.rxmit_drop);
		}
	}
	if (ifs->ecn_fallback_synloss) {
		IFP_PER_FLOW_STAT(ifs->ipv4, ecn_fallback_synloss);
	}
	if (ifs->ecn_fallback_droprst) {
		IFP_PER_FLOW_STAT(ifs->ipv4, ecn_fallback_droprst);
	}
	if (ifs->ecn_fallback_droprxmt) {
		IFP_PER_FLOW_STAT(ifs->ipv4, ecn_fallback_droprxmt);
	}
	if (ifs->ecn_fallback_ce) {
		IFP_PER_FLOW_STAT(ifs->ipv4, ecn_fallback_ce);
	}
	if (ifs->ecn_fallback_reorder) {
		IFP_PER_FLOW_STAT(ifs->ipv4, ecn_fallback_reorder);
	}
	if (ifs->ecn_recv_ce > 0) {
		IFP_PER_FLOW_STAT(ifs->ipv4, ecn_recv_ce);
	}
	if (ifs->ecn_recv_ece > 0) {
		IFP_PER_FLOW_STAT(ifs->ipv4, ecn_recv_ece);
	}

	tcp_flow_lim_stats(ifs, &ifp->if_lim_stat);

	/*
	 * Link heuristics are updated here only for NECP client flow when they close
	 * Socket flows are updated live
	 */
	os_atomic_add(&ifp->if_tcp_stat->linkheur_noackpri, ifs->linkheur_noackpri, relaxed);
	os_atomic_add(&ifp->if_tcp_stat->linkheur_comprxmt, ifs->linkheur_comprxmt, relaxed);
	os_atomic_add(&ifp->if_tcp_stat->linkheur_synrxmt, ifs->linkheur_synrxmt, relaxed);
	os_atomic_add(&ifp->if_tcp_stat->linkheur_rxmtfloor, ifs->linkheur_rxmtfloor, relaxed);

	ifnet_lock_done(ifp);
}

struct tseg_qent *
tcp_reass_qent_alloc(void)
{
	return zalloc_flags(tcp_reass_zone, Z_WAITOK | Z_NOFAIL);
}

void
tcp_reass_qent_free(struct tseg_qent *te)
{
	zfree(tcp_reass_zone, te);
}

struct tcp_rxt_seg *
tcp_rxt_seg_qent_alloc(void)
{
	return zalloc_flags(tcp_rxt_seg_zone, Z_WAITOK | Z_ZERO | Z_NOFAIL);
}

void
tcp_rxt_seg_qent_free(struct tcp_rxt_seg *te)
{
	zfree(tcp_rxt_seg_zone, te);
}


struct tcp_seg_sent *
tcp_seg_sent_qent_alloc(void)
{
	return zalloc_flags(tcp_seg_sent_zone, Z_WAITOK | Z_ZERO | Z_NOFAIL);
}

void
tcp_seg_sent_qent_free(struct tcp_seg_sent *te)
{
	zfree(tcp_seg_sent_zone, te);
}

#if SKYWALK

#include <skywalk/core/skywalk_var.h>
#include <skywalk/nexus/flowswitch/nx_flowswitch.h>

void
tcp_add_fsw_flow(struct tcpcb *tp, struct ifnet *ifp)
{
	struct inpcb *inp = tp->t_inpcb;
	struct socket *so = inp->inp_socket;
	uuid_t fsw_uuid;
	struct nx_flow_req nfr;
	int err;

	if (!NX_FSW_TCP_RX_AGG_ENABLED()) {
		return;
	}

	if (ifp == NULL || kern_nexus_get_flowswitch_instance(ifp, fsw_uuid)) {
		TCP_LOG_FSW_FLOW(tp, "skip ifp no fsw");
		return;
	}

	memset(&nfr, 0, sizeof(nfr));

	if (inp->inp_vflag & INP_IPV4) {
		ASSERT(!(inp->inp_laddr.s_addr == INADDR_ANY ||
		    inp->inp_faddr.s_addr == INADDR_ANY ||
		    IN_MULTICAST(ntohl(inp->inp_laddr.s_addr)) ||
		    IN_MULTICAST(ntohl(inp->inp_faddr.s_addr))));
		nfr.nfr_saddr.sin.sin_len = sizeof(struct sockaddr_in);
		nfr.nfr_saddr.sin.sin_family = AF_INET;
		nfr.nfr_saddr.sin.sin_port = inp->inp_lport;
		memcpy(&nfr.nfr_saddr.sin.sin_addr, &inp->inp_laddr,
		    sizeof(struct in_addr));
		nfr.nfr_daddr.sin.sin_len = sizeof(struct sockaddr_in);
		nfr.nfr_daddr.sin.sin_family = AF_INET;
		nfr.nfr_daddr.sin.sin_port = inp->inp_fport;
		memcpy(&nfr.nfr_daddr.sin.sin_addr, &inp->inp_faddr,
		    sizeof(struct in_addr));
	} else {
		ASSERT(!(IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_laddr) ||
		    IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_faddr) ||
		    IN6_IS_ADDR_MULTICAST(&inp->in6p_laddr) ||
		    IN6_IS_ADDR_MULTICAST(&inp->in6p_faddr)));
		nfr.nfr_saddr.sin6.sin6_len = sizeof(struct sockaddr_in6);
		nfr.nfr_saddr.sin6.sin6_family = AF_INET6;
		nfr.nfr_saddr.sin6.sin6_port = inp->inp_lport;
		memcpy(&nfr.nfr_saddr.sin6.sin6_addr, &inp->in6p_laddr,
		    sizeof(struct in6_addr));
		nfr.nfr_daddr.sin6.sin6_len = sizeof(struct sockaddr_in6);
		nfr.nfr_daddr.sin.sin_family = AF_INET6;
		nfr.nfr_daddr.sin6.sin6_port = inp->inp_fport;
		memcpy(&nfr.nfr_daddr.sin6.sin6_addr, &inp->in6p_faddr,
		    sizeof(struct in6_addr));
		/* clear embedded scope ID */
		if (IN6_IS_SCOPE_EMBED(&nfr.nfr_saddr.sin6.sin6_addr)) {
			nfr.nfr_saddr.sin6.sin6_addr.s6_addr16[1] = 0;
		}
		if (IN6_IS_SCOPE_EMBED(&nfr.nfr_daddr.sin6.sin6_addr)) {
			nfr.nfr_daddr.sin6.sin6_addr.s6_addr16[1] = 0;
		}
	}

	nfr.nfr_nx_port = 1;
	nfr.nfr_ip_protocol = IPPROTO_TCP;
	nfr.nfr_transport_protocol = IPPROTO_TCP;
	nfr.nfr_flags = NXFLOWREQF_ASIS;
	nfr.nfr_epid = (so != NULL ? so->last_pid : 0);
	if (NETNS_TOKEN_VALID(&inp->inp_netns_token)) {
		nfr.nfr_port_reservation = inp->inp_netns_token;
		nfr.nfr_flags |= NXFLOWREQF_EXT_PORT_RSV;
	}
	ASSERT(inp->inp_flowhash != 0);
	nfr.nfr_inp_flowhash = inp->inp_flowhash;

	uuid_generate_random(nfr.nfr_flow_uuid);
	err = kern_nexus_flow_add(kern_nexus_shared_controller(), fsw_uuid,
	    &nfr, sizeof(nfr));

	if (err == 0) {
		uuid_copy(tp->t_fsw_uuid, fsw_uuid);
		uuid_copy(tp->t_flow_uuid, nfr.nfr_flow_uuid);
	}

	TCP_LOG_FSW_FLOW(tp, "add err %d\n", err);
}

void
tcp_del_fsw_flow(struct tcpcb *tp)
{
	if (uuid_is_null(tp->t_fsw_uuid) || uuid_is_null(tp->t_flow_uuid)) {
		return;
	}

	struct nx_flow_req nfr;
	uuid_copy(nfr.nfr_flow_uuid, tp->t_flow_uuid);

	/* It's possible for this call to fail if the nexus has detached */
	int err = kern_nexus_flow_del(kern_nexus_shared_controller(),
	    tp->t_fsw_uuid, &nfr, sizeof(nfr));
	VERIFY(err == 0 || err == ENOENT || err == ENXIO);

	uuid_clear(tp->t_fsw_uuid);
	uuid_clear(tp->t_flow_uuid);

	TCP_LOG_FSW_FLOW(tp, "del err %d\n", err);
}

#endif /* SKYWALK */
