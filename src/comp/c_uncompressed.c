/*
 * Copyright 2010,2011,2012,2013,2014 Didier Barvaux
 * Copyright 2007,2009,2010,2012,2014 Viveris Technologies
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file c_uncompressed.c
 * @brief ROHC compression context for the uncompressed profile.
 * @author Didier Barvaux <didier.barvaux@toulouse.viveris.com>
 * @author Didier Barvaux <didier@barvaux.org>
 */

#include "rohc_comp_internals.h"
#include "rohc_traces.h"
#include "rohc_traces_internal.h"
#include "rohc_debug.h"
#include "schemes/cid.h"
#include "crc.h"

#include <assert.h>


/**
 * @brief The Uncompressed context
 *
 * The object defines the Uncompressed context that manages all kinds of
 * packets and headers.
 */
struct sc_uncompressed_context
{
	/// The number of IR packets sent by the compressor
	size_t ir_count;
	/// The number of Normal packets sent by the compressor
	size_t normal_count;
	/// @brief The number of packet sent while in non-IR states, used for the
	///        periodic refreshes of the context
	/// @see uncompressed_periodic_down_transition
	size_t go_back_ir_count;
};


/*
 * Prototypes of private functions
 */

/* create/destroy context */
static bool c_uncompressed_create(struct rohc_comp_ctxt *const context,
                                  const struct net_pkt *const packet)
	__attribute__((warn_unused_result, nonnull(1, 2)));
static void c_uncompressed_destroy(struct rohc_comp_ctxt *const context)
	__attribute__((nonnull(1)));
static bool c_uncompressed_check_profile(const struct rohc_comp *const comp,
                                         const struct net_pkt *const packet)
		__attribute__((warn_unused_result, nonnull(1, 2)));
bool c_uncompressed_use_udp_port(const struct rohc_comp_ctxt *const context,
                                 const unsigned int port);

/* check whether a packet belongs to a context */
static bool c_uncompressed_check_context(const struct rohc_comp_ctxt *const context,
                                         const struct net_pkt *const packet)
	__attribute__((warn_unused_result, nonnull(1, 2)));

/* encode uncompressed packets */
static int c_uncompressed_encode(struct rohc_comp_ctxt *const context,
                                 const struct net_pkt *const uncomp_pkt,
                                 unsigned char *const rohc_pkt,
                                 const size_t rohc_pkt_max_len,
                                 rohc_packet_t *const packet_type,
                                 size_t *const payload_offset)
		__attribute__((warn_unused_result, nonnull(1, 2, 3, 5, 6)));
static int uncompressed_code_packet(const struct rohc_comp_ctxt *const context,
                                    const struct net_pkt *const uncomp_pkt,
                                    unsigned char *const rohc_pkt,
                                    const size_t rohc_pkt_max_len,
                                    rohc_packet_t *const packet_type,
                                    size_t *const payload_offset)
		__attribute__((warn_unused_result, nonnull(1, 2, 3, 5, 6)));
static int uncompressed_code_IR_packet(const struct rohc_comp_ctxt *const context,
                                       const struct net_pkt *const uncomp_pkt,
                                       unsigned char *const rohc_pkt,
                                       const size_t rohc_pkt_max_len,
                                       size_t *const payload_offset)
		__attribute__((warn_unused_result, nonnull(1, 2, 3, 5)));
static int uncompressed_code_normal_packet(const struct rohc_comp_ctxt *const context,
                                           const struct net_pkt *const uncomp_pkt,
                                           unsigned char *const rohc_pkt,
                                           const size_t rohc_pkt_max_len,
                                           size_t *const payload_offset)
		__attribute__((warn_unused_result, nonnull(1, 2, 3, 5)));

/* re-initialize a context */
static bool c_uncompressed_reinit_context(struct rohc_comp_ctxt *const context);

/* deliver feedbacks */
static void c_uncompressed_feedback(struct rohc_comp_ctxt *const context,
                                    const struct c_feedback *feedback);

/* mode and state transitions */
static void uncompressed_decide_state(struct rohc_comp_ctxt *const context);
static void uncompressed_periodic_down_transition(struct rohc_comp_ctxt *const context);
static void uncompressed_change_mode(struct rohc_comp_ctxt *const context,
                                     const rohc_mode_t new_mode);
static void uncompressed_change_state(struct rohc_comp_ctxt *const context,
                                      const rohc_comp_state_t new_state);



/*
 * Definitions of private functions
 */


/**
 * @brief Create a new Uncompressed context and initialize it thanks
 *        to the given IP packet.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context  The compression context
 * @param packet   The packet given to initialize the new context
 * @return         true if successful, false otherwise
 */
static bool c_uncompressed_create(struct rohc_comp_ctxt *const context,
                                  const struct net_pkt *const packet)
{
	struct sc_uncompressed_context *uncomp_context;
	bool success = false;

	assert(context != NULL);
	assert(context->profile != NULL);
	assert(packet != NULL);

	uncomp_context = malloc(sizeof(struct sc_uncompressed_context));
	if(uncomp_context == NULL)
	{
		rohc_error(context->compressor, ROHC_TRACE_COMP, context->profile->id,
		           "no memory for the uncompressed context\n");
		goto quit;
	}
	context->specific = uncomp_context;

	uncomp_context->ir_count = 0;
	uncomp_context->normal_count = 0;
	uncomp_context->go_back_ir_count = 0;

	success = true;

quit:
	return success;
}


/**
 * @brief Destroy the Uncompressed context.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context The compression context
 */
static void c_uncompressed_destroy(struct rohc_comp_ctxt *const context)
{
	if(context->specific != NULL)
	{
		zfree(context->specific);
	}
}


/**
 * @brief Check if the given packet corresponds to the Uncompressed profile
 *
 * There are no condition. If this function is called, the packet always matches
 * the Uncompressed profile.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param comp    The ROHC compressor
 * @param packet  The packet to check
 * @return        Whether the packet corresponds to the profile:
 *                  \li true if the packet corresponds to the profile,
 *                  \li false if the packet does not correspond to
 *                      the profile

 */
static bool c_uncompressed_check_profile(const struct rohc_comp *const comp __attribute__((unused)),
                                         const struct net_pkt *const packet __attribute__((unused)))
{
	return true;
}


/**
 * @brief Check if an IP packet belongs to the Uncompressed context.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context  The compression context
 * @param packet   The packet to check
 * @return         Always return true to tell that the packet belongs
 *                 to the context
 */
static bool c_uncompressed_check_context(const struct rohc_comp_ctxt *const context __attribute__((unused)),
                                         const struct net_pkt *const packet __attribute__((unused)))
{
	return true;
}


/**
 * @brief Encode an IP packet according to a pattern decided by several
 *        different factors.
 *
 * 1. Decide state\n
 * 2. Code packet\n
 * \n
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context           The compression context
 * @param uncomp_pkt        The uncompressed packet to encode
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @param packet_type       OUT: The type of ROHC packet that is created
 * @param payload_offset    OUT: The offset for the payload in the IP packet
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int c_uncompressed_encode(struct rohc_comp_ctxt *const context,
                                 const struct net_pkt *const uncomp_pkt,
                                 unsigned char *const rohc_pkt,
                                 const size_t rohc_pkt_max_len,
                                 rohc_packet_t *const packet_type,
                                 size_t *const payload_offset)
{
	int size;

	/* STEP 1: decide state */
	uncompressed_decide_state(context);

	/* STEP 2: Code packet */
	size = uncompressed_code_packet(context, uncomp_pkt,
	                                rohc_pkt, rohc_pkt_max_len,
	                                packet_type, payload_offset);

	return size;
}


/**
 * @brief Re-initialize a given context
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context  The compression context
 * @return         true in case of success, false otherwise
 */
