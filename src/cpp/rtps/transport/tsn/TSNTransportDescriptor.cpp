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
 * @file TSNTransportDescriptor.cpp
 */

#include <fastdds/rtps/transport/TSNTransportDescriptor.hpp>

#include <rtps/transport/tsn/TSNTransport.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {

TSNTransportDescriptor::TSNTransportDescriptor()
    : PortBasedTransportDescriptor(tsn_max_message_size, s_maximumInitialPeersRange)
{
}

TransportInterface* TSNTransportDescriptor::create_transport() const
{
    return new TSNTransport(*this);
}

bool TSNTransportDescriptor::operator ==(
        const TSNTransportDescriptor& t) const
{
    return interface_name == t.interface_name &&
           uniconf_db_name == t.uniconf_db_name &&
           cuc_id == t.cuc_id &&
           cnc_instance_index == t.cnc_instance_index &&
           default_multicast_mac == t.default_multicast_mac &&
           default_vlan_id == t.default_vlan_id &&
           default_pcp == t.default_pcp &&
           socket_priority == t.socket_priority &&
           wait_for_cnc == t.wait_for_cnc &&
           cnc_wait_timeout_ms == t.cnc_wait_timeout_ms &&
           allow_fallback == t.allow_fallback &&
           use_gptp == t.use_gptp &&
           gptp_shmem_name == t.gptp_shmem_name &&
           avtp_header_version == t.avtp_header_version &&
           stream_subtype == t.stream_subtype &&
           control_subtype == t.control_subtype &&
           acf_message_type == t.acf_message_type &&
           reception_timeout_ms == t.reception_timeout_ms &&
           cnc_revocation_poll_ms == t.cnc_revocation_poll_ms &&
           PortBasedTransportDescriptor::operator ==(t);
}

} // namespace rtps
} // namespace fastdds
} // namespace eprosima
