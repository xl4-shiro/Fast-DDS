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
 * @file TSNTransport.cpp
 */

#include <rtps/transport/tsn/TSNTransport.hpp>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

#include <fastdds/dds/log/Log.hpp>
#include <fastdds/rtps/common/LocatorSelector.hpp>
#include <fastdds/rtps/common/LocatorSelectorEntry.hpp>
#include <fastdds/rtps/common/PortParameters.hpp>

#include <rtps/transport/tsn/TSNSenderResource.hpp>
#include <rtps/transport/tsn/Xl4Runtime.hpp>

extern "C" {
#include <xl4unibase/unibase.h>
#include <xl4combase/cb_ethernet.h>
} // extern "C"

namespace eprosima {
namespace fastdds {
namespace rtps {

using tsn::AvtpStream;
using tsn::AvtpStreamConfig;
using tsn::TsnCncConfig;
using tsn::TsnStream;
using tsn::TSNChannelResource;
using tsn::TSNSenderResource;

TSNTransport::TSNTransport(
        const TSNTransportDescriptor& descriptor)
    : TransportInterface(LOCATOR_KIND_ETHERNET)
    , configuration_(descriptor)
{
}

TSNTransport::~TSNTransport()
{
    shutdown();
}

bool TSNTransport::resolve_local_mac()
{
    // cb_get_mac_bydev() needs any socket to hang the ioctl on.
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "Cannot create a socket to query the interface MAC address");
        return false;
    }

    ub_macaddr_t mac;
    const int result = cb_get_mac_bydev(fd, configuration_.interface_name.c_str(), mac);

    // Take the MTU from the same socket: it sets how much RTPS fits one frame.
    struct ifreq request;
    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, configuration_.interface_name.c_str(), IFNAMSIZ - 1);
    if (0 == ioctl(fd, SIOCGIFMTU, &request) && request.ifr_mtu > 0)
    {
        interface_mtu_ = static_cast<uint32_t>(request.ifr_mtu);
    }

    close(fd);

    if (0 != result)
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT,
                "Cannot read the MAC address of interface '" << configuration_.interface_name << "'");
        return false;
    }

    memcpy(local_mac_.data(), mac, 6);
    return true;
}

uint32_t TSNTransport::max_rtps_message_for_interface() const
{
    // One RTPS message is one frame, so the budget is the MTU less every header
    // between it and the payload. AvtpStream recomputes this per stream from
    // what the socket reports; this is the same arithmetic applied up front, so
    // Fast DDS never builds a message the stream would have to drop.
    constexpr uint32_t vlan_tag_size = 4u;
    constexpr uint32_t stream_header_size = 24u;
    constexpr uint32_t control_header_size = 12u;
    constexpr uint32_t acf_header_size = 2u;
    constexpr uint32_t rtps_header_size = 6u; // tsn::TsnRtpsHeader::size

    const bool uses_acf = (0x82 == configuration_.avtp_subtype || 0x06 == configuration_.avtp_subtype);
    uint32_t overhead = vlan_tag_size + rtps_header_size;
    overhead += uses_acf ? (control_header_size + acf_header_size) : stream_header_size;

    if (interface_mtu_ <= overhead)
    {
        return 0;
    }
    return (interface_mtu_ - overhead) & ~3u;
}

