# DDS-TSN example

A publisher and a subscriber that talk over IEEE 1722 instead of UDP/IP,
implementing the DDSI-RTPS Ethernet PSM of the OMG DDS-TSN specification
(ptc/2023-03-03, Annex A), with the TSN Streams configured by a CNC through the
`ieee802-dot1q-cnc-config` YANG datastore.

Discovery runs over 1722 as well, so a node needs no IP stack at all.

## How a message travels

```
DataWriter::write()
  |
  RTPS message (Header + InfoTimestamp + Data), always a multiple of 4 octets
  |
  TSNTransport::send()  -- picks the stream for the destination locator
  |
  AvtpStream::send()    -- wraps it and hands it to a raw AF_PACKET socket

|--------------------------------+----------------------+-----------------------+--------------|
| Ethernet: dst MAC              | src MAC              | 802.1Q tag (VID, PCP) |       0x22F0 |
|--------------------------------+----------------------+-----------------------+--------------|
| AVTP stream header, 24 octets: |                      |                       |              |
| subtype 0x7F (EF_STREAM)       | sv                   | tv                    | sequence_num |
| stream_id (8)                  | avtp_timestamp (4)   |                       |              |
| stream_data_length (2)         |                      |                       |              |
|--------------------------------+----------------------+-----------------------+--------------|
| dst logical port (2)           |                      |                       |              |
|--------------------------------+----------------------+-----------------------+--------------|
| RTPS message                   |                      |                       |              |
|--------------------------------+----------------------+-----------------------+--------------|
```

IEEE 1722 registers no subtype for RTPS, so the Experimental Format Stream
(0x7F) is the honest choice. A *stream* subtype is used rather than a control
format because its header carries two things RTPS benefits from: an
`avtp_timestamp`, filled per frame from the gPTP-disciplined clock, and an
explicit `stream_data_length`.

The 2-octet header between the AVTP header and the RTPS message --- a single
`destination_logical_port` --- **is not defined by any standard.** It is specific
to this implementation, it is not interoperable with another DDS-TSN
implementation, and it contradicts the specification rather than extending it.
See [Known deviations](#known-deviations-from-the-specification) for why it is
here anyway.

It carries the port and nothing else. The message *length* is already on the
wire, in `stream_data_length` here and `acf_msg_length` under the control
framing, and a *source* port is not needed because RTPS replies to the locators a
peer announced in discovery rather than to wherever a message came from.

### Control-format framing

Setting `avtp_subtype` to NTSCF (0x82) or TSCF (0x06) instead wraps the same
payload in an ACF message of type `acf_message_type` (`ACF_USER0` by default),
which is where IEEE 1722 defines ACF messages to live. That trades the timestamp
away, but lets RTPS share a frame with other ACF traffic such as `ACF_CAN`.
An ACF message is padded to a quadlet boundary, and `ACF_USER0` being a
user-defined type, [1722] specifies no `pad` field for it --- but none is needed.
An RTPS message is always a multiple of 4 octets, so the 2-octet ACF header plus
this 2-octet header leave the ACF message quadlet-aligned with nothing to pad,
and `acf_msg_length` therefore gives the message length exactly.

## Building

### Excelfore dependencies

The transport needs `xl4avtp` (including its `acf` module), `xl4uniconf`,
`xl4combase`, `xl4unibase` and `xl4gptp`. Prebuilt packages are published at
`https://excelforejp.com/avbpub/xl4debs/`.

> **These packages are for evaluation only.** They are fully functional --- no
> feature is disabled --- but each run is limited to **4 hours**, after which the
> process stops. That is long enough to develop and test against, but not for
> deployment or for any long-running measurement. Contact Excelfore for
> production licensing, or build the stpl tree from source.

**Replace `DISTRO-RELEASE` below with the codename matching your own
distribution.** Packages are built for `noble` and `jammy` (Ubuntu), and
`trixie` and `bookworm` (Debian).

On a derivative, use the codename of the distribution it is built on, not its
own: Linux Mint 22.x takes `noble` and Mint 21.x takes `jammy`, and a
Debian-derived distribution takes whichever Debian release it tracks. Note that
`lsb_release -cs` prints the derivative's own codename, so it is not the answer
here — check what your distribution is based on. On Ubuntu and Debian proper,
`lsb_release -cs` is correct.

One-line format, in `/etc/apt/sources.list.d/xl4.list`:

```
deb [trusted=true] https://excelforejp.com/avbpub/xl4debs/DISTRO-RELEASE ./
```

Or DEB822 format, in `/etc/apt/sources.list.d/xl4.sources`:

```
types: deb
URIs: https://excelforejp.com/avbpub/xl4debs/DISTRO-RELEASE
Suites: ./
Trusted: yes
```

Then:

```bash
sudo apt update
sudo apt install xl4avtp xl4avtp-dev xl4uniconf xl4uniconf-dev \
                 xl4gptp xl4gptp-dev xl4combase xl4combase-dev \
                 xl4unibase xl4unibase-dev
```

Install both halves of each package. A `-dev` package carries only the headers
and the `pkg-config` file; every shared library, the `.so` development symlink
included, is in the runtime package, and `-dev` does not depend on it. Installing
`-dev` alone gets you a configure that succeeds and a link that fails.

Building the stpl tree from source works too; just make sure `make install` has
run for `xl4avtp` and `gptp2`, since a stale install is missing symbols the
current sources call.

CMake locates all of it through `pkg-config`, including the acf module of
xl4avtp, which carries the control-format framing.

`xl4avtp.pc` lists `x4acf` in its `Libs:` from version 1.0.2 on; against an older
package the library is looked up separately and CMake says so, which is
informational rather than a problem:

```
-- xl4avtp.pc does not list x4acf; linking /usr/lib/x86_64-linux-gnu/libx4acf.so directly
```

Point CMake at either half explicitly if they live somewhere unusual:

```bash
cmake -DTSN_TRANSPORT=ON \
      -DXL4ACF_INCLUDE_DIR=/usr/include/xl4tsn \
      -DXL4ACF_LIBRARY=/usr/lib/x86_64-linux-gnu/libx4acf.so ...
```

### Fast DDS dependencies

`fastcdr` comes from the in-tree submodule with `-DTHIRDPARTY=ON`.
`foonathan_memory` has no submodule and must be installed first, from
[foonathan_memory_vendor](https://github.com/eProsima/foonathan_memory_vendor):

```bash
git clone --depth 1 https://github.com/eProsima/foonathan_memory_vendor.git
cmake -S foonathan_memory_vendor -B fmv-build -DCMAKE_INSTALL_PREFIX=<prefix>
cmake --build fmv-build --target install
```

### Configure and build

```bash
cmake -B build -DTSN_TRANSPORT=ON -DCOMPILE_EXAMPLES=ON -DTHIRDPARTY=ON \
      -DCMAKE_PREFIX_PATH=<prefix>
cmake --build build -j
```

Look for this line to confirm the transport is in:

```
-- DDS-TSN transport enabled (xl4avtp 1.0.1, xl4uniconf 1.3.16)
```

## Running

Raw sockets need `CAP_NET_RAW`:

```bash
sudo setcap cap_net_raw+ep ./tsn
```

Then, on the talker node:

```bash
./tsn publisher -i eth0 -t ControlCommand --cuc-id br01 --uniconf-db /var/lib/uniconf.db
```

and on the listener node:

```bash
./tsn subscriber -i eth0 -t ControlCommand --cuc-id br01 --uniconf-db /var/lib/uniconf.db
```

`ControlCommand` is just a name --- nothing in the code requires it, and `-t`
defaults to `HelloWorldTopic`. What matters is that the topic name matches the
`station-name` the CUC provisioned for this interface, on both nodes, because
that is how the stream is found. Rename it to anything you like, as long as `-t`
and `station-name` change together; drop `-t` entirely and provision the
station-name as `HelloWorldTopic`.

Run `./tsn` with no arguments for the full option list.

## How the streams are configured

Nothing about a stream is configured in the DDS application. The CUC writes
Talker and Listener Groups into the `ieee802-dot1q-cnc-config` datastore and the
CNC answers with the stream's destination MAC address, VLAN tag, PCP and traffic
specification, plus an `accept` flag. This example reads that back:

- **`station-name` is the DDS topic name.** For each end-station interface the
  CUC provisions, the `station-name` leaf names the topic whose DataWriter or
  DataReader belongs to that stream. `TsnStreamLocators::find_stream_for_topic()`
  looks it up and returns the Ethernet locator, which the example puts on the
  endpoint's `multicast_locator_list`. This is what gives a topic its own stream:
  without it the endpoint would use the participant's locators and its frames
  would be indistinguishable from every other topic's.

- **The transport matches destinations back to streams.** When an RTPS message
  goes to a locator whose MAC and VLAN match a provisioned talker stream, the
  frame carries that stream's ID and PCP and is shaped to the granted rate
  (`max-frames-per-interval` x `max-frame-size` / `interval`). Traffic with no
  provisioned stream — discovery, above all — falls back to the descriptor's
  `default_vlan_id` and `default_pcp`.

- **The transport reports back.** Each end-station interface's `status` leaf is
  set to `CONNECTED` when the participant starts and `DISCONNECTED` when it
  stops, so the CUC can see which endpoints are live.

### Strict mode

By default a destination the CNC knows nothing about is still reached, over the
descriptor's `default_vlan_id` and `default_pcp` with a locally derived stream
ID. Traffic flows, but unscheduled. Discovery depends on this, since the CUC has
no reason to provision a stream for SPDP.

Set `allow_fallback` to false (`--no-fallback`) where sending off-schedule is
worse than not sending. Two things then change:

- The transport waits during initialisation for the CNC to provision *and*
  accept every stream for this node, for at most `cnc_wait_timeout_ms`
  (`--cnc-timeout`, default 10000). If that expires it logs an error and refuses
  to start, which makes `create_participant()` fail and return `nullptr`.
- An endpoint whose topic has no provisioned stream waits for that specific
  stream, on the same terms, and is refused at creation only once the wait
  expires --- rather than quietly sending on the participant's own locators.

  The endpoint waits for *its own* stream, not merely for the node to have some
  accepted stream. The two differ whenever a datastore already holds entries for
  other topics: the transport's check above is satisfied by those, so without a
  second, topic-specific wait an endpoint would give up instantly on a populated
  datastore while waiting patiently on an empty one. Whether an endpoint waited
  would depend on whether unrelated topics happened to be provisioned, which is
  not a useful distinction.

Discovery is deliberately *not* blocked. It is framed as a control format, not a
stream, so there is nothing to schedule and nothing to refuse --- which is what
subclause 8.2.2.1 intends when it puts discovery on non-critical channels. The
transport keeps a refusal for stream traffic with no schedule behind it, but
that is a safety net; the enforcement that matters is at the two points above.

The endpoint check lives in the example rather than the transport on purpose: a
transport sees only locators, so it cannot tell user data bound for no stream
from discovery reaching an unknown peer. The DDS layer knows which topic an
endpoint serves, and is the only layer that can.

```bash
./tsn publisher -i eth0 -t ControlCommand --uniconf-db /var/lib/uniconf.db \
      --no-fallback --cnc-timeout 30000
```

`cnc_wait_timeout_ms` governs both modes; only the consequence of expiry
differs:

| | timeout expires |
|---|---|
| `allow_fallback = true` (default) | warn, carry on with the default VLAN and PCP |
| `allow_fallback = false` | error; the transport does not start and participant creation fails, or, for an endpoint whose topic was never provisioned, the endpoint is refused |

A timeout of **0 waits indefinitely**. In strict mode that means participant
creation, and then endpoint creation, do not return until the CNC responds, and
the process can only be stopped by a signal --- the example exits immediately on
SIGINT/SIGTERM while still starting up, rather than appearing to hang.

Both waits report every five seconds which stream they are still waiting for and
why --- not provisioned, not accepted, or accepted without a
data-frame-specification. Those are warnings, and Fast DDS logs only errors by
default, so the example raises the verbosity to `Log::Warning` at startup. An
application that does not will see an indefinite wait produce no output at all.

## QoS

`apply_time_critical_qos()` in `main.cpp` applies what subclause 8.2.3 of the
specification recommends for a time-critical stream:

| QoS         | Value                | Why                                                  |
|-------------+----------------------+------------------------------------------------------|
| RELIABILITY | `BEST_EFFORT`        | No acknowledgements or repairs to break the schedule |
| DURABILITY  | `VOLATILE`           | Nothing replayed to a late joiner                    |
| HISTORY     | `KEEP_LAST`, depth 1 | Bounded, predictable message size                    |

Keep samples small enough that one RTPS message fits one Ethernet frame. The
transport reads the interface MTU at startup and caps the participant's maximum
message size to what actually fits — roughly 1468 octets on a 1500-octet MTU
with the stream framing above. Larger samples still work: RTPS fragments them
into `DataFrag` submessages, which is what subclause 8.2.1 expects, but the
schedule then has to account for several frames per sample.

## Testing without a TSN network

The transport needs a real link, so a single machine cannot run both ends over
one interface. A `veth` pair in two network namespaces gives you both ends on
one host:

```bash
sudo ip netns add tsn-a
sudo ip netns add tsn-b
sudo ip link add veth-a type veth peer name veth-b
sudo ip link set veth-a netns tsn-a
sudo ip link set veth-b netns tsn-b
sudo ip netns exec tsn-a ip link set veth-a up
sudo ip netns exec tsn-b ip link set veth-b up

sudo ip netns exec tsn-a ./tsn publisher  -i veth-a --no-gptp
sudo ip netns exec tsn-b ./tsn subscriber -i veth-b --no-gptp
```

Use `--no-gptp` where no `gptp2d` is running. The transport calls
`gptpmasterclock_init()` itself and falls back to the monotonic clock if the
shared memory is not there, so gPTP being absent degrades rather than breaks —
but saying so explicitly avoids the warning. Set
`TSNTransportDescriptor::gptp_shmem_name` if `gptp2d` publishes on a
non-default segment.

Without a CNC, leave `--uniconf-db` unset. The transport logs that uniconf is
unavailable and sends everything on the default VLAN and PCP, unscheduled, which
is enough to see samples flow.

## Reading the traffic in Wireshark

Wireshark decodes the IEEE 1722 framing on its own, but stops at the ACF payload
because ACF_USER0 is, by definition, whatever the application puts there. The
Lua dissector in `rtps_over_1722.lua` picks up from that point: it adds the
six-octet TSN-RTPS header and hands the message to Wireshark's own `rtps`
dissector, so submessages, GUIDs and QoS decode as usual.

```bash
tshark -X lua_script:rtps_over_1722.lua -r capture.pcap
wireshark -X lua_script:rtps_over_1722.lua capture.pcap
```

Copy it to `~/.local/lib/wireshark/plugins/` to load it for every capture.

It registers on the subdissector tables the built-in `ieee1722` dissector
already offers --- `ieee1722.subtype` for both subtypes, and `acf.msg_type` for
ACF_USER0 --- rather than claiming EtherType `0x22F0`. On the control subtype it
chains straight into Wireshark's own NTSCF dissector, so `ieee1722.*`, `ntscf.*`
and `acf.*` stay populated and other AVB traffic in the same capture is
unaffected:

```
eth:ethertype:vlan:ethertype:ieee1722:tsnrtps_ctrl:ntscf:acf:tsnrtps_acf:rtps
eth:ethertype:vlan:ethertype:ieee1722:tsnrtps_stream:rtps
```

A capture made against a talker with the pre-2026 `avtpcon` NTSCF bug decodes
too. That bug wrote `ntscf_data_length` into the wrong bit field, and Wireshark
--- correctly --- will not walk into a payload the header says is two octets
long, so the ACF message is never reached and the frame shows no RTPS at all.
When the declared length disagrees with the frame, this dissector decodes the
control header itself rather than chaining, and marks the frame:

```
ntscf_data_length 2, frame carries 544
NTSCF length is wrong; RTPS recovered from the fixed header offsets
```

Such frames appear as `ieee1722:tsnrtps_ctrl:rtps`, without the `ntscf` and
`acf` layers. If you see that on live traffic, the talker is running an
`avtpcon` that predates the fix.

The Info column names the builtin endpoint each message belongs to, so discovery
traffic can be read at a glance:

```
INFO_TS, DATA(p), Unknown[80]                   [SPDP, control port 7400]
INFO_TS, DATA(p), Unknown[80]                   [SPDP, control port 7410]
INFO_DST, INFO_TS, DATA(w) -> ControlCommand    [SEDP-pub, control port 7410]
INFO_DST, ACKNACK, Unknown[80]                  [SEDP-sub, control port 7410]
INFO_DST, INFO_TS, DATA -> ControlCommand       [stream port 7401]
```

The label comes from the RTPS writer entity ID, not the logical port, because
the port cannot tell SPDP from SEDP on its own --- both lines above say `SPDP`,
one on 7400 and one on 7410.

That is not a stray reply. Once a participant has been discovered, Fast DDS adds
its metatraffic unicast locator to the SPDP writer's destination list, so every
periodic announcement afterwards goes to the multicast address *and* separately
to each known peer, landing on the same port that carries SEDP. The two copies
are the same sample: same writer, same sequence number, sent within a
millisecond of each other.

```
 7  2.9760  -> 01:00:5e:7f:00:01 :7400  seq=1   first announcement, multicast only
14  2.9961  -> 82:a5:0d:66:eb:6f :7410  seq=1   peer now known
30  3.0761  -> 82:a5:0d:66:eb:6f :7410  seq=1   from here on, every tick is sent twice
31  3.0762  -> 01:00:5e:7f:00:01 :7400  seq=1
```

So discovery costs one frame per announcement plus one more per known
participant, which is worth remembering when sizing a control stream: with the
default 100 ms announcement period and N peers, that is 10*(N+1) frames per
second per participant.

`SEDP-topic`, `WLP` and `TypeLookup-req`/`-rep` are labelled too; user-defined
writers are left unlabelled so the column stays quiet for ordinary data.

Useful filters:

| Filter                             | Selects                                              |
|------------------------------------+------------------------------------------------------|
| `tsnrtps`                          | every RTPS-carrying 1722 frame                       |
| `rtps.sm.wrEntityId == 0x000100c2` | SPDP, on either port                                 |
| `rtps.sm.wrEntityId in             | SEDP, publications and subscriptions                 |
| {0x000003c2, 0x000004c2}`          |                                                      |
| `tsnrtps.dst_port == 7400`         | the metatraffic multicast channel                    |
| `tsnrtps.stream_uid == 1`          | frames carrying a given stream ID, stream or control |
| `ieee1722.subtype == 0x7f`         | stream framing, i.e. CNC-provisioned traffic         |
| `ieee1722.subtype == 0x82`         | control framing, i.e. discovery and fallback         |

If the transport is configured for a different subtype or ACF message type,
change the matching preference under *Protocols > TSNRTPS*; the dissector
re-registers itself when the preference changes.

## Known deviations from the specification

- **Default multicast address.** Subclause A.6.1.4.1 gives it as
  `01:00:5E::EF:FF:00:01`, which is not a well-formed MAC address. It is read
  here as the IPv4 multicast MAC for the UDP/IP PSM's default address
  239.255.0.1 under RFC 1112, giving `01:00:5e:7f:00:01`. Override it with
  `TSNTransportDescriptor::default_multicast_mac` to interoperate with an
  implementation that reads it differently.

- **`Locator_t::create_locator()`.** Fast DDS sets `address[0]` to `0xFF` for
  Ethernet locators, while Table A.1 requires the leading ten octets to be zero.
  That byte goes on the wire in discovery. `EthernetLocator::create_locator()`,
  which this transport uses throughout, follows the specification.

- **EtherType.** Frames use `0x22F0` (IEEE 1722). The specification registers no
  EtherType for RTPS and only notes that a future version may.

- **A 2-octet header in front of the RTPS message.** Subclause A.5 is explicit
  that there should be nothing there:

  > When RTPS operates over Ethernet, a Message is the contents (payload) of
  > exactly one Ethernet frame.

  This implementation puts a `destination_logical_port` there.

  The reason is that Annex A does not close its own loop. A.6.1 requires
  Endpoints to use the logical port expressions of [DDSI-RTPS] Tables 9.8 and
  9.9, and Table A.1 carries the port inside the locator, so a receiver is
  expected to distinguish a participant's metatraffic channel from its user
  traffic --- both of which arrive at the same MAC address. Yet A.5 leaves no
  field in which the port could travel. Something has to give, and this
  implementation chose to add the port rather than collapse the channels.

  The stream ID cannot stand in for it. A talker is keyed on destination MAC,
  VLAN and PCP alone, so one stream serves every logical port heading to the same
  address: in the no-CNC fallback SPDP and user multicast share the default
  multicast MAC and therefore one stream ID, and under RELIABLE QoS a reader's
  ACKNACKs reach the writer's user port at the MAC already carrying SEDP.

  Nothing else is carried. A.4.1 says a Message's length "is not sent explicitly
  by the DDSI-RTPS protocol", and it is not sent here either: an RTPS message is
  always a multiple of 4 octets, so `stream_data_length` bounds it exactly under
  a stream subtype, and under a control subtype the 2-octet ACF header plus this
  2-octet header leave the ACF message quadlet-aligned, making `acf_msg_length`
  exact too. A source port is not carried because RTPS replies to the locators a
  peer announced in discovery, not to where a message arrived from.

  A future revision of [DDS-TSN] that defines an RTPS EtherType would presumably
  also say how the logical port travels. Until then, treat this header as a local
  convention: both ends of a link must run this transport.

## Adapting to a future revision

Version 1.0 of [DDS-TSN] leaves enough unsaid that two independent
implementations are unlikely to interoperate: A.5 provides no field for the
logical port that A.6.1 requires, A.6.1.4.1 gives a multicast address that is not
a well-formed MAC address, and no EtherType is registered. Every deviation listed
above follows from one of those gaps. The aim here is therefore not
interoperability today, but staying cheap to correct when the specification
settles.

What that costs, per decision:

| Decision | To change it | Cost |
|---|---|---|
| Stream and control subtypes, ACF message type | `stream_subtype`, `control_subtype`, `acf_message_type` | runtime, no rebuild |
| Default multicast MAC, VLAN, PCP | `default_multicast_mac`, `default_vlan_id`, `default_pcp` | runtime, no rebuild |
| Framing header sizes | `TsnFraming` | one struct |
| The 2-octet TSN-RTPS header | `TsnRtpsHeader` plus its two call sites in `AvtpStream` | one file, two uses |
| Locator layout (Table A.1) | `EthernetLocator` | one class, public API |
| EtherType `0x22F0` | not ours --- `avtpcon` sets it | a registered RTPS EtherType would drop the 1722 framing entirely, and with it most of this transport's reason to exist in its present shape |

The framing sizes are worth a note. `AvtpStream` sizes its buffers per stream
from `avtpcon_get_max_payload_size()`, which is authoritative, while
`TSNTransport` caps the participant's message size up front from the interface
MTU --- before any stream exists. Those are two expressions of the same framing,
and they were two independent sets of literals until one of them was left behind
by a change to the header size. They now share `TsnFraming`, so a future revision
moves one struct rather than hunting for constants.

What is deliberately *not* abstracted: there is no version negotiation and no
capability exchange. Both ends of a link must run the same build. Adding a
framing-version field would be speculation about a specification that does not
exist yet, and the field itself would become another deviation to unwind.

## References

| Tag | Document |
|---|---|
| [DDS-TSN] | OMG, *DDS Extensions for Time Sensitive Networking*, v1.0 beta, ptc/2023-03-03 |
| [DDSI-RTPS] | OMG, *Real-Time Publish-Subscribe Protocol DDS Interoperability Wire Protocol*, v2.5, formal/2022-04-01 |
| [1722] | IEEE Std 1722-2025, *Standard for a Transport Protocol for Time-Sensitive Applications in Bridged Local Area Networks* |
| [802.1Qcc] | IEEE Std 802.1Qcc-2018, and the `ieee802-dot1q-cnc-config` YANG module it defines |
