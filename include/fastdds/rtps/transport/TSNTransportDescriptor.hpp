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
 * @file TSNTransportDescriptor.hpp
 */

#ifndef FASTDDS_RTPS_TRANSPORT__TSNTRANSPORTDESCRIPTOR_HPP
#define FASTDDS_RTPS_TRANSPORT__TSNTRANSPORTDESCRIPTOR_HPP

#include <cstdint>
#include <string>

#include <fastdds/fastdds_dll.hpp>
#include <fastdds/rtps/transport/PortBasedTransportDescriptor.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {

class TransportInterface;

/**
 * Configuration of the DDS-TSN transport: the DDSI-RTPS Ethernet PSM
 * (Annex A of [DDS-TSN], ptc/2023-03-03) carried over IEEE 1722 AVTP frames.
 *
 * The kind of the locators handled by this transport is
 * eprosima::fastdds::rtps::LOCATOR_KIND_ETHERNET.
 *
 * Every RTPS message is the payload of exactly one Ethernet frame, framed
 * according to whether the CNC has provisioned a stream for its destination.
 *
 * On a provisioned stream, a stream subtype:
 *
 * @code
 * [ Ethernet header | 802.1Q VLAN tag | EtherType 0x22F0 ]
 * [ AVTP stream header: subtype=0x7F, sequence_num, stream_id,
 *                       avtp_timestamp, stream_data_length ]
 * [ destination logical port | source logical port | rtps_length ]
 * [ RTPS message: Header + Submessages ]
 * @endcode
 *
 * Otherwise --- discovery, and anything else the CNC does not know about --- a
 * control format, whose payload is one ACF message:
 *
 * @code
 * [ Ethernet header | 802.1Q VLAN tag | EtherType 0x22F0 ]
 * [ NTSCF header: subtype=0x82, ntscf_data_length, sequence_num, stream_id ]
 * [ ACF header: msg_type=ACF_USER0, msg_length ]
 * [ destination logical port | source logical port | rtps_length ]
 * [ RTPS message: Header + Submessages ]
 * @endcode
 *
 * The split is not a preference. IEEE 1722 allows only one Talker per
 * destination address for streaming data, and discovery is many-to-many: every
 * participant announces to the same group address. Carrying that in a stream
 * subtype would put several talkers on one destination address. A control
 * format has no such restriction --- it is how ADP and MAAP already work --- at
 * the cost of the timestamp, which discovery does not need.
 *
 * Per-stream parameters --- destination MAC, VLAN ID, PCP, transmission
 * interval, maximum frame size --- are not configured here. They are read from
 * the @c ieee802-dot1q-cnc-config YANG datastore held by uniconf, which the CNC
 * populates in response to the CUC's Talker/Listener requests (subclause 7.3.3
 * of [DDS-TSN]). This descriptor only says *where* to find that datastore and
 * *which* streams belong to this participant.
 *
 * @ingroup TRANSPORT_MODULE
 */
struct TSNTransportDescriptor : public PortBasedTransportDescriptor
{
    /**
     * Upper bound on the RTPS message that fits one VLAN-tagged Ethernet frame.
     *
     * The exact figure depends on the subtype's header size and is computed per
     * stream from the interface MTU; this is only the cap applied to the
     * descriptor. A 24-octet stream header leaves about 1480 octets.
     */
    static constexpr uint32_t tsn_max_message_size = 1492;

    /**
     * Subtype for traffic on a CNC-provisioned stream: Experimental Format
     * Stream, per IEEE 1722-2016.
     *
     * A stream subtype suits provisioned traffic because such a stream has one
     * talker and its own destination address, which is what IEEE 1722 requires
     * of streaming data: "Only one Talker is allowed per destination_address."
     * Its header also carries @c avtp_timestamp and @c stream_data_length.
     * IEEE 1722 registers no subtype for RTPS, so the Experimental Format is the
     * honest choice; the Vendor Specific Format (0x6F) is the other candidate.
     */
    static constexpr uint8_t tsn_default_stream_subtype = 0x7F;

    /**
     * Subtype for traffic with no provisioned stream: NTSCF, a control format.
     *
     * Discovery is inherently many-to-many --- every participant announces to
     * the same group address --- which a stream subtype cannot represent without
     * breaking the one-talker-per-destination rule. Control formats carry no
     * such restriction, and many talkers on one well-known address is how IEEE
     * 1722 control protocols such as ADP and MAAP already work.
     */
    static constexpr uint8_t tsn_default_control_subtype = 0x82;

    //! ACF message type carrying RTPS inside a control-format PDU.
    static constexpr uint8_t tsn_default_acf_message_type = 0x78; // ACF_USER0

    //! Constructor
    FASTDDS_EXPORTED_API TSNTransportDescriptor();

    //! Copy constructor
    FASTDDS_EXPORTED_API TSNTransportDescriptor(
            const TSNTransportDescriptor& t) = default;

    //! Copy assignment
    FASTDDS_EXPORTED_API TSNTransportDescriptor& operator =(
            const TSNTransportDescriptor& t) = default;

    //! Destructor
    virtual FASTDDS_EXPORTED_API ~TSNTransportDescriptor() = default;

    FASTDDS_EXPORTED_API TransportInterface* create_transport() const override;

    //! Comparison operator
    FASTDDS_EXPORTED_API bool operator ==(
            const TSNTransportDescriptor& t) const;

    uint32_t min_send_buffer_size() const override
    {
        return maxMessageSize;
    }

    /**
     * Network interface the AVTP sockets are bound to, e.g. "eth0".
     *
     * Required. When a stream found in the CNC configuration names a different
     * interface, that stream is ignored by this transport instance.
     */
    std::string interface_name;

    /**
     * Path of the uniconf database holding the @c ieee802-dot1q-cnc-config data.
     *
     * When empty, the transport assumes uniconf has already been initialised by
     * the application (i.e. @c ydbi_access_init() has been called) and simply
     * uses the process-wide handle. Set it only when the transport should open
     * the database itself.
     */
    std::string uniconf_db_name;

    /**
     * Identifier of the CUC whose Talker/Listener entries describe this node,
     * matching the @c cuc-id key of the CNC configuration. The CNC uses the
     * bridge name here.
     */
    std::string cuc_id = "br01";

    /**
     * Instance index of the CNC configuration domain, i.e. the low byte of the
     * @c domain-id ("domain00" is 0).
     */
    uint8_t cnc_instance_index = 0;

    /**
     * Default multicast MAC address used by the SPDP built-in endpoints.
     *
     * Subclause A.6.1.4.1 of [DDS-TSN] gives this as "01:00:5E::EF:FF:00:01",
     * which is not a well-formed MAC address. It is read here as the IPv4
     * multicast MAC that maps the UDP/IP PSM default address 239.255.0.1
     * (0xEF 0xFF 0x00 0x01) following RFC 1112, which yields 01:00:5e:7f:00:01.
     * Override it if you interoperate with an implementation that reads the
     * specification differently.
     */
    std::string default_multicast_mac = "01:00:5e:7f:00:01";

    /**
     * VLAN ID used for traffic that has no CNC-provisioned stream, i.e.
     * discovery traffic. 0 means "no VLAN tag known"; frames are still sent
     * VLAN-tagged so that the PCP survives, as required to reach a traffic
     * class in the bridge.
     */
    uint16_t default_vlan_id = 0;

    /**
     * Priority Code Point used for traffic that has no CNC-provisioned stream.
     * Discovery is not time-critical (subclause 8.2.2.1 of [DDS-TSN]), so this
     * defaults to best effort.
     */
    uint8_t default_pcp = 0;

    /**
     * Socket priority applied with SO_PRIORITY. This is what a Linux qdisc such
     * as @c mqprio or @c taprio matches on; it is not necessarily the PCP.
     */
    uint8_t socket_priority = 0;

    /**
     * Whether traffic with no CNC-provisioned stream may still be sent.
     *
     * When true (the default), a destination the CNC knows nothing about is
     * reached over @ref default_vlan_id and @ref default_pcp with a locally
     * derived stream ID. Traffic flows, but unscheduled: the network cannot tell
     * it apart from any other topic's, so it gets no reserved bandwidth and no
     * traffic class. Discovery relies on this, since the CUC has no reason to
     * provision a stream for SPDP.
     *
     * When false the transport is strict. It waits during initialisation for the
     * CNC to provision and accept every stream for this node, for at most
     * @ref cnc_wait_timeout_ms; if that expires it logs an error and refuses to
     * start, which makes DomainParticipant creation fail. Once running it
     * refuses to send to any destination with no provisioned stream, rather than
     * emitting unscheduled frames. Use it where sending off-schedule is worse
     * than not sending.
     *
     * @warning With this false and @ref cnc_wait_timeout_ms set to 0, creating
     * the participant blocks until the CNC responds, with no way out but a
     * signal.
     */
    bool allow_fallback = true;

    /**
     * When true, block during initialisation until the CNC has accepted this
     * node's streams, then carry on with the defaults if it has not.
     *
     * Only consulted when @ref allow_fallback is true; strict mode always waits.
     */
    bool wait_for_cnc = true;

    /**
     * How long to wait for the CNC to provision and accept this node's streams,
     * in milliseconds. 0 waits indefinitely.
     *
     * What expiry means depends on @ref allow_fallback: with the fallback
     * allowed the transport carries on with the default VLAN and PCP; in strict
     * mode it logs an error and refuses to start.
     */
    uint32_t cnc_wait_timeout_ms = 10000;

    /**
     * When true, use the gPTP-disciplined clock for AVTP timestamps; when
     * false, use the monotonic system clock. Set it to false on nodes with no
     * gptp2d running.
     */
    bool use_gptp = true;

    /**
     * Shared memory segment published by gptp2d. Empty selects the library
     * default, which is what gptp2d creates unless configured otherwise.
     */
    std::string gptp_shmem_name;

    //! AVTP header version, 0 or 1.
    uint8_t avtp_header_version = 0;

    /**
     * IEEE 1722 subtype for traffic on a CNC-provisioned stream. Must be a
     * stream subtype.
     */
    uint8_t stream_subtype = tsn_default_stream_subtype;

    /**
     * IEEE 1722 subtype for traffic with no provisioned stream, discovery above
     * all. Must be a control format (NTSCF 0x82 or TSCF 0x06), so that several
     * nodes may share one destination address.
     */
    uint8_t control_subtype = tsn_default_control_subtype;

    //! ACF message type used to carry RTPS messages inside a control-format PDU.
    uint8_t acf_message_type = tsn_default_acf_message_type;

    /**
     * Receive timeout of the listener sockets in milliseconds. The reception
     * threads use it to check for shutdown, so it bounds how long closing an
     * input channel takes.
     */
    uint32_t reception_timeout_ms = 100;
};

} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_RTPS_TRANSPORT__TSNTRANSPORTDESCRIPTOR_HPP
