// -*- mode: c++; c-basic-offset: 4 -*-
/*
 * toipsummarydump.{cc,hh} -- element writes packet summary in ASCII
 * Eddie Kohler
 *
 * Copyright (c) 2001 International Computer Science Institute
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include "toipsumdump.hh"
#include <click/standard/scheduleinfo.hh>
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/packet_anno.hh>
#include <clicknet/ip.h>
#include <clicknet/udp.h>
#include <clicknet/tcp.h>
#include <unistd.h>
#include <time.h>
CLICK_DECLS

#ifdef i386
# define PUT4NET(p, d)	*reinterpret_cast<uint32_t *>((p)) = (d)
# define PUT4(p, d)	*reinterpret_cast<uint32_t *>((p)) = htonl((d))
# define PUT2NET(p, d)	*reinterpret_cast<uint16_t *>((p)) = (d)
#else
# define PUT4NET(p, d)	do { uint32_t d__ = ntohl((d)); (p)[0] = d__>>24; (p)[1] = d__>>16; (p)[2] = d__>>8; (p)[3] = d__; } while (0)
# define PUT4(p, d)	do { (p)[0] = (d)>>24; (p)[1] = (d)>>16; (p)[2] = (d)>>8; (p)[3] = (d); } while (0)
# define PUT2NET(p, d)	do { uint16_t d__ = ntohs((d)); (p)[0] = d__>>8; (p)[1] = d__; } while (0)
#endif
#define PUT1(p, d)	((p)[0] = (d))

ToIPSummaryDump::ToIPSummaryDump()
    : Element(1, 0), _f(0), _task(this)
{
    MOD_INC_USE_COUNT;
}

ToIPSummaryDump::~ToIPSummaryDump()
{
    MOD_DEC_USE_COUNT;
}

int
ToIPSummaryDump::configure(Vector<String> &conf, ErrorHandler *errh)
{
    int before = errh->nerrors();
    String save = "timestamp ip_src";
    bool verbose = false;
    bool bad_packets = false;
    bool careful_trunc = true;
    bool multipacket = false;
    bool binary = false;

    if (cp_va_parse(conf, this, errh,
		    cpFilename, "dump filename", &_filename,
		    cpKeywords,
		    "CONTENTS", cpArgument, "log contents", &save,
		    "VERBOSE", cpBool, "be verbose?", &verbose,
		    "BANNER", cpString, "banner", &_banner,
		    "MULTIPACKET", cpBool, "output multiple packets based on packet count anno?", &multipacket,
		    "BAD_PACKETS", cpBool, "output `!bad' messages for non-IP or bad IP packets?", &bad_packets,
		    "CAREFUL_TRUNC", cpBool, "output `!bad' messages for truncated IP packets?", &careful_trunc,
		    "BINARY", cpBool, "output binary data?", &binary,
		    0) < 0)
	return -1;

    Vector<String> v;
    cp_spacevec(save, v);
    _binary_size = 4;
    for (int i = 0; i < v.size(); i++) {
	String word = cp_unquote(v[i]);
	int what = parse_content(word);
	if (what >= W_NONE && what < W_LAST) {
	    _contents.push_back(what);
	    int s = content_binary_size(what);
	    if (s == 4 || s == 8)
		_binary_size = (_binary_size + 3) & ~3;
	    else if (s == 2)
		_binary_size = (_binary_size + 1) & ~1;
	    else if (s < 0 && binary)
		errh->error("cannot use CONTENTS %s with BINARY", word.cc());
	    _binary_size += s;
	} else
	    errh->error("unknown content type `%s'", word.cc());
    }
    if (_contents.size() == 0)
	errh->error("no contents specified");

    // remove _multipacket if packet count specified
    for (int i = 0; i < _contents.size(); i++)
	if (_contents[i] == W_COUNT)
	    _multipacket = false;
    
    _verbose = verbose;
    _bad_packets = bad_packets;
    _careful_trunc = careful_trunc;
    _multipacket = multipacket;
    _binary = binary;

    return (before == errh->nerrors() ? 0 : -1);
}

int
ToIPSummaryDump::initialize(ErrorHandler *errh)
{
    assert(!_f);
    if (_filename != "-") {
	_f = fopen(_filename.c_str(), "wb");
	if (!_f)
	    return errh->error("%s: %s", _filename.cc(), strerror(errno));
    } else {
	_f = stdout;
	_filename = "<stdout>";
    }

    if (input_is_pull(0)) {
	ScheduleInfo::join_scheduler(this, &_task, errh);
	_signal = Notifier::upstream_empty_signal(this, 0, &_task);
    }
    _active = true;
    _output_count = 0;

    // magic number
    StringAccum sa;
    sa << "!IPSummaryDump " << MAJOR_VERSION << '.' << MINOR_VERSION << '\n';

    if (_banner)
	sa << "!creator " << cp_quote(_banner) << '\n';
    
    // host and start time
    if (_verbose) {
	char buf[BUFSIZ];
	buf[BUFSIZ - 1] = '\0';	// ensure NUL-termination
	if (gethostname(buf, BUFSIZ - 1) >= 0)
	    sa << "!host " << buf << '\n';

	time_t when = time(0);
	const char *cwhen = ctime(&when);
	struct timeval tv;
	if (gettimeofday(&tv, 0) >= 0)
	    sa << "!runtime " << tv << " (" << String(cwhen, strlen(cwhen) - 1) << ")\n";
    }

    // data description
    sa << "!data ";
    for (int i = 0; i < _contents.size(); i++)
	sa << (i ? " " : "") << unparse_content(_contents[i]);
    sa << '\n';

    // binary marker
    if (_binary)
	sa << "!binary\n";

    // print output
    fwrite(sa.data(), 1, sa.length(), _f);

    return 0;
}

void
ToIPSummaryDump::cleanup(CleanupStage)
{
    if (_f && _f != stdout)
	fclose(_f);
    _f = 0;
}

bool
ToIPSummaryDump::bad_packet(StringAccum &sa, const String &s, int what) const
{
    assert(_bad_packets);
    int pos = s.find_left("%d");
    if (pos >= 0)
	sa << "!bad " << s.substring(0, pos) << what
	   << s.substring(pos + 2) << '\n';
    else
	sa << "!bad " << s << '\n';
    return false;
}

void
ToIPSummaryDump::store_ip_opt_ascii(const uint8_t *opt, int opt_len, int contents, StringAccum &sa)
{
    int initial_sa_len = sa.length();
    const uint8_t *end_opt = opt + opt_len;
    const char *sep = "";
    
    while (opt < end_opt)
	switch (*opt) {
	  case IPOPT_EOL:
	    if (contents & DO_IPOPT_PADDING)
		sa << sep << "eol";
	    goto done;
	  case IPOPT_NOP:
	    if (contents & DO_IPOPT_PADDING) {
		sa << sep << "nop";
		sep = ";";
	    }
	    opt++;
	    break;
	  case IPOPT_RR:
	    if (opt + opt[1] > end_opt || opt[1] < 3 || opt[2] < 4)
		goto bad_opt;
	    if (!(contents & DO_IPOPT_ROUTE))
		goto unknown;
	    sa << sep << "rr";
	    goto print_route;
	  case IPOPT_LSRR:
	    if (opt + opt[1] > end_opt || opt[1] < 3 || opt[2] < 4)
		goto bad_opt;
	    if (!(contents & DO_IPOPT_ROUTE))
		goto unknown;
	    sa << sep << "lsrr";
	    goto print_route;
	  case IPOPT_SSRR:
	    if (opt + opt[1] > end_opt || opt[1] < 3 || opt[2] < 4)
		goto bad_opt;
	    if (!(contents & DO_IPOPT_ROUTE))
		goto unknown;
	    sa << sep << "ssrr";
	    goto print_route;
	  print_route: {
		const uint8_t *o = opt + 3, *ol = opt + opt[1], *op = opt + opt[2] - 1;
		sep = "";
		sa << '{';
		for (; o + 4 <= ol; o += 4) {
		    if (o == op) {
			if (opt[0] == IPOPT_RR)
			    break;
			sep = "^";
		    }
		    sa << sep << (int)o[0] << '.' << (int)o[1] << '.' << (int)o[2] << '.' << (int)o[3];
		    sep = ",";
		}
		if (o == ol && o == op && opt[0] != IPOPT_RR)
		    sa << '^';
		sa << '}';
		if (o + 4 <= ol && o == op && opt[0] == IPOPT_RR)
		    sa << '+' << (ol - o) / 4;
		opt = ol;
		sep = ";";
		break;
	    }
	  case IPOPT_TS: {
	      if (opt + opt[1] > end_opt || opt[1] < 4 || opt[2] < 5)
		  goto bad_opt;
	      const uint8_t *o = opt + 4, *ol = opt + opt[1], *op = opt + opt[2] - 1;
	      int flag = opt[3] & 0xF;
	      int size = (flag >= 1 && flag <= 3 ? 8 : 4);
	      sa << sep << "ts";
	      if (flag == 1)
		  sa << ".ip";
	      else if (flag == 3)
		  sa << ".preip";
	      else if (flag != 0)
		  sa << "." << flag;
	      sa << '{';
	      sep = "";
	      for (; o + size <= ol; o += 4) {
		  if (o == op) {
		      if (flag != 3)
			  break;
		      sep = "^";
		  }
		  if (flag != 0) {
		      sa << sep << (int)o[0] << '.' << (int)o[1] << '.' << (int)o[2] << '.' << (int)o[3] << '=';
		      o += 4;
		  } else
		      sa << sep;
		  if (flag == 3 && o > op)
		      sa.pop_back();
		  else {
		      uint32_t v = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
		      if (v & 0x80000000U)
			  sa << '!';
		      sa << (v & 0x7FFFFFFFU);
		  }
		  sep = ",";
	      }
	      if (o == ol && o == op)
		  sa << '^';
	      sa << '}';
	      if (o + size <= ol && o == op)
		  sa << '+' << (ol - o) / size;
	      if (opt[3] & 0xF0)
		  sa << '+' << '+' << (opt[3] >> 4);
	      opt = ol;
	      sep = ";";
	      break;
	  }
	  default: {
	      if (opt + opt[1] > end_opt || opt[1] < 2)
		  goto bad_opt;
	      if (!(contents & DO_IPOPT_UNKNOWN))
		  goto unknown;
	      sa << sep << (int)(opt[0]);
	      const uint8_t *end_this_opt = opt + opt[1];
	      char opt_sep = '=';
	      for (opt += 2; opt < end_this_opt; opt++) {
		  sa << opt_sep << (int)(*opt);
		  opt_sep = ':';
	      }
	      sep = ";";
	      break;
	  }
	  unknown:
	    opt += (opt[1] >= 2 ? opt[1] : 128);
	    break;
	}

  done:
    if (sa.length() == initial_sa_len)
	sa << '.';
    return;

  bad_opt:
    sa.set_length(initial_sa_len);
    sa << '?';
}

inline void
ToIPSummaryDump::store_ip_opt_ascii(const click_ip *iph, int contents, StringAccum &sa)
{
    store_ip_opt_ascii(reinterpret_cast<const uint8_t *>(iph + 1), (iph->ip_hl << 2) - sizeof(click_ip), contents, sa);
}

void
ToIPSummaryDump::store_tcp_opt_ascii(const uint8_t *opt, int opt_len, int contents, StringAccum &sa)
{
    int initial_sa_len = sa.length();
    const uint8_t *end_opt = opt + opt_len;
    const char *sep = "";
    
    while (opt < end_opt)
	switch (*opt) {
	  case TCPOPT_EOL:
	    if (contents & DO_TCPOPT_PADDING)
		sa << sep << "eol";
	    goto done;
	  case TCPOPT_NOP:
	    if (contents & DO_TCPOPT_PADDING) {
		sa << sep << "nop";
		sep = ";";
	    }
	    opt++;
	    break;
	  case TCPOPT_MAXSEG:
	    if (opt + opt[1] > end_opt || opt[1] != TCPOLEN_MAXSEG)
		goto bad_opt;
	    if (!(contents & DO_TCPOPT_MSS))
		goto unknown;
	    sa << sep << "mss" << ((opt[2] << 8) | opt[3]);
	    opt += TCPOLEN_MAXSEG;
	    sep = ";";
	    break;
	  case TCPOPT_WSCALE:
	    if (opt + opt[1] > end_opt || opt[1] != TCPOLEN_WSCALE)
		goto bad_opt;
	    if (!(contents & DO_TCPOPT_WSCALE))
		goto unknown;
	    sa << sep << "wscale" << (int)(opt[2]);
	    opt += TCPOLEN_WSCALE;
	    sep = ";";
	    break;
	  case TCPOPT_SACK_PERMITTED:
	    if (opt + opt[1] > end_opt || opt[1] != TCPOLEN_SACK_PERMITTED)
		goto bad_opt;
	    if (!(contents & DO_TCPOPT_SACK))
		goto unknown;
	    sa << sep << "sackok";
	    opt += TCPOLEN_SACK_PERMITTED;
	    sep = ";";
	    break;
	  case TCPOPT_SACK: {
	      if (opt + opt[1] > end_opt || (opt[1] % 8 != 2))
		  goto bad_opt;
	      if (!(contents & DO_TCPOPT_SACK))
		  goto unknown;
	      const uint8_t *end_sack = opt + opt[1];
	      for (opt += 2; opt < end_sack; opt += 8) {
		  uint32_t buf[2];
		  memcpy(&buf[0], opt, 8);
		  sa << sep << "sack" << ntohl(buf[0]) << '-' << ntohl(buf[1]);
		  sep = ";";
	      }
	      break;
	  }
	  case TCPOPT_TIMESTAMP: {
	      if (opt + opt[1] > end_opt || opt[1] != TCPOLEN_TIMESTAMP)
		  goto bad_opt;
	      if (!(contents & DO_TCPOPT_TIMESTAMP))
		  goto unknown;
	      uint32_t buf[2];
	      memcpy(&buf[0], opt + 2, 8);
	      sa << sep << "ts" << ntohl(buf[0]) << ':' << ntohl(buf[1]);
	      opt += TCPOLEN_TIMESTAMP;
	      sep = ";";
	      break;
	  }
	  default: {
	      if (opt + opt[1] > end_opt || opt[1] < 2)
		  goto bad_opt;
	      if (!(contents & DO_TCPOPT_UNKNOWN))
		  goto unknown;
	      sa << sep << (int)(opt[0]);
	      const uint8_t *end_this_opt = opt + opt[1];
	      char opt_sep = '=';
	      for (opt += 2; opt < end_this_opt; opt++) {
		  sa << opt_sep << (int)(*opt);
		  opt_sep = ':';
	      }
	      sep = ";";
	      break;
	  }
	  unknown:
	    opt += (opt[1] >= 2 ? opt[1] : 128);
	    break;
	}

  done:
    if (sa.length() == initial_sa_len)
	sa << '.';
    return;

  bad_opt:
    sa.set_length(initial_sa_len);
    sa << '?';
}

inline void
ToIPSummaryDump::store_tcp_opt_ascii(const click_tcp *tcph, int contents, StringAccum &sa)
{
    store_tcp_opt_ascii(reinterpret_cast<const uint8_t *>(tcph + 1), (tcph->th_off << 2) - sizeof(click_tcp), contents, sa);
}

#define BAD_IP(msg, val)	do { if (_bad_packets) return bad_packet(sa, msg, val); iph = 0; } while (0)
#define BAD_TCP(msg, val)	do { if (_bad_packets) return bad_packet(sa, msg, val); tcph = 0; } while (0)
#define BAD_UDP(msg, val)	do { if (_bad_packets) return bad_packet(sa, msg, val); udph = 0; } while (0)

bool
ToIPSummaryDump::summary(Packet *p, StringAccum &sa) const
{
    // Not all of these will be valid, but we calculate them just once.
    const click_ip *iph = p->ip_header();
    const click_tcp *tcph = p->tcp_header();
    const click_udp *udph = p->udp_header();

    // Check that the IP header fields are valid.
    if (!iph)
	BAD_IP("no IP header", 0);
    else if (p->network_length() < (int)(sizeof(click_ip)))
	BAD_IP("truncated IP header", 0);
    else if (iph->ip_v != 4)
	BAD_IP("IP version %d", iph->ip_v);
    else if (iph->ip_hl < (sizeof(click_ip) >> 2))
	BAD_IP("IP header length %d", iph->ip_hl);
    else if (p->network_length() < (int)(iph->ip_hl << 2))
	BAD_IP("truncated IP header", 0);
    else if (ntohs(iph->ip_len) < (iph->ip_hl << 2))
	BAD_IP("IP length %d", ntohs(iph->ip_len));
    else if (p->network_length() + EXTRA_LENGTH_ANNO(p) < (uint32_t)(ntohs(iph->ip_len))
	     && _careful_trunc)
	BAD_IP("truncated IP missing %d", ntohs(iph->ip_len) - p->network_length() - EXTRA_LENGTH_ANNO(p));

    if (!iph || !tcph || iph->ip_p != IP_PROTO_TCP || !IP_FIRSTFRAG(iph))
	tcph = 0;
    else if (p->transport_length() < (int)(sizeof(click_tcp)))
	BAD_TCP((IP_ISFRAG(iph) ? "fragmented TCP header" : "truncated TCP header"), 0);
    else if (tcph->th_off < (sizeof(click_tcp) >> 2))
	BAD_TCP("TCP header length %d", tcph->th_off);
    else if (p->transport_length() < (int)(tcph->th_off << 2)
	     || ntohs(iph->ip_len) < (iph->ip_hl << 2) + (tcph->th_off << 2))
	BAD_TCP((IP_ISFRAG(iph) ? "fragmented TCP header" : "truncated TCP header"), 0);

    if (!iph || !udph || iph->ip_p != IP_PROTO_UDP || !IP_FIRSTFRAG(iph))
	udph = 0;
    else if (p->transport_length() < (int)(sizeof(click_udp))
	     || ntohs(iph->ip_len) < (iph->ip_hl << 2) + sizeof(click_udp))
	BAD_UDP((IP_ISFRAG(iph) ? "fragmented UDP header" : "truncated UDP header"), 0);

    // Adjust extra length, since we calculate lengths here based on ip_len.
    if (iph && EXTRA_LENGTH_ANNO(p) > 0) {
	int32_t full_len = p->length() + EXTRA_LENGTH_ANNO(p);
	if (ntohs(iph->ip_len) + 8 >= full_len - p->network_header_offset())
	    SET_EXTRA_LENGTH_ANNO(p, 0);
	else {
	    full_len = full_len - ntohs(iph->ip_len);
	    SET_EXTRA_LENGTH_ANNO(p, full_len);
	}
    }

    // Binary output if you don't like.
    if (_binary)
	return binary_summary(p, iph, tcph, udph, sa);
    
    // Print actual contents.
    for (int i = 0; i < _contents.size(); i++) {
	if (i)
	    sa << ' ';
	
	switch (_contents[i]) {

	  case W_NONE:
	    sa << '-';
	    break;
	  case W_TIMESTAMP:
	    sa << p->timestamp_anno();
	    break;
	  case W_TIMESTAMP_SEC:
	    sa << p->timestamp_anno().tv_sec;
	    break;
	  case W_TIMESTAMP_USEC:
	    sa << p->timestamp_anno().tv_usec;
	    break;
	  case W_SRC:
	    if (!iph) goto no_data;
	    sa << IPAddress(iph->ip_src);
	    break;
	  case W_DST:
	    if (!iph) goto no_data;
	    sa << IPAddress(iph->ip_dst);
	    break;
	  case W_IP_TOS:
	    if (!iph) goto no_data;
	    sa << iph->ip_tos;
	    break;
	  case W_IP_TTL:
	    if (!iph) goto no_data;
	    sa << iph->ip_ttl;
	    break;
	  case W_FRAG:
	    if (!iph) goto no_data;
	    sa << (IP_ISFRAG(iph) ? (IP_FIRSTFRAG(iph) ? 'F' : 'f') : '.');
	    break;
	  case W_FRAGOFF:
	    if (!iph) goto no_data;
	    sa << ((htons(iph->ip_off) & IP_OFFMASK) << 3);
	    if (iph->ip_off & htons(IP_MF))
		sa << '+';
	    break;
	  case W_SPORT:
	    if (tcph)
		sa << ntohs(tcph->th_sport);
	    else if (udph)
		sa << ntohs(udph->uh_sport);
	    else
		goto no_data;
	    break;
	  case W_DPORT:
	    if (tcph)
		sa << ntohs(tcph->th_dport);
	    else if (udph)
		sa << ntohs(udph->uh_dport);
	    else
		goto no_data;
	    break;
	  case W_IPID:
	    if (!iph) goto no_data;
	    sa << ntohs(iph->ip_id);
	    break;
	  case W_PROTO:
	    if (!iph) goto no_data;
	    switch (iph->ip_p) {
	      case IP_PROTO_TCP:	sa << 'T'; break;
	      case IP_PROTO_UDP:	sa << 'U'; break;
	      case IP_PROTO_ICMP:	sa << 'I'; break;
	      default:			sa << (int)(iph->ip_p); break;
	    }
	    break;
	  case W_IP_OPT:
	    if (!iph) goto no_data;
	    // skip function if no TCP options
	    if (iph->ip_hl <= 5)
		sa << '.';
	    else
		store_ip_opt_ascii(iph, DO_IPOPT_ALL_NOPAD, sa);
	    break;
	  case W_TCP_SEQ:
	    if (!tcph)
		goto no_data;
	    sa << ntohl(tcph->th_seq);
	    break;
	  case W_TCP_ACK:
	    if (!tcph)
		goto no_data;
	    sa << ntohl(tcph->th_ack);
	    break;
	  case W_TCP_FLAGS: {
	      if (!tcph)
		  goto no_data;
	      int flags = tcph->th_flags | (tcph->th_flags2 << 8);
	      if (flags == (TH_ACK | TH_PUSH))
		  sa << 'P' << 'A';
	      else if (flags == TH_ACK)
		  sa << 'A';
	      else if (flags == 0)
		  sa << '.';
	      else
		  for (int flag = 0; flag < 9; flag++)
		      if (flags & (1 << flag))
			  sa << tcp_flags_word[flag];
	      break;
	  }
	  case W_TCP_WINDOW:
	    if (!tcph)
		goto no_data;
	    sa << ntohs(tcph->th_win);
	    break;
	  case W_TCP_OPT:
	    if (!tcph)
		goto no_data;
	    // skip function if no TCP options
	    if (tcph->th_off <= 5)
		sa << '.';
	    else
		store_tcp_opt_ascii(tcph, DO_TCPOPT_ALL_NOPAD, sa);
	    break;
	  case W_TCP_NTOPT:
	    if (!tcph)
		goto no_data;
	    // skip function if no TCP options, or just timestamp option
	    if (tcph->th_off <= 5
		|| (tcph->th_off == 8
		    && *(reinterpret_cast<const uint32_t *>(tcph + 1)) == htonl(0x0101080A)))
		sa << '.';
	    else
		store_tcp_opt_ascii(tcph, DO_TCPOPT_NTALL, sa);
	    break;
	  case W_TCP_SACK:
	    if (!tcph)
		goto no_data;
	    // skip function if no TCP options, or just timestamp option
	    if (tcph->th_off <= 5
		|| (tcph->th_off == 8
		    && *(reinterpret_cast<const uint32_t *>(tcph + 1)) == htonl(0x0101080A)))
		sa << '.';
	    else
		store_tcp_opt_ascii(tcph, DO_TCPOPT_SACK, sa);
	    break;
	  case W_LENGTH: {
	      uint32_t len;
	      if (iph)
		  len = ntohs(iph->ip_len);
	      else
		  len = p->length();
	      sa << (len + EXTRA_LENGTH_ANNO(p));
	      break;
	  }
	  case W_PAYLOAD_LENGTH: {
	      uint32_t len;
	      if (iph) {
		  int32_t off = p->transport_header_offset();
		  if (tcph)
		      off += (tcph->th_off << 2);
		  else if (udph)
		      off += sizeof(click_udp);
		  else if (IP_FIRSTFRAG(iph) && (iph->ip_p == IP_PROTO_TCP || iph->ip_p == IP_PROTO_UDP))
		      off = ntohs(iph->ip_len) + p->network_header_offset();
		  len = ntohs(iph->ip_len) + p->network_header_offset() - off;
	      } else
		  len = p->length();
	      sa << (len + EXTRA_LENGTH_ANNO(p));
	      break;
	  }
	  case W_PAYLOAD: {
	      int32_t off;
	      uint32_t len;
	      if (iph) {
		  off = p->transport_header_offset();
		  if (tcph)
		      off += (tcph->th_off << 2);
		  else if (udph)
		      off += sizeof(click_udp);
		  len = ntohs(iph->ip_len) - off + p->network_header_offset();
		  if (len + off > p->length()) // EXTRA_LENGTH?
		      len = p->length() - off;
	      } else {
		  off = 0;
		  len = p->length();
	      }
	      String s = String::stable_string((const char *)(p->data() + off), len);
	      sa << cp_quote(s);
	      break;
	  }
	  case W_COUNT: {
	      uint32_t count = 1 + EXTRA_PACKETS_ANNO(p);
	      sa << count;
	      break;
	  }
	  case W_LINK: {
	      int link = PAINT_ANNO(p);
	      if (link == 0)
		  sa << '>';
	      else if (link == 1)
		  sa << '<';
	      else
		  sa << link;
	      break;
	  }
	  case W_AGGREGATE:
	    sa << AGGREGATE_ANNO(p);
	    break;
	  case W_FIRST_TIMESTAMP:
	    sa << FIRST_TIMESTAMP_ANNO(p);
	    break;
	  no_data:
	  default:
	    sa << '-';
	    break;
	}
    }
    sa << '\n';
    return true;
}

#undef BAD_IP
#undef BAD_TCP
#undef BAD_UDP

#define T ToIPSummaryDump
#define U ToIPSummaryDump::DO_IPOPT_UNKNOWN
static int ip_opt_contents_mapping[] = {
    T::DO_IPOPT_PADDING, T::DO_IPOPT_PADDING,	// EOL, NOP
    U, U, U, U,	U,				// 2, 3, 4, 5, 6
    T::DO_IPOPT_ROUTE,				// RR
    U, U, U, U, U, U, U, U, U, U, U, U, U, 	// 8-20
    U, U, U, U, U, U, U, U, U, U, 		// 21-30
    U, U, U, U, U, U, U, U, U, U, 		// 31-40
    U, U, U, U, U, U, U, U, U, U, 		// 41-50
    U, U, U, U, U, U, U, U, U, U, 		// 51-60
    U, U, U, U, U, U, U,	 		// 61-67
    T::DO_IPOPT_TS, U, U,			// TS, 69-70
    U, U, U, U, U, U, U, U, U, U, 		// 71-80
    U, U, U, U, U, U, U, U, U, U, 		// 81-90
    U, U, U, U, U, U, U, U, U, U, 		// 91-100
    U, U, U, U, U, U, U, U, U, U, 		// 101-110
    U, U, U, U, U, U, U, U, U, U, 		// 111-120
    U, U, U, U, U, U, U, U, U,			// 121-129
    T::DO_IPOPT_UNKNOWN, T::DO_IPOPT_ROUTE,	// SECURITY, LSRR
    U, U, U, U,					// 132-135
    T::DO_IPOPT_UNKNOWN, T::DO_IPOPT_ROUTE, 	// SATID, SSRR
    U, U, U,					// 138-140
    U, U, U, U, U, U, U,	 		// 141-147
    T::DO_IPOPT_UNKNOWN				// RA
};

static int tcp_opt_contents_mapping[] = {
    T::DO_TCPOPT_PADDING, T::DO_TCPOPT_PADDING,	// EOL, NOP
    T::DO_TCPOPT_MSS, T::DO_TCPOPT_WSCALE,	// MAXSEG, WSCALE
    T::DO_TCPOPT_SACK, T::DO_TCPOPT_SACK,	// SACK_PERMITTED, SACK
    T::DO_TCPOPT_UNKNOWN, T::DO_TCPOPT_UNKNOWN,	// 6, 7
    T::DO_TCPOPT_TIMESTAMP			// TIMESTAMP
};
#undef T
#undef U

int
ToIPSummaryDump::store_ip_opt_binary(const uint8_t *opt, int opt_len, int contents, StringAccum &sa)
{
    if (contents == (int)DO_IPOPT_ALL) {
	// store all options
	sa.append((char)opt_len);
	sa.append(opt, opt_len);
	return opt_len + 1;
    }

    const uint8_t *end_opt = opt + opt_len;
    int initial_sa_len = sa.length();
    sa.append('\0');
    
    while (opt < end_opt) {
	// one-byte options
	if (*opt == IPOPT_EOL) {
	    if (contents & DO_IPOPT_PADDING)
		sa.append(opt, 1);
	    goto done;
	} else if (*opt == IPOPT_NOP) {
	    if (contents & DO_IPOPT_PADDING)
		sa.append(opt, 1);
	    opt++;
	    continue;
	}
	
	// quit copying options if you encounter something obviously invalid
	if (opt[1] < 2 || opt + opt[1] > end_opt)
	    break;

	int this_content = (*opt > IPOPT_RA ? (int)DO_IPOPT_UNKNOWN : ip_opt_contents_mapping[*opt]);
	if (contents & this_content)
	    sa.append(opt, opt[1]);
	opt += opt[1];
    }

  done:
    sa[initial_sa_len] = sa.length() - initial_sa_len - 1;
    return sa.length() - initial_sa_len;
}

inline int
ToIPSummaryDump::store_ip_opt_binary(const click_ip *iph, int contents, StringAccum &sa)
{
    return store_ip_opt_binary(reinterpret_cast<const uint8_t *>(iph + 1), (iph->ip_hl << 2) - sizeof(click_ip), contents, sa);
}

int
ToIPSummaryDump::store_tcp_opt_binary(const uint8_t *opt, int opt_len, int contents, StringAccum &sa)
{
    if (contents == (int)DO_TCPOPT_ALL) {
	// store all options
	sa.append((char)opt_len);
	sa.append(opt, opt_len);
	return opt_len + 1;
    }

    const uint8_t *end_opt = opt + opt_len;
    int initial_sa_len = sa.length();
    sa.append('\0');
    
    while (opt < end_opt) {
	// one-byte options
	if (*opt == TCPOPT_EOL) {
	    if (contents & DO_TCPOPT_PADDING)
		sa.append(opt, 1);
	    goto done;
	} else if (*opt == TCPOPT_NOP) {
	    if (contents & DO_TCPOPT_PADDING)
		sa.append(opt, 1);
	    opt++;
	    continue;
	}
	
	// quit copying options if you encounter something obviously invalid
	if (opt[1] < 2 || opt + opt[1] > end_opt)
	    break;

	int this_content = (*opt > TCPOPT_TIMESTAMP ? (int)DO_TCPOPT_UNKNOWN : tcp_opt_contents_mapping[*opt]);
	if (contents & this_content)
	    sa.append(opt, opt[1]);
	opt += opt[1];
    }

  done:
    sa[initial_sa_len] = sa.length() - initial_sa_len - 1;
    return sa.length() - initial_sa_len;
}

inline int
ToIPSummaryDump::store_tcp_opt_binary(const click_tcp *tcph, int contents, StringAccum &sa)
{
    return store_tcp_opt_binary(reinterpret_cast<const uint8_t *>(tcph + 1), (tcph->th_off << 2) - sizeof(click_tcp), contents, sa);
}

bool
ToIPSummaryDump::binary_summary(Packet *p, const click_ip *iph, const click_tcp *tcph, const click_udp *udph, StringAccum &sa) const
{
    assert(sa.length() == 0);
    char *buf = sa.extend(_binary_size);
    int pos = 4;
    
    // Print actual contents.
    for (int i = 0; i < _contents.size(); i++) {
	uint32_t v = 0;
	switch (_contents[i]) {
	  case W_NONE:
	    break;
	  case W_TIMESTAMP:
	    PUT4(buf + pos, p->timestamp_anno().tv_sec);
	    PUT4(buf + pos + 4, p->timestamp_anno().tv_usec);
	    pos += 8;
	    break;
	  case W_TIMESTAMP_SEC:
	    v = p->timestamp_anno().tv_sec;
	    goto output_4_host;
	  case W_TIMESTAMP_USEC:
	    v = p->timestamp_anno().tv_usec;
	    goto output_4_host;
	  case W_SRC:
	    if (iph)
		v = iph->ip_src.s_addr;
	    goto output_4_net;
	  case W_DST:
	    if (iph)
		v = iph->ip_dst.s_addr;
	    goto output_4_net;
	  case W_IP_TOS:
	    if (iph)
		v = iph->ip_tos;
	    goto output_1;
	  case W_IP_TTL:
	    if (iph)
		v = iph->ip_ttl;
	    goto output_1;
	  case W_FRAG:
	    if (iph)
		v = (IP_ISFRAG(iph) ? (IP_FIRSTFRAG(iph) ? 'F' : 'f') : '.');
	    goto output_1;
	  case W_FRAGOFF:
	    if (iph)
		v = iph->ip_off;
	    goto output_2_net;
	  case W_IPID:
	    if (iph)
		v = iph->ip_id;
	    goto output_2_net;
	  case W_PROTO:
	    if (iph)
		v = iph->ip_p;
	    goto output_1;
	  case W_IP_OPT: {
	      if (!iph || iph->ip_hl <= (sizeof(click_ip) >> 2))
		  goto output_1;
	      int left = sa.length() - pos;
	      sa.set_length(pos);
	      pos += store_ip_opt_binary(iph, DO_IPOPT_ALL, sa);
	      sa.extend(left);
	      buf = sa.data();
	      break;
	  }
	  case W_SPORT:
	    if (tcph || udph)
		v = (tcph ? tcph->th_sport : udph->uh_sport);
	    goto output_2_net;
	  case W_DPORT:
	    if (tcph || udph)
		v = (tcph ? tcph->th_dport : udph->uh_dport);
	    goto output_2_net;
	  case W_TCP_SEQ:
	    if (tcph)
		v = tcph->th_seq;
	    goto output_4_net;
	  case W_TCP_ACK:
	    if (tcph)
		v = tcph->th_ack;
	    goto output_4_net;
	  case W_TCP_FLAGS:
	    if (tcph)
		v = tcph->th_flags;
	    goto output_1;
	  case W_TCP_WINDOW:
	    if (tcph)
		v = tcph->th_win;
	    goto output_2_net;
	  case W_TCP_OPT: {
	      if (!tcph || tcph->th_off <= (sizeof(click_tcp) >> 2))
		  goto output_1;
	      int left = sa.length() - pos;
	      sa.set_length(pos);
	      pos += store_tcp_opt_binary(tcph, DO_TCPOPT_ALL, sa);
	      sa.extend(left);
	      buf = sa.data();
	      break;
	  }
	  case W_TCP_NTOPT: {
	      if (!tcph || tcph->th_off <= (sizeof(click_tcp) >> 2)
		  || (tcph->th_off == 8
		      && *(reinterpret_cast<const uint32_t *>(tcph + 1)) == htonl(0x0101080A)))
		  goto output_1;
	      int left = sa.length() - pos;
	      sa.set_length(pos);
	      pos += store_tcp_opt_binary(tcph, DO_TCPOPT_NTALL, sa);
	      sa.extend(left);
	      buf = sa.data();
	      break;
	  }
	  case W_TCP_SACK: {
	      if (!tcph || tcph->th_off <= (sizeof(click_tcp) >> 2)
		  || (tcph->th_off == 8
		      && *(reinterpret_cast<const uint32_t *>(tcph + 1)) == htonl(0x0101080A)))
		  goto output_1;
	      int left = sa.length() - pos;
	      sa.set_length(pos);
	      pos += store_tcp_opt_binary(tcph, DO_TCPOPT_SACK, sa);
	      sa.extend(left);
	      buf = sa.data();
	      break;
	  }
	  case W_LENGTH:
	    if (iph) {
		v = ntohs(iph->ip_len);
		if (v == 65535 && p->length() + EXTRA_LENGTH_ANNO(p) > v)
		    v = p->length() + EXTRA_LENGTH_ANNO(p);
	    } else
		v = p->length() + EXTRA_LENGTH_ANNO(p);
	    goto output_4_host;
	  case W_PAYLOAD_LENGTH:
	    if (iph) {
		v = ntohs(iph->ip_len);
		if (v == 65535 && p->length() + EXTRA_LENGTH_ANNO(p) > v)
		    v = p->length() + EXTRA_LENGTH_ANNO(p);
		int32_t off = p->transport_header_offset();
		if (tcph)
		    off += (tcph->th_off << 2);
		else if (udph)
		    off += sizeof(click_udp);
		else if (IP_FIRSTFRAG(iph) && (iph->ip_p == IP_PROTO_TCP || iph->ip_p == IP_PROTO_UDP))
		    off = ntohs(iph->ip_len) + p->network_header_offset();
		v = ntohs(iph->ip_len) + p->network_header_offset() - off;
	    } else
		v = p->length() + EXTRA_LENGTH_ANNO(p);
	    goto output_4_host;
	  case W_COUNT:
	    v = 1 + EXTRA_PACKETS_ANNO(p);
	    goto output_4_host;
	  case W_LINK:
	    v = PAINT_ANNO(p);
	    goto output_1;
	  case W_AGGREGATE:
	    v = AGGREGATE_ANNO(p);
	    goto output_4_host;
	  case W_FIRST_TIMESTAMP:
	    PUT4(buf + pos, FIRST_TIMESTAMP_ANNO(p).tv_sec);
	    PUT4(buf + pos + 4, FIRST_TIMESTAMP_ANNO(p).tv_usec);
	    pos += 8;
	    break;
	  output_1:
	    PUT1(buf + pos, v);
	    pos++;
	    break;
	  output_2_net:
	    PUT2NET(buf + pos, v);
	    pos += 2;
	    break;
	  output_4_host:
	    PUT4(buf + pos, v);
	    pos += 4;
	    break;
	  output_4_net:
	  default:
	    PUT4NET(buf + pos, v);
	    pos += 4;
	    break;
	}
    }

    sa.set_length(pos);
    *(reinterpret_cast<uint32_t *>(buf)) = htonl(pos);
    return true;
}

void
ToIPSummaryDump::write_packet(Packet *p, bool multipacket)
{
    if (multipacket && EXTRA_PACKETS_ANNO(p) > 0) {
	uint32_t count = 1 + EXTRA_PACKETS_ANNO(p);
	uint32_t total_len = p->length() + EXTRA_LENGTH_ANNO(p);
	uint32_t len = p->length();
	if (total_len < count * len)
	    total_len = count * len;

	// do timestamp stepping
	struct timeval end_timestamp = p->timestamp_anno();
	struct timeval timestamp_delta;
	if (timerisset(&FIRST_TIMESTAMP_ANNO(p))) {
	    timestamp_delta = (end_timestamp - FIRST_TIMESTAMP_ANNO(p)) / (count - 1);
	    p->set_timestamp_anno(FIRST_TIMESTAMP_ANNO(p));
	} else
	    timestamp_delta = make_timeval(0, 0);
	
	SET_EXTRA_PACKETS_ANNO(p, 0);
	for (uint32_t i = count; i > 0; i--) {
	    uint32_t l = total_len / i;
	    SET_EXTRA_LENGTH_ANNO(p, l - len);
	    total_len -= l;
	    write_packet(p, false);
	    if (i == 1)
		p->timestamp_anno() = end_timestamp;
	    else
		p->timestamp_anno() += timestamp_delta;
	}
	
    } else {
	_sa.clear();
	if (summary(p, _sa))
	    fwrite(_sa.data(), 1, _sa.length(), _f);
	else
	    write_line(_sa.take_string());
	_output_count++;
    }
}

void
ToIPSummaryDump::push(int, Packet *p)
{
    if (_active)
	write_packet(p, _multipacket);
    p->kill();
}

bool
ToIPSummaryDump::run_task()
{
    if (!_active)
	return false;
    if (Packet *p = input(0).pull()) {
	write_packet(p, _multipacket);
	p->kill();
	_task.fast_reschedule();
	return true;
    } else if (_signal) {
	_task.fast_reschedule();
	return false;
    } else
	return false;
}

void
ToIPSummaryDump::write_line(const String &s)
{
    if (s.length()) {
	assert(s.back() == '\n');
	if (_binary) {
	    uint32_t marker = htonl(s.length() | 0x80000000U);
	    fwrite(&marker, 4, 1, _f);
	}
	fwrite(s.data(), 1, s.length(), _f);
    }
}

void
ToIPSummaryDump::add_note(const String &s)
{
    if (s.length()) {
	int extra = 1 + (s.back() == '\n' ? 0 : 1);
	if (_binary) {
	    uint32_t marker = htonl((s.length() + extra) | 0x80000000U);
	    fwrite(&marker, 4, 1, _f);
	}
	fputc('#', _f);
	fwrite(s.data(), 1, s.length(), _f);
	if (extra > 1)
	    fputc('\n', _f);
    }
}

void
ToIPSummaryDump::flush_buffer()
{
    fflush(_f);
}

void
ToIPSummaryDump::add_handlers()
{
    if (input_is_pull(0))
	add_task_handlers(&_task);
}

ELEMENT_REQUIRES(userlevel IPSummaryDumpInfo)
EXPORT_ELEMENT(ToIPSummaryDump)
CLICK_ENDDECLS