bool TSNTransport::init(
        const PropertyPolicy* properties,
        const uint32_t& max_msg_size_no_frag)
{
    static_cast<void>(properties);

    // Before anything else: every xl4 library below this point reaches its
    // clock and allocator through pointers unibase_init() installs.
    tsn::init_xl4_runtime();

    if (configuration_.interface_name.empty())
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "TSNTransportDescriptor::interface_name is required");
        return false;
    }

    if (!EthernetLocator::string_to_mac(configuration_.default_multicast_mac, default_multicast_mac_))
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT,
                "TSNTransportDescriptor::default_multicast_mac '" << configuration_.default_multicast_mac
                                                                  << "' is not a MAC address");
        return false;
    }

    if (!resolve_local_mac())
    {
        return false;
    }

    // One RTPS message is one Ethernet frame, so the message size cannot exceed
    // what a frame holds. RTPS fragments anything larger into DataFrag
    // submessages, which is what subclause 8.2.1 of [DDS-TSN] expects.
    const uint32_t frame_budget = max_rtps_message_for_interface();
    if (0 == frame_budget)
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "Interface " << configuration_.interface_name
                                                       << " has an MTU of " << interface_mtu_
                                                       << ", too small to carry an RTPS message");
        return false;
    }
    if (configuration_.maxMessageSize > frame_budget)
    {
        EPROSIMA_LOG_INFO(TSN_TRANSPORT, "Capping max message size from " << configuration_.maxMessageSize
                                                                          << " to " << frame_budget
                                                                          << " octets, what one frame carries on "
                                                                          << configuration_.interface_name
                                                                          << " (MTU " << interface_mtu_ << ")");
        configuration_.maxMessageSize = frame_budget;
    }
    if (0 != max_msg_size_no_frag && max_msg_size_no_frag < configuration_.maxMessageSize)
    {
        configuration_.maxMessageSize = max_msg_size_no_frag;
    }

    // The CNC configuration is optional: without it the transport still works,
    // it just sends everything on the descriptor's default VLAN and PCP with no
    // scheduled stream behind it.
    cnc_config_ = TsnCncConfig::create(configuration_);
    if (cnc_config_)
    {
        if (configuration_.wait_for_cnc)
        {
            cnc_config_->wait_for_accepted_streams(configuration_.cnc_wait_timeout_ms);
        }
        cnc_config_->report_status_for_all(tsn::EndStationStatus::connected);
    }
    else if (configuration_.require_accepted_streams)
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT,
                "require_accepted_streams is set but the CNC configuration is unavailable");
        return false;
    }

    initialized_.store(true);
    EPROSIMA_LOG_INFO(TSN_TRANSPORT, "TSN transport ready on " << configuration_.interface_name
                                                               << " (" << EthernetLocator::mac_to_string(local_mac_)
                                                               << "), max RTPS message "
                                                               << configuration_.maxMessageSize << " octets");
    return true;
}

void TSNTransport::shutdown()
{
    if (!initialized_.exchange(false))
    {
        return;
    }

    if (cnc_config_)
    {
        cnc_config_->report_status_for_all(tsn::EndStationStatus::disconnected);
    }

    {
        std::lock_guard<std::mutex> guard(input_mutex_);
        for (auto& entry : input_channels_)
        {
            entry.second->disable();
        }
        input_channels_.clear();
    }

    {
        std::lock_guard<std::mutex> guard(output_mutex_);
        talkers_.clear();
    }

    cnc_config_.reset();
}

bool TSNTransport::IsLocatorSupported(
        const Locator& locator) const
{
    return locator.kind == transport_kind_;
}

bool TSNTransport::is_locator_allowed(
        const Locator& locator) const
{
    return IsLocatorSupported(locator);
}

bool TSNTransport::is_locator_reachable(
        const Locator_t& locator)
{
    // Any MAC address on the configured interface's link is reachable; a raw
    // socket has no routing table to consult.
    return IsLocatorSupported(locator);
}

bool TSNTransport::is_local_locator(
        const Locator& locator) const
{
    return IsLocatorSupported(locator) && EthernetLocator::mac(locator) == local_mac_;
}

Locator TSNTransport::RemoteToMainLocal(
        const Locator& remote) const
{
    Locator main_local(remote);
    if (!IsLocatorSupported(remote))
    {
        main_local.kind = LOCATOR_KIND_INVALID;
    }
    main_local.set_Invalid_Address();
    return main_local;
}

bool TSNTransport::transform_remote_locator(
        const Locator& remote_locator,
        Locator& result_locator) const
{
    if (!IsLocatorSupported(remote_locator))
    {
        return false;
    }

    result_locator = remote_locator;
    return true;
}

Locator TSNTransport::local_locator() const
{
    return EthernetLocator::create_locator(local_mac_, configuration_.default_vlan_id,
                   configuration_.default_pcp, source_logical_port_.load());
}

TSNTransport::ListenerKey TSNTransport::listener_key(
        const Locator& locator)
{
    ListenerKey key;
    key.destination_mac = EthernetLocator::mac(locator);
    key.vlan_id = EthernetLocator::vid(locator);
    return key;
}

tsn::AvtpStreamConfig TSNTransport::stream_config_for(
        const Locator& locator,
        bool talker) const
{
    AvtpStreamConfig config;
    config.interface_name = configuration_.interface_name;
    config.destination_mac = EthernetLocator::mac(locator);
    config.vlan_id = EthernetLocator::vid(locator);
    config.pcp = EthernetLocator::pcp(locator);
    config.socket_priority = configuration_.socket_priority;
    config.subtype = configuration_.avtp_subtype;
    config.header_version = configuration_.avtp_header_version;
    config.acf_message_type = configuration_.acf_message_type;
    config.use_gptp = configuration_.use_gptp;
    config.gptp_shmem_name = configuration_.gptp_shmem_name;
    config.reception_timeout_us = configuration_.reception_timeout_ms * 1000u;

    // A locator built by Fast DDS from the default port expressions carries no
    // VLAN tag, so fall back to the descriptor's defaults.
    if (0 == config.vlan_id)
    {
        config.vlan_id = configuration_.default_vlan_id;
    }
    if (0 == config.pcp)
    {
        config.pcp = configuration_.default_pcp;
    }

    // The stream ID identifies this node's traffic: the local MAC address plus
    // a unique ID, per subclause 7.3.3 of [DDS-TSN]. Without a provisioned
    // stream, the RTPS logical port serves as that unique ID.
    config.stream_id = tsn::make_stream_id(local_mac_, EthernetLocator::logical_port(locator));

    if (talker && cnc_config_)
    {
        TsnStream stream;
        if (cnc_config_->find_talker_for_destination(config.destination_mac,
                EthernetLocator::vid(locator), stream))
        {
            config.stream_id = stream.stream_id;
            config.vlan_id = stream.vlan_id;
            config.pcp = stream.pcp;
            config.socket_priority = stream.pcp;
            config.shaper_rate_kbps = stream.shaper_rate_kbps();
            config.shaper_hi_credit = stream.max_frame_size;

            EPROSIMA_LOG_INFO(TSN_TRANSPORT, "Destination " << EthernetLocator::mac_to_string(locator)
                                                            << " uses CNC stream "
                                                            << tsn::stream_id_to_string(stream.stream_id)
                                                            << " on VLAN " << stream.vlan_id
                                                            << " PCP " << static_cast<int>(stream.pcp));
        }
        else if (configuration_.require_accepted_streams)
        {
            EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "No CNC stream for destination "
                    << EthernetLocator::mac_to_string(locator) << "; the frame will not be sent");
        }
    }

    return config;
}

bool TSNTransport::OpenOutputChannel(
        SendResourceList& sender_resource_list,
        const Locator& locator)
{
    if (!is_locator_allowed(locator))
    {
        return false;
    }

    // One sender resource serves the whole transport: the talkers behind it are
    // keyed by destination, not by the resource that reaches them.
    for (const auto& resource : sender_resource_list)
    {
        if (resource->kind() == transport_kind_)
        {
            return true;
        }
    }

    try
    {
        sender_resource_list.emplace_back(static_cast<SenderResource*>(new TSNSenderResource(*this)));
    }
    catch (const std::exception& error)
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "Cannot create the TSN sender resource: " << error.what());
        return false;
    }

    return true;
}

AvtpStream* TSNTransport::get_or_open_talker(
        const Locator& locator)
{
    const AvtpStreamConfig config = stream_config_for(locator, true);

    TalkerKey key;
    key.destination_mac = config.destination_mac;
    key.vlan_id = config.vlan_id;
    key.pcp = config.pcp;

    std::lock_guard<std::mutex> guard(output_mutex_);
    auto it = talkers_.find(key);
    if (it != talkers_.end())
    {
        return it->second.get();
    }

    std::unique_ptr<AvtpStream> stream = AvtpStream::open_talker(config);
    if (!stream)
    {
        return nullptr;
    }

    AvtpStream* raw = stream.get();
    talkers_.emplace(key, std::move(stream));
    return raw;
}

bool TSNTransport::send(
        const std::vector<NetworkBuffer>& buffers,
        uint32_t total_bytes,
        LocatorsIterator* destination_locators_begin,
        LocatorsIterator* destination_locators_end)
{
    if (!initialized_.load())
    {
        return false;
    }

    bool sent_to_any = false;
    const uint16_t source_port = source_logical_port_.load();

    LocatorsIterator& it = *destination_locators_begin;
    while (it != *destination_locators_end)
    {
        const Locator& locator = *it;
        if (IsLocatorSupported(locator))
        {
            AvtpStream* talker = get_or_open_talker(locator);
            if (nullptr != talker)
            {
                sent_to_any |= talker->send(buffers, total_bytes,
                                EthernetLocator::logical_port(locator), source_port);
            }
        }
        ++it;
    }

    return sent_to_any;
}

bool TSNTransport::OpenInputChannel(
        const Locator& locator,
        TransportReceiverInterface* receiver,
        uint32_t max_msg_size)
{
    if (!is_locator_allowed(locator))
    {
        return false;
    }

    const ListenerKey key = listener_key(locator);

    std::lock_guard<std::mutex> guard(input_mutex_);
    auto it = input_channels_.find(key);
    if (it == input_channels_.end())
    {
        AvtpStreamConfig config = stream_config_for(locator, false);
        const uint32_t buffer_size = std::min(max_msg_size, configuration_.maxMessageSize);
        auto channel = TSNChannelResource::create(config, buffer_size,
                        configuration_.get_thread_config_for_port(EthernetLocator::logical_port(locator)));
        if (!channel)
        {
            EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "Cannot open an input channel for " << locator);
            return false;
        }
        it = input_channels_.emplace(key, std::move(channel)).first;

        EPROSIMA_LOG_INFO(TSN_TRANSPORT, "Listening on " << EthernetLocator::mac_to_string(locator)
                                                         << " VLAN " << key.vlan_id);
    }

    if (!it->second->add_receiver(locator, receiver))
    {
        EPROSIMA_LOG_ERROR(TSN_TRANSPORT, "Logical port " << EthernetLocator::logical_port(locator)
                                                          << " is already taken on this channel");
        return false;
    }

    // Announce the first unicast port we listen on as the source of our
    // outgoing messages, so remote locators built by peers point back here.
    if (!EthernetLocator::is_multicast(locator))
    {
        uint16_t expected = 0;
        source_logical_port_.compare_exchange_strong(expected, EthernetLocator::logical_port(locator));
    }

    return true;
}

bool TSNTransport::CloseInputChannel(
        const Locator& locator)
{
    std::unique_ptr<TSNChannelResource> to_destroy;

    {
        std::lock_guard<std::mutex> guard(input_mutex_);
        auto it = input_channels_.find(listener_key(locator));
        if (it == input_channels_.end())
        {
            return false;
        }

        if (0 == it->second->remove_receiver(locator))
        {
            // Move the channel out and destroy it outside the lock: its
            // destructor joins the reception thread, which may be inside
            // OnDataReceived and reach back into the transport.
            it->second->disable();
            to_destroy = std::move(it->second);
            input_channels_.erase(it);
        }
    }

    return true;
}

bool TSNTransport::IsInputChannelOpen(
        const Locator& locator) const
{
    if (!IsLocatorSupported(locator))
    {
        return false;
    }

    // The question is per locator, not per socket. One socket serves every
    // logical port that shares a MAC address and VLAN, and NetworkFactory skips
    // building a ReceiverResource whenever this returns true --- so answering
    // for the socket would leave the participant's user traffic with no
    // receiver at all, because its metatraffic already opened the socket.
    std::lock_guard<std::mutex> guard(input_mutex_);
    auto it = input_channels_.find(listener_key(locator));
    return it != input_channels_.end() && it->second->has_receiver_for(locator);
}

bool TSNTransport::DoInputLocatorsMatch(
        const Locator& left,
        const Locator& right) const
{
    return EthernetLocator::same_channel(left, right);
}

LocatorList TSNTransport::NormalizeLocator(
        const Locator& locator)
{
    LocatorList list;

    if (IsAddressDefined(locator))
    {
        list.push_back(locator);
    }
    else
    {
        // No MAC address given: this node's own interface is meant.
        Locator normalized = locator;
        EthernetLocator::set_mac(normalized, local_mac_);
        list.push_back(normalized);
    }

    return list;
}

void TSNTransport::select_locators(
        LocatorSelector& selector) const
{
    fastdds::ResourceLimitedVector<LocatorSelectorEntry*>& entries = selector.transport_starts();

    for (size_t i = 0; i < entries.size(); ++i)
    {
        LocatorSelectorEntry* entry = entries[i];
        if (!entry->transport_should_process)
        {
            continue;
        }

        bool selected = false;

        // A multicast destination reaches every listener of the stream in one
        // frame, which is exactly what a TSN stream is provisioned for, so
        // prefer it whenever the remote offers one.
        for (size_t j = 0; j < entry->multicast.size() && !selected; ++j)
        {
            if (IsLocatorSupported(entry->multicast[j]))
            {
                entry->state.multicast.push_back(j);
                selected = true;
            }
        }

        if (!selected)
        {
            for (size_t j = 0; j < entry->unicast.size(); ++j)
            {
                if (IsLocatorSupported(entry->unicast[j]) && !selector.is_selected(entry->unicast[j]))
                {
                    entry->state.unicast.push_back(j);
                    selected = true;
                }
            }
        }

        if (selected)
        {
            selector.select(i);
        }
    }
}

void TSNTransport::AddDefaultOutputLocator(
        LocatorList& defaultList)
{
    defaultList.push_back(EthernetLocator::create_locator(default_multicast_mac_,
            configuration_.default_vlan_id, configuration_.default_pcp, 0));
}

bool TSNTransport::getDefaultMetatrafficMulticastLocators(
        LocatorList& locators,
        uint32_t metatraffic_multicast_port) const
{
    // Subclause A.6.1.4.1 of [DDS-TSN]: every participant announces and listens
    // on this address so that plug-and-play discovery works.
    locators.push_back(EthernetLocator::create_locator(default_multicast_mac_,
            configuration_.default_vlan_id, configuration_.default_pcp,
            static_cast<uint16_t>(metatraffic_multicast_port)));
    return true;
}

bool TSNTransport::getDefaultMetatrafficUnicastLocators(
        LocatorList& locators,
        uint32_t metatraffic_unicast_port) const
{
    locators.push_back(EthernetLocator::create_locator(local_mac_,
            configuration_.default_vlan_id, configuration_.default_pcp,
            static_cast<uint16_t>(metatraffic_unicast_port)));
    return true;
}

bool TSNTransport::getDefaultUnicastLocators(
        LocatorList& locators,
        uint32_t unicast_port) const
{
    locators.push_back(EthernetLocator::create_locator(local_mac_,
            configuration_.default_vlan_id, configuration_.default_pcp,
            static_cast<uint16_t>(unicast_port)));
    return true;
}

bool TSNTransport::fillMetatrafficMulticastLocator(
        Locator& locator,
        uint32_t metatraffic_multicast_port) const
{
    if (0 == EthernetLocator::logical_port(locator))
    {
        EthernetLocator::set_logical_port(locator, static_cast<uint16_t>(metatraffic_multicast_port));
    }
    if (!IsAddressDefined(locator))
    {
        EthernetLocator::set_mac(locator, default_multicast_mac_);
    }
    return true;
}

bool TSNTransport::fillMetatrafficUnicastLocator(
        Locator& locator,
        uint32_t metatraffic_unicast_port) const
{
    if (0 == EthernetLocator::logical_port(locator))
    {
        EthernetLocator::set_logical_port(locator, static_cast<uint16_t>(metatraffic_unicast_port));
    }
    if (!IsAddressDefined(locator))
    {
        EthernetLocator::set_mac(locator, local_mac_);
    }
    return true;
}

bool TSNTransport::fillUnicastLocator(
        Locator& locator,
        uint32_t well_known_port) const
{
    if (0 == EthernetLocator::logical_port(locator))
    {
        EthernetLocator::set_logical_port(locator, static_cast<uint16_t>(well_known_port));
    }
    return true;
}

bool TSNTransport::configureInitialPeerLocator(
        Locator& locator,
        const PortParameters& port_params,
        uint32_t domainId,
        LocatorList& list) const
{
    if (0 == EthernetLocator::logical_port(locator))
    {
        // A peer given without a logical port stands for the whole range of
        // participants on that node, as in the UDP/IP PSM.
        for (uint32_t i = 0; i < configuration_.maxInitialPeersRange; ++i)
        {
            Locator peer = locator;
            EthernetLocator::set_logical_port(peer,
                    static_cast<uint16_t>(port_params.getUnicastPort(domainId, i)));
            list.push_back(peer);
        }
    }
    else
    {
        list.push_back(locator);
    }

    return true;
}

} // namespace rtps
} // namespace fastdds
} // namespace eprosima
