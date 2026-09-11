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
 * @file TSNChannelResource.hpp
 */

#ifndef FASTDDS_RTPS_TRANSPORT_TSN__TSNCHANNELRESOURCE_HPP
#define FASTDDS_RTPS_TRANSPORT_TSN__TSNCHANNELRESOURCE_HPP

#include <map>
#include <memory>
#include <mutex>

#include <fastdds/rtps/attributes/ThreadSettings.hpp>
#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/transport/TransportReceiverInterface.hpp>

#include <rtps/transport/ChannelResource.h>
#include <rtps/transport/tsn/AvtpStream.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

/**
 * One AVTP listener socket and the thread draining it.
 *
 * A raw socket can only filter on the destination MAC address, so a single
 * channel serves every RTPS logical port that shares a MAC address and VLAN
 * tag --- which, for a participant, is all of its metatraffic and user
 * unicast traffic. The logical port travels in the ACF header (see
 * @ref TsnRtpsHeader) and this class uses it to pick the receiver.
 */
class TSNChannelResource : public ChannelResource
{
public:

    /**
     * Open the listener socket and start its reception thread.
     *
     * @param stream_config  Everything the socket needs. Only the interface,
     *                       destination MAC and timeouts are used; a listener
     *                       accepts any stream ID reaching that MAC.
     * @param max_msg_size   Size of the reception buffer.
     * @param thread_config  Settings for the reception thread.
     *
     * @return nullptr when the socket cannot be opened.
     */
    static std::unique_ptr<TSNChannelResource> create(
            const AvtpStreamConfig& stream_config,
            uint32_t max_msg_size,
            const ThreadSettings& thread_config);

    ~TSNChannelResource() override;

    /**
     * Route messages addressed to @c locator's logical port to @c receiver.
     *
     * @return false when another receiver already holds that logical port.
     */
    bool add_receiver(
            const Locator& locator,
            TransportReceiverInterface* receiver);

    /**
     * Stop routing messages addressed to @c locator's logical port.
     *
     * @return the number of receivers still registered.
     */
    size_t remove_receiver(
            const Locator& locator);

    //! Whether a receiver is registered for @c locator's logical port.
    bool has_receiver_for(
            const Locator& locator) const;

    void disable() override;

private:

    TSNChannelResource(
            uint32_t max_msg_size);

    void perform_listen_operation();

    struct ReceiverEntry
    {
        TransportReceiverInterface* receiver = nullptr;
        Locator locator;
    };

    std::unique_ptr<AvtpStream> stream_;

    mutable std::mutex receivers_mutex_;
    //! Receivers indexed by the RTPS logical port they listen on.
    std::map<uint16_t, ReceiverEntry> receivers_;

    //! VLAN ID this channel was opened for, used to build the remote locator.
    uint16_t vlan_id_ = 0;
};

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_RTPS_TRANSPORT_TSN__TSNCHANNELRESOURCE_HPP