static bool c_uncompressed_reinit_context(struct rohc_comp_ctxt *const context)
{
	assert(context != NULL);

	/* go back to U-mode and IR state */
	uncompressed_change_mode(context, ROHC_U_MODE);
	uncompressed_change_state(context, ROHC_COMP_STATE_IR);

	return true;
}


/**
 * @brief Update the profile when feedback arrives.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context  The compression context
 * @param feedback The feedback information including the whole feedback packet
 */
static void c_uncompressed_feedback(struct rohc_comp_ctxt *const context,
                                    const struct c_feedback *feedback)
{
	unsigned char *p = feedback->data + feedback->specific_offset;

	if(feedback->type == 1) /* ACK */
	{
		rohc_comp_debug(context, "FEEDBACK-1 received\n");
	}
	else if(feedback->type == 2) /* FEEDBACK-2 */
	{
		unsigned int crc_in_packet = 0; /* initialized to avoid a GCC warning */
		bool is_crc_used = false;
		unsigned char mode = (p[0] >> 4) & 3;
		int remaining = feedback->specific_size - 2;
		p += 2;

		rohc_comp_debug(context, "FEEDBACK-2 received\n");

		/* parse options */
		while(remaining > 0)
		{
			int opt = p[0] >> 4;
			int optlen = p[0] & 0x0f;

			switch(opt)
			{
				case 1: /* CRC */
					crc_in_packet = p[1];
					is_crc_used = true;
					p[1] = 0; /* set to zero for crc computation */
					break;
//				case 2: /* Reject */
//					break;
				case 3: /* SN-Not-Valid */
					/* ignore the option */
					break;
				case 4: /* SN */
					/* ignore the option */
					break;
//				case 7: /* Loss */
//					break;
				default:
					rohc_warning(context->compressor, ROHC_TRACE_COMP,
					             context->profile->id,
					             "unknown feedback option %d\n", opt);
					break;
			}

			remaining -= 1 + optlen;
			p += 1 + optlen;
		}

		/* check CRC if present in feedback */
		if(is_crc_used == true)
		{
			unsigned int crc_computed;

			/* compute the CRC of the feedback packet */
			crc_computed = crc_calculate(ROHC_CRC_TYPE_8,
			                             feedback->data, feedback->size,
			                             CRC_INIT_8, context->compressor->crc_table_8);

			/* ignore feedback in case of bad CRC */
			if(crc_in_packet != crc_computed)
			{
				rohc_warning(context->compressor, ROHC_TRACE_COMP,
				             context->profile->id,
				             "CRC check failed (size = %zu)\n", feedback->size);
				return;
			}
		}

		/* change mode if present in feedback */
		if(mode != 0 && mode != context->mode)
		{
			rohc_info(context->compressor, ROHC_TRACE_COMP, context->profile->id,
			          "mode change (%d -> %d) requested by feedback for CID %d\n",
			          context->mode, mode, context->profile->id);

			/* mode can be changed only if feedback is protected by a CRC */
			if(is_crc_used == true)
			{
				uncompressed_change_mode(context, mode);
			}
			else
			{
				rohc_warning(context->compressor, ROHC_TRACE_COMP,
				             context->profile->id,
				             "mode change requested but no CRC was given\n");
			}
		}

		switch(feedback->acktype)
		{
			case ACK:
				rohc_info(context->compressor, ROHC_TRACE_COMP,
				          context->profile->id, "ACK received\n");
				break;
			case NACK:
				rohc_warning(context->compressor, ROHC_TRACE_COMP,
				             context->profile->id, "NACK received\n");
				break;
			case STATIC_NACK:
				rohc_warning(context->compressor, ROHC_TRACE_COMP,
				             context->profile->id, "STATIC-NACK received\n");
				uncompressed_change_state(context, ROHC_COMP_STATE_IR);
				break;
			case RESERVED:
				rohc_warning(context->compressor, ROHC_TRACE_COMP,
				             context->profile->id, "reserved field used\n");
				break;
			default:
				/* impossible value */
				rohc_warning(context->compressor, ROHC_TRACE_COMP,
				             context->profile->id, "unknown ACK type (%d)\n",
				             feedback->acktype);
		}

	}
	else
	{
		rohc_warning(context->compressor, ROHC_TRACE_COMP, context->profile->id,
		             "feedback type not implemented (%d)\n", feedback->type);
	}
}


/**
 * @brief Decide the state that should be used for the next packet.
 *
 * @param context The compression context
 */
static void uncompressed_decide_state(struct rohc_comp_ctxt *const context)
{
	struct sc_uncompressed_context *uncomp_context =
		(struct sc_uncompressed_context *) context->specific;

	if(context->state == ROHC_COMP_STATE_IR &&
	   uncomp_context->ir_count >= MAX_IR_COUNT)
	{
		uncompressed_change_state(context, ROHC_COMP_STATE_FO);
	}

	if(context->mode == ROHC_U_MODE)
	{
		uncompressed_periodic_down_transition(context);
	}
}


/**
 * @brief Periodically change the context state after a certain number
 *        of packets.
 *
 * @param context The compression context
 */
static void uncompressed_periodic_down_transition(struct rohc_comp_ctxt *const context)
{
	struct sc_uncompressed_context *uncomp_context =
		(struct sc_uncompressed_context *) context->specific;

	if(uncomp_context->go_back_ir_count >=
	   context->compressor->periodic_refreshes_ir_timeout)
	{
		rohc_comp_debug(context, "periodic change to IR state\n");
		uncomp_context->go_back_ir_count = 0;
		uncompressed_change_state(context, ROHC_COMP_STATE_IR);
	}

	if(context->state == ROHC_COMP_STATE_FO)
	{
		uncomp_context->go_back_ir_count++;
	}
}


/**
 * @brief Change the mode of the context.
 *
 * @param context  The compression context
 * @param new_mode The new mode the context must enter in
 */
static void uncompressed_change_mode(struct rohc_comp_ctxt *const context,
                                     const rohc_mode_t new_mode)
{
	if(context->mode != new_mode)
	{
		context->mode = new_mode;
		uncompressed_change_state(context, ROHC_COMP_STATE_IR);
	}
}


/**
 * @brief Change the state of the context.
 *
 * @param context   The compression context
 * @param new_state The new state the context must enter in
 */
static void uncompressed_change_state(struct rohc_comp_ctxt *const context,
                                      const rohc_comp_state_t new_state)
{
	struct sc_uncompressed_context *uncomp_context =
		(struct sc_uncompressed_context *) context->specific;

	/* reset counters only if different state */
	if(context->state != new_state)
	{
		/* reset counters */
		uncomp_context->ir_count = 0;
		uncomp_context->normal_count = 0;

		/* change state */
		context->state = new_state;
	}
}


/**
 * @brief Build the ROHC packet to send.
 *
 * @param context           The compression context
 * @param uncomp_pkt        The uncompressed packet to encode
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @param packet_type       OUT: The type of ROHC packet that is created
 * @param payload_offset    OUT: the offset of the payload in the buffer
 * @return                  The length of the ROHC packet if successful,
 *                         -1 otherwise
 */
static int uncompressed_code_packet(const struct rohc_comp_ctxt *context,
                                    const struct net_pkt *const uncomp_pkt,
                                    unsigned char *const rohc_pkt,
                                    const size_t rohc_pkt_max_len,
                                    rohc_packet_t *const packet_type,
                                    size_t *const payload_offset)
{
	int (*code_packet)(const struct rohc_comp_ctxt *const context,
	                   const struct net_pkt *const uncomp_pkt,
	                   unsigned char *const rohc_pkt,
	                   const size_t rohc_pkt_max_len,
	                   size_t *const payload_offset)
		__attribute__((warn_unused_result, nonnull(1, 2, 3, 5)));
	struct sc_uncompressed_context *uncomp_context =
		(struct sc_uncompressed_context *) context->specific;
	int size;

	/* decide what packet to send depending on state and uncompressed packet */
	if(context->state == ROHC_COMP_STATE_IR)
	{
		*packet_type = ROHC_PACKET_IR;
	}
	else if(context->state == ROHC_COMP_STATE_FO)
	{
		/* non-IPv4/6 packets cannot be compressed with Normal packets
		 * because the first byte could be mis-interpreted as ROHC packet
		 * types (see note at the end of §5.10.2 in RFC 3095) */
		if(ip_get_version(&uncomp_pkt->outer_ip) != IPV4 &&
		   ip_get_version(&uncomp_pkt->outer_ip) != IPV6)
		{
			rohc_comp_debug(context, "force IR packet to avoid conflict between "
			                "first payload byte and ROHC packet types\n");
			*packet_type = ROHC_PACKET_IR;
		}
		else
		{
			*packet_type = ROHC_PACKET_NORMAL;
		}
	}
	else
	{
		rohc_warning(context->compressor, ROHC_TRACE_COMP, context->profile->id,
		             "unknown state, cannot build packet\n");
		*packet_type = ROHC_PACKET_UNKNOWN;
		assert(0); /* should not happen */
		goto error;
	}

	if((*packet_type) == ROHC_PACKET_IR)
	{
		rohc_comp_debug(context, "build IR packet\n");
		uncomp_context->ir_count++;
		code_packet = uncompressed_code_IR_packet;
	}
	else /* ROHC_PACKET_NORMAL */
	{
		rohc_comp_debug(context, "build normal packet\n");
		uncomp_context->normal_count++;
		code_packet = uncompressed_code_normal_packet;
	}

	/* code packet according to the selected type */
	size = code_packet(context, uncomp_pkt, rohc_pkt, rohc_pkt_max_len,
	                   payload_offset);

	return size;

error:
	return -1;
}


/**
 * @brief Build the IR packet.
 *
 * \verbatim

 IR packet (5.10.1)

     0   1   2   3   4   5   6   7
    --- --- --- --- --- --- --- ---
 1 :         Add-CID octet         : if for small CIDs and (CID != 0)
   +---+---+---+---+---+---+---+---+
 2 | 1   1   1   1   1   1   0 |res|
   +---+---+---+---+---+---+---+---+
   :                               :
 3 /    0-2 octets of CID info     / 1-2 octets if for large CIDs
   :                               :
   +---+---+---+---+---+---+---+---+
 4 |          Profile = 0          | 1 octet
   +---+---+---+---+---+---+---+---+
 5 |              CRC              | 1 octet
   +---+---+---+---+---+---+---+---+
   :                               : (optional)
 6 /           IP packet           / variable length
   :                               :
    --- --- --- --- --- --- --- ---

\endverbatim
 *
 * Part 6 is not managed by this function.
 *
 * @param context           The compression context
 * @param uncomp_pkt        The uncompressed packet to encode
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @param payload_offset    OUT: the offset of the payload in the buffer
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int uncompressed_code_IR_packet(const struct rohc_comp_ctxt *context,
                                       const struct net_pkt *const uncomp_pkt __attribute__((unused)),
                                       unsigned char *const rohc_pkt,
                                       const size_t rohc_pkt_max_len,
                                       size_t *const payload_offset)
{
	size_t counter;
	size_t first_position;

	rohc_comp_debug(context, "code IR packet (CID = %zu)\n", context->cid);

	/* parts 1 and 3:
	 *  - part 2 will be placed at 'first_position'
	 *  - part 4 will start at 'counter'
	 */
	counter = code_cid_values(context->compressor->medium.cid_type, context->cid,
	                          rohc_pkt, rohc_pkt_max_len, &first_position);

	/* part 2 */
	rohc_pkt[first_position] = 0xfc;
	rohc_comp_debug(context, "first byte = 0x%02x\n", rohc_pkt[first_position]);

	/* part 4 */
	rohc_pkt[counter] = ROHC_PROFILE_UNCOMPRESSED;
	rohc_comp_debug(context, "Profile ID = 0x%02x\n", rohc_pkt[counter]);
	counter++;

	/* part 5 */
	rohc_pkt[counter] = 0;
	rohc_pkt[counter] = crc_calculate(ROHC_CRC_TYPE_8, rohc_pkt, counter,
	                                  CRC_INIT_8,
	                                  context->compressor->crc_table_8);
	rohc_comp_debug(context, "CRC on %zu bytes = 0x%02x\n", counter,
	                rohc_pkt[counter]);
	counter++;

	*payload_offset = 0;

	return counter;
}


/**
 * @brief Build the Normal packet.
 *
 * \verbatim

 Normal packet (5.10.2)

     0   1   2   3   4   5   6   7
    --- --- --- --- --- --- --- ---
 1 :         Add-CID octet         : if for small CIDs and (CID != 0)
   +---+---+---+---+---+---+---+---+
 2 |   first octet of IP packet    |
   +---+---+---+---+---+---+---+---+
   :                               :
 3 /    0-2 octets of CID info     / 1-2 octets if for large CIDs
   :                               :
   +---+---+---+---+---+---+---+---+
   |                               |
 4 /      rest of IP packet        / variable length
   |                               |
   +---+---+---+---+---+---+---+---+

\endverbatim
 *
 * Part 4 is not managed by this function.
 *
 * @param context           The compression context
 * @param uncomp_pkt        The uncompressed packet to encode
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @param payload_offset    OUT: the offset of the payload in the buffer
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int uncompressed_code_normal_packet(const struct rohc_comp_ctxt *context,
                                           const struct net_pkt *const uncomp_pkt,
                                           unsigned char *const rohc_pkt,
                                           const size_t rohc_pkt_max_len,
                                           size_t *const payload_offset)
{
	size_t counter;
	size_t first_position;

	rohc_comp_debug(context, "code normal packet (CID = %zu)\n", context->cid);

	/* parts 1 and 3:
	 *  - part 2 will be placed at 'first_position'
	 *  - part 4 will start at 'counter'
	 */
	counter = code_cid_values(context->compressor->medium.cid_type, context->cid,
	                          rohc_pkt, rohc_pkt_max_len, &first_position);

	/* part 2 */
	rohc_pkt[first_position] = uncomp_pkt->data[0];

	rohc_comp_debug(context, "header length = %zu, payload length = %zu\n",
	                counter - 1, uncomp_pkt->len);

	*payload_offset = 1;
	return counter;
}


/**
 * @brief Whether the profile uses the given UDP port
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context The compression context
 * @param port    The port number to check
 * @return        Always return false because the Uncompressed profile does not
 *                use UDP port
 */
bool c_uncompressed_use_udp_port(const struct rohc_comp_ctxt *const context __attribute__((unused)),
                                 const unsigned int port __attribute__((unused)))
{
	return false;
}


/**
 * @brief Define the compression part of the Uncompressed profile as described
 *        in the RFC 3095.
 */
const struct rohc_comp_profile c_uncompressed_profile =
{
	.id             = ROHC_PROFILE_UNCOMPRESSED, /* profile ID (RFC3095, §8) */
	.protocol       = 0,                         /* IP protocol */
	.create         = c_uncompressed_create,     /* profile handlers */
	.destroy        = c_uncompressed_destroy,
	.check_profile  = c_uncompressed_check_profile,
	.check_context  = c_uncompressed_check_context,
	.encode         = c_uncompressed_encode,
	.reinit_context = c_uncompressed_reinit_context,
	.feedback       = c_uncompressed_feedback,
	.use_udp_port   = c_uncompressed_use_udp_port,
};

