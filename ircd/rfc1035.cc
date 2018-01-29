// Copyright (C) Matrix Construct Developers, Authors & Contributors
// Copyright (C) 2016-2018 Jason Volk
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice is present in all copies.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
// IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

ircd::mutable_buffer
ircd::rfc1035::make_query(const mutable_buffer &out,
                          const uint16_t &id,
                          const question &q)
{
	const ilist<const question> questions{q};
	return make_query(out, id, questions);
}

ircd::mutable_buffer
ircd::rfc1035::make_query(const mutable_buffer &out,
                          const uint16_t &id,
                          const vector_view<const question> &questions)
{
	header h{0};
	h.id = id;
	h.rd = true;
	h.qdcount = bswap(uint16_t(questions.size()));
	return make_query(out, h, questions);
}

ircd::mutable_buffer
ircd::rfc1035::make_query(const mutable_buffer &out,
                          const header &header,
                          const vector_view<const question> &questions)
{
	assert(bswap(header.qdcount) == questions.size());
	stream_buffer sb{out};

	sb([&header](const mutable_buffer &buf)
	{
		const const_buffer headbuf
		{
			reinterpret_cast<const char *>(&header), sizeof(header)
		};

		return copy(buf, headbuf);
	});

	for(const auto &question : questions)
		sb([&question](const mutable_buffer &buf)
		{
			return size(question.print(buf));
		});

	return sb.completed();
}

ircd::rfc1035::question::question(const string_view &fqdn,
                                  const uint16_t &qtype)
:qtype{qtype}
,namelen
{
	size(make_name(mutable_buffer{name}, fqdn))
}
{
}

ircd::const_buffer
ircd::rfc1035::question::parse(const const_buffer &in)
{
	if(size(in) < 2 + 2 + 2)
		throw error
		{
			"Answer input buffer underflow"
		};

	namelen = parse_name(name, in);
	const char *pos(data(in) + namelen);
	if(pos + 2 + 2 > end(in))
		throw error
		{
			"Question input buffer is incomplete (%zu bytes)", size(in)
		};

	qtype = bswap(*(const uint16_t *)pos);     pos += 2;
	qclass = bswap(*(const uint16_t *)pos);    pos += 2;

	assert(size_t(pos - data(in)) <= size(in));
	return { data(in), pos };
}

ircd::mutable_buffer
ircd::rfc1035::question::print(const mutable_buffer &buf)
const
{
	const size_t required
	{
		namelen + 2 + 2
	};

	if(unlikely(size(buf) < required))
		throw error
		{
			"Not enough space in question buffer; %zu bytes required", required
		};

	char *pos
	{
		data(buf) + copy(buf, const_buffer{name, namelen})
	};

	assert(pos > data(buf));
	assert(*(pos - 1) == '\0');
	*(uint16_t *)pos = bswap(qtype);   pos += 2;
	*(uint16_t *)pos = bswap(qclass);  pos += 2;

	assert(size_t(std::distance(data(buf), pos)) == required);
	return mutable_buffer
	{
		data(buf), pos
	};
}

ircd::const_buffer
ircd::rfc1035::answer::parse(const const_buffer &in)
{
	if(unlikely(size(in) < 2 + 2 + 2 + 4 + 2))
		throw error
		{
			"Answer input buffer underflow"
		};

	namelen = parse_name(name, in);
	const char *pos(data(in) + namelen);
	if(unlikely(pos + 2 + 2 + 4 + 2 > end(in)))
		throw error
		{
			"Answer input buffer is incomplete (%zu bytes)", size(in)
		};

	qtype = bswap(*(const uint16_t *)pos);     pos += 2;
	qclass = bswap(*(const uint16_t *)pos);    pos += 2;
	ttl = bswap(*(const uint32_t *)pos);       pos += 4;
	rdlength = bswap(*(const uint16_t *)pos);  pos += 2;

	if(unlikely(qclass != 1))
		throw error
		{
			"Resource record not for IN (internet); corrupt data?"
		};

	if(unlikely(pos + rdlength > end(in)))
		throw error
		{
			"Answer input buffer has incomplete data (rdlength: %u)", rdlength
		};

	rdata = const_buffer{pos, rdlength};
	pos += rdlength;

	assert(size_t(pos - data(in)) <= size(in));
	return { data(in), pos };
}

