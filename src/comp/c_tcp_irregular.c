/*
 * Copyright 2012,2013,2014,2015,2016 Didier Barvaux
 * Copyright 2013,2014 Viveris Technologies
 * Copyright 2012 WBX
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file   c_tcp_irregular.c
 * @brief  Handle the irregular chain of the TCP compression profile
 * @author FWX <rohc_team@dialine.fr>
 * @author Didier Barvaux <didier@barvaux.org>
 * @author Didier Barvaux <didier.barvaux@toulouse.viveris.com>
 */

#include "c_tcp_irregular.h"

#include "c_tcp_defines.h"
#include "protocols/ip_numbers.h"
#include "protocols/ip.h"

#include <assert.h>

static int tcp_code_irregular_ipv4_part(const struct rohc_comp_ctxt *const context,
                                        const ip_context_t *const ip_context,
                                        const struct ipv4_hdr *const ipv4,
                                        const rohc_ip_id_behavior_t ip_id_behavior,
                                        const bool is_innermost,
                                        const bool ecn_used,
                                        const bool ttl_irreg_chain_flag,
                                        uint8_t *const rohc_data,
                                        const size_t rohc_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 8)));

static int tcp_code_irregular_ipv6_part(const struct rohc_comp_ctxt *const context,
                                        const ip_context_t *const ip_context,
                                        const struct ipv6_hdr *const ipv6,
                                        const bool is_innermost,
                                        const bool ecn_used,
                                        const bool ttl_irreg_chain_flag,
                                        uint8_t *const rohc_data,
                                        const size_t rohc_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 7)));

static int tcp_code_irregular_ipv6_opt_part(const struct rohc_comp_ctxt *const context,
                                            const ip_option_context_t *const opt_ctxt,
                                            const struct rohc_pkt_ip_ext_hdr *const ext,
                                            uint8_t *const rohc_data,
                                            const size_t rohc_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 4)));

static int tcp_code_irregular_tcp_part(const struct rohc_comp_ctxt *const context,
                                       const struct rohc_comp_ctxt *const ref_ctxt,
                                       const struct rohc_pkt_hdrs *const uncomp_pkt_hdrs,
                                       const struct tcp_tmp_variables *const tmp,
                                       const uint8_t ip_inner_ecn,
                                       uint8_t *const rohc_data,
                                       const size_t rohc_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 4, 6)));


