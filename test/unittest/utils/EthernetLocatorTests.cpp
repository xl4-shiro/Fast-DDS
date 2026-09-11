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

#include <gtest/gtest.h>

#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/utils/EthernetLocator.hpp>

#include <rtps/transport/tsn/TsnRtpsEncapsulation.hpp>

using namespace eprosima::fastdds::rtps;

class EthernetLocatorTests : public ::testing::Test
{
};

TEST_F(EthernetLocatorTests, create_locator_packs_the_port_as_the_psm_defines)
{
    const MacAddress mac{{0x01, 0x23, 0x45, 0x67, 0x89, 0xab}};
    const Locator_t locator = EthernetLocator::create_locator(mac, 100, 3, 7410);

    ASSERT_EQ(LOCATOR_KIND_ETHERNET, locator.kind);

    // Table A.1 of [DDS-TSN]: VID in the leading 12 bits, PCP in the next 4,
    // the RTPS logical port in the trailing 2 octets.
    EXPECT_EQ((100u << 20) | (3u << 16) | 7410u, locator.port);
    EXPECT_EQ(100u, EthernetLocator::vid(locator));
    EXPECT_EQ(3u, EthernetLocator::pcp(locator));
    EXPECT_EQ(7410u, EthernetLocator::logical_port(locator));
}

TEST_F(EthernetLocatorTests, create_locator_zeroes_the_leading_address_octets)
{
    const MacAddress mac{{0x01, 0x23, 0x45, 0x67, 0x89, 0xab}};
    const Locator_t locator = EthernetLocator::create_locator(mac, 0, 0, 1);

    // Table A.1 of [DDS-TSN] requires the leading 10 octets to be zero on the
    // wire, so an implementation that reads them cannot be confused.
    for (int i = 0; i < 10; ++i)
    {
        EXPECT_EQ(0, locator.address[i]) << "address[" << i << "] must be zero";
    }
    for (int i = 0; i < 6; ++i)
    {
        EXPECT_EQ(mac[i], locator.address[10 + i]);
    }
}

TEST_F(EthernetLocatorTests, create_locator_rejects_out_of_range_vid_and_pcp)
{
    const MacAddress mac{{0x01, 0x23, 0x45, 0x67, 0x89, 0xab}};

    EXPECT_EQ(LOCATOR_KIND_INVALID, EthernetLocator::create_locator(mac, 4096, 0, 1).kind);
    EXPECT_EQ(LOCATOR_KIND_INVALID, EthernetLocator::create_locator(mac, 0, 8, 1).kind);
    EXPECT_EQ(LOCATOR_KIND_ETHERNET, EthernetLocator::create_locator(mac, 4095, 7, 1).kind);
}

TEST_F(EthernetLocatorTests, string_round_trip)
{
    const Locator_t locator = EthernetLocator::create_locator("01:23:45:67:89:AB", 10, 2, 7400);

    ASSERT_EQ(LOCATOR_KIND_ETHERNET, locator.kind);
    EXPECT_EQ("01:23:45:67:89:ab", EthernetLocator::mac_to_string(locator));

    // The CNC database writes MAC addresses with either separator.
    MacAddress dashed;
    ASSERT_TRUE(EthernetLocator::string_to_mac("01-23-45-67-89-AB", dashed));
    EXPECT_EQ(EthernetLocator::mac(locator), dashed);
}

TEST_F(EthernetLocatorTests, string_to_mac_rejects_malformed_input)
{
    MacAddress mac;

    EXPECT_FALSE(EthernetLocator::string_to_mac("", mac));
    EXPECT_FALSE(EthernetLocator::string_to_mac("01:23:45:67:89", mac));
    EXPECT_FALSE(EthernetLocator::string_to_mac("01:23:45:67:89:ab:cd", mac));
    EXPECT_FALSE(EthernetLocator::string_to_mac("01:23:45:67:89:zz", mac));
    EXPECT_FALSE(EthernetLocator::string_to_mac("0123456789ab-----", mac));
    EXPECT_EQ(LOCATOR_KIND_INVALID, EthernetLocator::create_locator("nonsense", 0, 0, 0).kind);
}

TEST_F(EthernetLocatorTests, setters_leave_the_other_port_fields_alone)
{
    Locator_t locator = EthernetLocator::create_locator("01:23:45:67:89:ab", 100, 3, 7410);

    EthernetLocator::set_vid(locator, 4095);
    EXPECT_EQ(4095u, EthernetLocator::vid(locator));
    EXPECT_EQ(3u, EthernetLocator::pcp(locator));
    EXPECT_EQ(7410u, EthernetLocator::logical_port(locator));

    EthernetLocator::set_pcp(locator, 6);
    EXPECT_EQ(4095u, EthernetLocator::vid(locator));
    EXPECT_EQ(6u, EthernetLocator::pcp(locator));
    EXPECT_EQ(7410u, EthernetLocator::logical_port(locator));

    EthernetLocator::set_logical_port(locator, 7411);
    EXPECT_EQ(4095u, EthernetLocator::vid(locator));
    EXPECT_EQ(6u, EthernetLocator::pcp(locator));
    EXPECT_EQ(7411u, EthernetLocator::logical_port(locator));
}

TEST_F(EthernetLocatorTests, is_multicast_follows_the_group_bit)
{
    EXPECT_TRUE(EthernetLocator::is_multicast(
                EthernetLocator::create_locator("01:00:5e:7f:00:01", 0, 0, 0)));
    EXPECT_FALSE(EthernetLocator::is_multicast(
                EthernetLocator::create_locator("00:1b:21:00:00:01", 0, 0, 0)));
    EXPECT_TRUE(EthernetLocator::is_multicast(
                EthernetLocator::create_locator("ff:ff:ff:ff:ff:ff", 0, 0, 0)));
}

TEST_F(EthernetLocatorTests, same_channel_ignores_the_pcp)
{
    const Locator_t a = EthernetLocator::create_locator("01:23:45:67:89:ab", 100, 3, 7410);
    const Locator_t b = EthernetLocator::create_locator("01:23:45:67:89:ab", 100, 6, 7410);
    const Locator_t different_port = EthernetLocator::create_locator("01:23:45:67:89:ab", 100, 3, 7411);
    const Locator_t different_vid = EthernetLocator::create_locator("01:23:45:67:89:ab", 101, 3, 7410);
    const Locator_t different_mac = EthernetLocator::create_locator("01:23:45:67:89:ac", 100, 3, 7410);

    // PCP selects a traffic class on egress; it does not change which frames a
    // listener accepts, so it must not split one input channel into two.
    EXPECT_TRUE(EthernetLocator::same_channel(a, b));
    EXPECT_FALSE(EthernetLocator::same_channel(a, different_port));
    EXPECT_FALSE(EthernetLocator::same_channel(a, different_vid));
    EXPECT_FALSE(EthernetLocator::same_channel(a, different_mac));
}

TEST_F(EthernetLocatorTests, locator_streams_as_the_ethernet_kind)
{
    const Locator_t locator = EthernetLocator::create_locator("01:23:45:67:89:ab", 0, 0, 7410);

    std::stringstream stream;
    stream << locator;
    EXPECT_EQ("ETH:[01:23:45:67:89:ab]:7410", stream.str());
}

TEST_F(EthernetLocatorTests, streaming_a_locator_does_not_leak_hex_formatting)
{
    const Locator_t ethernet = EthernetLocator::create_locator("01:23:45:67:89:ab", 0, 0, 7410);
    const Locator_t udp = Locator_t::create_locator(LOCATOR_KIND_UDPv4, "127.0.0.1", 7410);

    // Printing the MAC address puts the stream in std::hex. Leaving it there
    // would misprint every number streamed afterwards, this locator's own port
    // first of all.
    std::stringstream stream;
    stream << ethernet << " " << udp << " " << 7410;
    EXPECT_EQ("ETH:[01:23:45:67:89:ab]:7410 UDPv4:[127.0.0.1]:7410 7410", stream.str());
}

class TsnRtpsHeaderTests : public ::testing::Test
{
};

using eprosima::fastdds::rtps::tsn::TsnRtpsHeader;

TEST_F(TsnRtpsHeaderTests, round_trip)
{
    TsnRtpsHeader written;
    written.destination_logical_port = 7410;
    written.source_logical_port = 7411;
    written.rtps_length = 64;

    uint8_t buffer[TsnRtpsHeader::size + 64] = {0};
    written.serialize(buffer);

    TsnRtpsHeader read;
    ASSERT_TRUE(read.deserialize(buffer, sizeof(buffer)));
    EXPECT_EQ(written.destination_logical_port, read.destination_logical_port);
    EXPECT_EQ(written.source_logical_port, read.source_logical_port);
    EXPECT_EQ(written.rtps_length, read.rtps_length);
}

TEST_F(TsnRtpsHeaderTests, fields_are_big_endian)
{
    TsnRtpsHeader header;
    header.destination_logical_port = 0x1234;
    header.source_logical_port = 0x5678;
    header.rtps_length = 0x9abc;

    uint8_t buffer[TsnRtpsHeader::size] = {0};
    header.serialize(buffer);

    // Every other IEEE 1722 header field is big endian; this one matches.
    const uint8_t expected[TsnRtpsHeader::size] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
    EXPECT_EQ(0, memcmp(buffer, expected, sizeof(expected)));
}

TEST_F(TsnRtpsHeaderTests, header_size_keeps_the_acf_message_quadlet_aligned)
{
    // Only the control-format framing needs this: an RTPS message is always a
    // multiple of 4 octets, and adding the 2-octet ACF header plus this header
    // must land on a quadlet boundary, or the ACF layer pads and the padding
    // becomes indistinguishable from submessage data. A stream subtype carries
    // an explicit stream_data_length and never pads.
    constexpr uint32_t acf_header_size = 2u;
    for (uint32_t rtps_length = 20; rtps_length <= 1480; rtps_length += 4)
    {
        EXPECT_EQ(0u, (acf_header_size + TsnRtpsHeader::size + rtps_length) % 4u)
            << "RTPS message of " << rtps_length << " octets would need padding";
    }
}

TEST_F(TsnRtpsHeaderTests, deserialize_rejects_a_truncated_buffer)
{
    uint8_t buffer[TsnRtpsHeader::size] = {0};
    TsnRtpsHeader header;

    EXPECT_FALSE(header.deserialize(buffer, TsnRtpsHeader::size - 1));
}

TEST_F(TsnRtpsHeaderTests, deserialize_rejects_a_length_that_overruns_the_buffer)
{
    TsnRtpsHeader written;
    written.rtps_length = 1000;

    uint8_t buffer[TsnRtpsHeader::size + 100] = {0};
    written.serialize(buffer);

    TsnRtpsHeader read;
    EXPECT_FALSE(read.deserialize(buffer, sizeof(buffer)));
}

TEST_F(TsnRtpsHeaderTests, deserialize_rejects_a_message_shorter_than_an_rtps_header)
{
    TsnRtpsHeader written;
    written.rtps_length = 8;

    uint8_t buffer[TsnRtpsHeader::size + 64] = {0};
    written.serialize(buffer);

    TsnRtpsHeader read;
    EXPECT_FALSE(read.deserialize(buffer, sizeof(buffer)));
}

TEST_F(TsnRtpsHeaderTests, rtps_magic_is_recognised)
{
    const uint8_t good[] = {'R', 'T', 'P', 'S', 2, 3, 0, 0};
    const uint8_t bad[] = {'R', 'T', 'P', 'X', 2, 3, 0, 0};

    EXPECT_TRUE(eprosima::fastdds::rtps::tsn::has_rtps_magic(good, sizeof(good)));
    EXPECT_FALSE(eprosima::fastdds::rtps::tsn::has_rtps_magic(bad, sizeof(bad)));
    EXPECT_FALSE(eprosima::fastdds::rtps::tsn::has_rtps_magic(good, 3));
}

int main(
        int argc,
        char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
