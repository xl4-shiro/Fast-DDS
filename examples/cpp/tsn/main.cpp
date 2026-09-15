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
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/log/Log.hpp>
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
std::atomic<bool> g_started{false};

void signal_handler(
        int signal_number)
{
    // Until the send/receive loop is reached we may be inside a call that waits
    // for the CNC and never looks at g_running --- create_participant(), and
    // equally the endpoint setup below it, which waits for this topic's own
    // stream. Asking those to stop politely would be ignored, so exit outright:
    // a startup that cannot be interrupted is worse than an abrupt one.
    //
    // g_started therefore means "the loop that polls g_running is running", not
    // "the participant exists". Setting it any earlier reintroduces a window in
    // which the process cannot be signalled.
    if (!g_started.load())
    {
        std::_Exit(128 + signal_number);
    }
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
    uint32_t cnc_timeout_ms = 10000;
    bool allow_fallback = true;
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
        "      --no-fallback       Never send unscheduled. Waits for the CNC to provision and\n"
        "                          accept this node's streams, then refuses any destination it\n"
        "                          has no stream for. Fails to start if the wait times out\n"
        "      --cnc-timeout <ms>  How long to wait for the CNC (default 10000, 0 = forever)\n"
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
        else if ("--cnc-timeout" == arg && has_value)
        {
            options.cnc_timeout_ms = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if ("--no-fallback" == arg)
        {
            options.allow_fallback = false;
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

void list_streams(
        const Options& options,
        const TSNTransportDescriptor& descriptor);

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
    descriptor.allow_fallback = options.allow_fallback;
    descriptor.cnc_wait_timeout_ms = options.cnc_timeout_ms;
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
    if (!options.uniconf_db.empty())
    {
        list_streams(options, descriptor);
    }

    if (!options.allow_fallback)
    {
        // Fast DDS logs at Error by default, so the transport's "waiting for the
        // CNC" warning would not show and this would look like a hang.
        std::cout << "Strict mode: waiting for the CNC to provision and accept streams for cuc-id '"
                  << options.cuc_id << "' on " << options.interface_name << " (";
        if (0 == options.cnc_timeout_ms)
        {
            std::cout << "no timeout; interrupt to give up";
        }
        else
        {
            std::cout << "up to " << options.cnc_timeout_ms << " ms, then giving up";
        }
        std::cout << ")." << std::endl;
    }

    DomainParticipantQos qos = PARTICIPANT_QOS_DEFAULT;
    qos.name(options.publisher ? "tsn_publisher" : "tsn_subscriber");
    qos.transport().use_builtin_transports = false;
    qos.transport().user_transports.push_back(
        std::make_shared<TSNTransportDescriptor>(descriptor));

    return DomainParticipantFactory::get_instance()->create_participant(options.domain_id, qos);
}

/**
 * Look up the TSN Stream the CNC provisioned for a topic.
 *
 * @return false when the CUC has not named a stream for it.
 */
/**
 * Print every stream the CNC has provisioned for this node.
 *
 * Without this, a node that stalls waiting for the CNC, or that reports no
 * stream for its topic, gives no clue which of the two it is: an entry missing
 * altogether, one on another interface, one not yet accepted, or one whose
 * station-name does not match the topic.
 */
void list_streams(
        const Options& options,
        const TSNTransportDescriptor& descriptor)
{
    const auto talkers = tsn::TsnStreamLocators::talker_streams(descriptor, 0);
    const auto listeners = tsn::TsnStreamLocators::listener_streams(descriptor, 0);

    std::cout << "CNC streams for cuc-id '" << options.cuc_id << "' on " << options.interface_name
              << ": " << talkers.size() << " talker, " << listeners.size() << " listener" << std::endl;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const auto& b : (0 == pass ? talkers : listeners))
        {
            std::cout << "  " << (0 == pass ? "talker  " : "listener")
                      << " station-name='" << b.topic_name << "'"
                      << " if=" << b.interface_name
                      << " stream=" << b.stream_id
                      << " accepted=" << (b.accepted ? "yes" : "no");
            if (b.has_destination)
            {
                std::cout << " dest=" << EthernetLocator::mac_to_string(b.locator)
                          << " vlan=" << EthernetLocator::vid(b.locator)
                          << " pcp=" << static_cast<int>(EthernetLocator::pcp(b.locator));
            }
            else
            {
                std::cout << " dest=<none: no data-frame-specification>";
            }
            std::cout << std::endl;
        }
    }
}

bool find_stream(
        const Options& options,
        const TSNTransportDescriptor& descriptor,
        bool talker,
        tsn::TsnStreamBinding& binding)
{
    if (!tsn::TsnStreamLocators::find_stream_for_topic(descriptor, options.topic_name,
            talker, 0, binding))
    {
        // The lookup logs through Fast DDS, which writes from its own thread.
        // Without this the library's parting error and the lines below interleave
        // mid-sentence, exactly where the reader most needs a clear message.
        Log::Flush();

        // In strict mode the lookup has already waited for the CNC, so getting
        // here means the wait expired rather than that the name was simply
        // absent. Say which, or the message reads as an instant failure.
        if (options.allow_fallback)
        {
            std::cout << "No CNC " << (talker ? "talker" : "listener") << " stream is named '"
                      << options.topic_name << "'; using the participant's default locators."
                      << std::endl;
        }
        else
        {
            std::cout << "Gave up waiting for a CNC " << (talker ? "talker" : "listener")
                      << " stream named '" << options.topic_name << "'." << std::endl;
        }
        return false;
    }

    // Only the reader ends up carrying a locator, so only the reader should be
    // described as bound to one. On the writer the locator here is a bare
    // destination with no RTPS logical port --- nothing fills that in, because
    // the writer sends to whatever its matched readers announce.
    std::cout << "Topic '" << binding.topic_name << "' "
              << (talker ? "will be sent on" : "is bound to")
              << " stream " << binding.stream_id
              << " (VLAN " << EthernetLocator::vid(binding.locator)
              << ", PCP " << static_cast<int>(EthernetLocator::pcp(binding.locator))
              << ", dest " << EthernetLocator::mac_to_string(binding.locator)
              << ", accepted by the CNC: " << (binding.accepted ? "yes" : "no") << ")"
              << std::endl;
    return true;
}

