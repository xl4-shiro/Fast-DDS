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
 * @file AvtpStream.hpp
 */

#ifndef FASTDDS_RTPS_TRANSPORT_TSN__AVTPSTREAM_HPP
#define FASTDDS_RTPS_TRANSPORT_TSN__AVTPSTREAM_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fastdds/rtps/common/Types.hpp>
#include <fastdds/rtps/transport/NetworkBuffer.hpp>
#include <fastdds/utils/EthernetLocator.hpp>

struct avtpcon_data;

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

//! An IEEE 1722 stream ID: 6 octets of MAC address plus a 16-bit unique ID.
using StreamId = std::array<uint8_t, 8>;

//! Build a stream ID from a MAC address and a unique ID, per subclause 7.3.3 of [DDS-TSN].
StreamId make_stream_id(
        const MacAddress& mac,
        uint16_t unique_id);

//! Format a stream ID as "aa:bb:cc:dd:ee:ff:gg:hh".
std::string stream_id_to_string(
        const StreamId& stream_id);

//! Parse "aa:bb:cc:dd:ee:ff:gg:hh" into a stream ID. Returns false if malformed.
bool string_to_stream_id(
        const std::string& str,
        StreamId& stream_id);

/**
 * Parameters of one IEEE 1722 stream. The CNC supplies most of these through
 * the @c ieee802-dot1q-cnc-config datastore; the rest come from the transport
 * descriptor.
 */
struct AvtpStreamConfig
{
    //! Network interface to bind to, e.g. "eth0".
    std::string interface_name;
    //! Destination MAC address. For a listener this is also the socket's frame filter.
    MacAddress destination_mac{};
    //! IEEE 1722 stream ID. Ignored by listeners, which accept every stream ID reaching them.
    StreamId stream_id{};
    //! Whether @ref stream_id came from the CNC rather than being derived locally.
    bool stream_id_from_cnc = false;
    //! VLAN ID of the 802.1Q tag.
    uint16_t vlan_id = 0;
    //! Priority Code Point of the 802.1Q tag.
    uint8_t pcp = 0;
    //! Socket priority, applied with SO_PRIORITY. Matched by qdiscs such as taprio.
    uint8_t socket_priority = 0;
    //! IEEE 1722 subtype. Experimental Format Stream (0x7F) unless overridden.
    uint8_t subtype = 0x7F;
    //! AVTP header version, 0 or 1.
    uint8_t header_version = 0;
    //! ACF message type carrying the RTPS message. Only used by a control subtype.
    uint8_t acf_message_type = 0x78;
    //! Use the gPTP-disciplined clock for AVTP timestamps instead of the monotonic clock.
    bool use_gptp = true;
    //! gptp2d shared memory segment. Empty selects the library default.
    std::string gptp_shmem_name;
    //! Receive timeout in microseconds. 0 blocks indefinitely.
    uint32_t reception_timeout_us = 100000;
    //! Send timeout in microseconds. 0 blocks indefinitely.
    uint32_t send_timeout_us = 0;
    /**
     * Software credit-based shaper rate in kilobytes per second, computed from
     * the CNC traffic specification. 0 disables the shaper, which is the right
     * setting when the NIC or a qdisc already shapes the stream.
     */
    uint32_t shaper_rate_kbps = 0;
    //! Software shaper high credit, in octets. Only meaningful with a non-zero rate.
    uint32_t shaper_hi_credit = 0;
};

//! Result of AvtpStream::receive().
struct AvtpReceivedMessage
{
    //! Logical port the message is addressed to.
    uint16_t destination_logical_port = 0;
    //! Logical port of the sender.
    uint16_t source_logical_port = 0;
    //! Length of the RTPS message written into the caller's buffer.
    uint32_t rtps_length = 0;
    //! Stream ID carried by the NTSCF header.
    StreamId stream_id{};
    //! VLAN ID of the received frame.
    uint16_t vlan_id = 0;
};