/**
 * @brief Code the irregular chain of one CO packet
 *
 * @param context           The real compression context for traces and update
 * @param ref_ctxt          The reference compression context to detect changes
 * @param uncomp_pkt_hdrs   The uncompressed headers to encode
 * @param tmp               The temporary state for the compressed packet
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
int tcp_code_irreg_chain(const struct rohc_comp_ctxt *const context,
                         const struct rohc_comp_ctxt *const ref_ctxt,
                         const struct rohc_pkt_hdrs *const uncomp_pkt_hdrs,
                         const struct tcp_tmp_variables *const tmp,
                         uint8_t *const rohc_pkt,
                         const size_t rohc_pkt_max_len)
{
	const struct sc_tcp_context *const tcp_context = ref_ctxt->specific;
	uint8_t *rohc_remain_data = rohc_pkt;
	size_t rohc_remain_len = rohc_pkt_max_len;
	uint8_t ip_inner_ecn;
	uint8_t ip_hdr_pos;
	int ret;

	for(ip_hdr_pos = 0; ip_hdr_pos < uncomp_pkt_hdrs->ip_hdrs_nr; ip_hdr_pos++)
	{
		const struct rohc_pkt_ip_hdr *const ip_hdr =
			&(uncomp_pkt_hdrs->ip_hdrs[ip_hdr_pos]);
		const ip_context_t *const ip_context = &(tcp_context->ip_contexts[ip_hdr_pos]);
		const bool is_innermost = !!((ip_hdr_pos + 1) == uncomp_pkt_hdrs->ip_hdrs_nr);

		/* irregular part for IP header */
		if(ip_hdr->version == IPV4)
		{
			const rohc_ip_id_behavior_t ip_id_behavior =
				tmp->changes[ip_hdr_pos].ip_id_behavior;

			ret = tcp_code_irregular_ipv4_part(context, ip_context, ip_hdr->ipv4,
			                                   ip_id_behavior, is_innermost,
			                                   tmp->ecn_used, tmp->ttl_irreg_chain_flag,
			                                   rohc_remain_data, rohc_remain_len);
			if(ret < 0)
			{
				rohc_comp_warn(context, "failed to build the IPv4 base header part "
				               "of the irregular chain");
				goto error;
			}
			rohc_remain_data += ret;
			rohc_remain_len -= ret;
		}
		else /* IPv6 */
		{
			uint8_t ip_ext_pos;

			ret = tcp_code_irregular_ipv6_part(context, ip_context, ip_hdr->ipv6,
			                                   is_innermost, tmp->ecn_used,
			                                   tmp->ttl_irreg_chain_flag,
			                                   rohc_remain_data, rohc_remain_len);
			if(ret < 0)
			{
				rohc_comp_warn(context, "failed to build the IPv6 base header part "
				               "of the irregular chain");
				goto error;
			}
			rohc_remain_data += ret;
			rohc_remain_len -= ret;

			/* irregular part for IPv6 extension headers */
			for(ip_ext_pos = 0; ip_ext_pos < ip_hdr->exts_nr; ip_ext_pos++)
			{
				const struct rohc_pkt_ip_ext_hdr *const ext =
					&(ip_hdr->exts[ip_ext_pos]);
				const ip_option_context_t *const opt_ctxt =
					&(ip_context->opts[ip_ext_pos]);

				rohc_comp_debug(context, "IPv6 option #%u: type %u / length %u",
				                ip_ext_pos + 1, ext->type, ext->len);
				ret = tcp_code_irregular_ipv6_opt_part(context, opt_ctxt, ext,
				                                       rohc_remain_data, rohc_remain_len);
				if(ret < 0)
				{
					rohc_comp_warn(context, "failed to encode the IPv6 extension headers "
					               "part of the irregular chain");
					goto error;
				}
				rohc_remain_data += ret;
				rohc_remain_len -= ret;
			}
		}
	}

	/* get the ECN of the innermost IP header */
	if(uncomp_pkt_hdrs->innermost_ip_hdr->version == IPV4)
	{
		const struct ipv4_hdr *const inner_ipv4 =
			uncomp_pkt_hdrs->innermost_ip_hdr->ipv4;
		ip_inner_ecn = inner_ipv4->ecn;
	}
	else /* IPv6 */
	{
		const struct ipv6_hdr *const inner_ipv6 =
			uncomp_pkt_hdrs->innermost_ip_hdr->ipv6;
		ip_inner_ecn = inner_ipv6->ecn;
	}

	/* TCP part (base header + options) of the irregular chain */
	ret = tcp_code_irregular_tcp_part(context, ref_ctxt, uncomp_pkt_hdrs, tmp,
	                                  ip_inner_ecn, rohc_remain_data, rohc_remain_len);
	if(ret < 0)
	{
		rohc_comp_warn(context, "failed to build the TCP header part "
		               "of the irregular chain");
		goto error;
	}
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	rohc_remain_data += ret;
#endif
	rohc_remain_len -= ret;

	return (rohc_pkt_max_len - rohc_remain_len);

error:
	return -1;
}


/**
 * @brief Build the irregular part of the IPv4 header
 *
 * See RFC 4996 page 63
 *
 * @param context               The real compression context for traces and update
 * @param ip_context            The specific IP compression context
 * @param ipv4                  The IPv4 header
 * @param ip_id_behavior        The IP-ID behavior of the IPv4 header
 * @param is_innermost          True if IP header is the innermost of the packet
 * @param ecn_used              The indicator of ECN usage
 * @param ttl_irreg_chain_flag  Whether the TTL of an outer header changed
 * @param[out] rohc_data        The ROHC packet being built
 * @param rohc_max_len          The max remaining length in the ROHC buffer
 * @return                      The length appended in the ROHC buffer if positive,
 *                              -1 in case of error
 */
