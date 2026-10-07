#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
"""Inject a PDU Session Resource Modify Indication into the SMF over N11.

The AMF in oai-cn5g-amf v2.2.1 has no handler for NGAP procedure 27
(PDUSessionResourceModifyIndication): it decodes the message, finds no branch
for it and drops it, so nothing reaches the SMF.  Until that is implemented,
this script stands in for the AMF: it posts the UpdateSMContext request the AMF
would have sent, carrying the very N2 SM information the CU-CP put on the wire.

It does not guess the shape of that request.  The SMF logs the raw body of
every UpdateSMContext it receives at debug level ("Message content"), so the
script reads the last real one out of the container log and reuses its MIME
envelope verbatim -- boundary, part order, part headers -- replacing only the
n2SmInfoType in the JSON part and the bytes of the NGAP part.

  # move the downlink to CU-UP1 (192.168.71.161), TEID from the CU-CP log
  ./cuup-reloc-inject-mod-ind.py --addr 192.168.71.161 --teid 0x6d60de2c

  # or let it read the endpoint out of the CU-CP's own log line
  ./cuup-reloc-inject-mod-ind.py --from-cucp-log

Exit status is 0 only if the SMF answered 200.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from ipaddress import IPv4Address

SMF = "rfsim5g-oai-smf"
CUCP = "rfsim5g-oai-cucp"


# --------------------------------------------------------------------------
# PDUSessionResourceModifyIndicationTransfer (TS 38.413 9.3.4.8), aligned PER
# --------------------------------------------------------------------------
#
# The layout below is not read off the ASN.1 by hand, it is the layout the
# CU-CP's asn1c encoder actually produced, confirmed bit by bit against the N2
# capture of a real change of gNB-CU-UP:
#
#   00 0f 80  c0 a8 47 a1  6d 60 de 2c  00 01
#   `------'  `---------'  `---------'  `---'
#      |          |             |         `- associatedQosFlowList, one QFI 1
#      |          |             `----------- GTP-TEID 0x6d60de2c
#      |          `------------------------- transportLayerAddress 192.168.71.161
#      `------------------------------------ 9 preamble bits, then the address
#                                            length (32, as 8 bits of 31), then
#                                            7 bits of padding to the octet
#
# The nine preamble bits are, in order: the transfer SEQUENCE's extension bit
# and its three optional-field bits (additionalDLQosFlowPerTNLInformation,
# secondaryRATUsageInformation, iE-Extensions), QosFlowPerTNLInformation's
# extension bit and its iE-Extensions bit, the UPTransportLayerInformation
# CHOICE index (0 = gTPTunnel), and GTPTunnel's extension and iE-Extensions
# bits.  All zero: no extensions, no optional fields present.


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, value, width):
        for i in range(width - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def bytes(self):
        while len(self.bits) % 8:
            self.bits.append(0)
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            byte = 0
            for bit in self.bits[i : i + 8]:
                byte = (byte << 1) | bit
            out.append(byte)
        return bytes(out)


def encode_transfer(addr, teid, qfis):
    """Build a PDUSessionResourceModifyIndicationTransfer."""
    if not 1 <= len(qfis) <= 64:
        raise ValueError("associatedQosFlowList holds 1..64 items")

    w = BitWriter()
    w.put(0, 9)  # the nine preamble bits described above
    w.put(32 - 1, 8)  # transportLayerAddress: BIT STRING length 32
    head = w.bytes()  # octet-aligns: the address starts on a byte boundary
    assert head == b"\x00\x0f\x80", head.hex()

    t = BitWriter()
    t.put(len(qfis) - 1, 6)  # SEQUENCE (SIZE(1..maxnoofQosFlows)) OF ...
    for qfi in qfis:
        t.put(0, 3)  # item extension bit + qosFlowMappingIndication + iE-Ext
        t.put(0, 1)  # QosFlowIdentifier extension bit
        t.put(qfi, 6)  # QosFlowIdentifier INTEGER (0..63)

    return head + IPv4Address(addr).packed + teid.to_bytes(4, "big") + t.bytes()


# --------------------------------------------------------------------------
# the envelope, taken from a real AMF request in the SMF's own log
# --------------------------------------------------------------------------


def docker_logs(container):
    r = subprocess.run(
        ["docker", "logs", container], capture_output=True, check=False
    )
    if r.returncode != 0:
        sys.exit(
            "could not read the log of %s: %s"
            % (container, r.stderr.decode("utf-8", "replace").strip())
        )
    # the bodies hold raw NGAP octets, so never decode as UTF-8
    return r.stdout.decode("latin-1")


def last_envelope(log):
    """Return (boundary, json_headers, ngap_headers, closing) from the log.

    OAI's logger emits the whole body in one call, so the first line carries
    the usual timestamp prefix and the rest of the body does not.  A block
    therefore runs from a "Message content" line to the next line that starts
    with a log timestamp.
    """
    blocks = []
    lines = log.split("\n")
    stamp = re.compile(r"^\[?\d{4}-\d\d-\d\d")
    for i, line in enumerate(lines):
        if "Message content" not in line:
            continue
        body = []
        for nxt in lines[i + 1 :]:
            if stamp.match(nxt):
                break
            body.append(nxt)
        text = "\n".join(body)
        if "n2SmInfoType" in text:
            blocks.append(text)
    if not blocks:
        sys.exit(
            "no UpdateSMContext body with an n2SmInfoType in the SMF log.\n"
            "Attach the UE first: the PDU Session Resource Setup Response is\n"
            "the request this script borrows its envelope from.  Check that\n"
            "log_level.general is debug in mini_nonrf_config_3slices.yaml."
        )

    text = blocks[-1]
    m = re.search(r"^(--[^\r\n]+)\r?$", text, re.M)
    if not m:
        sys.exit("could not find a MIME boundary in:\n" + text[:2000])
    boundary = m.group(1)

    parts = text.split(boundary)
    headers = []
    for part in parts[1:]:
        if part.startswith("--"):  # the closing delimiter
            break
        head = re.split(r"\r?\n\r?\n", part.lstrip("\r\n"), maxsplit=1)[0]
        headers.append(head)
    if len(headers) < 2:
        sys.exit(
            "expected at least a JSON part and an NGAP part, found %d in:\n%s"
            % (len(headers), text[:2000])
        )

    json_head, ngap_head = headers[0], None
    for head in headers[1:]:
        if "ngap" in head.lower():
            ngap_head = head
    if ngap_head is None:
        sys.exit(
            "no part with an NGAP content type; the borrowed request carried\n"
            "only: " + " | ".join(h.replace("\n", " ") for h in headers)
        )
    return boundary, json_head, ngap_head


def last_smf_ref(log):
    refs = re.findall(r"smf_ref (\S+), method modify", log)
    if not refs:
        refs = re.findall(r"smf_ref (\S+), method", log)
    if not refs:
        sys.exit("no 'smf_ref ... method' line in the SMF log; pass --ref")
    return refs[-1]






# --------------------------------------------------------------------------


def from_cucp_log():
    log = docker_logs(CUCP)
    teids = re.findall(r"TEID\s+0x([0-9a-fA-F]{8})", log)
    addrs = re.findall(r"(?:address|addr)\s+(\d+\.\d+\.\d+\.\d+)", log)
    if not teids:
        sys.exit(
            "could not find a TEID in the CU-CP log; read the\n"
            "'Send PDU Session Resource Modify Indication' lines yourself and\n"
            "pass --addr/--teid"
        )
    return (addrs[-1] if addrs else None), int(teids[-1], 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--addr", help="downlink endpoint the NG-RAN moved to")
    ap.add_argument("--teid", help="downlink GTP-U TEID, e.g. 0x6d60de2c")
    ap.add_argument(
        "--qfi", type=lambda s: int(s, 0), action="append", default=[],
        help="associated QoS flow (repeatable, default 1)",
    )
    ap.add_argument("--from-cucp-log", action="store_true",
                    help="read the endpoint out of the CU-CP container log")
    ap.add_argument("--ref", help="smContextRef (default: last one in the log)")
    ap.add_argument("--host", default="192.168.71.133", help="SMF SBI address")
    ap.add_argument("--port", default="80")
    ap.add_argument("--api", default="v1", help="sbi_api_version")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the request instead of sending it")
    args = ap.parse_args()

    if args.from_cucp_log:
        addr, teid = from_cucp_log()
        addr = args.addr or addr
        teid = int(args.teid, 0) if args.teid else teid
        if not addr:
            sys.exit("the CU-CP log gave a TEID but no address; pass --addr")
    else:
        if not args.addr or not args.teid:
            sys.exit("pass --addr and --teid, or --from-cucp-log")
        addr, teid = args.addr, int(args.teid, 0)

    qfis = args.qfi or [1]
    transfer = encode_transfer(addr, teid, qfis)
    print("N2 SM information (%d bytes): %s" % (len(transfer), transfer.hex(" ")))
    print("  downlink -> %s, TEID 0x%08x, QFI %s"
          % (addr, teid, ",".join(str(q) for q in qfis)))

    log = docker_logs(SMF)
    boundary, json_head, ngap_head = last_envelope(log)
    ref = args.ref or last_smf_ref(log)
    print("borrowed envelope: boundary %r" % boundary)
    print("smContextRef: %s" % ref)

    body = json.dumps(
        {"n2SmInfoType": "PDU_RES_MOD_IND",
         "n2SmInfo": {"contentId": "n2msg"}}
    ).encode()

    crlf = b"\r\n"
    out = bytearray()
    out += boundary.encode("latin-1") + crlf
    out += json_head.replace("\r", "").replace("\n", "\r\n").encode("latin-1")
    out += crlf + crlf + body + crlf
    out += boundary.encode("latin-1") + crlf
    out += ngap_head.replace("\r", "").replace("\n", "\r\n").encode("latin-1")
    out += crlf + crlf + transfer + crlf
    out += boundary.encode("latin-1") + b"--" + crlf

    ctype = "multipart/related; boundary=%s" % boundary.lstrip("-")
    url = "http://%s:%s/nsmf-pdusession/%s/sm-contexts/%s/modify" % (
        args.host, args.port, args.api, ref)

    print("\nPUT %s" % url)
    print("Content-Type: %s" % ctype)
    print(bytes(out).decode("latin-1"))

    if args.dry_run:
        return 0
    if not shutil.which("curl"):
        sys.exit("curl is not installed")

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(out)
        path = f.name

    # http_version in mini_nonrf_config_3slices.yaml is 1, so HTTP/1.1 is the
    # expected answer; try prior-knowledge HTTP/2 only if that is refused.
    for extra, label in (([], "HTTP/1.1"), (["--http2-prior-knowledge"], "HTTP/2")):
        cmd = ["curl", "-sS", "-i", "-X", "PUT", url,
               "-H", "Content-Type: " + ctype,
               "--data-binary", "@" + path] + extra
        r = subprocess.run(cmd, capture_output=True)
        head = r.stdout.decode("latin-1", "replace")
        print("\n--- %s ---\n%s" % (label, head.strip() or r.stderr.decode()))
        if r.returncode == 0 and head.strip():
            code = head.split()[1] if len(head.split()) > 1 else "?"
            if code.startswith("2"):
                print("\nSMF accepted it (%s)." % code)
                return 0
            print("\nSMF refused it (%s); the log below says why." % code)
            print("\n".join(docker_logs(SMF).split("\n")[-25:]))
            return 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
