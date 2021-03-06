/*	$FreeBSD$	*/
/*	$KAME: if_stf.c,v 1.42 2000/08/15 07:24:23 itojun Exp $	*/

/*
 * Copyright (C) 2000 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * 6to4 interface, based on draft-ietf-ngtrans-6to4-06.txt.
 *
 * 6to4 interface is NOT capable of link-layer (I mean, IPv4) multicasting.
 * There is no address mapping defined from IPv6 multicast address to IPv4
 * address.  Therefore, we do not have IFF_MULTICAST on the interface.
 *
 * Due to the lack of address mapping for link-local addresses, we cannot
 * throw packets toward link-local addresses (fe80::x).  Also, we cannot throw
 * packets to link-local multicast addresses (ff02::x).
 *
 * Here are interesting symptoms due to the lack of link-local address:
 *
 * Unicast routing exchange:
 * - RIPng: Impossible.  Uses link-local multicast packet toward ff02::9,
 *   and link-local addresses as nexthop.
 * - OSPFv6: Impossible.  OSPFv6 assumes that there's link-local address
 *   assigned to the link, and makes use of them.  Also, HELLO packets use
 *   link-local multicast addresses (ff02::5 and ff02::6).
 * - BGP4+: Maybe.  You can only use global address as nexthop, and global
 *   address as TCP endpoint address.
 *
 * Multicast routing protocols:
 * - PIM: Hello packet cannot be used to discover adjacent PIM routers.
 *   Adjacent PIM routers must be configured manually (is it really spec-wise
 *   correct thing to do?).
 *
 * ICMPv6:
 * - Redirects cannot be used due to the lack of link-local address.
 *
 * Starting from 04 draft, the specification suggests how to construct
 * link-local address for 6to4 interface.
 * However, it seems to have no real use and does not help the above symptom
 * much.  Even if we assign link-locals to interface, we cannot really
 * use link-local unicast/multicast on top of 6to4 cloud, and the above
 * analysis does not change.
 *
 * 6to4 interface has security issues.  Refer to
 * http://playground.iijlab.net/i-d/draft-itojun-ipv6-transition-abuse-00.txt
 * for details.  The code tries to filter out some of malicious packets.
 * Note that there is no way to be 100% secure.
 */

//#include "opt_inet.h"
//#include "opt_inet6.h"

#include <sys/param.h>
//#include <sys/systm.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/errno.h>
#include <sys/protosw.h>


#include <sys/malloc.h>


#include <net/if.h>
#include <net/route.h>
#include <net/netisr.h>
#include <net/if_types.h>
#include <net/if_stf.h>

#include <netinet/in.h>

#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/in_var.h>

#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet6/in6_gif.h>
#include <netinet6/in6_var.h>

#include <netinet/ip_ecn.h>

#include <netinet/ip_encap.h>

//#include <machine/stdarg.h>

//#include <net/net_osdep.h>

//#include "bpf.h"
#define NBPFILTER	NBPF
//#include "stf.h"
//#include "gif.h"	/*XXX*/

#if NBPFILTER > 0
#include <net/bpf.h>
#endif

#if NGIF > 0
#include <net/if_gif.h>
#endif


#if NSTF > 0
#if NSTF != 1
# error only single stf interface allowed
#endif

#define IN6_IS_ADDR_6TO4(x)	(ntohs((x)->s6_addr16[0]) == 0x2002)
#define GET_V4(x)	((struct in_addr *)(&(x)->s6_addr16[1]))

struct route_in6 {
	struct	rtentry *ro_rt;
	struct	sockaddr_in6  ro_dst6;/*XXX*/
};

struct stf_softc {
	struct ifnet	sc_if;	   /* common area */
	union {
		struct route  __sc_ro4;
		struct route_in6 __sc_ro6; /* just for safety */
	} __sc_ro46;
#define sc_ro	__sc_ro46.__sc_ro4
	const struct encaptab *encap_cookie;
};

static struct stf_softc *stf;
static int nstf;

#if NGIF > 0
extern int ip_gif_ttl;	/*XXX*/
#else
static int ip_gif_ttl = 40;	/*XXX*/
#endif

extern struct protosw in_stf_protosw;

