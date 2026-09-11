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
 * @file TSNSenderResource.hpp
 */

#ifndef FASTDDS_RTPS_TRANSPORT_TSN__TSNSENDERRESOURCE_HPP
#define FASTDDS_RTPS_TRANSPORT_TSN__TSNSENDERRESOURCE_HPP

#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/transport/SenderResource.hpp>

#include <rtps/transport/ChainingSenderResource.hpp>
#include <rtps/transport/tsn/TSNTransport.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

/**
 * Send side of the TSN transport.
 *
 * The AVTP talkers are owned by @ref TSNTransport rather than by this resource,
 * because a talker is a TSN Stream: its sequence numbering and its shaper must
 * be shared by everything that sends on it, no matter which sender resource the
 * message came through.
 */
class TSNSenderResource : public SenderResource
{
public:

    explicit TSNSenderResource(
            TSNTransport& transport)
        : SenderResource(transport.kind())
        , transport_(transport)
    {
        send_buffers_lambda_ = [this](
            const std::vector<NetworkBuffer>& buffers,
            uint32_t total_bytes,
            LocatorsIterator* destination_locators_begin,
            LocatorsIterator* destination_locators_end,
            const std::chrono::steady_clock::time_point& max_blocking_time_point) -> bool
                {
                    // AVTP sends are non-blocking once the shaper lets the frame
                    // through, so there is nothing useful to do with the deadline.
                    static_cast<void>(max_blocking_time_point);
                    return transport_.send(buffers, total_bytes, destination_locators_begin,
                                   destination_locators_end);
                };
    }

    ~TSNSenderResource() override
    {
        if (clean_up)
        {
            clean_up();
        }
    }

    void add_locators_to_list(
            LocatorList& locators) const override
    {
        locators.push_back(transport_.local_locator());
    }

    static TSNSenderResource* cast(
            TransportInterface& transport,
            SenderResource* sender_resource)
    {
        TSNSenderResource* returned_resource = nullptr;

        if (sender_resource->kind() == transport.kind())
        {
            returned_resource = dynamic_cast<TSNSenderResource*>(sender_resource);

            //! May be chained
            if (nullptr == returned_resource)
            {
                auto chaining_sender = dynamic_cast<ChainingSenderResource*>(sender_resource);
                if (nullptr != chaining_sender)
                {
                    returned_resource = dynamic_cast<TSNSenderResource*>(chaining_sender->lower_sender_cast());
                }
            }
        }

        return returned_resource;
    }

private:

    TSNSenderResource() = delete;
    TSNSenderResource(
            const TSNSenderResource&) = delete;
    TSNSenderResource& operator =(
            const TSNSenderResource&) = delete;

    TSNTransport& transport_;
};

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_RTPS_TRANSPORT_TSN__TSNSENDERRESOURCE_HPP
