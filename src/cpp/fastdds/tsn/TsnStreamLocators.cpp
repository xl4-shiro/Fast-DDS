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
 * @file TsnStreamLocators.cpp
 */

#include <fastdds/dds/tsn/TsnStreamLocators.hpp>

#include <fastdds/dds/log/Log.hpp>
#include <fastdds/utils/EthernetLocator.hpp>

#include <rtps/transport/tsn/TsnCncConfig.hpp>

namespace eprosima {
namespace fastdds {
namespace dds {
namespace tsn {

using rtps::EthernetLocator;
using rtps::Locator_t;
using rtps::TSNTransportDescriptor;
using rtps::tsn::TsnCncConfig;
using rtps::tsn::TsnStream;

//! Turn a CNC stream into the locator a DDS endpoint should carry.
static bool to_binding(
        const TsnStream& stream,
        uint16_t logical_port,
        TsnStreamBinding& out)
{
    out.has_destination = stream.has_data_frame_specification;
    out.interface_name = stream.interface_name;
    out.topic_name = stream.station_name;
    out.stream_id = rtps::tsn::stream_id_to_string(stream.stream_id);
    out.accepted = stream.accepted;
    out.max_frame_size = stream.max_frame_size;
    out.max_frames_per_interval = stream.max_frames_per_interval;
    if (!stream.has_data_frame_specification)
    {
        // Describable but not usable: the caller decides whether that matters.
        return true;
    }

    out.locator = EthernetLocator::create_locator(stream.destination_mac, stream.vlan_id,
                    stream.pcp, logical_port);

    return rtps::IsLocatorValid(out.locator);
}

static std::vector<TsnStreamBinding> collect(
        const TSNTransportDescriptor& descriptor,
        uint16_t logical_port,
        bool talker)
{
    std::vector<TsnStreamBinding> bindings;

    std::unique_ptr<TsnCncConfig> config = TsnCncConfig::create(descriptor);
    if (!config)
    {
        return bindings;
    }

    const std::vector<TsnStream> streams = talker ? config->talkers() : config->listeners();
    for (const TsnStream& stream : streams)
    {
        TsnStreamBinding binding;
        if (to_binding(stream, logical_port, binding))
        {
            bindings.push_back(std::move(binding));
        }
    }

    return bindings;
}

std::vector<TsnStreamBinding> TsnStreamLocators::talker_streams(
        const TSNTransportDescriptor& descriptor,
        uint16_t logical_port)
{
    return collect(descriptor, logical_port, true);
}

std::vector<TsnStreamBinding> TsnStreamLocators::listener_streams(
        const TSNTransportDescriptor& descriptor,
        uint16_t logical_port)
{
    return collect(descriptor, logical_port, false);
}

bool TsnStreamLocators::find_stream_for_topic(
        const TSNTransportDescriptor& descriptor,
        const std::string& topic_name,
        bool talker,
        uint16_t logical_port,
        TsnStreamBinding& out)
{
    std::unique_ptr<TsnCncConfig> config = TsnCncConfig::create(descriptor);
    if (!config)
    {
        return false;
    }

    TsnStream stream;
    if (!config->find_by_station_name(topic_name, talker, stream))
    {
        EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "No " << (talker ? "talker" : "listener")
                                                  << " stream is named '" << topic_name
                                                  << "' in the CNC configuration for cuc-id '"
                                                  << descriptor.cuc_id << "'");
        return false;
    }

    if (!to_binding(stream, logical_port, out) || !out.has_destination)
    {
        EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "Stream '" << stream.station_name
                                                       << "' has no data-frame-specification; the CNC has not "
                                                       << "assigned it a destination MAC address yet");
        return false;
    }
    return true;
}

} // namespace tsn
} // namespace dds
} // namespace fastdds
} // namespace eprosima
