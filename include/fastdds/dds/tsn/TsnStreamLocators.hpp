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
 * @file TsnStreamLocators.hpp
 *
 * Binding between DDS endpoints and the TSN Streams the CNC has provisioned.
 *
 * @note Only available when Fast DDS is built with @c -DTSN_TRANSPORT=ON.
 */

#ifndef FASTDDS_DDS_TSN__TSNSTREAMLOCATORS_HPP
#define FASTDDS_DDS_TSN__TSNSTREAMLOCATORS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <fastdds/fastdds_dll.hpp>
#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/transport/TSNTransportDescriptor.hpp>

namespace eprosima {
namespace fastdds {
namespace dds {
namespace tsn {

//! One TSN Stream, as far as a DDS endpoint needs to know about it.
struct TsnStreamBinding
{
    //! @c station-name of the end-station interface: the DDS topic name.
    std::string topic_name;
    //! Stream ID formatted as "aa:bb:cc:dd:ee:ff:gg:hh".
    std::string stream_id;
    //! Locator a DataWriter sends to, or a DataReader listens on.
    rtps::Locator_t locator;
    //! Whether the CNC accepted this end-station interface.
    bool accepted = false;
    /**
     * Whether the stream has a data-frame-specification, and therefore whether
     * @ref locator is usable. False means the CNC has not assigned the stream a
     * destination MAC address yet.
     */
    bool has_destination = false;
    //! @c interface-name of the end-station interface this entry belongs to.
    std::string interface_name;
    //! Largest frame the CNC granted, octets. 0 when the CNC did not say.
    uint16_t max_frame_size = 0;
    //! Frames per interval the CNC granted. 0 when the CNC did not say.
    uint16_t max_frames_per_interval = 0;
};

/**
 * Look up the TSN Streams a node's DDS endpoints should use.
 *
 * Subclause 7.3.3 of [DDS-TSN] maps a @c TsnTalker onto a Talker Group whose
 * end-station interface identifies the DataWriter. This class reads that
 * mapping back out of the @c ieee802-dot1q-cnc-config datastore, using the
 * @c station-name of the end-station interface as the DDS topic name, and turns
 * the stream's data-frame-specification into the Ethernet locator to put on the
 * endpoint's QoS.
 *
 * Without this step a DataWriter would use the participant's own locators and
 * its traffic would not be distinguishable, at the network, from any other
 * topic's --- so it could not be scheduled as its own TSN Stream.
 *
 * @ingroup TRANSPORT_MODULE
 */
class TsnStreamLocators
{
public:

    /**
     * Read the streams this node talks on, keyed by topic name.
     *
     * @param descriptor     Names the interface, the CUC and the datastore. The
     *                       same descriptor the transport was built from.
     * @param logical_port   RTPS logical port to place in the locators. Use the
     *                       participant's user multicast port, or 0 to let Fast
     *                       DDS fill it in.
     *
     * Every entry provisioned for this node is returned, including incomplete
     * ones --- no @c station-name, or no destination MAC yet. That is
     * deliberate: a listing that hid them would be silent in exactly the cases
     * worth diagnosing. Check @ref TsnStreamBinding::has_destination and
     * @ref TsnStreamBinding::topic_name before using an entry.
     *
     * @return The talker streams found. Empty when uniconf is unavailable or
     * the CNC has provisioned nothing for this node.
     */
    FASTDDS_EXPORTED_API static std::vector<TsnStreamBinding> talker_streams(
            const rtps::TSNTransportDescriptor& descriptor,
            uint16_t logical_port = 0);

    //! Read the streams this node listens to, keyed by topic name.
    FASTDDS_EXPORTED_API static std::vector<TsnStreamBinding> listener_streams(
            const rtps::TSNTransportDescriptor& descriptor,
            uint16_t logical_port = 0);

    /**
     * Find the stream provisioned for one topic.
     *
     * @param descriptor    As in @ref talker_streams.
     * @param topic_name    Matched against the stream's @c station-name.
     * @param talker        Search the talker list rather than the listener list.
     * @param logical_port  RTPS logical port to place in the locator.
     * @param [out] out     The binding, untouched unless this returns true.
     *
     * @return false when no stream carries that station name.
     */
    FASTDDS_EXPORTED_API static bool find_stream_for_topic(
            const rtps::TSNTransportDescriptor& descriptor,
            const std::string& topic_name,
            bool talker,
            uint16_t logical_port,
            TsnStreamBinding& out);
};

} // namespace tsn
} // namespace dds
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_DDS_TSN__TSNSTREAMLOCATORS_HPP
