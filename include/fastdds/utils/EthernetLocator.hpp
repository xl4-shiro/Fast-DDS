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
 * @file EthernetLocator.hpp
 *
 * Helpers for the LOCATOR_KIND_ETHERNET locators defined by the
 * DDSI-RTPS Ethernet PSM (Annex A of the OMG DDS-TSN specification,
 * ptc/2023-03-03).
 */

#ifndef FASTDDS_UTILS__ETHERNETLOCATOR_HPP
#define FASTDDS_UTILS__ETHERNETLOCATOR_HPP

#include <array>
#include <cstdint>
#include <string>

#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/common/Types.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {

//! An IEEE 802 MAC address in wire order.
using MacAddress = std::array<octet, 6>;

/**
 * Helper functions for LOCATOR_KIND_ETHERNET locators.
 *
 * Per Table A.1 of [DDS-TSN], an Ethernet locator packs three independent
 * values into the 32-bit @c port field and the MAC address into the last 6
 * octets of the 16-octet @c address field:
 *
 * @code
 * 0...2...........8...............16.............24...............31
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |              locatorKind = LOCATOR_KIND_ETHERNET              |
 * +-----------------------+-------+---------------+---------------+
 * |      VID (12 bits)    |  PCP  |     Logical Port (2 bytes)    |
 * +---------------+-------+-------+---------------+---------------+
 * |0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|                               |
 * +-------------------------------+      MAC Address (6 bytes)    +
 * |                                                               |
 * +---------------+---------------+---------------+---------------+
 * @endcode
 *
 * @note The specification mandates that the leading 10 octets of the address
 * are zero. Locators built through these helpers honour that; locators built
 * through Locator_t::create_locator() set @c address[0] to 0xFF and are
 * therefore not wire-compatible with other DDS-TSN implementations.
 *
 * @ingroup UTILITIES_MODULE
 */
class EthernetLocator
{
public:

    //! Bit offset of the VLAN ID (VID) within the locator port.
    static constexpr uint32_t vid_shift = 20u;
    //! Bit offset of the Priority Code Point (PCP) within the locator port.
    static constexpr uint32_t pcp_shift = 16u;

    //! Highest valid VLAN ID. 0 means "unknown", 4095 is reserved by [802.1Q].
    static constexpr uint16_t max_vid = 4095u;
    //! Highest valid Priority Code Point.
    static constexpr uint8_t max_pcp = 7u;

    /**
     * Build an Ethernet locator from its constituent parts.
     *
     * @param mac           Destination MAC address.
     * @param vid           VLAN ID, in [0, 4095]. 0 means the VID is unknown.
     * @param pcp           Priority Code Point, in [0, 7].
     * @param logical_port  RTPS logical port.
     *
     * @return The locator, or an invalid locator if @c vid or @c pcp are out of range.
     */
    FASTDDS_EXPORTED_API static Locator_t create_locator(
            const MacAddress& mac,
            uint16_t vid,
            uint8_t pcp,
            uint16_t logical_port);

    /**
     * Build an Ethernet locator from a colon-separated MAC address.
     *
     * @param mac           Destination MAC address as "AA:BB:CC:DD:EE:FF".
     * @param vid           VLAN ID, in [0, 4095]. 0 means the VID is unknown.
     * @param pcp           Priority Code Point, in [0, 7].
     * @param logical_port  RTPS logical port.
     *
     * @return The locator, or an invalid locator if any argument is malformed.
     */
    FASTDDS_EXPORTED_API static Locator_t create_locator(
            const std::string& mac,
            uint16_t vid,
            uint8_t pcp,
            uint16_t logical_port);

    //! Set the MAC address of an Ethernet locator, zeroing the leading 10 octets.
    FASTDDS_EXPORTED_API static void set_mac(
            Locator_t& locator,
            const MacAddress& mac);

    //! Set the MAC address of an Ethernet locator from "AA:BB:CC:DD:EE:FF". Returns false if malformed.
    FASTDDS_EXPORTED_API static bool set_mac(
            Locator_t& locator,
            const std::string& mac);

    //! Get the MAC address of an Ethernet locator.
    FASTDDS_EXPORTED_API static MacAddress mac(
            const Locator_t& locator);

    //! Get the MAC address of an Ethernet locator as "aa:bb:cc:dd:ee:ff".
    FASTDDS_EXPORTED_API static std::string mac_to_string(
            const Locator_t& locator);

    //! Parse "AA:BB:CC:DD:EE:FF" into @c mac. Returns false if malformed.
    FASTDDS_EXPORTED_API static bool string_to_mac(
            const std::string& str,
            MacAddress& mac);

    //! Format a MAC address as "aa:bb:cc:dd:ee:ff".
    FASTDDS_EXPORTED_API static std::string mac_to_string(
            const MacAddress& mac);

    //! Get the VLAN ID encoded in the locator port.
    static uint16_t vid(
            const Locator_t& locator)
    {
        return static_cast<uint16_t>((locator.port >> vid_shift) & max_vid);
    }

    //! Get the Priority Code Point encoded in the locator port.
    static uint8_t pcp(
            const Locator_t& locator)
    {
        return static_cast<uint8_t>((locator.port >> pcp_shift) & max_pcp);
    }

    //! Get the RTPS logical port encoded in the locator port.
    static uint16_t logical_port(
            const Locator_t& locator)
    {
        return static_cast<uint16_t>(locator.port & 0xFFFFu);
    }

    //! Set the VLAN ID encoded in the locator port, leaving PCP and logical port untouched.
    static void set_vid(
            Locator_t& locator,
            uint16_t vid)
    {
        locator.port = (locator.port & ~(static_cast<uint32_t>(max_vid) << vid_shift)) |
                (static_cast<uint32_t>(vid & max_vid) << vid_shift);
    }

    //! Set the Priority Code Point encoded in the locator port, leaving VID and logical port untouched.
    static void set_pcp(
            Locator_t& locator,
            uint8_t pcp)
    {
        locator.port = (locator.port & ~(static_cast<uint32_t>(max_pcp) << pcp_shift)) |
                (static_cast<uint32_t>(pcp & max_pcp) << pcp_shift);
    }

    //! Set the RTPS logical port encoded in the locator port, leaving VID and PCP untouched.
    static void set_logical_port(
            Locator_t& locator,
            uint16_t logical_port)
    {
        locator.port = (locator.port & 0xFFFF0000u) | logical_port;
    }

    //! Whether the locator's MAC address is an IEEE 802 group (multicast) address.
    static bool is_multicast(
            const Locator_t& locator)
    {
        return 0 != (locator.address[10] & 0x01);
    }

    /**
     * Whether two locators name the same input channel.
     *
     * PCP is excluded: it selects a traffic class on egress and does not
     * discriminate the frames a listener accepts.
     */
    static bool same_channel(
            const Locator_t& lhs,
            const Locator_t& rhs)
    {
        return lhs.kind == rhs.kind &&
               vid(lhs) == vid(rhs) &&
               logical_port(lhs) == logical_port(rhs) &&
               0 == memcmp(&lhs.address[10], &rhs.address[10], 6);
    }

};

} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_UTILS__ETHERNETLOCATOR_HPP
