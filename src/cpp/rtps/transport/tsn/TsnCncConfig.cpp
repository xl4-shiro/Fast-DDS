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
 * @file TsnCncConfig.cpp
 */

#include <rtps/transport/tsn/TsnCncConfig.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

#include <fastdds/dds/log/Log.hpp>

#include <rtps/transport/tsn/Xl4Runtime.hpp>

extern "C" {
#include <xl4unibase/unibase.h>
#include <xl4uniconf/uc_dbal.h>
#include <xl4uniconf/yangs/yang_db_access.h>
#include <xl4uniconf/yangs/ieee802-dot1q-cnc-config_access.h>
} // extern "C"

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

/**
 * The uniconf access handle, or nullptr when uniconf is not usable.
 *
 * ydbi_access_handle() returns the address of a process-wide struct whether or
 * not ydbi_access_init() has run, so a non-null return proves nothing. The
 * database pointer inside it is what says the datastore is actually open;
 * without that check, the first query dereferences null inside uniconf.
 */
static yang_db_item_access_t* uniconf_handle()
{
    yang_db_item_access_t* ydbia = ydbi_access_handle();
    return (nullptr != ydbia && nullptr != ydbia->dbald) ? ydbia : nullptr;
}

/**
 * Releases the uniconf read transaction when it goes out of scope.
 *
 * Every read into uniconf implicitly opens a transaction and holds the
 * database's semaphore until it is released. Nothing releases it on the caller's
 * behalf: uc_get_range() acquires the transaction and, when the range it was
 * asked for matches nothing, returns NULL without giving it back, and the
 * iterator only releases a range it actually got. A scan that finds no entries
 * for this node --- the normal case before the CUC has provisioned anything ---
 * therefore leaves the transaction open, and the next process to touch the same
 * database blocks in sem_timedwait() until it gives up.
 *
 * So the transaction is released here, at the end of every read or write burst.
 */
class DbTransactionGuard
{
public:

    explicit DbTransactionGuard(
            yang_db_item_access_t* ydbia)
        : ydbia_(ydbia)
    {
    }

    ~DbTransactionGuard()
    {
        if (nullptr != ydbia_ && nullptr != ydbia_->dbald)
        {
            uc_dbal_releasedb(ydbia_->dbald);
        }
    }

    DbTransactionGuard(
            const DbTransactionGuard&) = delete;
    DbTransactionGuard& operator =(
            const DbTransactionGuard&) = delete;

private:

    yang_db_item_access_t* ydbia_ = nullptr;
};

uint32_t TsnStream::shaper_rate_kbps() const
{
    if (0 == interval_numerator || 0 == interval_denominator ||
            0 == max_frames_per_interval || 0 == max_frame_size)
    {
        return 0;
    }

    // interval, in seconds, is numerator / denominator. The octets allowed in
    // one interval divided by its length gives octets per second; avtpcon wants
    // kilobytes per second.
    const uint64_t octets_per_interval =
            static_cast<uint64_t>(max_frames_per_interval) * static_cast<uint64_t>(max_frame_size);
    const uint64_t octets_per_second =
            octets_per_interval * static_cast<uint64_t>(interval_denominator) /
            static_cast<uint64_t>(interval_numerator);

    // Round up, so the shaper never sits just below the CNC-granted rate.
    const uint64_t kbps = (octets_per_second + 999u) / 1000u;
    return static_cast<uint32_t>(std::min<uint64_t>(kbps, UINT32_MAX));
}

std::unique_ptr<TsnCncConfig> TsnCncConfig::create(
        const TSNTransportDescriptor& descriptor)
{
    init_xl4_runtime();

    std::unique_ptr<TsnCncConfig> config(new TsnCncConfig());
    config->interface_name_ = descriptor.interface_name;
    config->cuc_id_ = descriptor.cuc_id;
    config->instance_index_ = descriptor.cnc_instance_index;

    if (!descriptor.uniconf_db_name.empty())
    {
        if (nullptr != uniconf_handle())
        {
            EPROSIMA_LOG_INFO(TSN_TRANSPORT,
                    "uniconf is already initialised; ignoring uniconf_db_name '"
                    << descriptor.uniconf_db_name << "'");
        }
        else
        {
            config->owned_db_ = uc_dbal_open(descriptor.uniconf_db_name.c_str(), "w", 0);
            if (nullptr == config->owned_db_)
            {
                EPROSIMA_LOG_ERROR(TSN_TRANSPORT,
                        "Cannot open the uniconf database '" << descriptor.uniconf_db_name << "'");
                return nullptr;
            }
            ydbi_access_init(config->owned_db_, nullptr);
        }
    }

    if (nullptr == uniconf_handle())
    {
        if (descriptor.uniconf_db_name.empty())
        {
            // Running without a CNC is a supported configuration: everything
            // goes out on the descriptor's default VLAN and PCP, unscheduled.
            EPROSIMA_LOG_WARNING(TSN_TRANSPORT,
                    "No CNC configuration: uniconf is not initialised and no uniconf_db_name is set. "
                    "Streams will use the transport descriptor's default VLAN and PCP.");
        }
        else
        {
            EPROSIMA_LOG_ERROR(TSN_TRANSPORT,
                    "uniconf did not initialise from '" << descriptor.uniconf_db_name << "'");
        }
        return nullptr;
    }

    if (!config->refresh())
    {
        EPROSIMA_LOG_WARNING(TSN_TRANSPORT, "Cannot read the ieee802-dot1q-cnc-config datastore");
    }

    return config;
}

TsnCncConfig::~TsnCncConfig()
{
    if (nullptr != owned_db_)
    {
        report_status_for_all(EndStationStatus::disconnected);
        uc_dbal_close(owned_db_, 0);
        ydbi_access_close();
        owned_db_ = nullptr;
    }
}

bool TsnCncConfig::read_end_stations(
        bool talker,
        std::vector<TsnStream>& out)
{
    yang_db_item_access_t* ydbia = uniconf_handle();
    if (nullptr == ydbia)
    {
        return false;
    }

    out.clear();

    // Two passes: the accept flag and the station name live under different
    // leaves of the same list, and each iterator reports only one of them.
    uc_range* range = nullptr;
    cc_endstation_info_t entry;
    while (true)
    {
        memset(&entry, 0, sizeof(entry));
        const int result = talker ?
                ydbi_iterate_talkers_cc(ydbia, &range, instance_index_, cuc_id_.c_str(), &entry) :
                ydbi_iterate_listeners_cc(ydbia, &range, instance_index_, cuc_id_.c_str(), &entry);
        if (0 != result)
        {
            break;
        }

        // Every pointer in `entry` aims into the database's own buffers and is
        // invalidated as soon as the iterator moves on, so copy right away.
        if (nullptr == entry.interface_name || interface_name_ != entry.interface_name)
        {
            continue;
        }

        TsnStream stream;
        stream.talker = talker;
        stream.interface_name = entry.interface_name;
        stream.accepted = (0 != entry.accept);
        stream.listener_index = entry.lindex;
        if (nullptr != entry.streamid)
        {
            memcpy(stream.stream_id.data(), entry.streamid, 8);
        }
        if (nullptr != entry.mac_address)
        {
            EthernetLocator::string_to_mac(entry.mac_address, stream.station_mac);
        }
        out.push_back(std::move(stream));
    }

    // Second pass for the station names, matched back by stream id.
    range = nullptr;
    while (true)
    {
        memset(&entry, 0, sizeof(entry));
        const int result = talker ?
                ydbi_iterate_talker_names_cc(ydbia, &range, instance_index_, cuc_id_.c_str(), &entry) :
                ydbi_iterate_listener_names_cc(ydbia, &range, instance_index_, cuc_id_.c_str(), &entry);
        if (0 != result)
        {
            break;
        }
        if (nullptr == entry.streamid || nullptr == entry.station_name)
        {
            continue;
        }

        StreamId stream_id;
        memcpy(stream_id.data(), entry.streamid, 8);
        const std::string station_name = entry.station_name;
        const uint32_t listener_index = entry.lindex;

        for (TsnStream& stream : out)
        {
            if (stream.stream_id == stream_id &&
                    (talker || stream.listener_index == listener_index))
            {
                stream.station_name = station_name;
                break;
            }
        }
    }

    return true;
}

void TsnCncConfig::read_stream_info(
        TsnStream& stream)
{
    yang_db_item_access_t* ydbia = uniconf_handle();
    if (nullptr == ydbia)
    {
        return;
    }

    cc_stream_info_t info;
    memset(&info, 0, sizeof(info));

    // The data-frame-specification and traffic-specification live under the
    // talker, whichever side of the stream we are on.
    if (0 != ydbi_get_streaminfo_cc(ydbia, instance_index_, cuc_id_.c_str(),
            stream.stream_id.data(), &info, 0))
    {
        return;
    }

    stream.rank = info.rank;
    stream.pcp = info.dfinfo.pcp;
    stream.vlan_id = info.dfinfo.vlanid;
    stream.interval_numerator = info.tsinfo.interval_numer;
    stream.interval_denominator = info.tsinfo.interval_denom;
    stream.max_frames_per_interval = info.tsinfo.max_frame_interval;
    stream.max_frame_size = info.tsinfo.max_frame_size;
    stream.transmission_selection = info.tsinfo.transmission_selection;

    if ('\0' != info.dfinfo.dest_mac_address[0] &&
            EthernetLocator::string_to_mac(info.dfinfo.dest_mac_address, stream.destination_mac))
    {
        stream.has_data_frame_specification = true;
    }
    if ('\0' != info.dfinfo.src_mac_address[0])
    {
        EthernetLocator::string_to_mac(info.dfinfo.src_mac_address, stream.source_mac);
    }
}

bool TsnCncConfig::refresh()
{
    yang_db_item_access_t* ydbia = uniconf_handle();
    if (nullptr == ydbia)
    {
        return false;
    }
    DbTransactionGuard release_on_exit(ydbia);

    std::vector<TsnStream> talkers;
    std::vector<TsnStream> listeners;

    if (!read_end_stations(true, talkers) || !read_end_stations(false, listeners))
    {
        return false;
    }

    for (TsnStream& stream : talkers)
    {
        read_stream_info(stream);
    }
    for (TsnStream& stream : listeners)
    {
        read_stream_info(stream);
    }

    std::lock_guard<std::mutex> guard(mutex_);
    talkers_ = std::move(talkers);
    listeners_ = std::move(listeners);

    EPROSIMA_LOG_INFO(TSN_TRANSPORT, "CNC configuration for cuc-id '" << cuc_id_ << "' on "
                                                                     << interface_name_ << ": "
                                                                     << talkers_.size() << " talker(s), "
                                                                     << listeners_.size() << " listener(s)");
    return true;
}

bool TsnCncConfig::wait_for_accepted_streams(
        uint32_t timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true)
    {
        bool any = false;
        bool all_accepted = true;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            for (const auto* list : {&talkers_, &listeners_})
            {
                for (const TsnStream& stream : *list)
                {
                    any = true;
                    all_accepted = all_accepted && stream.accepted;
                }
            }
        }

        if (any && all_accepted)
        {
            return true;
        }

        if (!any)
        {
            // Nothing to wait for. The timeout covers the CNC accepting streams
            // the CUC has already requested; when the datastore holds no entry
            // for this node at all, sitting here would just stall startup by the
            // whole timeout and still fall back to the defaults.
            EPROSIMA_LOG_WARNING(TSN_TRANSPORT,
                    "No TSN stream is configured for this node (cuc-id '" << cuc_id_ << "', interface '"
                                                                          << interface_name_
                                                                          << "'); using the transport "
                                                                          << "descriptor's default VLAN and PCP");
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline)
        {
            EPROSIMA_LOG_WARNING(TSN_TRANSPORT,
                    "Timed out waiting for the CNC to accept every configured stream");
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        refresh();
    }
}

std::vector<TsnStream> TsnCncConfig::talkers() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return talkers_;
}

std::vector<TsnStream> TsnCncConfig::listeners() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return listeners_;
}

bool TsnCncConfig::find_talker_for_destination(
        const MacAddress& destination_mac,
        uint16_t vlan_id,
        TsnStream& out) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    for (const TsnStream& stream : talkers_)
    {
        if (!stream.has_data_frame_specification)
        {
            continue;
        }
        if (stream.destination_mac != destination_mac)
        {
            continue;
        }
        // A locator with no VLAN ID matches whatever VLAN the CNC assigned.
        if (0 != vlan_id && stream.vlan_id != vlan_id)
        {
            continue;
        }
        out = stream;
        return true;
    }
    return false;
}

bool TsnCncConfig::find_by_station_name(
        const std::string& station_name,
        bool talker,
        TsnStream& out) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    const std::vector<TsnStream>& list = talker ? talkers_ : listeners_;
    for (const TsnStream& stream : list)
    {
        if (stream.station_name == station_name)
        {
            out = stream;
            return true;
        }
    }
    return false;
}

bool TsnCncConfig::report_status(
        const TsnStream& stream,
        EndStationStatus status)
{
    yang_db_item_access_t* ydbia = uniconf_handle();
    if (nullptr == ydbia)
    {
        return false;
    }
    DbTransactionGuard release_on_exit(ydbia);

    // The setters take the key fields by pointer and do not retain them.
    StreamId stream_id = stream.stream_id;
    const std::string mac = EthernetLocator::mac_to_string(stream.station_mac);

    cc_endstation_info_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.instIndex = instance_index_;
    entry.cuc_id = cuc_id_.c_str();
    entry.streamid = stream_id.data();
    entry.lindex = stream.listener_index;
    entry.mac_address = mac.c_str();
    entry.interface_name = stream.interface_name.c_str();
    entry.status = static_cast<cc_endst_status_t>(status);

    const int result = stream.talker ?
            ydbi_set_talker_status_cc(ydbia, instance_index_, cuc_id_.c_str(), &entry) :
            ydbi_set_listener_status_cc(ydbia, instance_index_, cuc_id_.c_str(), &entry);

    return 0 == result;
}

void TsnCncConfig::report_status_for_all(
        EndStationStatus status)
{
    for (const TsnStream& stream : talkers())
    {
        report_status(stream, status);
    }
    for (const TsnStream& stream : listeners())
    {
        report_status(stream, status);
    }
}

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima
