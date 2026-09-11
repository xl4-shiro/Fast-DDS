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
 * @file EthernetLocator.cpp
 */

#include <fastdds/utils/EthernetLocator.hpp>

#include <cstdio>
#include <cstring>

#include <fastdds/dds/log/Log.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {

//! Parse two hexadecimal digits. Returns -1 when either character is not a hex digit.
static int parse_hex_octet(
        const char* str)
{
    int value = 0;
    for (int i = 0; i < 2; ++i)
    {
        const char c = str[i];
        int digit;
        if (c >= '0' && c <= '9')
        {
            digit = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            digit = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F')
        {
            digit = c - 'A' + 10;
        }
        else
        {
            return -1;
        }
        value = (value << 4) | digit;
    }
    return value;
}

bool EthernetLocator::string_to_mac(
        const std::string& str,
        MacAddress& mac)
{
    // "AA:BB:CC:DD:EE:FF": 6 octets, 5 separators. '-' is accepted as well, as
    // the CNC configuration database uses either notation depending on source.
    if (str.size() != 17)
    {
        return false;
    }

    for (size_t i = 0; i < 6; ++i)
    {
        const size_t pos = i * 3;
        if (i != 0 && str[pos - 1] != ':' && str[pos - 1] != '-')
        {
            return false;
        }
        const int octet_value = parse_hex_octet(str.data() + pos);
        if (octet_value < 0)
        {
            return false;
        }
        mac[i] = static_cast<octet>(octet_value);
    }

    return true;
}

std::string EthernetLocator::mac_to_string(
        const MacAddress& mac)
{
    char buffer[18];
    snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buffer);
}

std::string EthernetLocator::mac_to_string(
        const Locator_t& locator)
{
    return mac_to_string(mac(locator));
}

void EthernetLocator::set_mac(
        Locator_t& locator,
        const MacAddress& mac)
{
    // Annex A of [DDS-TSN]: the leading 10 octets of the address shall be zero.
    memset(locator.address, 0, 10);
    memcpy(&locator.address[10], mac.data(), 6);
}

bool EthernetLocator::set_mac(
        Locator_t& locator,
        const std::string& mac)
{
    MacAddress parsed;
    if (!string_to_mac(mac, parsed))
    {
        EPROSIMA_LOG_WARNING(ETHERNET_LOCATOR,
                "Ethernet address " << mac << " error format. Expected XX:XX:XX:XX:XX:XX");
        return false;
    }
    set_mac(locator, parsed);
    return true;
}

MacAddress EthernetLocator::mac(
        const Locator_t& locator)
{
    MacAddress result;
    memcpy(result.data(), &locator.address[10], 6);
    return result;
}

Locator_t EthernetLocator::create_locator(
        const MacAddress& mac,
        uint16_t vid,
        uint8_t pcp,
        uint16_t logical_port)
{
    Locator_t locator;
    locator.kind = LOCATOR_KIND_INVALID;
    locator.set_Invalid_Address();
    locator.port = 0;

    if (vid > max_vid)
    {
        EPROSIMA_LOG_WARNING(ETHERNET_LOCATOR, "VLAN ID " << vid << " out of range [0, 4095]");
        return locator;
    }
    if (pcp > max_pcp)
    {
        EPROSIMA_LOG_WARNING(ETHERNET_LOCATOR, "PCP " << static_cast<int>(pcp) << " out of range [0, 7]");
        return locator;
    }

    locator.kind = LOCATOR_KIND_ETHERNET;
    locator.port = (static_cast<uint32_t>(vid) << vid_shift) |
            (static_cast<uint32_t>(pcp) << pcp_shift) |
            logical_port;
    set_mac(locator, mac);

    return locator;
}

Locator_t EthernetLocator::create_locator(
        const std::string& mac,
        uint16_t vid,
        uint8_t pcp,
        uint16_t logical_port)
{
    MacAddress parsed;
    if (!string_to_mac(mac, parsed))
    {
        EPROSIMA_LOG_WARNING(ETHERNET_LOCATOR,
                "Ethernet address " << mac << " error format. Expected XX:XX:XX:XX:XX:XX");
        Locator_t locator;
        locator.kind = LOCATOR_KIND_INVALID;
        locator.set_Invalid_Address();
        locator.port = 0;
        return locator;
    }
    return create_locator(parsed, vid, pcp, logical_port);
}

} // namespace rtps
} // namespace fastdds
} // namespace eprosima
