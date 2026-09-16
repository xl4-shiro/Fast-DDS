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
 * @file TSNTransport.hpp
 */

#ifndef FASTDDS_RTPS_TRANSPORT_TSN__TSNTRANSPORT_HPP
#define FASTDDS_RTPS_TRANSPORT_TSN__TSNTRANSPORT_HPP

#include <atomic>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fastdds/rtps/transport/TransportInterface.hpp>

#include <rtps/transport/MulticastTransportInterface.hpp>
#include <fastdds/rtps/transport/TSNTransportDescriptor.hpp>
#include <fastdds/utils/EthernetLocator.hpp>

#include <rtps/transport/tsn/AvtpStream.hpp>
#include <rtps/transport/tsn/TsnCncConfig.hpp>
#include <rtps/transport/tsn/TSNChannelResource.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {

/**
 * The DDSI-RTPS Ethernet PSM (Annex A of [DDS-TSN]) carried over IEEE 1722.
 *
 * Every RTPS message --- discovery included, so a participant can run with no
 * IP stack --- becomes the payload of exactly one Ethernet frame, encapsulated
 * in an NTSCF PDU with a single ACF message. Per-stream parameters come from
 * the @c ieee802-dot1q-cnc-config datastore: when a destination locator matches
 * a stream the CNC provisioned, the frame goes out on that stream's ID, VLAN
 * tag and PCP, shaped to the granted rate. Traffic with no provisioned stream,
 * such as discovery, falls back to the descriptor's defaults.
 *
 * @ingroup TRANSPORT_MODULE
 */
class TSNTransport
    : public TransportInterface
    , public MulticastTransportInterface
{
public:

    explicit TSNTransport(
            const TSNTransportDescriptor& descriptor);

    ~TSNTransport() override;

    const TSNTransportDescriptor* configuration() const
    {
        return &configuration_;
    }

    TransportDescriptorInterface* get_configuration() override
    {
        return &configuration_;
    }

    bool init(
            const PropertyPolicy* properties = nullptr,
            const uint32_t& max_msg_size_no_frag = 0) override;

    void shutdown() override;

    bool IsInputChannelOpen(
            const Locator&) const override;

    bool IsLocatorSupported(
            const Locator&) const override;

    bool is_locator_allowed(
            const Locator&) const override;

    bool is_locator_reachable(
            const Locator_t& locator) override;

    Locator RemoteToMainLocal(
            const Locator& remote) const override;

    bool transform_remote_locator(
            const Locator& remote_locator,
            Locator& result_locator) const override;

    bool OpenOutputChannel(
            SendResourceList& sender_resource_list,
            const Locator&) override;

    bool OpenInputChannel(
            const Locator&,
            TransportReceiverInterface*,
            uint32_t) override;

    bool CloseInputChannel(
            const Locator&) override;

    bool DoInputLocatorsMatch(
            const Locator&,
            const Locator&) const override;

    LocatorList NormalizeLocator(
            const Locator& locator) override;

    void select_locators(
            LocatorSelector& selector) const override;

    bool is_local_locator(
            const Locator& locator) const override;

    void AddDefaultOutputLocator(
            LocatorList& defaultList) override;

    /**
     * Multicast locators for user traffic.
     *
     * Implementing MulticastTransportInterface is what lets Fast DDS fill in the
     * logical port of an endpoint's multicast locator: NetworkFactory reaches
     * these through a dynamic_cast, and without it a locator supplied by the
     * application keeps whatever port it was built with, which for a CNC-derived
     * one is none.
     */
    bool getDefaultMulticastLocators(
            LocatorList& locators,
            uint32_t multicast_port) const override;

    bool fillMulticastLocator(
            Locator& locator,
            uint32_t well_known_port) const override;

    bool getDefaultMetatrafficMulticastLocators(
            LocatorList& locators,
            uint32_t metatraffic_multicast_port) const override;

    bool getDefaultMetatrafficUnicastLocators(
            LocatorList& locators,
            uint32_t metatraffic_unicast_port) const override;

    bool getDefaultUnicastLocators(
            LocatorList& locators,
            uint32_t unicast_port) const override;

    bool fillMetatrafficMulticastLocator(
            Locator& locator,
            uint32_t metatraffic_multicast_port) const override;

    bool fillMetatrafficUnicastLocator(
            Locator& locator,
            uint32_t metatraffic_unicast_port) const override;

    bool configureInitialPeerLocator(
            Locator& locator,
            const PortParameters& port_params,
            uint32_t domainId,
            LocatorList& list) const override;

    bool fillUnicastLocator(
            Locator& locator,
            uint32_t well_known_port) const override;

    uint32_t max_recv_buffer_size() const override
    {
        return configuration_.maxMessageSize;
    }

    /**
     * Send one RTPS message to every supported destination locator.
     *
     * Called by @ref tsn::TSNSenderResource, which owns no state of its own:
     * the AVTP talkers live here so that several sender resources reaching the
     * same destination share one stream, and with it one AVTP sequence number
     * space and one shaper.
     */
    bool send(
            const std::vector<NetworkBuffer>& buffers,
            uint32_t total_bytes,
            LocatorsIterator* destination_locators_begin,
            LocatorsIterator* destination_locators_end);

    //! Locator naming this node on this transport, for @c add_locators_to_list.
    Locator local_locator() const;

private:

    //! Identity of one AVTP talker: everything that fixes the frames it emits.
    struct TalkerKey
    {
        MacAddress destination_mac;
        uint16_t vlan_id;
        uint8_t pcp;

        bool operator <(
                const TalkerKey& other) const
        {
            if (destination_mac != other.destination_mac)
            {
                return destination_mac < other.destination_mac;
            }
            if (vlan_id != other.vlan_id)
            {
                return vlan_id < other.vlan_id;
            }
            return pcp < other.pcp;
        }

    };

    //! Identity of one AVTP listener socket: what the raw socket can filter on.
    struct ListenerKey
    {
        MacAddress destination_mac;
        uint16_t vlan_id;

        bool operator <(
                const ListenerKey& other) const
        {
            if (destination_mac != other.destination_mac)
            {
                return destination_mac < other.destination_mac;
            }
            return vlan_id < other.vlan_id;
        }

    };

    static ListenerKey listener_key(
            const Locator& locator);

    /**
     * Fill in the stream parameters for a destination locator.
     *
     * Consults the CNC configuration first; falls back to the locator's own
     * VLAN tag and the descriptor defaults when no stream matches.
     */
    tsn::AvtpStreamConfig stream_config_for(
            const Locator& locator,
            bool talker) const;

    //! Find or open the talker reaching @c locator.
    tsn::AvtpStream* get_or_open_talker(
            const Locator& locator);

    //! Read the MAC address and MTU of the configured interface.
    bool resolve_local_mac();

    //! Largest RTPS message one frame carries on this interface, 0 if none fits.
    uint32_t max_rtps_message_for_interface() const;

    TSNTransportDescriptor configuration_;
    MacAddress local_mac_{};
    MacAddress default_multicast_mac_{};
    uint32_t interface_mtu_ = 1500;
    std::atomic<bool> initialized_{false};

    std::unique_ptr<tsn::TsnCncConfig> cnc_config_;

    /**
     * Re-read the CNC configuration periodically and disconnect or reconnect
     * streams whose @c accept has changed, until @ref monitor_running_ is
     * cleared.
     */
    void revocation_monitor();

    //! One pass: compare every open stream against the CNC's current accept.
    void apply_cnc_acceptance();

    //! Log the state change and report the reached state back to the CNC.
    void report_connectivity(
            const tsn::TsnStream& stream);

    /**
     * Last @c accept seen for each stream provisioned for this interface.
     *
     * Keyed on stream ID rather than on anything the transport has open, so that
     * a stream the node is not currently using is still tracked: a talker is
     * only opened once there is something to send to, and a deletion arriving
     * before that must not go unnoticed. Guarded by @ref accept_mutex_.
     */
    mutable std::mutex accept_mutex_;
    std::map<tsn::StreamId, tsn::CncAccept> last_accept_;

    //! Streams already announced as deleted, so that is reported once each.
    std::set<tsn::StreamId> deleted_streams_;

    std::thread revocation_thread_;
    std::atomic<bool> monitor_running_{false};

    mutable std::mutex input_mutex_;
    std::map<ListenerKey, std::unique_ptr<tsn::TSNChannelResource>> input_channels_;

    mutable std::mutex output_mutex_;
    std::map<TalkerKey, std::unique_ptr<tsn::AvtpStream>> talkers_;

    /**
     * Source of the unique ID half of a locally derived stream ID, handed out
     * once per talker. Guarded by @ref output_mutex_.
     */
    uint16_t next_stream_unique_id_ = 1;

    /**
     * Logical port used to describe this transport's own locator.
     *
     * Set from the first unicast input channel opened, which is the
     * participant's metatraffic unicast port. It is not put on the wire --- the
     * framing carries only the destination port --- and serves to give
     * @ref local_locator() something more useful than zero for the sender
     * resource's bookkeeping.
     */
    std::atomic<uint16_t> local_logical_port_{0};
};

} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_RTPS_TRANSPORT_TSN__TSNTRANSPORT_HPP