ircd::rfc1035::answer::A::A(const const_buffer &rdata)
{
	if(unlikely(size(rdata) < 4))
		throw error
		{
			"A record data underflow"
		};

	ip4 = bswap(*(const uint32_t *)data(rdata));
}

ircd::rfc1035::answer::AAAA::AAAA(const const_buffer &rdata)
{
	if(unlikely(size(rdata) < 16))
		throw error
		{
			"AAAA record data underflow"
		};

	ip6 = bswap(*(const uint128_t *)data(rdata));
}

ircd::rfc1035::answer::SRV::SRV(const const_buffer &rdata)
{
	if(unlikely(size(rdata) < 2 + 2 + 2 + 1))
		throw error
		{
			"SRV record data underflow"
		};

	const char *pos(data(rdata));
	priority = bswap(*(const uint16_t *)pos);   pos += 2;
	weight = bswap(*(const uint16_t *)pos);     pos += 2;
	port = bswap(*(const uint16_t *)pos);       pos += 2;

	const const_buffer tgtbuf{pos, end(rdata)};
	tgtlen = parse_name(tgt, tgtbuf);
	pos += tgtlen;

	assert(std::distance(pos, end(rdata)) == 0);
}

ircd::const_buffer
ircd::rfc1035::make_name(const mutable_buffer &out,
                         const string_view &fqdn)
{
	assert(!empty(out));
	char *pos{data(out)};
	ircd::tokens(fqdn, '.', [&pos, &out](const string_view &label)
	{
		if(unlikely(size(label) >= 64))
			throw error
			{
				"Single part of domain cannot exceed 63 characters"
			};

		*pos++ = size(label);
		pos += strlcpy(pos, label, std::distance(pos, end(out)));
	});

	// The null terminator is included in the returned buffer view
	assert(*pos == '\0');
	++pos;

	return { data(out), size_t(pos - begin(out)) };
}

size_t
ircd::rfc1035::parse_name(const mutable_buffer &out,
                          const const_buffer &in)
{
	assert(!empty(out));
	if(unlikely(empty(in)))
		throw error
		{
			"Name input buffer underflow"
		};

	const char *pos(data(in));
	if(*pos & uint8_t(192))
	{
		//throw error{"Pointer format not implemented"};
		out[0] = '\0';
		pos += 2;
		return pos - begin(in);
	}

	out[0] = '\0';
	for(uint8_t len(*pos++); len && pos + len < end(in); len = *pos++)
	{
		const string_view label
		{
			reinterpret_cast<const char *>(pos), len
		};

		strlcat(out, label);
		strlcat(out, ".");
		pos += len;
	}

	assert(size_t(pos - begin(in)) <= size(in));
	assert(size_t(pos - begin(in)) <= size(out));
	return pos - begin(in);
}

std::string
ircd::rfc1035::header::debug()
const
{
	std::stringstream ss;
	ss << "id       : " << id << '\n';
	ss << "opcode   : " << uint(opcode) << '\n';
	ss << "rcode    : " << uint(rcode) << ' ' << (rfc1035::rcode[rcode]) << '\n';
	ss << "rd       : " << (rd? "recursive" : "not recursive") << '\n';
	ss << "tc       : " << (tc? "truncated" : "not truncated") << '\n';
	ss << "aa       : " << (aa? "authoritative" : "not authoritative") << '\n';
	ss << "qr       : " << (qr? "query" : "response") << '\n';
	ss << "cd       : " << (cd? "checking disabled" : "checking enabled") << '\n';
	ss << "ad       : " << (ad? "authentic data" : "not authentic data") << '\n';
	ss << "ra       : " << (ra? "recursion available" : "recursion unavailable") << '\n';
	ss << "qdcount  : " << qdcount << '\n';
	ss << "ancount  : " << ancount << '\n';
	ss << "nscount  : " << nscount << '\n';
	ss << "arcount  : " << arcount << '\n';
	return ss.str();
}

decltype(ircd::rfc1035::rcode)
ircd::rfc1035::rcode
{
	"NoError No Error [RFC1035]",                                    // 0
	"FormErr Format Error [RFC1035]",                                // 1
	"ServFail Server Failure [RFC1035]",                             // 2
	"NXDomain Non-Existent Domain [RFC1035]",                        // 3
	"NotImp Not Implemented [RFC1035]",                              // 4
	"Refused Query Refused [RFC1035]",                               // 5
	"YXDomain Name Exists when it should not [RFC2136][RFC6672]",    // 6
	"YXRRSet RR Set Exists when it should not [RFC2136]",            // 7
	"NXRRSet RR Set that should exist does not [RFC2136]",           // 8
	"NotAuth Not Authorized [RFC2845]",                              // 9
	"NotZone Name not contained in zone [RFC2136]",                  // 10
	"Unassigned",                                                    // 11
	"Unassigned",                                                    // 12
	"Unassigned",                                                    // 13
	"Unassigned",                                                    // 14
	"Unassigned",                                                    // 15
	"BADVERS Bad OPT Version [RFC6891]",                             // 16
	"BADSIG TSIG Signature Failure [RFC2845]",                       // 17
	"BADKEY Key not recognized [RFC2845]",                           // 18
	"BADTIME Signature out of time window [RFC2845]",                // 19
	"BADMODE Bad TKEY Mode [RFC2930]",                               // 20
	"BADNAME Duplicate key name [RFC2930]",                          // 21
	"BADALG Algorithm not supported [RFC2930]",                      // 22
	"BADTRUNC Bad Truncation [RFC4635]",                             // 23
	"BADCOOKIE Bad/missing Server Cookie [RFC7873]",                 // 24
};

decltype(ircd::rfc1035::qtype)
ircd::rfc1035::qtype
{
	{ "A",           1 },      // a host address [RFC1035]
	{ "NS",          2 },      // an authoritative name server [RFC1035]
	{ "MD",          3 },      // a mail destination (OBSOLETE - use MX) [RFC1035]
	{ "MF",          4 },      // a mail forwarder (OBSOLETE - use MX) [RFC1035]
	{ "CNAME",       5 },      // the canonical name for an alias	[RFC1035]
	{ "SOA",         6 },      // marks the start of a zone of authority [RFC1035]
	{ "MB",          7 },      // a mailbox domain name (EXPERIMENTAL) [RFC1035]
	{ "MG",          8 },      // a mail group member (EXPERIMENTAL)	[RFC1035]
	{ "MR",          9 },      // a mail rename domain name (EXPERIMENTAL) [RFC1035]
	{ "NULL",        10 },     // a null RR (EXPERIMENTAL) [RFC1035]
	{ "WKS",         11 },     // a well known service description	[RFC1035]
	{ "PTR",         12 },     // a domain name pointer [RFC1035]
	{ "HINFO",       13 },     // host information [RFC1035]
	{ "MINFO",       14 },     // mailbox or mail list information [RFC1035]
	{ "MX",          15 },     // mail exchange	[RFC1035]
	{ "TXT",         16 },     // text strings	[RFC1035]
	{ "RP",          17 },     // for Responsible Person [RFC1183]
	{ "AFSDB",       18 },     // for AFS Data Base location [RFC1183][RFC5864]
	{ "X25",         19 },     // for X.25 PSDN address [RFC1183]
	{ "ISDN",        20 },     // for ISDN address [RFC1183]
	{ "RT",          21 },     // for Route Through	[RFC1183]
	{ "NSAP",        22 },     // for NSAP address,                                                                                     NSAP style A record [RFC1706]
	{ "NSAP-PTR",    23 },     // for domain name pointer,                                                                              NSAP style	[RFC1348][RFC1637][RFC1706]
	{ "SIG",         24 },     // for security signature [RFC4034][RFC3755][RFC2535][RFC2536][RFC2537][RFC2931][RFC3110][RFC3008]
	{ "KEY",         25 },     // for security key	[RFC4034][RFC3755][RFC2535][RFC2536][RFC2537][RFC2539][RFC3008][RFC3110]
	{ "PX",          26 },     // X.400 mail mapping information [RFC2163]
	{ "GPOS",        27 },     // Geographical Position [RFC1712]
	{ "AAAA",        28 },     // IP6 Address	[RFC3596]
	{ "LOC",         29 },     // Location Information	[RFC1876]
	{ "NXT",         30 },     // Next Domain (OBSOLETE) [RFC3755][RFC2535]
	{ "EID",         31 },     // Endpoint Identifier [Michael_Patton][http://ana-3.lcs.mit.edu/~jnc/nimrod/dns.txt]
	{ "NIMLOC",      32 },     // Nimrod Locator [1][Michael_Patton][http://ana-3.lcs.mit.edu/~jnc/nimrod/dns.txt]
	{ "SRV",         33 },     // Server Selection	[1][RFC2782]
	{ "ATMA",        34 },     // ATM Address	[ATM Forum Technical Committee]
	{ "NAPTR",       35 },     // Naming Authority Pointer [RFC2915][RFC2168][RFC3403]
	{ "KX",          36 },     // Key Exchanger	[RFC2230]
	{ "CERT",        37 },     // CERT [RFC4398]
	{ "A6",          38 },     // A6 (OBSOLETE - use AAAA) [RFC3226][RFC2874][RFC6563]
	{ "DNAME",       39 },     // DNAME [RFC6672]
	{ "SINK",        40 },     // SINK [Donald_E_Eastlake][http://tools.ietf.org/html/draft-eastlake-kitchen-sink]		1997-11
	{ "OPT",         41 },     // OPT [RFC6891][RFC3225]
	{ "APL",         42 },     // APL [RFC3123]
	{ "DS",          43 },     // Delegation Signer [RFC4034][RFC3658]
	{ "SSHFP",       44 },     // SSH Key Fingerprint [RFC4255]
	{ "IPSECKEY",    45 },     // IPSECKEY [RFC4025]
	{ "RRSIG",       46 },     // RRSIG [RFC4034][RFC3755]
	{ "NSEC",        47 },     // NSEC [RFC4034][RFC3755]
	{ "DNSKEY",      48 },     // DNSKEY [RFC4034][RFC3755]
	{ "DHCID",       49 },     // DHCID [RFC4701]
	{ "NSEC3",       50 },     // NSEC3 [RFC5155]
	{ "NSEC3PARAM",  51 },     // NSEC3PARAM [RFC5155]
	{ "TLSA",        52 },     // TLSA [RFC6698]
	{ "SMIMEA",      53 },     // S/MIME cert association [RFC8162]
	{ "HIP",         55 },     // Host Identity Protocol [RFC8005]
	{ "NINFO",       56 },     // NINFO [Jim_Reid] NINFO/ninfo-completed-template
	{ "RKEY",        57 },     // RKEY [Jim_Reid] RKEY/rkey-completed-template
	{ "TALINK",      58 },     // Trust Anchor LINK	[Wouter_Wijngaards]	TALINK/talink-completed-template
	{ "CDS",         59 },     // Child DS	[RFC7344] CDS/cds-completed-template
	{ "CDNSKEY",     60 },     // DNSKEY(s) the Child wants reflected in DS [RFC7344]
	{ "OPENPGPKEY",  61 },     // OpenPGP Key [RFC7929]	OPENPGPKEY/openpgpkey-completed-template
	{ "CSYNC",       62 },     // Child-To-Parent Synchronization [RFC7477]
	{ "SPF",         99 },     // [RFC7208]
	{ "UINFO",       100 },    // IANA-Reserved
	{ "UID",         101 },    // IANA-Reserved
	{ "GID",         102 },    // IANA-Reserved
	{ "UNSPEC",      103 },    // IANA-Reserved
	{ "NID",         104 },    // [RFC6742] ILNP/nid-completed-template
	{ "L32",         105 },    // [RFC6742] ILNP/l32-completed-template
	{ "L64",         106 },    // [RFC6742] ILNP/l64-completed-template
	{ "LP",          107 },    // [RFC6742] ILNP/lp-completed-template
	{ "EUI48",       108 },    // an EUI-48 address [RFC7043]
	{ "EUI64",       109 },    // an EUI-64 address [RFC7043]
	{ "TKEY",        249 },    // Transaction Key [RFC2930]
	{ "TSIG",        250 },    // Transaction Signature [RFC2845]
	{ "IXFR",        251 },    // incremental transfer [RFC1995]
	{ "AXFR",        252 },    // transfer of an entire zone [RFC1035][RFC5936]
	{ "MAILB",       253 },    // mailbox-related RRs (MB,                                                                              MG or MR) [RFC1035]
	{ "MAILA",       254 },    // mail agent RRs (OBSOLETE - see MX) [RFC1035]
	{ "*",           255 },    // A request for all records the server/cache has available [RFC1035][RFC6895]
	{ "URI",         256 },    // URI [RFC7553] URI/uri-completed-template
	{ "CAA",         257 },    // Certification Authority Restriction	[RFC6844]
	{ "AVC",         258 },    // Application Visibility and Control [Wolfgang_Riedel]
	{ "DOA",         259 },    // Digital Object Architecture	[draft-durand-doa-over-dns]
	{ "TA",          32768 },  // DNSSEC Trust Authorities [Sam_Weiler][http://cameo.library.cmu.edu/]
};
