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
 * Annex A of [DDS-TSN] maps RTPS onto Ethernet but leaves two things
 * unspecified that a receiver needs in order to demultiplex:
 *
 *  - The RTPS *logical port*. The locator carries it (Table A.1) but the
 *    Ethernet frame has no field to put it in, and a node's metatraffic and
 *    user traffic reach the same MAC address. Both ports travel here instead.
 *
 *  - The exact message length. Under a stream subtype the AVTP header's
 *    @c stream_data_length already gives it, but under a control subtype the
 *    ACF message is padded to a quadlet boundary and @c ACF_USERn carries no
 *    payload length, so trailing padding would be indistinguishable from
 *    submessage data. Carrying the length here keeps one framing for both and
 *    lets the receiver validate what it got.
 *
 * The header is 6 octets, so that with the 2-octet ACF header in front of it an
 * RTPS message (always a multiple of 4 octets, since every submessage is
 * aligned on a 32-bit boundary) needs no ACF padding at all.
 *
 * @code
 * 0...............8..............16..............24..............31
 * +---------------+---------------+---------------+---------------+
 * |   destination logical port    |     source logical port       |
 * +---------------+---------------+---------------+---------------+
 * |          rtps_length          |  RTPS message ...
 * +---------------+---------------+-------------------------------+
 * @endcode
 *
 * All fields are big endian, matching the byte order of every other IEEE 1722
 * header field.
 */
struct TsnRtpsHeader
{
    static constexpr uint32_t size = 6u;

    //! Logical port of the destination locator the message is addressed to.
    uint16_t destination_logical_port = 0;
    //! Logical port of the sending participant, used to build the remote locator.
    uint16_t source_logical_port = 0;
    //! Length in octets of the RTPS message that follows.
    uint16_t rtps_length = 0;

    //! Serialize into @c buffer, which must have room for @ref size octets.
    void serialize(
            uint8_t* buffer) const
    {
        buffer[0] = static_cast<uint8_t>(destination_logical_port >> 8);
        buffer[1] = static_cast<uint8_t>(destination_logical_port & 0xFF);
        buffer[2] = static_cast<uint8_t>(source_logical_port >> 8);
        buffer[3] = static_cast<uint8_t>(source_logical_port & 0xFF);
        buffer[4] = static_cast<uint8_t>(rtps_length >> 8);
        buffer[5] = static_cast<uint8_t>(rtps_length & 0xFF);
    }

    /**
     * Deserialize from @c buffer.
     *
     * @param buffer     Start of the ACF message payload.
     * @param available  Octets available in @c buffer, padding included.
     *
     * @return true when the header is complete and the announced RTPS message
     * fits in @c available.
     */
    bool deserialize(
            const uint8_t* buffer,
            uint32_t available)
    {
        if (available < size)
        {
            return false;
        }
        destination_logical_port = static_cast<uint16_t>((buffer[0] << 8) | buffer[1]);
        source_logical_port = static_cast<uint16_t>((buffer[2] << 8) | buffer[3]);
        rtps_length = static_cast<uint16_t>((buffer[4] << 8) | buffer[5]);

        return rtps_length >= min_rtps_message_size &&
               static_cast<uint32_t>(rtps_length) + size <= available;
    }

    /**
     * Smallest RTPS message worth passing up: the 20-octet RTPS Header
     * (subclause 9.4.4 of [DDSI-RTPS]) with no submessages.
     */
    static constexpr uint16_t min_rtps_message_size = 20u;
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