static int tcp_code_irregular_ipv4_part(const struct rohc_comp_ctxt *const context,
                                        const ip_context_t *const ip_context,
                                        const struct ipv4_hdr *const ipv4,
                                        const rohc_ip_id_behavior_t ip_id_behavior,
                                        const bool is_innermost,
                                        const bool ecn_used,
                                        const bool ttl_irreg_chain_flag,
                                        uint8_t *const rohc_data,
                                        const size_t rohc_max_len)
{
	uint8_t *rohc_remain_data = rohc_data;
	size_t rohc_remain_len = rohc_max_len;

	assert(ip_context->version == IPV4);

	rohc_comp_debug(context, "ecn_used = %d, is_innermost = %d, "
	                "ttl_irreg_chain_flag = %d",
	                ecn_used, is_innermost, ttl_irreg_chain_flag);
	rohc_comp_debug(context, "IP version = 4, ip_id_behavior = %d", ip_id_behavior);

	/* ip_id =:= ip_id_enc_irreg( ip_id_behavior.UVALUE ) */
	if(ip_id_behavior == ROHC_IP_ID_BEHAVIOR_RAND)
	{
		if(rohc_remain_len < sizeof(uint16_t))
		{
			rohc_comp_warn(context, "ROHC buffer too small for the IPv4 base header "
			               "irregular part: %zu bytes required for random IP-ID, "
			               "but only %zu bytes available", sizeof(uint16_t),
			               rohc_remain_len);
			goto error;
		}
		memcpy(rohc_remain_data, &ipv4->id, sizeof(uint16_t));
		rohc_remain_data += sizeof(uint16_t);
		rohc_remain_len -= sizeof(uint16_t);
		rohc_comp_debug(context, "random IP-ID 0x%04x", rohc_ntoh16(ipv4->id));
	}

	if(!is_innermost)
	{
		/* ipv4_outer_with/without_ttl_irregular:
		 *   dscp =:= static_or_irreg( ecn_used.UVALUE )
		 *   ip_ecn_flags =:= static_or_irreg( ecn_used.UVALUE ) */
		if(ecn_used)
		{
			if(rohc_remain_len < 1)
			{
				rohc_comp_warn(context, "ROHC buffer too small for the IPv4 base header "
				               "irregular part: 1 byte required for DSCP and ECN, "
				               "but only %zu bytes available", rohc_remain_len);
				goto error;
			}
			rohc_remain_data[0] = ipv4->dscp_ecn;
			rohc_comp_debug(context, "DSCP / ip_ecn_flags = 0x%02x",
			                rohc_remain_data[0]);
			rohc_remain_data++;
			rohc_remain_len--;
		}

		/* ipv4_outer_with_ttl_irregular:
		 *   ttl_hopl =:= irregular(8) */
		if(ttl_irreg_chain_flag)
		{
			if(rohc_remain_len < 1)
			{
				rohc_comp_warn(context, "ROHC buffer too small for the IPv4 base header "
				               "irregular part: 1 byte required for TTL, but only "
				               "%zu bytes available", rohc_remain_len);
				goto error;
			}
			rohc_remain_data[0] = ipv4->ttl;
			rohc_comp_debug(context, "ttl_hopl = 0x%02x", rohc_remain_data[0]);
			rohc_remain_data++;
			rohc_remain_len--;
		}
	}

	rohc_comp_dump_buf(context, "IP irregular part", rohc_data,
	                   rohc_max_len - rohc_remain_len);

	return (rohc_max_len - rohc_remain_len);

error:
	return -1;
}


/**
 * @brief Build the irregular part of the IPv6 header
 *
 * See RFC 4996 page 63
 *
 * @param context               The real compression context for traces and update
 * @param ip_context            The specific IP compression context
 * @param ipv6                  The IPv6 header
 * @param is_innermost          True if IP header is the innermost of the packet
 * @param ecn_used              The indicator of ECN usage
 * @param ttl_irreg_chain_flag  Whether the TTL of an outer header changed
 * @param[out] rohc_data        The ROHC packet being built
 * @param rohc_max_len          The max remaining length in the ROHC buffer
 * @return                      The length appended in the ROHC buffer if positive,
 *                              -1 in case of error
 */
static int tcp_code_irregular_ipv6_part(const struct rohc_comp_ctxt *const context,
                                        const ip_context_t *const ip_context,
                                        const struct ipv6_hdr *const ipv6,
                                        const bool is_innermost,
                                        const bool ecn_used,
                                        const bool ttl_irreg_chain_flag,
                                        uint8_t *const rohc_data,
                                        const size_t rohc_max_len)
{
	uint8_t *rohc_remain_data = rohc_data;
	size_t rohc_remain_len = rohc_max_len;

	assert(ip_context->version == IPV6);

	rohc_comp_debug(context, "ecn_used = %d, is_innermost = %d, "
	                "ttl_irreg_chain_flag = %d",
	                ecn_used, is_innermost, ttl_irreg_chain_flag);
	rohc_comp_debug(context, "IP version = 6, ip_id_behavior = %d",
	                ip_context->ip_id_behavior);

	if(!is_innermost)
	{
		/* ipv6_outer_with/without_ttl_irregular:
		 *   dscp =:= static_or_irreg( ecn_used.UVALUE )
		 *   ip_ecn_flags =:= static_or_irreg( ecn_used.UVALUE ) */
		if(ecn_used)
		{
			if(rohc_remain_len < 1)
			{
				rohc_comp_warn(context, "ROHC buffer too small for the IPv6 base header "
				               "irregular part: 1 byte required for DSCP and ECN, "
				               "but only %zu bytes available", rohc_remain_len);
				goto error;
			}
			rohc_remain_data[0] = ipv6_get_tc(ipv6);
			rohc_comp_debug(context, "add DSCP and ip_ecn_flags = 0x%02x",
			                rohc_remain_data[0]);
			rohc_remain_data++;
			rohc_remain_len--;
		}
		/* ipv6_outer_with_ttl_irregular:
		 *   ttl_hopl =:= irregular(8) */
		if(ttl_irreg_chain_flag)
		{
			if(rohc_remain_len < 1)
			{
				rohc_comp_warn(context, "ROHC buffer too small for the IPv6 base header "
				               "irregular part: 1 byte required for Hop Limit, but "
				               "only %zu bytes available", rohc_remain_len);
				goto error;
			}
			rohc_remain_data[0] = ipv6->hl;
			rohc_comp_debug(context, "add ttl_hopl = 0x%02x", rohc_remain_data[0]);
			rohc_remain_data++;
			rohc_remain_len--;
		}
	}

	rohc_comp_dump_buf(context, "IP irregular part", rohc_data,
	                   rohc_max_len - rohc_remain_len);

	return (rohc_max_len - rohc_remain_len);

error:
	return -1;
}


/**
 * @brief Build the irregular part of the IPv6 option header
 *
 * @param context         The real compression context for traces and update
 * @param opt_ctxt        The compression context of the IPv6 option
 * @param ext             The IPv6 extension header
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int tcp_code_irregular_ipv6_opt_part(const struct rohc_comp_ctxt *const context,
                                            const ip_option_context_t *const opt_ctxt __attribute__((unused)),
                                            const struct rohc_pkt_ip_ext_hdr *const ext,
                                            uint8_t *const rohc_data __attribute__((unused)),
                                            const size_t rohc_max_len __attribute__((unused)))
{
	size_t irreg_ipv6_opt_len = 0;

	switch(ext->type)
	{
		case ROHC_IPPROTO_GRE:  /* TODO: GRE not yet supported */
		case ROHC_IPPROTO_MINE: /* TODO: MINE not yet supported */
		case ROHC_IPPROTO_AH:   /* TODO: AH not yet supported */
			assert(0);
			break;
		default:
			break;
	}

	rohc_comp_dump_buf(context, "IPv6 option irregular part",
	                   rohc_data, irreg_ipv6_opt_len);

	return irreg_ipv6_opt_len;
}


/**
 * @brief Build the irregular part of the TCP header.
 *
 * @param context         The real compression context for traces and update
 * @param ref_ctxt        The reference compression context to detect changes
 * @param uncomp_pkt_hdrs The uncompressed headers to encode
 * @param tmp             The temporary state for compressed packet
 * @param ip_inner_ecn    The ECN flags of the innermost IP header
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int tcp_code_irregular_tcp_part(const struct rohc_comp_ctxt *const context,
                                       const struct rohc_comp_ctxt *const ref_ctxt,
                                       const struct rohc_pkt_hdrs *const uncomp_pkt_hdrs,
                                       const struct tcp_tmp_variables *const tmp,
                                       const uint8_t ip_inner_ecn,
                                       uint8_t *const rohc_data,
                                       const size_t rohc_max_len)
{
	const struct sc_tcp_context *const tcp_context = ref_ctxt->specific;
	const struct tcphdr *const tcp = (struct tcphdr *) uncomp_pkt_hdrs->tcp;
	uint8_t *rohc_remain_data = rohc_data;
	size_t rohc_remain_len = rohc_max_len;
	int ret;

	/* ip_ecn_flags = := tcp_irreg_ip_ecn(ip_inner_ecn)
	 * tcp_res_flags =:= static_or_irreg(ecn_used.CVALUE,4)
	 * tcp_ecn_flags =:= static_or_irreg(ecn_used.CVALUE,2) */
	if(tmp->ecn_used)
	{
		if(rohc_remain_len < 1)
		{
			rohc_comp_warn(context, "ROHC buffer too small for the TCP irregular "
			               "part: 1 byte required for ECN used flag, but only %zu "
			               "bytes available", rohc_remain_len);
			goto error;
		}
		rohc_remain_data[0] =
			(ip_inner_ecn << 6) | (tcp->res_flags << 2) | tcp->ecn_flags;
		rohc_comp_debug(context, "add inner IP ECN + TCP ECN + TCP RES = 0x%02x",
		                rohc_remain_data[0]);
		rohc_remain_data++;
		rohc_remain_len--;
	}

	/* checksum =:= irregular(16) */
	if(rohc_remain_len < sizeof(uint16_t))
	{
		rohc_comp_warn(context, "ROHC buffer too small for the TCP irregular "
		               "part: %zu bytes required for TCP checksum, but only %zu "
		               "bytes available", sizeof(uint16_t), rohc_remain_len);
		goto error;
	}
	memcpy(rohc_remain_data, &tcp->checksum, sizeof(uint16_t));
	rohc_remain_data += sizeof(uint16_t);
	rohc_remain_len -= sizeof(uint16_t);
	rohc_comp_debug(context, "add TCP checksum = 0x%04x",
	                rohc_ntoh16(tcp->checksum));

	/* irregular part for TCP options */
	ret = c_tcp_code_tcp_opts_irreg(context, uncomp_pkt_hdrs,
	                                &tcp_context->tcp_opts, &tmp->tcp_opts,
	                                tmp->tcp_opts.list_item_needed,
	                                rohc_remain_data, rohc_remain_len);
	if(ret < 0)
	{
		rohc_comp_warn(context, "failed to compress TCP options in irregular chain");
		goto error;
	}
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	rohc_remain_data += ret;
#endif
	rohc_remain_len -= ret;

	rohc_comp_dump_buf(context, "TCP irregular part", rohc_data,
	                   rohc_max_len - rohc_remain_len);

	return (rohc_max_len - rohc_remain_len);

error:
	return -1;
}

