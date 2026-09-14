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
 * @file TsnCncConfig.hpp
 */

#ifndef FASTDDS_RTPS_TRANSPORT_TSN__TSNCNCCONFIG_HPP
#define FASTDDS_RTPS_TRANSPORT_TSN__TSNCNCCONFIG_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fastdds/rtps/transport/TSNTransportDescriptor.hpp>
#include <fastdds/utils/EthernetLocator.hpp>

#include <rtps/transport/tsn/AvtpStream.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

//! State reported back to the CNC for one end-station interface.
enum class EndStationStatus : uint8_t
{
    initial = 0,
    connected = 1,
    disconnected = 2,
    deleted = 3,
    operation_failed = 4
};

/**
 * One TSN Stream as the CNC has provisioned it, flattened from the
 * @c ieee802-dot1q-cnc-config datastore.
 *
 * Talker entries carry the full data-frame-specification and traffic
 * specification. Listener entries only identify the stream to receive; the
 * destination MAC and VLAN tag of a listener come from the talker entry of the
 * same stream, which is where the CNC writes them.
 */
struct TsnStream
{
    //! IEEE 1722 / 802.1Qcc stream ID.
    StreamId stream_id{};
    //! @c station-name of the end-station interface. Bound to a DDS topic name.
    std::string station_name;
    //! @c interface-name of the end-station interface, e.g. "eth0".
    std::string interface_name;
    //! @c mac-address of the end-station interface, i.e. this node's MAC.
    MacAddress station_mac{};
    //! Destination MAC of the stream's data-frame-specification.
    MacAddress destination_mac{};
    //! Source MAC of the stream's data-frame-specification.
    MacAddress source_mac{};
    //! VLAN ID of the stream's data-frame-specification.
    uint16_t vlan_id = 0;
    //! Priority Code Point of the stream's data-frame-specification.
    uint8_t pcp = 0;
    //! Numerator of the transmission interval, in seconds.
    uint32_t interval_numerator = 0;
    //! Denominator of the transmission interval.
    uint32_t interval_denominator = 0;
    //! Frames the talker may send per interval.
    uint16_t max_frames_per_interval = 0;
    //! Largest frame the talker may send, octets.
    uint16_t max_frame_size = 0;
    //! Transmission selection algorithm: 0 strict priority, 1 CBS, 2 ETS, 3 ATS.
    uint8_t transmission_selection = 0;
    //! Stream rank. 1 for a DDS-TSN talker, per subclause 7.3.3 of [DDS-TSN].
    uint8_t rank = 0;
    //! Whether the CNC has accepted this end-station interface.
    bool accepted = false;
    //! Whether this entry came from the talker list or the listener list.
    bool talker = false;
    //! Index of the listener within the stream. Always 0 for a talker.
    uint32_t listener_index = 0;
    //! Whether a data-frame-specification was found for the stream.
    bool has_data_frame_specification = false;

    /**
     * Rate for the software credit-based shaper, in kilobytes per second.
     *
     * Derived from the traffic specification as
     * @c max_frames_per_interval * @c max_frame_size / interval. Returns 0 when
     * the traffic specification is incomplete, which disables the shaper.
     */
    uint32_t shaper_rate_kbps() const;
};

/**
 * Read-side binding to the @c ieee802-dot1q-cnc-config datastore held by uniconf.
 *
 * The CUC writes Talker and Listener groups into this datastore and the CNC
 * answers with the stream configuration and an @c accept flag. This class is
 * the endpoint's half of that exchange: it reads back what the CNC decided and
 * reports the interface status, so the transport can open its AVTP streams with
 * the destination MAC, VLAN tag and PCP the network actually expects.
 */
class TsnCncConfig
{
public:

    /**
     * Attach to the datastore described by @c descriptor.
     *
     * When @c descriptor.uniconf_db_name is empty the process-wide uniconf
     * handle must already exist; otherwise the database is opened here and
     * closed on destruction.
     *
     * @return nullptr when uniconf is unavailable.
     */
    static std::unique_ptr<TsnCncConfig> create(
            const TSNTransportDescriptor& descriptor);

    ~TsnCncConfig();

    TsnCncConfig(
            const TsnCncConfig&) = delete;
    TsnCncConfig& operator =(
            const TsnCncConfig&) = delete;

    /**
     * Re-read every talker and listener entry belonging to this node.
     *
     * Entries naming an interface other than the descriptor's are skipped.
     *
     * @return true when the datastore could be read, even if it held no streams.
     */
    bool refresh();

    /**
     * Block until every stream configured for this node is accepted by the CNC.
     *
     * @param timeout_ms  Give up after this long. 0 waits indefinitely.
     * @param require_streams  When true, keep waiting even if the datastore
     *                         holds no entry for this node yet --- the CUC may
     *                         not have written it. When false, return as soon as
     *                         it is clear there is nothing to wait for, so an
     *                         unconfigured node does not stall startup.
     * @return true when all streams are accepted, false on timeout or when
     * there is nothing to wait for. The caller decides what a false means:
     * fall back to the defaults, or refuse to start.
     */
    bool wait_for_accepted_streams(
            uint32_t timeout_ms,
            bool require_streams = false);

    //! Snapshot of the talker streams belonging to this node.
    std::vector<TsnStream> talkers() const;

    //! Snapshot of the listener streams belonging to this node.
    std::vector<TsnStream> listeners() const;

    /**
     * Find the stream whose destination MAC and VLAN ID match a locator.
     *
     * This is how traffic gets its stream ID, framing and shaping: the locator
     * is what the transport has, and the CNC keyed the stream on the same MAC
     * and VLAN.
     *
     * Both lists are searched. A node that only listens to a stream has no
     * talker entry for it --- the talker entry names another node's interface
     * and is filtered out when reading --- but its listener entry carries the
     * same data-frame-specification, read from the talker under the same
     * stream id.
     *
     * @param vlan_id  VLAN ID to match, or 0 to match on MAC address alone.
     * @return false when no stream matches.
     */
    bool find_stream_for_destination(
            const MacAddress& destination_mac,
            uint16_t vlan_id,
            TsnStream& out) const;

    //! Find a stream by the @c station-name the CUC gave it.
    bool find_by_station_name(
            const std::string& station_name,
            bool talker,
            TsnStream& out) const;

    //! Report the state of one end-station interface back to the CNC.
    bool report_status(
            const TsnStream& stream,
            EndStationStatus status);

    //! Report @c status for every stream belonging to this node.
    void report_status_for_all(
            EndStationStatus status);

private:

    TsnCncConfig() = default;

    //! Read one side of the configuration. Caller holds @ref mutex_.
    bool read_end_stations(
            bool talker,
            std::vector<TsnStream>& out);

    //! Fill the data-frame and traffic specification of @c stream from the talker list.
    void read_stream_info(
            TsnStream& stream);

    std::string interface_name_;
    std::string cuc_id_;
    uint8_t instance_index_ = 0;

    //! Non-null only when this instance opened the database itself. A uniconf uc_dbald.
    void* owned_db_ = nullptr;

    mutable std::mutex mutex_;
    std::vector<TsnStream> talkers_;
    std::vector<TsnStream> listeners_;
};

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_RTPS_TRANSPORT_TSN__TSNCNCCONFIG_HPP