/**
 * One IEEE 1722 socket, either sending (talker) or receiving (listener).
 *
 * A talker is bound to a fixed destination MAC, VLAN tag and stream ID: it maps
 * one-to-one onto a TSN Stream as provisioned by the CNC, so the AVTP sequence
 * numbering and the software shaper apply per stream, as they should.
 *
 * A listener filters incoming frames by destination MAC address only --- that
 * is what the raw socket can do --- and leaves demultiplexing by logical port
 * to the caller.
 */
class AvtpStream
{
public:

    //! Open a talker. Returns nullptr on failure.
    static std::unique_ptr<AvtpStream> open_talker(
            const AvtpStreamConfig& config);

    //! Open a listener. Returns nullptr on failure.
    static std::unique_ptr<AvtpStream> open_listener(
            const AvtpStreamConfig& config);

    ~AvtpStream();

    AvtpStream(
            const AvtpStream&) = delete;
    AvtpStream& operator =(
            const AvtpStream&) = delete;

    /**
     * Encapsulate an RTPS message and send it as one Ethernet frame.
     *
     * @param buffers                    Slices making up the RTPS message.
     * @param total_bytes                Sum of the slice sizes.
     * @param destination_logical_port   Logical port of the destination locator.
     * @param source_logical_port        Logical port announced as the source.
     *
     * @return true when the whole message was handed to the socket.
     */
    bool send(
            const std::vector<NetworkBuffer>& buffers,
            uint32_t total_bytes,
            uint16_t destination_logical_port,
            uint16_t source_logical_port);

    /**
     * Receive one frame and extract the RTPS message it carries.
     *
     * Blocks for at most @c reception_timeout_us. Frames that do not carry a
     * well-formed RTPS message are dropped and reported as a timeout, so the
     * caller can simply loop.
     *
     * @param buffer     Destination for the RTPS message.
     * @param capacity   Octets available in @c buffer.
     * @param [out] out  Description of the received message. Only valid when this returns true.
     *
     * @return true when a message was written into @c buffer.
     */
    bool receive(
            octet* buffer,
            uint32_t capacity,
            AvtpReceivedMessage& out);

    //! Largest RTPS message that fits one frame on this stream.
    uint32_t max_rtps_message_size() const
    {
        return max_rtps_message_size_;
    }

    //! MAC address of the interface this stream is bound to.
    const MacAddress& local_mac() const
    {
        return local_mac_;
    }

    //! Stop a blocked receive() and make every later call fail.
    void disable();

private:

    AvtpStream() = default;

    static std::unique_ptr<AvtpStream> open(
            const AvtpStreamConfig& config,
            bool talker);

    avtpcon_data* connection_ = nullptr;
    MacAddress local_mac_{};
    uint32_t max_rtps_message_size_ = 0;
    uint8_t acf_message_type_ = 0x78;
    //! Whether the configured subtype wraps the payload in an ACF message.
    bool uses_acf_ = false;
    bool talker_ = false;
    std::atomic<bool> enabled_{true};

    /**
     * Serializes send().
     *
     * The participant already serializes its own sends behind
     * m_send_resources_mutex_, but avtpcon keeps the AVTP sequence number and
     * the shaper credit per connection, and @ref send_buffer_ is shared, so the
     * invariant belongs here rather than in a caller that might change.
     */
    std::mutex send_mutex_;

    /**
     * Scratch buffer for one frame, in two non-overlapping halves: the encoded
     * ACF message at the front, and from @ref staging_offset_ the RTPS message
     * as it is gathered. Keeping them apart is required, not just tidy: the ACF
     * encoder zeroes its whole output before copying the payload in.
     */
    std::vector<uint8_t> send_buffer_;

    //! Offset of the staging half of @ref send_buffer_.
    uint32_t staging_offset_ = 0;
};

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_RTPS_TRANSPORT_TSN__AVTPSTREAM_HPP
