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
 * @file Xl4Runtime.cpp
 */

#include <rtps/transport/tsn/Xl4Runtime.hpp>

#include <mutex>

#include <fastdds/dds/log/Log.hpp>

extern "C" {
#include <xl4unibase/unibase.h>
#include <xl4unibase/unibase_binding.h>
#include <gptp2/gptpmasterclock.h>
} // extern "C"

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

void init_xl4_runtime()
{
    static std::once_flag once;
    std::call_once(once, []()
            {
                unibase_init_para_t init_para;
                ubb_default_initpara(&init_para);
                init_para.ub_log_initstr =
                        UBL_OVERRIDE_ISTR("4,ubase:45,cbase:45,uconf:45,avtp:45", "UBL_FASTDDS_TSN");
                unibase_init(&init_para);
            });
}

bool gptp_clock_available(
        const std::string& shmem_name)
{
    static std::once_flag once;
    static bool available = false;

    std::call_once(once, [&shmem_name]()
            {
                init_xl4_runtime();
                // An empty name makes gptpmasterclock_init() use its own default
                // segment, which is what gptp2d creates unless told otherwise.
                available = (0 == gptpmasterclock_init(shmem_name.empty() ? nullptr : shmem_name.c_str()));
                if (!available)
                {
                    EPROSIMA_LOG_WARNING(TSN_TRANSPORT,
                            "No gPTP clock available on shared memory '"
                            << (shmem_name.empty() ? "<default>" : shmem_name)
                            << "'; AVTP frames will be timestamped from the monotonic clock. "
                            << "Start gptp2d, or set TSNTransportDescriptor::use_gptp to false "
                            << "to make this explicit.");
                }
            });

    return available;
}

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima
