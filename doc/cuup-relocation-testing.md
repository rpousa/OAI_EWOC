<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Testing the change of gNB-CU-UP (comnetsemu VM, Docker)

Ordered so that each stage can fail on its own terms: encoding first (no core, no
radio), then the build, then the live path. Stop at the first stage that fails —
each one rules out a whole class of problem for the next.

---

## 0. Host setup (once)

The build host is the **`comnetsemu` Vagrant VM**, not Windows and not WSL2. The
repository lives on the Windows Desktop and reaches the VM as a VirtualBox shared
folder:

```
C:\Users\Ricardo Pousa\Desktop\comnetsemu\...\OAI_EWOC   (Windows)
~/comnetsemu/examples/5g/OAI_EWOC                            (inside the VM)
```

### 0.1 Line endings

The tree was cloned on Windows with `core.autocrlf=true`, so every text file in
the working copy has CRLF endings while the blobs in `HEAD` have LF. Two
consequences, both of which have to be cleared before anything else:

* `git status` reports ~700 files as modified, which hides the ones that actually
  changed;
* `cmake_targets/build_oai` and the other shell scripts carry `\r` at the end of
  every line, so `bash` fails on them with `$'\r': command not found`. Since
  `docker/Dockerfile.build.ubuntu` runs `./build_oai`, the **image build fails**
  too, not just a local build.

Fix it once, from inside the VM, in this order — the save/restore around
`git checkout` is what keeps the CU-UP-relocation work:

```bash
cd ~/comnetsemu/examples/5g/OAI_EWOC

# 1. set the 27 files of this patch aside
cat > /tmp/cuup-files.txt <<'EOF'
CMakeLists.txt
ci-scripts/yaml_files/5g_rfsimulator_e1/reloc-override.yaml
common/utils/telnetsrv/telnetsrv_rrc.c
doc/cuup-relocation-testing.md
openair2/COMMON/f1ap_messages_types.h
openair2/COMMON/ngap_messages_def.h
openair2/COMMON/ngap_messages_types.h
openair2/E1AP/lib/e1ap_bearer_context_management.c
openair2/E1AP/tests/e1ap_lib_test.c
openair2/F1AP/lib/f1ap_ue_context.c
openair2/F1AP/tests/f1ap_lib_test.c
openair2/LAYER2/NR_MAC_gNB/mac_rrc_dl_handler.c
openair2/LAYER2/nr_pdcp/cucp_cuup_handler.c
openair2/RRC/NR/nr_rrc_defs.h
openair2/RRC/NR/nr_rrc_proto.h
openair2/RRC/NR/rrc_gNB.c
openair2/RRC/NR/rrc_gNB_NGAP.c
openair2/RRC/NR/rrc_gNB_NGAP.h
openair2/RRC/NR/rrc_gNB_cuup.c
openair2/RRC/NR/rrc_gNB_cuup_reloc.c
openair2/RRC/NR/rrc_gNB_cuup_reloc.h
openair3/NGAP/ngap_gNB.c
openair3/NGAP/ngap_gNB_handlers.c
openair3/NGAP/ngap_gNB_mobility_management.c
openair3/NGAP/ngap_gNB_mobility_management.h
openair3/NGAP/ngap_msg_includes.h
openair3/NGAP/tests/ngap_lib_test.c
EOF
tar -cf /tmp/cuup-files.tar -T /tmp/cuup-files.txt

# 2. stop git rewriting endings, for this repo
git config core.autocrlf false
git config core.eol lf

# 3. restore the whole tree from HEAD, now with LF
git checkout -- .

# 4. put our files back
tar -xf /tmp/cuup-files.tar

git status --short        # expect exactly 23 M + 4 ??
```

Step 3 discards every uncommitted change in the tree. Step 1 captured the
CU-UP-relocation files, but if anything else of yours is uncommitted, commit it
on a branch first.

If the VM's `git` is older than 2.10 and refuses `core.eol`, `core.autocrlf
false` alone is enough.

### 0.2 Where to build

Building directly in the shared folder works but is slow, and vboxsf does not
carry the executable bit reliably. Copy into the VM's own disk first:

```bash
cp -r ~/comnetsemu/examples/5g/OAI_EWOC ~/OAI_EWOC
cd ~/OAI_EWOC
chmod +x cmake_targets/build_oai ci-scripts/*.sh
```

Everything below runs from `~/OAI_EWOC`. Copy results back to the shared folder
only when you want them on Windows.

### 0.3 Resources

Give the VM (or Docker Desktop, if you build the images on Windows instead)
**8 GB RAM, 4 CPUs, 40 GB disk** minimum. The deployment below is a core plus
five RAN containers plus a UE.

Work on a branch, so `git diff` stays available as the record of what changed:

```bash
git checkout -b cuup-reloc
git add -A && git commit -m "inter-CU-UP relocation: TS 38.401 8.9.5"
```

---

## 1. Unit tests — encoding and decoding

This stage needs no core, no radio and no containers beyond the build image. It
is also the only stage that tests the new ASN.1 code directly, so run it first
and do not move on until it passes.

```bash
docker build . -f docker/Dockerfile.base.ubuntu  -t ran-base:latest
docker build . -f docker/Dockerfile.build.ubuntu -t ran-build:latest

docker run --rm -it -v ~/OAI_EWOC:/oai -w /oai ran-build:latest bash
# inside the container:
mkdir -p build && cd build
cmake .. -GNinja -DENABLE_TESTS=ON
ninja e1ap_lib_test f1ap_lib_test ngap_lib_test
ctest -R 'e1ap_lib_test|f1ap_lib_test|ngap_lib_test' --output-on-failure
```

What each one proves:

| Test | Covers |
|---|---|
| `e1ap_lib_test` | The new `nG-UL-UP-TNL-Information` in the E1 *PDU Session Resource To Modify* item survives encode → decode → copy → compare, **and** an item that carries only that endpoint and no DRB list — the shape a UPF change produces, and the one that used to dereference an absent list on decode. |
| `f1ap_lib_test` | The new **DRBs-ToBeModified** list on the F1 UE Context Modification Request: DRB ID and uplink endpoints round-trip. |
| `ngap_lib_test` | The **PDU Session Resource Modify Indication**'s mandatory IEs (downlink TNL and the associated QoS flow list) survive a roundtrip, and the **Modify Confirm** decoder picks up the uplink endpoint the core returns. |

Then the whole suite, which must not regress:

```bash
ctest -E '^physim\.|^benchmark_' --output-on-failure
```

> The Docker build compiles with `-Werror` (`docker/Dockerfile.build.ubuntu`).
> A warning that a plain local build tolerates will fail the image build in stage
> 2, so fix anything `ninja` reports here even if it is only a warning.

---

## 2. Images

```bash
cd ~/OAI_EWOC
docker build . -f docker/Dockerfile.gNB.ubuntu     -t oai-gnb:reloc
docker build . -f docker/Dockerfile.nr-cuup.ubuntu -t oai-nr-cuup:reloc
docker build . -f docker/Dockerfile.nrUE.ubuntu    -t oai-nr-ue:reloc
```

Rebuild all three after **every** code change. Docker layer caching will happily
serve a stale binary when only the final stage is rebuilt.

`libtelnetsrv_rrc.so` is already copied into the gNB image, so the trigger in
stage 4 needs no extra build step.

---

## 3. The test bed

Start from `ci-scripts/yaml_files/5g_rfsimulator_e1`: one CU-CP, three CU-UPs,
three DUs, three UEs. Two things have to change before it can exercise a
relocation.

**(a) The F1-U networks are isolated on purpose.** That deployment gives each
DU/CU-UP pair its own `f1u_N_net` precisely so that a UE only works if it landed
on the right CU-UP. A relocation needs DU1 to reach CU-UP2, so CU-UP2 has to join
`f1u_1_net` and bind its F1-U there.

**(b) The CU-CP needs the telnet server** to take the trigger.

Save this next to the compose file as `reloc-override.yaml`:

```yaml
services:
    oai-cucp:
        image: oai-gnb:reloc
        environment:
            USE_ADDITIONAL_OPTIONS: --log_config.global_log_options level,nocolor,time
                                    --gNBs.[0].E1_INTERFACE.[0].ipv4_cucp 192.168.77.2
                                    --gNBs.[0].local_s_address 192.168.72.2
                                    --telnetsrv --telnetsrv.shrmod rrc
                                    --telnetsrv.listenaddr 0.0.0.0
        ports:
            - "9090:9090"

    oai-cuup:
        image: oai-nr-cuup:reloc

    # CU-UP2 joins DU1's F1-U network and binds F1-U there, so that the DU can be
    # repointed at it. Its slice is set to the one UE1 uses, so that both CU-UPs
    # are legitimate choices for this UE.
    oai-cuup2:
        image: oai-nr-cuup:reloc
        environment:
            USE_ADDITIONAL_OPTIONS: --log_config.global_log_options level,nocolor,time
                                    --gNBs.[0].gNB_CU_UP_ID 0xe01
                                    --gNBs.[0].E1_INTERFACE.[0].ipv4_cucp 192.168.77.2
                                    --gNBs.[0].E1_INTERFACE.[0].ipv4_cuup 192.168.77.4
                                    --gNBs.[0].local_s_address 192.168.73.4
                                    --gNBs.[0].remote_s_address 127.0.0.1
                                    --gNBs.[0].NETWORK_INTERFACES.GNB_IPV4_ADDRESS_FOR_NGU 192.168.71.162
                                    --gNBs.[0].plmn_list.[0].snssaiList.[0].sst 1
        networks:
            core_net:
                ipv4_address: 192.168.71.162
            f1u_1_net:
                ipv4_address: 192.168.73.4
            e1_net:
                ipv4_address: 192.168.77.4

    oai-du:
        image: oai-gnb:reloc
    oai-nr-ue:
        image: oai-nr-ue:reloc
```

Bring it up — only the first DU and UE are needed, so start a subset:

```bash
cd ci-scripts/yaml_files/5g_rfsimulator_e1
C="docker compose -f docker-compose.yaml -f reloc-override.yaml"

$C up -d mysql oai-amf oai-smf oai-upf
$C ps -a                      # wait for healthy

$C up -d oai-cucp oai-cuup oai-cuup2 oai-du
docker logs rfsim5g-oai-cucp | grep -i "CU-UP"
# expect two: "Accepting new CU-UP ID 3584" (0xe00) and "ID 3585" (0xe01)

$C up -d oai-nr-ue
docker exec rfsim5g-oai-nr-ue ping -c 3 192.168.72.135   # traffic through CU-UP1
```

---

## 4. Trigger the change

```bash
# find the UE's RRC UE ID in the CU-CP log, then:
telnet localhost 9090
softmodem_gnb> rrc cuup_reloc 1 0xe01
```

`cuup_reloc [rrc_ue_id [cuup_id]]`. Both numbers accept hex. **Name the target
CU-UP ID**: without it the CU-CP picks any other connected CU-UP, which is fine
in production but makes the test non-deterministic.

Watch the CU-CP log for the five steps, in this order:

```
UE 1: moving 1 PDU session(s) from gNB-CU-UP ID 0xe00 (assoc_id N) to gNB-CU-UP ID 0xe01 (assoc_id M)
UE 1: target gNB-CU-UP assoc_id M holds the bearers, repointing the DU
UE 1: asked the DU to send 1 DRB(s) to the new CU-UP
UE 1: the DU sends uplink to gNB-CU-UP assoc_id M, asking the source for the PDCP SN status
UE 1: source gNB-CU-UP reported the PDCP status of 1 DRB(s), handing it to the target
UE 1: target gNB-CU-UP is serving the bearers, announcing the new downlink endpoint to the core
```

and in the DU log, the step that proves the F1 side landed:

```
UE xxxx: DRB 1 sends uplink to TEID 0x... from now on
```

---

## 5. What to measure, and what will not work

**Uplink is the test.** Between the DU switching over and the core answering,
uplink works and downlink does not — by design, because the uplink destination
(the UPF) never changed while the core still sends downlink to the old CU-UP. So:

```bash
# on the traffic host, before triggering:
iperf3 -s
# on the UE, UL only, running across the trigger:
docker exec rfsim5g-oai-nr-ue iperf3 -c 192.168.72.135 -t 60 -i 1
```

The uplink rate should dip for the packets in flight and then continue. **Ping and
any TCP test will stall at the trigger** and that is not a bug: it needs the
downlink, which only returns once the core acts on the Modify Indication.

**The OAI SMF will not act on it.** It has no `PDU_RES_MOD_IND` handling at all —
not in its N2 SM info switch, not in its enum — so the procedure stops after the
Indication is sent and the CU-CP stays in "waiting for the PDU Session Resource
Modify Confirm". The source CU-UP's bearer context is deliberately held until
that Confirm arrives (releasing it earlier would leave the UPF sending to a TEID
that no longer exists, and the resulting GTP-U Error Indication can make the SMF
tear the session down); it is released when the UE is released instead.

**So validate the encoding against a third party rather than against the core.**
Wireshark decodes NGAP, E1AP and F1AP, and it is an independent check that the
three new messages are well-formed — which is the part of this work the core
cannot confirm for you:

```bash
# capture on the Docker bridges: N2 (core), E1, F1-C
sudo tcpdump -i rfsim5g-core -w n2.pcap sctp
sudo tcpdump -i rfsim5g-e1   -w e1.pcap sctp
sudo tcpdump -i rfsim5g-f1c  -w f1c.pcap sctp
```

Filter for, in order of the procedure:

| Where | Filter | Expect |
|---|---|---|
| `e1.pcap` | `e1ap` | Bearer Context Setup Request to the target, then Modification Request to the source with **PDCP SN Status Request**, then to the target with **PDCP SN Status Information** and DL UP parameters |
| `f1c.pcap` | `f1ap` | UE Context Modification Request carrying **DRBs-ToBeModified-List**, with the target's TEID in `uLUPTNLInformation-ToBeSetup-List` |
| `n2.pcap` | `ngap.procedureCode == 27` | **PDUSessionResourceModifyIndication**, with `dLQosFlowPerTNLInformation` holding the target CU-UP's N3 address and TEID |

If all three decode without errors and carry the right endpoints, the RAN side is
correct and only the core is missing.

---

## 6. Negative paths worth exercising

Each is a one-line change and each takes a different branch of the state machine:

- **Unknown CU-UP ID**: `rrc cuup_reloc 1 0xdead` → refused before anything is
  sent.
- **Target goes away mid-procedure**: `docker stop rfsim5g-oai-cuup2` right after
  triggering → the UE should be put back on CU-UP1, with the DU repointed back at
  it (the source's F1-U endpoints are kept for exactly this).
- **Source goes away after the switch**: `docker stop rfsim5g-oai-cuup` once the
  DU has switched → the change should carry on unaffected.
- **Relocate twice**: trigger back to `0xe00`. The second one exercises the same
  path with the roles reversed and catches state left behind by the first.

---

## 7. If the build fails

- Missing `NGAP_PDUSessionResourceModify*` types: the ASN.1 headers are added to
  `openair3/NGAP/ngap_msg_includes.h` by the patch; the generated sources were
  already in `ngap-15.8.0.cmake`, so a clean `cmake` re-run is usually enough.
- Missing `F1AP_DRBs_ToBeModified_*`: same, generated from
  `f1ap-16.21.0.asn` and already listed in `f1ap-16.21.0.cmake`.
- `ngap_lib_test` failing to link: the two new codec functions live in
  `ngap_gNB_mobility_management.c` on purpose — that object has no ITTI
  dependencies, which is what lets the test link against `ngap` alone. Moving
  them back into `ngap_gNB_pdu_session_management.c` would break the link.
