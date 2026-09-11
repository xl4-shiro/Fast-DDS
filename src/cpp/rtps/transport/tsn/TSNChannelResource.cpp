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
 * @file TSNChannelResource.cpp
 */

#include <rtps/transport/tsn/TSNChannelResource.hpp>

#include <fastdds/dds/log/Log.hpp>
#include <fastdds/utils/EthernetLocator.hpp>

#include <utils/threading.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

TSNChannelResource::TSNChannelResource(
        uint32_t max_msg_size)
    : ChannelResource(max_msg_size)
{
}

std::unique_ptr<TSNChannelResource> TSNChannelResource::create(
        const AvtpStreamConfig& stream_config,
        uint32_t max_msg_size,
        const ThreadSettings& thread_config)
{
    std::unique_ptr<TSNChannelResource> channel(new TSNChannelResource(max_msg_size));
    channel->vlan_id_ = stream_config.vlan_id;
    channel->stream_ = AvtpStream::open_listener(stream_config);
    if (!channel->stream_)
    {
        return nullptr;
    }

    TSNChannelResource* raw = channel.get();
    auto listen = [raw]()
            {
                raw->perform_listen_operation();
            };
    // Name the thread after the last two octets of the MAC it listens on, which
    // is what distinguishes one channel from another on the same interface.
    const uint32_t tag = (static_cast<uint32_t>(stream_config.destination_mac[4]) << 8) |
            stream_config.destination_mac[5];
    channel->thread(create_thread(listen, thread_config, "dds.tsn.%u", tag));

    return channel;
}

TSNChannelResource::~TSNChannelResource()
{
    disable();
    // ChannelResource's destructor joins the thread, but the thread can only
    // finish once the stream stops blocking in receive(), so tear the stream
    // down after the join rather than before.
    if (thread_.joinable())
    {
        thread_.join();
    }
    stream_.reset();
}

void TSNChannelResource::disable()
{
    ChannelResource::disable();
    if (stream_)
    {
        stream_->disable();
    }
}

bool TSNChannelResource::add_receiver(
        const Locator& locator,
        TransportReceiverInterface* receiver)
{
    const uint16_t logical_port = EthernetLocator::logical_port(locator);

    std::lock_guard<std::mutex> guard(receivers_mutex_);
    auto it = receivers_.find(logical_port);
    if (it != receivers_.end())
    {
        return it->second.receiver == receiver;
    }

    ReceiverEntry entry;
    entry.receiver = receiver;
    entry.locator = locator;
    receivers_.emplace(logical_port, entry);
    return true;
}

size_t TSNChannelResource::remove_receiver(
        const Locator& locator)
{
    std::lock_guard<std::mutex> guard(receivers_mutex_);
    receivers_.erase(EthernetLocator::logical_port(locator));
    return receivers_.size();
}

bool TSNChannelResource::has_receiver_for(
        const Locator& locator) const
{
    std::lock_guard<std::mutex> guard(receivers_mutex_);
    return receivers_.count(EthernetLocator::logical_port(locator)) != 0;
}

void TSNChannelResource::perform_listen_operation()
{
    AvtpReceivedMessage message;

    while (alive())
    {
        CDRMessage_t& msg = message_buffer();
        if (!stream_->receive(msg.buffer, msg.max_size, message))
        {
            // Either the receive timed out or the frame was not for us.
            continue;
        }
        msg.length = message.rtps_length;

        TransportReceiverInterface* receiver = nullptr;
        Locator input_locator;
        {
            std::lock_guard<std::mutex> guard(receivers_mutex_);
            auto it = receivers_.find(message.destination_logical_port);
            if (it != receivers_.end())
            {
                receiver = it->second.receiver;
                input_locator = it->second.locator;
            }
        }

        if (nullptr == receiver)
        {
            // Another participant on this MAC address; not an error.
            EPROSIMA_LOG_INFO(TSN_TRANSPORT, "No receiver for logical port "
                    << message.destination_logical_port << " on this channel");
            continue;
        }

        // Subclause 7.3.3 of [DDS-TSN] puts the talker's MAC address in the
        // leading six octets of the stream id, and that is the only place the
        // sender's address appears once the raw socket has handed us the frame.
        MacAddress remote_mac;
        memcpy(remote_mac.data(), message.stream_id.data(), 6);
        const Locator remote_locator = EthernetLocator::create_locator(
            remote_mac, message.vlan_id, EthernetLocator::pcp(input_locator),
            message.source_logical_port);

        receiver->OnDataReceived(msg.buffer, msg.length, input_locator, remote_locator);
    }

    std::lock_guard<std::mutex> guard(receivers_mutex_);
    receivers_.clear();
}

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima
