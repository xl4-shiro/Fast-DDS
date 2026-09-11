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
 * @file main.cpp
 *
 * A DDS-TSN publisher and subscriber talking over IEEE 1722, with the streams
 * configured by a CNC through the ieee802-dot1q-cnc-config datastore.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/tsn/TsnStreamLocators.hpp>
#include <fastdds/rtps/transport/TSNTransportDescriptor.hpp>
#include <fastdds/utils/EthernetLocator.hpp>

#include "HelloWorldPubSubTypes.hpp"

using namespace eprosima::fastdds::dds;
using eprosima::fastdds::rtps::EthernetLocator;
using eprosima::fastdds::rtps::TSNTransportDescriptor;

namespace {

std::atomic<bool> g_running{true};

void signal_handler(
        int)
{
    g_running.store(false);
}

struct Options
{
    bool publisher = false;
    std::string interface_name = "eth0";
    std::string topic_name = "HelloWorldTopic";
    std::string cuc_id = "br01";
    std::string uniconf_db;
    uint8_t cnc_instance_index = 0;
    uint16_t vlan_id = 0;
    uint8_t pcp = 0;
    uint32_t domain_id = 0;
    uint32_t samples = 0;
    uint32_t period_ms = 100;
    uint32_t payload_size = 0;
    bool require_streams = false;
    bool no_gptp = false;
};

void print_usage(
        const char* program)
{
    std::cout <<
        "Usage: " << program << " <publisher|subscriber> [options]\n"
        "\n"
        "  -i, --interface <dev>   Network interface to bind the AVTP sockets to (default eth0)\n"
        "  -t, --topic <name>      DDS topic name; also the station-name looked up in the\n"
        "                          CNC configuration (default HelloWorldTopic)\n"
        "      --cuc-id <id>       cuc-id key of the CNC configuration (default br01)\n"
        "      --uniconf-db <path> uniconf database to open. Omit if the process already\n"
        "                          called ydbi_access_init()\n"
        "      --instance <n>      CNC configuration domain instance index (default 0)\n"
        "      --vlan <id>         VLAN ID for traffic with no provisioned stream (default 0)\n"
        "      --pcp <n>           PCP for traffic with no provisioned stream (default 0)\n"
        "  -d, --domain <id>       DDS domain id (default 0)\n"
        "  -s, --samples <n>       Stop after n samples (default: run until interrupted)\n"
        "  -p, --period <ms>       Publication period in milliseconds (default 100)\n"
        "      --payload <bytes>   Pad each sample to this size. Above about 1400 octets RTPS\n"
        "                          splits the sample across several frames as DataFrag\n"
        "      --require-streams   Refuse to send on a stream the CNC has not accepted\n"
        "      --no-gptp           Timestamp with the monotonic clock instead of gPTP\n"
        << std::endl;
}

bool parse_options(
        int argc,
        char** argv,
        Options& options)
{
    if (argc < 2)
    {
        return false;
    }

    const std::string mode = argv[1];
    if ("publisher" == mode)
    {
        options.publisher = true;
    }
    else if ("subscriber" != mode)
    {
        return false;
    }

    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;

        if (("-i" == arg || "--interface" == arg) && has_value)
        {
            options.interface_name = argv[++i];
        }
        else if (("-t" == arg || "--topic" == arg) && has_value)
        {
            options.topic_name = argv[++i];
        }
        else if ("--cuc-id" == arg && has_value)
        {
            options.cuc_id = argv[++i];
        }
        else if ("--uniconf-db" == arg && has_value)
        {
            options.uniconf_db = argv[++i];
        }
        else if ("--instance" == arg && has_value)
        {
            options.cnc_instance_index = static_cast<uint8_t>(std::stoul(argv[++i]));
        }
        else if ("--vlan" == arg && has_value)
        {
            options.vlan_id = static_cast<uint16_t>(std::stoul(argv[++i]));
        }
        else if ("--pcp" == arg && has_value)
        {
            options.pcp = static_cast<uint8_t>(std::stoul(argv[++i]));
        }
        else if (("-d" == arg || "--domain" == arg) && has_value)
        {
            options.domain_id = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if (("-s" == arg || "--samples" == arg) && has_value)
        {
            options.samples = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if (("-p" == arg || "--period" == arg) && has_value)
        {
            options.period_ms = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if ("--payload" == arg && has_value)
        {
            options.payload_size = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if ("--require-streams" == arg)
        {
            options.require_streams = true;
        }
        else if ("--no-gptp" == arg)
        {
            options.no_gptp = true;
        }
        else
        {
            std::cerr << "Unknown or incomplete option: " << arg << std::endl;
            return false;
        }
    }

    return true;
}

TSNTransportDescriptor make_descriptor(
        const Options& options)
{
    TSNTransportDescriptor descriptor;
    descriptor.interface_name = options.interface_name;
    descriptor.cuc_id = options.cuc_id;
    descriptor.uniconf_db_name = options.uniconf_db;
    descriptor.cnc_instance_index = options.cnc_instance_index;
    descriptor.default_vlan_id = options.vlan_id;
    descriptor.default_pcp = options.pcp;
    descriptor.socket_priority = options.pcp;
    descriptor.require_accepted_streams = options.require_streams;
    descriptor.use_gptp = !options.no_gptp;
    return descriptor;
}

/**
 * Build a participant that speaks only the Ethernet PSM.
 *
 * Clearing the built-in transports is what makes this the full Annex A
 * deployment: discovery runs over IEEE 1722 too, so the node needs no IP stack.
 */
DomainParticipant* create_participant(
        const Options& options,
        const TSNTransportDescriptor& descriptor)
{
    DomainParticipantQos qos = PARTICIPANT_QOS_DEFAULT;
    qos.name(options.publisher ? "tsn_publisher" : "tsn_subscriber");
    qos.transport().use_builtin_transports = false;
    qos.transport().user_transports.push_back(
        std::make_shared<TSNTransportDescriptor>(descriptor));

    return DomainParticipantFactory::get_instance()->create_participant(options.domain_id, qos);
}

/**
 * Point an endpoint at the TSN Stream the CNC provisioned for its topic.
 *
 * Without this the endpoint would use the participant's own locators and its
 * frames would be indistinguishable, at the network, from every other topic's,
 * so the CNC could not schedule them as a stream of their own.
 */
template<typename EndpointQos>
void bind_to_stream(
        EndpointQos& qos,
        const Options& options,
        const TSNTransportDescriptor& descriptor)
{
    tsn::TsnStreamBinding binding;
    if (!tsn::TsnStreamLocators::find_stream_for_topic(descriptor, options.topic_name,
            options.publisher, 0, binding))
    {
        std::cout << "No CNC stream is named '" << options.topic_name
                  << "'; using the participant's default locators." << std::endl;
        return;
    }

    std::cout << "Topic '" << binding.topic_name << "' is bound to stream " << binding.stream_id
              << " at " << binding.locator
              << " (VLAN " << EthernetLocator::vid(binding.locator)
              << ", PCP " << static_cast<int>(EthernetLocator::pcp(binding.locator))
              << ", accepted by the CNC: " << (binding.accepted ? "yes" : "no") << ")"
              << std::endl;

    qos.endpoint().multicast_locator_list.push_back(binding.locator);

    // Subclause 8.2.2.2 of [DDS-TSN]: matching is by topic, type and QoS, so a
    // DataWriter that is a Talker could otherwise match a DataReader that is not
    // a Listener of the stream. Partitioning on the stream name keeps the
    // endpoints of one stream to themselves.
    qos.properties().properties().emplace_back("dds.tsn.stream_id", binding.stream_id);
}

/**
 * Apply the QoS that subclause 8.2.3 of [DDS-TSN] recommends for a time-critical
 * stream: no acknowledgements, no repairs, no history to replay to late joiners,
 * and therefore a message size the schedule can count on.
 */
void apply_time_critical_qos(
        DataWriterQos& qos)
{
    qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    qos.durability().kind = VOLATILE_DURABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
}

void apply_time_critical_qos(
        DataReaderQos& qos)
{
    qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    qos.durability().kind = VOLATILE_DURABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
}

class WriterListener : public DataWriterListener
{
public:

    void on_publication_matched(
            DataWriter*,
            const PublicationMatchedStatus& info) override
    {
        matched_.store(info.current_count);
        std::cout << "Matched subscribers: " << info.current_count << std::endl;
    }

    int32_t matched() const
    {
        return matched_.load();
    }

private:

    std::atomic<int32_t> matched_{0};
};

class ReaderListener : public DataReaderListener
{
public:

    void on_subscription_matched(
            DataReader*,
            const SubscriptionMatchedStatus& info) override
    {
        std::cout << "Matched publishers: " << info.current_count << std::endl;
    }

    void on_data_available(
            DataReader* reader) override
    {
        HelloWorld sample;
        SampleInfo info;
        while (RETCODE_OK == reader->take_next_sample(&sample, &info))
        {
            if (info.valid_data)
            {
                ++received_;
                std::cout << "Received " << sample.message().size() << " octets, index "
                          << sample.index() << std::endl;
            }
        }
    }

    uint32_t received() const
    {
        return received_;
    }

private:

    uint32_t received_ = 0;
};

int run_publisher(
        const Options& options,
        const TSNTransportDescriptor& descriptor)
{
    DomainParticipant* participant = create_participant(options, descriptor);
    if (nullptr == participant)
    {
        std::cerr << "Cannot create the participant. Raw sockets need CAP_NET_RAW: try "
                  << "'sudo setcap cap_net_raw+ep <binary>' or run as root." << std::endl;
        return 1;
    }

    TypeSupport type(new HelloWorldPubSubType());
    type.register_type(participant);

    Topic* topic = participant->create_topic(options.topic_name, type.get_type_name(), TOPIC_QOS_DEFAULT);
    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);

    DataWriterQos writer_qos = DATAWRITER_QOS_DEFAULT;
    apply_time_critical_qos(writer_qos);
    bind_to_stream(writer_qos, options, descriptor);

    WriterListener listener;
    DataWriter* writer = publisher->create_datawriter(topic, writer_qos, &listener);
    if (nullptr == writer)
    {
        std::cerr << "Cannot create the DataWriter" << std::endl;
        return 1;
    }

    HelloWorld sample;
    sample.message(options.payload_size > 0 ? std::string(options.payload_size, 'x') : "Hello TSN");
    uint32_t sent = 0;

    while (g_running.load() && (0 == options.samples || sent < options.samples))
    {
        if (listener.matched() > 0)
        {
            sample.index(++sent);
            writer->write(&sample);
            std::cout << "Sent " << sample.message().size() << " octets, index " << sample.index() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.period_ms));
    }

    std::cout << "Published " << sent << " samples" << std::endl;
    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}

int run_subscriber(
        const Options& options,
        const TSNTransportDescriptor& descriptor)
{
    DomainParticipant* participant = create_participant(options, descriptor);
    if (nullptr == participant)
    {
        std::cerr << "Cannot create the participant. Raw sockets need CAP_NET_RAW: try "
                  << "'sudo setcap cap_net_raw+ep <binary>' or run as root." << std::endl;
        return 1;
    }

    TypeSupport type(new HelloWorldPubSubType());
    type.register_type(participant);

    Topic* topic = participant->create_topic(options.topic_name, type.get_type_name(), TOPIC_QOS_DEFAULT);
    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);

    DataReaderQos reader_qos = DATAREADER_QOS_DEFAULT;
    apply_time_critical_qos(reader_qos);
    bind_to_stream(reader_qos, options, descriptor);

    ReaderListener listener;
    DataReader* reader = subscriber->create_datareader(topic, reader_qos, &listener);
    if (nullptr == reader)
    {
        std::cerr << "Cannot create the DataReader" << std::endl;
        return 1;
    }

    while (g_running.load() && (0 == options.samples || listener.received() < options.samples))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Received " << listener.received() << " samples" << std::endl;
    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}

} // anonymous namespace

int main(
        int argc,
        char** argv)
{
    Options options;
    if (!parse_options(argc, argv, options))
    {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    const TSNTransportDescriptor descriptor = make_descriptor(options);

    return options.publisher ? run_publisher(options, descriptor) : run_subscriber(options, descriptor);
}
