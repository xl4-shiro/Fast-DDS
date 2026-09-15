--
-- Copyright (C) 2026 Excelfore Corporation
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--
-- Wireshark dissector for RTPS carried over IEEE 1722, as the Fast DDS
-- DDS-TSN transport frames it.
--
--     tshark -X lua_script:rtps_over_1722.lua -r capture.pcap
--     wireshark -X lua_script:rtps_over_1722.lua capture.pcap
--
-- Or drop it in ~/.local/lib/wireshark/plugins/ to load it always.
--
-- Two framings are decoded, matching the transport:
--
--   stream subtype (EF_STREAM 0x7F by default), for CNC-provisioned streams:
--     [ AVTP stream header, 24 octets ][ TSN-RTPS header, 2 ][ RTPS message ]
--
--   control format (NTSCF 0x82 / TSCF 0x06), for discovery and anything with
--   no provisioned stream:
--     [ control header, 12 ][ ACF header, 2 ][ TSN-RTPS header, 2 ][ RTPS ]
--
-- The RTPS message itself is handed to Wireshark's own "rtps" dissector, so
-- submessages, GUIDs and QoS decode as usual.
--
-- This hangs off the subdissector tables the built-in ieee1722 dissector
-- already offers, rather than claiming EtherType 0x22F0:
--
--   ieee1722.subtype  for the stream subtype, which the built-in parses far
--                     enough to read the subtype and then leaves as data, and
--                     for the control subtype, where we chain straight into
--                     the built-in NTSCF dissector before adding anything
--   acf.msg_type      for ACF_USER0, after NTSCF and the ACF header are decoded
--
-- So the built-in keeps ownership of everything it already understands, its
-- ieee1722.*, ntscf.* and acf.* fields stay populated for filters and colouring
-- rules, and a mixed AVB/TSN capture is otherwise untouched.
--
-- One wrinkle needs the control subtype handled rather than left alone. A
-- talker with the pre-2026 avtpcon NTSCF bug writes ntscf_data_length into the
-- wrong bit field, so the built-in dissector -- correctly -- refuses to walk
-- into a payload the header says is two octets long, and the ACF message is
-- never reached. Captures taken against such a talker would otherwise show no
-- RTPS at all. When the declared length disagrees with the frame, this
-- dissector falls back to reading the ACF and TSN-RTPS headers at their fixed
-- offsets and flags the frame, so old captures stay readable.
--

local p_tsn = Proto("tsnrtps", "RTPS over IEEE 1722")

-- The subtype and ACF message type are preferences, because the transport
-- makes both configurable.
p_tsn.prefs.stream_subtype = Pref.uint("Stream subtype", 0x7F,
    "AVTP subtype carrying RTPS on a provisioned stream (EF_STREAM 0x7F, VSF 0x6F)")
p_tsn.prefs.control_subtype = Pref.uint("Control subtype", 0x82,
    "AVTP subtype carrying RTPS with no provisioned stream (NTSCF 0x82, TSCF 0x06)")
p_tsn.prefs.acf_msg_type = Pref.uint("ACF message type", 0x78,
    "ACF message type carrying RTPS inside a control-format PDU (ACF_USER0 0x78)")

local f = {
    tv         = ProtoField.bool("tsnrtps.tv", "Timestamp Valid", 8, nil, 0x01),
    seq        = ProtoField.uint8("tsnrtps.seq", "Sequence Number", base.DEC),
    stream_id  = ProtoField.bytes("tsnrtps.stream_id", "Stream ID"),
    stream_mac = ProtoField.ether("tsnrtps.stream_mac", "Stream ID: talker MAC"),
    stream_uid = ProtoField.uint16("tsnrtps.stream_uid", "Stream ID: unique ID", base.DEC),
    timestamp  = ProtoField.uint32("tsnrtps.timestamp", "AVTP Timestamp", base.DEC),
    data_len   = ProtoField.uint16("tsnrtps.data_length", "Stream Data Length", base.DEC),
    acf_type   = ProtoField.uint8("tsnrtps.acf_type", "ACF Message Type", base.HEX),
    acf_quads  = ProtoField.uint16("tsnrtps.acf_quadlets", "ACF Length (quadlets)", base.DEC),
    dport      = ProtoField.uint16("tsnrtps.dst_port", "Destination logical port", base.DEC),
    rtps_len   = ProtoField.uint32("tsnrtps.rtps_length", "RTPS length (derived)", base.DEC),
}
p_tsn.fields = f

local e_len = ProtoExpert.new("tsnrtps.length_mismatch", "Declared length disagrees with the frame",
    expert.group.MALFORMED, expert.severity.WARN)
local e_rtps = ProtoExpert.new("tsnrtps.not_rtps", "Payload does not start with the RTPS magic",
    expert.group.MALFORMED, expert.severity.WARN)
local e_recovered = ProtoExpert.new("tsnrtps.recovered",
    "NTSCF length is wrong; RTPS recovered from the fixed header offsets",
    expert.group.MALFORMED, expert.severity.NOTE)
p_tsn.experts = { e_len, e_rtps, e_recovered }

local rtps_dissector = nil

-- Which builtin endpoint a message belongs to, keyed by RTPS entity ID. Every
-- submessage that matters names a writer --- DATA, HEARTBEAT and ACKNACK alike,
-- since an ACKNACK names the writer it acknowledges --- so reading the writer
-- ID is enough to tell SPDP from SEDP without guessing from the logical port.
-- The port cannot do it: SPDP appears on both the metatraffic multicast port
-- and, as the unicast reply to an announcement, on the metatraffic unicast port
-- that also carries SEDP.
--
-- Reader IDs are listed alongside the writers so the same table can be reused
-- if the reader ID is ever wanted.
local builtin_endpoints = {
    [0x000100c2] = "SPDP",           [0x000100c7] = "SPDP",
    [0x000003c2] = "SEDP-pub",       [0x000003c7] = "SEDP-pub",
    [0x000004c2] = "SEDP-sub",       [0x000004c7] = "SEDP-sub",
    [0x000002c2] = "SEDP-topic",     [0x000002c7] = "SEDP-topic",
    [0x000200c2] = "WLP",            [0x000200c7] = "WLP",
    [0x000300c3] = "TypeLookup-req", [0x000300c4] = "TypeLookup-req",
    [0x000301c3] = "TypeLookup-rep", [0x000301c4] = "TypeLookup-rep",
}

-- Read back from the tree the RTPS dissector has just built, rather than
-- walking the submessages again here.
local f_wr_entity = Field.new("rtps.sm.wrEntityId")

--- Name the builtin endpoints a message touches, e.g. "SPDP" or "SEDP-pub".
--- One RTPS message may batch submessages from several writers, so this can
--- name more than one; user-defined writers are left unnamed, which keeps the
--- column quiet for ordinary data.
--- @return string, empty when nothing builtin was found
local function builtin_kinds()
    local seen, names = {}, {}
    for _, fi in ipairs({ f_wr_entity() }) do
        local name = builtin_endpoints[fi.value]
        if name and not seen[name] then
            seen[name] = true
            names[#names + 1] = name
        end
    end
    return table.concat(names, "+")
end

--- Add the 2-octet header the transport puts in front of every RTPS message,
--- and hand the message itself to the RTPS dissector.
---
--- The header carries only the destination logical port. The length is not on
--- the wire: an RTPS message is always a multiple of 4 octets, so the framing
--- around it bounds it exactly --- stream_data_length under a stream subtype,
--- and acf_msg_length under a control subtype, where the 2-octet ACF header
--- plus this 2-octet header keep the ACF message quadlet-aligned with no
--- padding.
---
--- @param offset  where the TSN-RTPS header starts within tvb
--- @param framing "stream" or "control", for the info column
--- @param quiet   when true, give up silently rather than flagging the frame.
---                The recovery path guesses at offsets in a frame already known
---                to be malformed, so a miss there says nothing worth reporting
---                --- and the frame may not be ours at all.
--- @return true when an RTPS message was decoded
local function dissect_rtps_payload(tvb, pinfo, tree, offset, framing, quiet)
    local rtps_len = tvb:len() - (offset + 2)
    if rtps_len < 20 then
        if not quiet then
            tree:add_proto_expert_info(e_len, "Truncated before the RTPS message")
        end
        return false
    end

    local hdr = tree:add(p_tsn, tvb(offset, 2), "TSN-RTPS header")
    hdr:add(f.dport, tvb(offset, 2))
    hdr:add(f.rtps_len, tvb(offset + 2, rtps_len), rtps_len):set_generated()

    local rtps_tvb = tvb(offset + 2, rtps_len):tvb()
    if rtps_tvb(0, 4):string() ~= "RTPS" then
        if not quiet then
            tree:add_proto_expert_info(e_rtps)
        end
        return false
    end

    if rtps_dissector then
        rtps_dissector:call(rtps_tvb, pinfo, tree)
    end

    -- After the call, not before: the RTPS dissector sets both columns from
    -- scratch, so anything written here first is discarded.
    pinfo.cols.protocol:set("RTPS/1722")
    local kinds = builtin_kinds()
    if kinds ~= "" then
        pinfo.cols.info:append(string.format("  [%s, %s port %d]",
            kinds, framing, tvb(offset, 2):uint()))
    else
        pinfo.cols.info:append(string.format("  [%s port %d]",
            framing, tvb(offset, 2):uint()))
    end
    return true
end

-- The stream framing. Reached through ieee1722.subtype, which hands over the
-- whole AVTP PDU starting at the subtype octet, so the 24-octet stream header
-- is still in front of us. The built-in has already added subtype, sv and
-- version; the rest of the header is ours.
local p_stream = Proto("tsnrtps_stream", "RTPS over IEEE 1722 (stream format)")

function p_stream.dissector(tvb, pinfo, tree)
    if tvb:len() < 24 then
        return 0
    end

    local t = tree:add(p_tsn, tvb(), "RTPS over IEEE 1722 (stream format)")
    t:add(f.tv, tvb(1, 1))
    t:add(f.seq, tvb(2, 1))

    local sid = t:add(f.stream_id, tvb(4, 8))
    sid:add(f.stream_mac, tvb(4, 6))
    sid:add(f.stream_uid, tvb(10, 2))

    t:add(f.timestamp, tvb(12, 4))
    local dl = t:add(f.data_len, tvb(20, 2))
    local declared = tvb(20, 2):uint()
    if declared ~= tvb:len() - 24 then
        dl:add_proto_expert_info(e_len,
            string.format("stream_data_length %d, frame carries %d", declared, tvb:len() - 24))
    end

    dissect_rtps_payload(tvb, pinfo, t, 24, "stream")
    return tvb:len()
end

-- The control framing, good path. Reached through acf.msg_type, by which point
-- NTSCF (or TSCF) and the ACF header are decoded and trimmed off, so the
-- TSN-RTPS header is at offset 0 and there is nothing left to check that the
-- built-in has not already checked.
local p_acf = Proto("tsnrtps_acf", "RTPS over IEEE 1722 (ACF payload)")

-- Set when the good path runs, so the recovery below knows to stay out of the
-- way. Reset for every control-format frame before NTSCF is called.
local acf_path_taken = false

function p_acf.dissector(tvb, pinfo, tree)
    acf_path_taken = true
    local t = tree:add(p_tsn, tvb(), "RTPS over IEEE 1722 (control format)")
    dissect_rtps_payload(tvb, pinfo, t, 0, "control", false)
    return tvb:len()
end

-- The control framing, entry point. Registered on ieee1722.subtype so that the
-- built-in NTSCF dissector can be chained explicitly: called this way it
-- decodes exactly as it would have on its own, including its own expert infos,
-- and dispatches ACF_USER0 to p_acf above.
local p_ctrl = Proto("tsnrtps_ctrl", "RTPS over IEEE 1722 (control format)")

local ntscf_dissector = nil

--- Where the ACF message starts, for a control PDU of the given subtype.
--- NTSCF has a 12-octet header; TSCF carries a timestamp and a format-specific
--- word as well, for 24.
local function control_header_size(subtype)
    if subtype == 0x06 then
        return 24
    end
    return 12
end

function p_ctrl.dissector(tvb, pinfo, tree)
    if tvb:len() < 12 then
        return 0
    end

    local subtype = tvb(0, 1):uint()
    local hdr_size = control_header_size(subtype)
    local declared = nil
    if subtype == 0x82 then
        declared = bit.band(tvb(1, 2):uint(), 0x07FF)
    end

    -- The frame is well formed, or is a subtype whose length we do not check:
    -- hand it straight to the built-in, which decodes the header and the ACF
    -- message and dispatches ACF_USER0 back to p_acf above. The result is what
    -- stock Wireshark would have produced, plus the RTPS layer.
    if declared == nil or declared == tvb:len() - hdr_size then
        acf_path_taken = false
        if ntscf_dissector then
            ntscf_dissector:call(tvb, pinfo, tree)
        end
        return tvb:len()
    end

    -- The declared length is wrong, so decode it here instead of chaining.
    -- The built-in cannot be called first and corrected afterwards: it trusts
    -- ntscf_data_length and calls set_actual_length(), which shortens the tvb
    -- to the length the broken header claims and puts the rest of the frame --
    -- the ACF message and the RTPS payload -- permanently out of reach.
    local t = tree:add(p_tsn, tvb(), "RTPS over IEEE 1722 (control format, recovered)")
    local dl = t:add(f.data_len, tvb(1, 2), declared)
    dl:add_proto_expert_info(e_len,
        string.format("ntscf_data_length %d, frame carries %d", declared, tvb:len() - hdr_size))
    t:add(f.seq, tvb(3, 1))

    local sid = t:add(f.stream_id, tvb(4, 8))
    sid:add(f.stream_mac, tvb(4, 6))
    sid:add(f.stream_uid, tvb(10, 2))

    if tvb:len() < hdr_size + 2 then
        return tvb:len()
    end

    -- ACF header: msg_type(7) length_in_quadlets(9).
    local acf = tvb(hdr_size, 2):uint()
    local msg_type = bit.rshift(acf, 9)
    local a = t:add(p_tsn, tvb(hdr_size, 2), "ACF message")
    a:add(f.acf_type, tvb(hdr_size, 1), msg_type)
    a:add(f.acf_quads, tvb(hdr_size, 2), bit.band(acf, 0x01FF))

    if msg_type ~= p_tsn.prefs.acf_msg_type then
        return tvb:len()
    end

    if dissect_rtps_payload(tvb, pinfo, t, hdr_size + 2, "control", false) then
        t:add_proto_expert_info(e_recovered)
    end
    return tvb:len()
end


function p_tsn.init()
    rtps_dissector = Dissector.get("rtps")
    ntscf_dissector = Dissector.get("ntscf")
end

local subtype_table = DissectorTable.get("ieee1722.subtype")
local acf_table = DissectorTable.get("acf.msg_type")
local registered = {}

--- Register on whichever subtypes and ACF message type the preferences name,
--- dropping any earlier registration first. Called once at load and again
--- whenever the preferences change.
local function register()
    for _, r in ipairs(registered) do
        r.table:remove(r.value, r.proto)
    end
    registered = {
        { table = subtype_table, value = p_tsn.prefs.stream_subtype,  proto = p_stream },
        { table = subtype_table, value = p_tsn.prefs.control_subtype, proto = p_ctrl },
        { table = acf_table,     value = p_tsn.prefs.acf_msg_type,    proto = p_acf },
    }
    for _, r in ipairs(registered) do
        r.table:add(r.value, r.proto)
    end
end

p_tsn.prefs_changed = register

register()