/**
 * Point a DataReader at the stream provisioned for its topic.
 *
 * Only the reader needs this. An endpoint's locator lists say where that
 * endpoint can be reached, and a writer sends to the locators its matched
 * readers announced --- so putting the stream's group address on the reader is
 * what actually moves the data onto the stream.
 */
bool bind_reader_to_stream(
        DataReaderQos& qos,
        const Options& options,
        const TSNTransportDescriptor& descriptor)
{
    tsn::TsnStreamBinding binding;
    if (!find_stream(options, descriptor, false, binding))
    {
        // In strict mode an unbound endpoint would receive its data on the
        // participant's own locators, off any schedule. Refuse it here: this is
        // the only layer that knows which topic an endpoint serves, so it is
        // the only one that can tell unscheduled user data from discovery.
        return options.allow_fallback;
    }

    qos.endpoint().multicast_locator_list.push_back(binding.locator);
    qos.properties().properties().emplace_back("dds.tsn.stream_id", binding.stream_id);
    return true;
}

/**
 * Report the talker stream this node will send the topic on.
 *
 * The writer needs no locator of its own: when it sends to the group address
 * the reader announced, TSNTransport matches that destination back to this
 * stream and applies its ID, PCP and shaper rate. Putting the address on the
 * writer's own multicast_locator_list would instead advertise where the writer
 * receives ACKNACKs, which under the BEST_EFFORT QoS above do not exist.
 *
 * Looking it up is still worth doing: it checks that the CUC has provisioned
 * the talker side, which is what the transport will require when sending.
 */
bool report_talker_stream(
        DataWriterQos& qos,
        const Options& options,
        const TSNTransportDescriptor& descriptor)
{
    tsn::TsnStreamBinding binding;
    if (!find_stream(options, descriptor, true, binding))
    {
        return options.allow_fallback;
    }

    qos.properties().properties().emplace_back("dds.tsn.stream_id", binding.stream_id);
    return true;
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
        // Do not guess at the cause: creation fails for several reasons and the
        // transport has already logged the specific one.
        std::cerr << "Cannot create the participant; see the errors above. Common causes: "
                  << "raw sockets need CAP_NET_RAW ('sudo setcap cap_net_raw+ep <binary>' or run "
                  << "as root), and with --no-fallback the CNC must provision this node's streams "
                  << "before the timeout." << std::endl;
        return 1;
    }

    TypeSupport type(new HelloWorldPubSubType());
    type.register_type(participant);

    Topic* topic = participant->create_topic(options.topic_name, type.get_type_name(), TOPIC_QOS_DEFAULT);
    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);

    DataWriterQos writer_qos = DATAWRITER_QOS_DEFAULT;
    apply_time_critical_qos(writer_qos);
    if (!report_talker_stream(writer_qos, options, descriptor))
    {
        std::cerr << "No CNC talker stream for topic '" << options.topic_name
                  << "' and --no-fallback is set; refusing to publish unscheduled." << std::endl;
        return 1;
    }

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

    g_started.store(true);

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
        // Do not guess at the cause: creation fails for several reasons and the
        // transport has already logged the specific one.
        std::cerr << "Cannot create the participant; see the errors above. Common causes: "
                  << "raw sockets need CAP_NET_RAW ('sudo setcap cap_net_raw+ep <binary>' or run "
                  << "as root), and with --no-fallback the CNC must provision this node's streams "
                  << "before the timeout." << std::endl;
        return 1;
    }

    TypeSupport type(new HelloWorldPubSubType());
    type.register_type(participant);

    Topic* topic = participant->create_topic(options.topic_name, type.get_type_name(), TOPIC_QOS_DEFAULT);
    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);

    DataReaderQos reader_qos = DATAREADER_QOS_DEFAULT;
    apply_time_critical_qos(reader_qos);
    if (!bind_reader_to_stream(reader_qos, options, descriptor))
    {
        std::cerr << "No CNC listener stream for topic '" << options.topic_name
                  << "' and --no-fallback is set; refusing to subscribe unscheduled." << std::endl;
        return 1;
    }

    ReaderListener listener;
    DataReader* reader = subscriber->create_datareader(topic, reader_qos, &listener);
    if (nullptr == reader)
    {
        std::cerr << "Cannot create the DataReader" << std::endl;
        return 1;
    }

    // Read back the locators the reader ended up announcing. The logical port is
    // assigned during creation, so this is the only point at which it is real.
    eprosima::fastdds::rtps::LocatorList listening;
    if (RETCODE_OK == reader->get_listening_locators(listening))
    {
        for (const auto& locator : listening)
        {
            std::cout << "  listening on " << locator << std::endl;
        }
    }

    g_started.store(true);

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

    // Fast DDS logs only errors by default, and every diagnostic the TSN
    // transport emits while it waits for the CNC is a warning --- so at the
    // default level, `--no-fallback --cnc-timeout 0` blocks indefinitely without
    // printing anything, which is indistinguishable from a hang. An example
    // should show its working.
    Log::SetVerbosity(Log::Warning);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    const TSNTransportDescriptor descriptor = make_descriptor(options);

    return options.publisher ? run_publisher(options, descriptor) : run_subscriber(options, descriptor);
}
