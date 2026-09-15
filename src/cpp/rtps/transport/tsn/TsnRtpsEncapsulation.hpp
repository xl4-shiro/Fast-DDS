// Copyright (C) 2026 Excelfore Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file TsnRtpsEncapsulation.hpp
 *
 * Encapsulation of an RTPS message inside an IEEE 1722 PDU.
 */

#ifndef FASTDDS_RTPS_TRANSPORT_TSN__TSNRTPSENCAPSULATION_HPP
#define FASTDDS_RTPS_TRANSPORT_TSN__TSNRTPSENCAPSULATION_HPP

#include <cstdint>
#include <cstring>

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

/**
 * Header prefixed to an RTPS message inside an IEEE 1722 PDU payload.
 *
 * Annex A of [DDS-TSN] maps RTPS onto Ethernet but leaves one thing unspecified
 * that a receiver needs in order to demultiplex: the RTPS *logical port*. The
 * locator carries it (Table A.1) and A.6.1 requires endpoints to use the port
 * expressions of [DDSI-RTPS], yet the Ethernet frame has no field to put it in,
 * and a node's metatraffic and user traffic reach the same MAC address.
 *
 * The stream ID cannot stand in for it. A talker is keyed on destination MAC,
 * VLAN and PCP alone, so one stream serves every logical port heading to the
 * same address --- in the no-CNC fallback, SPDP and user multicast share the
 * default multicast MAC and therefore share a stream ID, and under RELIABLE QoS
 * a reader's ACKNACKs reach the writer's user port at the MAC already carrying
 * SEDP. Nothing but the port separates those.
 *
 * Nothing else needs to travel here:
 *
 *  - The *length* is already on the wire. Under a stream subtype the AVTP
 *    header's @c stream_data_length gives it; under a control subtype an RTPS
 *    message is always a multiple of 4 octets, so this 2-octet header behind the
 *    2-octet ACF header leaves the ACF message quadlet-aligned with no padding,
 *    and @c acf_msg_length gives it exactly.
 *
 *  - The *source* port is not needed to route a reply. RTPS sends to the
 *    locators a peer announced in discovery, not to where a datagram came from.
 *
 * @code
 * 0...............8..............15
 * +---------------+---------------+
 * |   destination logical port    |  RTPS message ...
 * +---------------+---------------+
 * @endcode
 *
 * Big endian, matching every other IEEE 1722 header field.
 */
struct TsnRtpsHeader
{
    static constexpr uint32_t size = 2u;

    //! Logical port of the destination locator the message is addressed to.
    uint16_t destination_logical_port = 0;

    //! Serialize into @c buffer, which must have room for @ref size octets.
    void serialize(
            uint8_t* buffer) const
    {
        buffer[0] = static_cast<uint8_t>(destination_logical_port >> 8);
        buffer[1] = static_cast<uint8_t>(destination_logical_port & 0xFF);
    }

    /**
     * Deserialize from @c buffer and report the length of the message behind it.
     *
     * @param buffer       Start of the encapsulated payload.
     * @param available    Octets available in @c buffer.
     * @param [out] rtps_length  Length of the RTPS message following the header.
     *
     * @return true when the header is complete and a plausible RTPS message
     * follows it.
     */
    bool deserialize(
            const uint8_t* buffer,
            uint32_t available,
            uint32_t& rtps_length)
    {
        if (available < size + min_rtps_message_size)
        {
            return false;
        }
        destination_logical_port = static_cast<uint16_t>((buffer[0] << 8) | buffer[1]);

        // Whatever follows the header is the message: the framing below already
        // bounded it, exactly under a stream subtype and to a quadlet under a
        // control subtype, where an RTPS message's own 4-octet alignment means
        // the bound is exact too.
        rtps_length = available - size;
        return true;
    }

    /**
     * Smallest RTPS message worth passing up: the 20-octet RTPS Header
     * (subclause 9.4.4 of [DDSI-RTPS]) with no submessages.
     */
    static constexpr uint16_t min_rtps_message_size = 20u;
};

/**
 * Octet sizes of the framing an RTPS message is wrapped in.
 *
 * Gathered here because they are the parts most likely to move when [DDS-TSN]
 * settles the questions its first version leaves open --- a registered EtherType
 * would remove the 1722 headers entirely, and a defined encapsulation would
 * replace @ref TsnRtpsHeader. Two places need these numbers: AvtpStream, which
 * sizes its buffers per stream, and TSNTransport, which caps the participant's
 * message size before a message is ever built. Keeping a private copy in each
 * is how they drift apart.
 */
struct TsnFraming
{
    //! AVTP stream header: subtype, stream id, timestamp, data length.
    static constexpr uint32_t avtp_stream_header = 24u;
    //! AVTP control header (NTSCF/TSCF): subtype, data length, stream id.
    static constexpr uint32_t avtp_control_header = 12u;
    //! ACF message header: msg_type and length in quadlets.
    static constexpr uint32_t acf_header = 2u;
    //! 802.1Q tag, always present since a TSN stream is VLAN-tagged.
    static constexpr uint32_t vlan_tag = 4u;

    /**
     * Worst-case octets between the Ethernet payload and the RTPS message.
     *
     * One participant sends discovery on the control format and user data on a
     * stream subtype, and a single maximum message size has to fit both, so this
     * takes the larger of the two.
     */
    static constexpr uint32_t worst_case_overhead()
    {
        return vlan_tag + TsnRtpsHeader::size +
               (avtp_control_header + acf_header > avtp_stream_header
                        ? avtp_control_header + acf_header
                        : avtp_stream_header);
    }
};

//! Whether @c buffer starts with the RTPS protocol magic, "RTPS".
inline bool has_rtps_magic(
        const uint8_t* buffer,
        uint32_t size)
{
    return size >= 4u && buffer[0] == 'R' && buffer[1] == 'T' && buffer[2] == 'P' && buffer[3] == 'S';
}

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_RTPS_TRANSPORT_TSN__TSNRTPSENCAPSULATION_HPP
