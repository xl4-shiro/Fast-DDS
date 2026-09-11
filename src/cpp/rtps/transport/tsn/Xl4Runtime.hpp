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
 * @file Xl4Runtime.hpp
 */

#ifndef FASTDDS_RTPS_TRANSPORT_TSN__XL4RUNTIME_HPP
#define FASTDDS_RTPS_TRANSPORT_TSN__XL4RUNTIME_HPP

#include <string>

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace tsn {

/**
 * Bring up the Excelfore runtime, once per process.
 *
 * Every xl4 library reaches its clock, logging and allocator through function
 * pointers that @c unibase_init() installs. Until it runs those pointers are
 * null, and the first call into uniconf or avtpcon jumps to address zero --- so
 * this has to happen before anything else in the stack is touched.
 *
 * A DDS application has no reason to know that, so the transport does it rather
 * than making the caller do it. @c unibase_init() ignores every call after the
 * first, so an application that already initialised unibase itself keeps its own
 * settings, including its log configuration.
 *
 * The log level follows the xl4 convention and can be overridden with the
 * @c UBL_FASTDDS_TSN environment variable, e.g.
 * @c UBL_FASTDDS_TSN="4,ubase:45,cbase:45,uconf:45,avtp:46".
 */
void init_xl4_runtime();

/**
 * Attach to the gPTP-disciplined clock, once per process.
 *
 * @c gptpmasterclock_getts64() returns -1 until @c gptpmasterclock_init() has
 * mapped gptp2d's shared memory, and avtpcon writes that straight into the AVTP
 * header as 0xFFFFFFFF. Callers use the result to decide whether to timestamp
 * from gPTP or fall back to the monotonic clock.
 *
 * @param shmem_name  gptp2d's shared memory segment, or empty for the default.
 * @return true when gPTP time is available.
 */
bool gptp_clock_available(
        const std::string& shmem_name);

} // namespace tsn
} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // FASTDDS_RTPS_TRANSPORT_TSN__XL4RUNTIME_HPP
