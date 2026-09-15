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
 * @file AvtpStream.cpp
 */

#include <rtps/transport/tsn/AvtpStream.hpp>

#include <cstdio>

#include <fastdds/dds/log/Log.hpp>

#include <rtps/transport/tsn/TsnRtpsEncapsulation.hpp>
#include <rtps/transport/tsn/Xl4Runtime.hpp>

extern "C" {
#include <xl4unibase/unibase.h>
#include <xl4combase/cb_ethernet.h>
#include <xl4avtp/avtpcon/avtpcon.h>
#include <xl4avtp/acf/acf_serdes.h>
} // extern "C"

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

//! Octets the ACF layer prepends to our own header: the ACF type_length field.
static constexpr uint32_t acf_header_size = 2u;

/**
 * Whether this subtype carries its payload as ACF messages.
 *
 * IEEE 1722 defines ACF messages only inside the two control formats, TSCF and
 * NTSCF. A stream subtype carries its payload directly, and its header already
 * has an explicit @c stream_data_length, so wrapping the RTPS message in an ACF
 * message there would add a layer that says nothing.
 */
static bool subtype_uses_acf(
        uint8_t subtype)
{
    return AVBTP_SUBTYPE_TSCF == subtype || AVBTP_SUBTYPE_NTSCF == subtype;
}

StreamId make_stream_id(
        const MacAddress& mac,
        uint16_t unique_id)
{
    StreamId stream_id{};
    memcpy(stream_id.data(), mac.data(), 6);
    stream_id[6] = static_cast<uint8_t>(unique_id >> 8);
    stream_id[7] = static_cast<uint8_t>(unique_id & 0xFF);
    return stream_id;
}

std::string stream_id_to_string(
        const StreamId& stream_id)
{
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
            stream_id[0], stream_id[1], stream_id[2], stream_id[3],
            stream_id[4], stream_id[5], stream_id[6], stream_id[7]);
    return std::string(buffer);
}

bool string_to_stream_id(
        const std::string& str,
        StreamId& stream_id)
{
    // "aa:bb:cc:dd:ee:ff:gg:hh": 8 octets, 7 separators.
    if (str.size() != 23)
    {
        return false;
    }

    for (size_t i = 0; i < 8; ++i)
    {
        const size_t pos = i * 3;
        if (i != 0 && str[pos - 1] != ':' && str[pos - 1] != '-')
        {
            return false;
        }
        unsigned int value = 0;
        if (1 != sscanf(str.data() + pos, "%2x", &value))
        {
            return false;
        }
        stream_id[i] = static_cast<uint8_t>(value);
    }

    return true;
}

std::unique_ptr<AvtpStream> AvtpStream::open(
        const AvtpStreamConfig& config,
        bool talker)
{
    init_xl4_runtime();

    std::unique_ptr<AvtpStream> stream(new AvtpStream());
    stream->talker_ = talker;
    stream->acf_message_type_ = config.acf_message_type;

    stream->connection_ = avtpcon_init();
    if (nullptr == stream->connection_)
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "avtpcon_init failed for interface " << config.interface_name);
        return nullptr;
    }

    avtpcon_conpara_t* conpara = avtpcon_get_conpara(stream->connection_);

    // Only claim gPTP if the clock is actually mapped. avtpcon writes whatever
    // gptpmasterclock_getts64() returns straight into the header, and that is
    // -1 until gptpmasterclock_init() has run --- an 0xFFFFFFFF timestamp on
    // every frame, which is worse than honestly using the monotonic clock.
    conpara->nogptp = !(config.use_gptp && gptp_clock_available(config.gptp_shmem_name));

    // A "cbethN" interface is not a kernel device: avtpcon tunnels the frames
    // over UDP on the loopback, VLAN tag and all, so the receive path has to
    // expect a tagged frame instead of one the kernel has already stripped.
    if (0 == config.interface_name.compare(0, strlen(CB_VIRTUAL_ETHDEV_PREFIX),
            CB_VIRTUAL_ETHDEV_PREFIX))
    {
        conpara->rec_tagged = true;
    }
    if (talker)
    {
        conpara->send_tmout_us = static_cast<int>(config.send_timeout_us);
        conpara->tshape_rate = config.shaper_rate_kbps;
        conpara->tshape_hicredit = config.shaper_hi_credit;
    }
    else
    {
        conpara->rec_tmout_us = static_cast<int>(config.reception_timeout_us);
    }

    avtpcon_set_version(stream->connection_, config.header_version);

    // avtpcon takes the MAC and stream id as non-const arrays.
    ub_macaddr_t destination_mac;
    memcpy(destination_mac, config.destination_mac.data(), 6);
    ub_streamid_t stream_id;
    memcpy(stream_id, config.stream_id.data(), 8);

    if (0 != avtpcon_set_conpara(stream->connection_, config.subtype, config.interface_name.c_str(),
            destination_mac, config.vlan_id, config.pcp, config.socket_priority, stream_id))
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "avtpcon_set_conpara failed for interface " << config.interface_name);
        return nullptr;
    }

    if (0 != avtpcon_connect(stream->connection_))
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "Cannot open AVTP socket on interface " << config.interface_name
                                                                                 << ". Raw sockets need CAP_NET_RAW.");
        return nullptr;
    }

    stream->uses_acf_ = subtype_uses_acf(config.subtype);

    // A stream subtype has an avtp_timestamp field; setting the tv (timestamp
    // valid) bit makes avtpcon stamp every frame from the gPTP-disciplined
    // clock, or the monotonic one when gPTP is not in use. Control formats have
    // no such field, which is the main thing they give up.
    if (!stream->uses_acf_)
    {
        avbtp_cm_stream_header_t* cmsh = &conpara->presethead.avtphead.cmshv0;
        cmsh->bf = cmsh_tv_set_bit_field(cmsh->bf, 1);
    }

    // The source MAC is filled in by avtpcon_connect() from the bound interface.
    memcpy(stream->local_mac_.data(), conpara->presethead.l2head.l2tag.h_source, 6);

    const uint16_t max_payload = avtpcon_get_max_payload_size(stream->connection_);
    const uint32_t overhead = (stream->uses_acf_ ? acf_header_size : 0u) + TsnRtpsHeader::size;
    if (max_payload <= overhead)
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "Interface " << config.interface_name << " MTU leaves no room for RTPS");
        return nullptr;
    }
    // An RTPS message is always a multiple of 4 octets, and keeping it that way
    // is what lets the ACF message avoid padding altogether.
    stream->max_rtps_message_size_ = (max_payload - overhead) & ~3u;

    if (talker)
    {
        // Two non-overlapping regions: the ACF message the library encodes, and
        // behind it the staging area the RTPS message is gathered into. They
        // must not alias --- acf_serdes_encode_packet() zeroes its whole output
        // before copying the payload in, so encoding in place would memset the
        // payload away and emit an all-zero frame.
        if (stream->uses_acf_)
        {
            const uint32_t field_size = acf_header_size + TsnRtpsHeader::size + stream->max_rtps_message_size_;
            stream->staging_offset_ = (field_size + 3u) & ~3u;
            stream->send_buffer_.resize(stream->staging_offset_ + stream->max_rtps_message_size_);
        }
        else
        {
            // Sent as-is, so no staging copy is needed.
            stream->staging_offset_ = 0;
            stream->send_buffer_.resize(TsnRtpsHeader::size + stream->max_rtps_message_size_);
        }
    }

    return stream;
}

std::unique_ptr<AvtpStream> AvtpStream::open_talker(
        const AvtpStreamConfig& config)
{
    return open(config, true);
}

std::unique_ptr<AvtpStream> AvtpStream::open_listener(
        const AvtpStreamConfig& config)
{
    return open(config, false);
}

AvtpStream::~AvtpStream()
{
    if (nullptr != connection_)
    {
        avtpcon_disconnect(connection_);
        avtpcon_close(connection_);
        connection_ = nullptr;
    }
}

void AvtpStream::disable()
{
    enabled_.store(false);
}

bool AvtpStream::send(
        const std::vector<NetworkBuffer>& buffers,
        uint32_t total_bytes,
        uint16_t destination_logical_port)
{
    if (!enabled_.load() || nullptr == connection_)
    {
        return false;
    }

    if (total_bytes > max_rtps_message_size_)
    {
        EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "RTPS message of " << total_bytes
                                                               << " octets exceeds the " << max_rtps_message_size_
                                                               << " octets that fit one Ethernet frame. Lower the "
                                                               << "participant's max message size so RTPS fragments.");
        return false;
    }

    std::lock_guard<std::mutex> guard(send_mutex_);

    // Gather the message into the staging area, clear of the ACF message the
    // library is about to write at the front of the buffer.
    uint8_t* const staging = send_buffer_.data() + staging_offset_;
    uint8_t* const payload = staging + TsnRtpsHeader::size;
    uint32_t offset = 0;
    for (const NetworkBuffer& buffer : buffers)
    {
        if (offset + buffer.size > total_bytes)
        {
            // Honour total_bytes as the boundary, as SenderResource::send() documents.
            const uint32_t remaining = total_bytes - offset;
            memcpy(payload + offset, buffer.buffer, remaining);
            offset = total_bytes;
            break;
        }
        memcpy(payload + offset, buffer.buffer, buffer.size);
        offset += buffer.size;
    }

    TsnRtpsHeader header;
    header.destination_logical_port = destination_logical_port;
    header.serialize(staging);

    int encoded = static_cast<int>(TsnRtpsHeader::size + offset);
    if (uses_acf_)
    {
        acf_msg_field_t field;
        memset(&field, 0, sizeof(field));
        field.msg_type = acf_message_type_;
        field.pl_length = static_cast<uint16_t>(TsnRtpsHeader::size + offset);
        field.payload = staging;

        encoded = acf_serdes_encode_packet(&field, send_buffer_.data());
        if (encoded <= 0)
        {
            EPROSIMA_LOG_WARNING(TSN_TRANSPORT,
                    "Cannot encode ACF message of type " << static_cast<int>(acf_message_type_));
            return false;
        }
    }

    // ts64 of 0 lets avtpcon fill the timestamp from the clock itself.
    const int sent = avtpcon_send_packet(connection_, encoded, send_buffer_.data(), 0);
    if (sent < 0)
    {
        EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "Error sending AVTP frame of " << encoded << " octets");
        return false;
    }
    if (0 == sent)
    {
        // The software shaper refused the frame because the stream is over rate.
        EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "AVTP frame dropped by the traffic shaper");
        return false;
    }

    return true;
}

bool AvtpStream::receive(
        octet* buffer,
        uint32_t capacity,
        AvtpReceivedMessage& out)
{
    if (!enabled_.load() || nullptr == connection_)
    {
        return false;
    }

    uint8_t* payload = nullptr;
    const int payload_size = avtpcon_rec_packet(connection_, &payload);
    if (payload_size <= 0 || nullptr == payload)
    {
        // 0 is a receive timeout, which is the normal way this returns; a
        // negative value has already been logged by avtpcon.
        return false;
    }

    const uint8_t* encapsulated = payload;
    uint32_t encapsulated_size = static_cast<uint32_t>(payload_size);

    if (uses_acf_)
    {
        acf_msg_field_t field;
        memset(&field, 0, sizeof(field));
        if (acf_serdes_decode_packet(payload, payload_size, &field) <= 0)
        {
            EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "Discarding frame with no decodable ACF message");
            return false;
        }

        if (field.msg_type != acf_message_type_)
        {
            // Another application may legitimately share this stream.
            return false;
        }

        encapsulated = static_cast<const uint8_t*>(field.payload);
        encapsulated_size = field.pl_length;
    }

    TsnRtpsHeader header;
    uint32_t rtps_length = 0;
    if (!header.deserialize(encapsulated, encapsulated_size, rtps_length))
    {
        EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "Discarding a PDU with a malformed RTPS header");
        return false;
    }

    const uint8_t* const rtps_message = encapsulated + TsnRtpsHeader::size;
    if (!has_rtps_magic(rtps_message, rtps_length))
    {
        EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "Discarding a PDU that does not carry an RTPS message");
        return false;
    }

    if (rtps_length > capacity)
    {
        EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "Discarding RTPS message of " << rtps_length
                                                                          << " octets, larger than the "
                                                                          << capacity << " octet receive buffer");
        return false;
    }

    memcpy(buffer, rtps_message, rtps_length);
    out.destination_logical_port = header.destination_logical_port;
    out.rtps_length = rtps_length;
    out.vlan_id = avtpcon_rec_vid(connection_);

    // Subclause 7.3.3 of [DDS-TSN] puts the talker node's MAC address in the
    // leading six octets of the stream id, which is the only place the sender's
    // address survives: the raw socket hands us no source MAC.
    union avtphead* avtp_header = avtpcon_rec_avtphead(connection_);
    if (nullptr != avtp_header)
    {
        memcpy(out.stream_id.data(), avtp_header->cmch.stream_id, 8);
    }

    return true;
}

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima
