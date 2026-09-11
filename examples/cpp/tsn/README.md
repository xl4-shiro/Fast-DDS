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
  v
+-------------------------------------------------------------+
| Ethernet: dst MAC | src MAC | 802.1Q tag (VID, PCP) | 0x22F0 |
+-------------------------------------------------------------+
| AVTP stream header, 24 octets:                               |
|   subtype 0x7F (EF_STREAM) | sv | tv | sequence_num          |
|   stream_id (8) | avtp_timestamp (4)                         |
|   stream_data_length (2)                                     |
+-------------------------------------------------------------+
| dst logical port (2) | src logical port (2) | rtps_length(2) |
+-------------------------------------------------------------+
| RTPS message                                                 |
+-------------------------------------------------------------+
```

IEEE 1722 registers no subtype for RTPS, so the Experimental Format Stream
(0x7F) is the honest choice. A *stream* subtype is used rather than a control
format because its header carries two things RTPS benefits from: an
`avtp_timestamp`, filled per frame from the gPTP-disciplined clock, and an
explicit `stream_data_length`.

The 6-octet header between the AVTP header and the RTPS message covers a gap
Annex A leaves open: **the logical ports**. Table A.1 puts the RTPS logical port
in the locator, but the Ethernet frame has nowhere to carry it, and a
participant's metatraffic and user traffic arrive at the same MAC address.
Without the port on the wire there is no way to tell those channels apart.
`rtps_length` duplicates `stream_data_length` here, and is kept so the receiver
can validate what it got and so the control-format framing below shares the same
header.

### Control-format framing

Setting `avtp_subtype` to NTSCF (0x82) or TSCF (0x06) instead wraps the same
payload in an ACF message of type `acf_message_type` (`ACF_USER0` by default),
which is where IEEE 1722 defines ACF messages to live. That trades the timestamp
away, but lets RTPS share a frame with other ACF traffic such as `ACF_CAN`.
There, `rtps_length` is load-bearing: an ACF message is padded to a quadlet
boundary and `ACF_USERn` carries no payload length, so trailing padding would
otherwise be indistinguishable from submessage data. The 6-octet header is sized
so that, with the 2-octet ACF header, an RTPS message needs no padding at all.

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

Set `require_accepted_streams` (`--require-streams`) to refuse sending on a
stream the CNC has not accepted, rather than falling back.

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
message size to what actually fits — roughly 1466 octets on a 1500-octet MTU
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