void stfattach __P((void *));
static int stf_encapcheck __P((const struct mbuf *, int, int, void *));
static struct in6_ifaddr *stf_getsrcifa6 __P((struct ifnet *));
static int stf_output __P((struct ifnet *, struct mbuf *, struct sockaddr *,
	struct rtentry *));
static int stf_checkaddr4 __P((struct in_addr *, struct ifnet *));
static int stf_checkaddr6 __P((struct in6_addr *, struct ifnet *));
static void stf_rtrequest __P((int, struct rtentry *, struct sockaddr *));
static int stf_ioctl __P((struct ifnet *, u_long, caddr_t));

void
stfattach(dummy)
	void *dummy;
{
	struct stf_softc *sc;
	int i;
	const struct encaptab *p;

	nstf = NSTF;
	stf = malloc(nstf * sizeof(struct stf_softc), M_DEVBUF, M_WAIT);
	bzero(stf, nstf * sizeof(struct stf_softc));
	sc = stf;
	/* XXX just in case... */
	for (i = 0; i < nstf; i++) {
		sc = &stf[i];
		bzero(sc, sizeof(*sc));
		sc->sc_if.if_name = "stf";
		sc->sc_if.if_unit = i;

		p = encap_attach_func(AF_INET, IPPROTO_IPV6, stf_encapcheck,
		    &in_stf_protosw, sc);
		if (p == NULL) {
			diag_printf("%s: attach failed\n", if_name(&sc->sc_if));
			continue;
		}
		sc->encap_cookie = p;

		sc->sc_if.if_mtu    = IPV6_MMTU;
		sc->sc_if.if_flags  = 0;
		sc->sc_if.if_ioctl  = stf_ioctl;
		sc->sc_if.if_output = stf_output;
		sc->sc_if.if_type   = IFT_STF;
		sc->sc_if.if_snd.ifq_maxlen = IFQ_MAXLEN;
		if_attach(&sc->sc_if);
#if NBPFILTER > 0
#ifdef HAVE_OLD_BPF
		bpfattach(&sc->sc_if, DLT_NULL, sizeof(u_int));
#else
		bpfattach(&sc->sc_if.if_bpf, &sc->sc_if, DLT_NULL, sizeof(u_int));
#endif
#endif
	}
}

PSEUDO_SET(stfattach, if_stf);

static int
stf_encapcheck(m, off, proto, arg)
	const struct mbuf *m;
	int off;
	int proto;
	void *arg;
{
	struct ip ip;
	struct in6_ifaddr *ia6;
	struct stf_softc *sc;
	struct in_addr a, b, tmp;

	sc = (struct stf_softc *)arg;
	if (sc == NULL)
		return 0;

	if ((sc->sc_if.if_flags & IFF_UP) == 0)
		return 0;

	if (proto != IPPROTO_IPV6)
		return 0;

	/* LINTED const cast */
	m_copydata((struct mbuf *)m, 0, sizeof(ip), (caddr_t)&ip);
	if (ip.ip_v != 4)
		return 0;
	ia6 = stf_getsrcifa6(&sc->sc_if);
	if (ia6 == NULL)
		return 0;

	/*
	 * check if IPv4 dst matches the IPv4 address derived from the
	 * local 6to4 address.
	 * success on: dst = 10.1.1.1, ia6->ia_addr = 2002:0a01:0101:...
	 */
	if (bcmp(GET_V4(&ia6->ia_addr.sin6_addr), &ip.ip_dst,
	    sizeof(ip.ip_dst)) != 0)
		return 0;

#if 0/*TODO XXX <Check For what??>    by ziqiang*/
	/*
	 * check if IPv4 src matches the IPv4 address derived from the
	 * local 6to4 address masked by prefixmask.                                  
	 * success on: src = 10.1.1.1, ia6->ia_addr = 2002:0a00:.../24
	 * fail on: src = 10.1.1.1, ia6->ia_addr = 2002:0b00:.../24
	 */
	bzero(&a, sizeof(a));
	bcopy(GET_V4(&ia6->ia_addr.sin6_addr),&a,4);
	bcopy(GET_V4(&ia6->ia_prefixmask.sin6_addr),&tmp,sizeof(tmp));
	a.s_addr &=tmp.s_addr;
	bcopy(&(ip.ip_src),&b,sizeof(b));
	b.s_addr &=tmp.s_addr;
	if (a.s_addr != b.s_addr)
		return 0;
#endif
	
	/* stf interface makes single side match only */
	return 32;
}

static struct in6_ifaddr *
stf_getsrcifa6(ifp)
	struct ifnet *ifp;
{
	struct ifaddr *ia;
	struct in_ifaddr *ia4;
	struct sockaddr_in6 *sin6;
	struct in_addr in;

	for (ia = ifp->if_addrlist.tqh_first;
	     ia;
	     ia = ia->ifa_list.tqe_next)
	{
		if (ia->ifa_addr == NULL)
			continue;
		if (ia->ifa_addr->sa_family != AF_INET6)
			continue;
		sin6 = (struct sockaddr_in6 *)ia->ifa_addr;
		if (!IN6_IS_ADDR_6TO4(&sin6->sin6_addr))
			continue;

		bcopy(GET_V4(&sin6->sin6_addr), &in, sizeof(in));
		for (ia4 = TAILQ_FIRST(&in_ifaddrhead);
		     ia4;
		     ia4 = TAILQ_NEXT(ia4, ia_link))
		{
			if (ia4->ia_addr.sin_addr.s_addr == in.s_addr)
				break;
		}
		if (ia4 == NULL)
			continue;

		return (struct in6_ifaddr *)ia;
	}

	return NULL;
}

static int
stf_output(ifp, m, dst, rt)
	struct ifnet *ifp;
	struct mbuf *m;
	struct sockaddr *dst;
	struct rtentry *rt;
{
	struct stf_softc *sc;
	struct sockaddr_in6 *dst6;
	struct sockaddr_in *dst4;
	u_int8_t tos;
	struct ip *ip;
	struct ip6_hdr *ip6;
	struct in6_ifaddr *ia6;


	sc = (struct stf_softc*)ifp;
	dst6 = (struct sockaddr_in6 *)dst;

	/* just in case */
	if ((ifp->if_flags & IFF_UP) == 0) {
		m_freem(m);
		return ENETDOWN;
	}

	/*
	 * If we don't have an ip4 address that match my inner ip6 address,
	 * we shouldn't generate output.  Without this check, we'll end up
	 * using wrong IPv4 source.
	 */
	ia6 = stf_getsrcifa6(ifp);
	if (ia6 == NULL) {
		m_freem(m);
		return ENETDOWN;
	}

	if (m->m_len < sizeof(*ip6)) {
		m = m_pullup(m, sizeof(*ip6));
		if (!m)
			{
			return ENOBUFS;
			}
	}
	ip6 = mtod(m, struct ip6_hdr *);
	tos = (ntohl(ip6->ip6_flow) >> 20) & 0xff;

	M_PREPEND(m, sizeof(struct ip), M_DONTWAIT);
	if (m && m->m_len < sizeof(struct ip))
		m = m_pullup(m, sizeof(struct ip));
	if (m == NULL)
		{
		return ENOBUFS;
		}
	ip = mtod(m, struct ip *);

	bzero(ip, sizeof(*ip));

	/*
	*we always use the stf interface's ipv4 address as IPv4 src 
	*/
	bcopy(GET_V4(&((struct sockaddr_in6 *)&ia6->ia_addr)->sin6_addr),
	    &ip->ip_src, sizeof(ip->ip_src));
	
	/*
	*If the dst is not a 6to4 IPv6 address ,we use the default gateway enbeded IPv4 address
	*as IPv4 dst.the default gateway is in the <dst>
	*/
	bcopy(GET_V4(&dst6->sin6_addr), &ip->ip_dst, sizeof(ip->ip_dst));
	ip->ip_p = IPPROTO_IPV6;
	ip->ip_ttl = ip_gif_ttl;	/*XXX*/
	ip->ip_len = m->m_pkthdr.len;	/*host order*/

	if (ifp->if_flags & IFF_LINK1)
		ip_ecn_ingress(ECN_ALLOWED, &ip->ip_tos, &tos);

	dst4 = (struct sockaddr_in *)&sc->sc_ro.ro_dst;
	if (dst4->sin_family != AF_INET ||
	    bcmp(&dst4->sin_addr, &ip->ip_dst, sizeof(ip->ip_dst)) != 0) {
		/* cache route doesn't match */
		dst4->sin_family = AF_INET;
		dst4->sin_len = sizeof(struct sockaddr_in);
		bcopy(&ip->ip_dst, &dst4->sin_addr, sizeof(dst4->sin_addr));
		if (sc->sc_ro.ro_rt) {
			RTFREE(sc->sc_ro.ro_rt);
			sc->sc_ro.ro_rt = NULL;
		}
	}

	if (sc->sc_ro.ro_rt == NULL) {
		rtalloc(&sc->sc_ro);
		if (sc->sc_ro.ro_rt == NULL) {
			m_freem(m);
			return ENETUNREACH;
		}
	}
	return ip_output(m, NULL, &sc->sc_ro, 0, NULL);
}

static int
stf_checkaddr4(in, ifp)
	struct in_addr *in;
	struct ifnet *ifp;	/* incoming interface */
{
	struct in_ifaddr *ia4;
	/*
	 * reject packets with the following address:
	 * 224.0.0.0/4 0.0.0.0/8 127.0.0.0/8 255.0.0.0/8
	 */
	if (IN_MULTICAST(ntohl(in->s_addr)))
		return -1;
	switch ((ntohl(in->s_addr) & 0xff000000) >> 24) {
	case 0: case 127: case 255:
		return -1;
	}

	/*
	 * reject packets with broadcast
	 */
	for (ia4 = TAILQ_FIRST(&in_ifaddrhead);
	     ia4;
	     ia4 = TAILQ_NEXT(ia4, ia_link))
	{
		if ((ia4->ia_ifa.ifa_ifp->if_flags & IFF_BROADCAST) == 0)
			continue;
		if (in->s_addr == ia4->ia_broadaddr.sin_addr.s_addr)
			return -1;
	}

	/*
	 * perform ingress filter
	 */
	if (ifp) {
		struct sockaddr_in sin;
		struct rtentry *rt;

		bzero(&sin, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_len = sizeof(struct sockaddr_in);
		sin.sin_addr = *in;
		rt = rtalloc1((struct sockaddr *)&sin, 0, 0UL);
		if (!rt)
			return -1;
		if (rt->rt_ifp != ifp) {
			rtfree(rt);
			return -1;
		}
		rtfree(rt);
	}

	return 0;
}

static int
stf_checkaddr6(in6, ifp)
	struct in6_addr *in6;
	struct ifnet *ifp;	/* incoming interface */
{
	int ipv4_addr;
	/*
	 * check 6to4 addresses
	 */
	bcopy(GET_V4(in6),&ipv4_addr,sizeof (ipv4_addr));//fix alignment crash ,
	if (IN6_IS_ADDR_6TO4(in6))
		return stf_checkaddr4(&ipv4_addr, ifp);

	/*
	 * reject anything that look suspicious.  the test is implemented
	 * in ip6_input too, but we check here as well to
	 * (1) reject bad packets earlier, and
	 * (2) to be safe against future ip6_input change.
	 */
	if (IN6_IS_ADDR_V4COMPAT(in6) || IN6_IS_ADDR_V4MAPPED(in6))
		return -1;

	return 0;
}


void
in_stf_input(struct mbuf *m, int len)
{
	int off, proto;
	struct stf_softc *sc;
	struct ip *ip;
	struct ip6_hdr *ip6;
	u_int8_t otos, itos;
	int s, isr;
	struct ifqueue *ifq = NULL;
	struct ifnet *ifp;

	off = len;
	ip = mtod(m, struct ip *);
	proto = ip->ip_p;

	if (proto != IPPROTO_IPV6) {
		m_freem(m);
		return;
	}

	sc = (struct stf_softc *)encap_getarg(m);
	if (sc == NULL || (sc->sc_if.if_flags & IFF_UP) == 0) {
		m_freem(m);
		return;
	}
	ifp = &sc->sc_if;

	/*
	 * perform sanity check against outer src/dst.
	 * for source, perform ingress filter as well.
	 */
	if (stf_checkaddr4(&ip->ip_dst, NULL) < 0 ||
	    stf_checkaddr4(&ip->ip_src, m->m_pkthdr.rcvif) < 0) {
		m_freem(m);
		return;
	}

	otos = ip->ip_tos;
	m_adj(m, off);

	if (m->m_len < sizeof(*ip6)) {
		m = m_pullup(m, sizeof(*ip6));
		if (!m)
			{
			return;
			}
	}
	ip6 = mtod(m, struct ip6_hdr *);

	/*
	 * perform sanity check against inner src/dst.
	 * for source, perform ingress filter as well.
	 */

/*TODO:XXXX what if the src is an Native IPv6 address??*/
	if (stf_checkaddr6(&ip6->ip6_dst, NULL) < 0 ||
	    stf_checkaddr6(&ip6->ip6_src, m->m_pkthdr.rcvif) < 0) {
		m_freem(m);
				return;
	}
	itos = (ntohl(ip6->ip6_flow) >> 20) & 0xff;
	if ((ifp->if_flags & IFF_LINK1) != 0)
		ip_ecn_egress(ECN_ALLOWED, &otos, &itos);
	ip6->ip6_flow &= ~htonl(0xff << 20);
	ip6->ip6_flow |= htonl((u_int32_t)itos << 20);

	m->m_pkthdr.rcvif = ifp;
	
#if NBPFILTER > 0
	if (ifp->if_bpf) {
		/*
		 * We need to prepend the address family as
		 * a four byte field.  Cons up a dummy header
		 * to pacify bpf.  This is safe because bpf
		 * will only read from the mbuf (i.e., it won't
		 * try to free it or keep a pointer a to it).
		 */
		struct mbuf m0;
		u_int af = AF_INET6;
		
		m0.m_next = m;
		m0.m_len = 4;
		m0.m_data = (char *)&af;
		
#ifdef HAVE_OLD_BPF
		bpf_mtap(ifp, &m0);
#else
		bpf_mtap(ifp->if_bpf, &m0);
#endif
	}
#endif /*NBPFILTER > 0*/

	/*
	 * Put the packet to the network layer input queue according to the
	 * specified address family.
	 * See net/if_gif.c for possible issues with packet processing
	 * reorder due to extra queueing.
	 */
	ifq = &ip6intrq;
	isr = NETISR_IPV6;

	s = splimp();
	if (IF_QFULL(ifq)) {
		IF_DROP(ifq);	/* update statistics */
		m_freem(m);
		splx(s);
		return;
	}
	IF_ENQUEUE(ifq, m);
	schednetisr(isr);
	ifp->if_ipackets++;
	ifp->if_ibytes += m->m_pkthdr.len;
	splx(s);
}

/* ARGSUSED */
static void
stf_rtrequest(cmd, rt, sa)
	int cmd;
	struct rtentry *rt;
#if defined(__bsdi__) && _BSDI_VERSION >= 199802
	struct rt_addrinfo *sa;
#else
	struct sockaddr *sa;
#endif
{

	if (rt)
		rt->rt_rmx.rmx_mtu = IPV6_MMTU;
}

static int
stf_ioctl(ifp, cmd, data)
	struct ifnet *ifp;
	u_long cmd;
	caddr_t data;
{
	struct ifaddr *ifa;
	struct ifreq *ifr;
	struct sockaddr_in6 *sin6;
	int error;
	
	error = 0;
	switch (cmd) {
	case SIOCSIFADDR:
		ifa = (struct ifaddr *)data;
		if (ifa == NULL || ifa->ifa_addr->sa_family != AF_INET6) {
			error = EAFNOSUPPORT;
			break;
		}
		sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
		if (IN6_IS_ADDR_6TO4(&sin6->sin6_addr)) {
			ifa->ifa_rtrequest = stf_rtrequest;
			ifp->if_flags |= IFF_UP;
		} else
			error = EINVAL;
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		ifr = (struct ifreq *)data;
		if (ifr && ifr->ifr_addr.sa_family == AF_INET6)
			;
		else
			error = EAFNOSUPPORT;
		break;

	default:
		error = EINVAL;
		break;
	}

	return error;
}

#endif /* NSTF > 0 */
