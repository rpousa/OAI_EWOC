<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Testing the change of gNB-CU-UP (Docker)

Ordered so that each stage can fail on its own terms: encoding first (no core, no
radio), then the build, then the live path. Stop at the first stage that fails —
each one rules out a whole class of problem for the next.

---

## 0. Bringing up a machine

Nothing here is specific to one host. Run §0.1 on any candidate machine and it
tells you in one pass whether it can do this.

### 0.0 Where each step happens

Two machines, with git as the only channel between them:

| | Authoring machine | Test machine |
|---|---|---|
| What it is | the `comnetsemu` Vagrant VM | a native Linux box |
| What happens there | edit, commit, push | resync, build, deploy, capture |
| Needs AVX2 | no | **yes** |
| Needs Docker | no | yes |

The authoring machine never builds, which is why it does not need AVX2 and why
the shared-folder and line-ending caveats in §0.5 stop mattering there. The test
machine is a pure consumer: nothing is edited on it, because
`ci-scripts/cuup-reloc-sync.sh` resets it hard to the remote and would discard
the edit. Anything that needs changing gets changed on the authoring machine and
pushed.

### 0.1 Preflight

```bash
./ci-scripts/cuup-reloc-preflight.sh
```

It is read-only: loads no modules, installs nothing, changes no configuration.
Exit status 0 means every hard requirement is met. What it checks and why:

| Check | Why it is hard, not advisory |
|---|---|
| `avx`, `avx2`, `xsave` | `openair1/PHY/TOOLS/oai_dfts.c` calls `_mm256_*` unconditionally — no `#ifdef`, no runtime dispatch, no 128-bit fallback. Without AVX2 the file will not compile; without `xsave` the kernel cannot enable YMM state, so the instructions fault even if it did. |
| `sse4_2` | assumed throughout the PHY. |
| SCTP | F1AP, E1AP and NGAP all run over it. Without it nothing connects. |
| docker + compose v2 | the deployment is a compose file. |
| source not on a shared folder | vboxsf/9p/cifs cannot execute binaries the build generates and then runs (`common/utils/T/genids`), and a header-heavy build pays enormously for every `#include`. |

Cores, memory and disk are reported as advisories: 4 cores, 8 GB and 40 GB free
are comfortable; less works but slowly.

**If AVX2 is missing and the machine is a VM**, that is the hypervisor, not the
CPU. See §0.5.

### 0.2 Get the source

On the authoring machine, publish the branch:

```bash
cd ~/comnetsemu/examples/5g/OAI_EWOC
git add -A -- . ':!openair2/E2AP/flexric'
git commit -m "<what changed>"
git push origin cuup-reloc
```

On the test machine, clone once:

```bash
git clone --branch cuup-reloc --single-branch https://github.com/rpousa/OAI_EWOC.git
cd OAI_EWOC
git submodule update --init --recursive openair2/E2AP/flexric
./ci-scripts/cuup-reloc-preflight.sh
```

Clone onto local disk, never a shared folder or a network mount. The submodule is
needed because `docker/Dockerfile.build.ubuntu` builds FlexRIC even with the E2
agent disabled.

Every subsequent update is one command on the test machine:

```bash
./ci-scripts/cuup-reloc-sync.sh
```

It refuses to run on a dirty tree, fetches and hard-resets to `origin/cuup-reloc`,
updates submodules, prints the commits that arrived, and rebuilds `ran-build` and
the three images. `--full` also rebuilds `ran-base`, which is only needed when the
toolchain or the installed dependencies change. `--no-build` resyncs and stops.

### 0.3 Build the dependency image

```bash
docker build . -f docker/Dockerfile.base.ubuntu -t ran-base:latest
```

Roughly 15 minutes. This is the image whose `build_oai -I` installs the
toolchain and puts `asn1c` in `/opt/asn1c/bin`; everything else builds on it.
It needs network access to Docker Hub and GitHub.

### 0.4 Two ways forward

**For the unit tests** (§1), do not build `ran-build` — run the seven targets in
a throwaway container over `ran-base`. Minutes, not an hour.

**For the live run** (§2 onwards), build the full image. On a machine that
passed preflight the stock command works:

```bash
docker build . -f docker/Dockerfile.build.ubuntu -t ran-build:latest \
       --build-arg BUILD_OPTION="--cmake-opt -DE2_AGENT=OFF"
```

`E2_AGENT=OFF` because the pinned FlexRIC commit does not compile against this
tree; the relocation path does not touch E2, so nothing under test is lost.
`cuup-reloc-sync.sh` does this and the three image builds in one go — the
commands here are for when you want to run a single stage by hand.

### 0.5 When the test machine is a VM

Only relevant if you try to build inside a VM. The authoring machine in §0.0 is a
VM and none of this applies to it, because it never compiles anything. Three
things bite, in this order:

**AVX2 masked out of CPUID.** `/proc/cpuinfo` shows no `avx`, `avx2` or `xsave`
even though the host CPU has them. On VirtualBox under Windows the cause is
Hyper-V: with it active VirtualBox runs on the Hyper-V backend and cannot pass
CPU features through. Turn it off — `bcdedit /set hypervisorlaunchtype off`,
Core Isolation → Memory Integrity off, and uncheck the Hyper-V, Virtual Machine
Platform and Windows Hypervisor Platform features — reboot, then with the VM
powered off:

```
VBoxManage modifyvm <vm> --cpu-profile host
```

On KVM use `-cpu host`; on VMware enable "Expose hardware assisted
virtualization"; cloud instances essentially all expose AVX2 already.

**Shared folders.** A VirtualBox shared folder cannot execute what the build
produces, so the build dies at `genids: Permission denied` even though the file
is `-rwxrwxrwx` and the mount has no `noexec` — `execve` needs to map the pages
privately and vboxsf does not support that. It is also punishingly slow: a build
on one showed `0.5% us, 34% sy` with load 10, almost all time in the kernel
serving file operations. Clone to the VM's own disk.

**Line endings.** A tree checked out on Windows with `core.autocrlf=true` and
read from Linux leaves `cmake_targets/build_oai` with `#!/bin/bash\r`, which
fails as `bad interpreter`, and reports hundreds of files as modified. A fresh
`git clone` on Linux avoids this entirely; an existing tree is fixed with
`git config core.autocrlf false` and `git checkout -- .` (which discards
uncommitted work — commit first).

---

## 1. Unit tests — encoding and decoding

This stage needs no core, no radio and no containers beyond the build image. It
is also the only stage that tests the new ASN.1 code directly, so run it first
and do not move on until it passes.

Only `ran-base` is needed — that is the image whose `build_oai -I` installs the
toolchain and puts `asn1c` in `/opt/asn1c/bin`. Do **not** build `ran-build` for
this stage: it compiles every softmodem target and takes far longer than the
tests do.

```bash
cd ~/OAI_EWOC                                                        # the test machine's clone
docker build . -f docker/Dockerfile.base.ubuntu -t ran-base:latest   # once, ~15 min
```

### 1.1 Build the seven targets

`ran-base` carries only `cmake_targets/build_oai` and `oaienv` under `/oai-ran`,
so the source comes in over a bind mount. Keep the *build tree* in a named
Docker volume rather than in the mounted source: it is faster, it survives
`--rm`, and on a VM it is what makes the generated helper binaries executable.

```bash
docker volume create oai-build
mkdir -p ~/.cache/cpm ~/.cache/ccache

docker run --rm -it \
  -v $PWD:/oai-ran \
  -v oai-build:/build \
  -v ~/.cache/cpm:/root/.cache/cpm \
  -v ~/.cache/ccache:/root/.cache/ccache \
  -w /oai-ran ran-base:latest bash
```

```bash
# inside the container
ls CMakeLists.txt                      # if this fails, the mount source was wrong
ccache -M 20G
git config --global --add safe.directory /oai-ran
apt-get update && apt-get install -y libgtest-dev libbenchmark-dev   # optional

cmake -S /oai-ran -B /build -GNinja \
  -DENABLE_TESTS=ON \
  -DCMAKE_C_FLAGS=-Werror -DCMAKE_CXX_FLAGS=-Werror

cd /build
ninja -k 0 ngap e1ap_lib f1ap_lib L2_NR e1ap_lib_test f1ap_lib_test ngap_lib_test
ctest -R 'e1ap_lib_test|f1ap_lib_test|ngap_lib_test' --output-on-failure
```

Docker creates an empty directory instead of failing when a `-v` source does not
exist, which produces a confusing *"source directory does not appear to contain
CMakeLists.txt"* from a container that otherwise looks fine — hence the `ls`.

### 1.2 Why these seven targets

| Target | Covers |
|---|---|
| `ngap` | `ngap_gNB.c`, `ngap_gNB_handlers.c`, `ngap_gNB_mobility_management.c` |
| `e1ap_lib` | `e1ap_bearer_context_management.c` |
| `f1ap_lib` | `f1ap_ue_context.c` |
| `L2_NR` | `rrc_gNB.c`, `rrc_gNB_NGAP.c`, `rrc_gNB_cuup.c`, **`rrc_gNB_cuup_reloc.c`**, `mac_rrc_dl_handler.c`, `cucp_cuup_handler.c` |
| the three tests | the test files, then `ctest` |

Together they cover all 24 changed files. The three test executables alone do
not: the state machine lives in `L2_NR`, so a green `ctest` without it would
prove nothing about whether the feature compiles.

`-Werror` matches what `docker/Dockerfile.build.ubuntu` passes, so a warning
caught here is a warning that would have failed the image build. `-k 0` reports
every error in one pass instead of stopping at the first — worth it, since each
round trip otherwise costs a full rebuild.

Then the whole suite, which must not regress:

```bash
ninja tests && ctest --output-on-failure
```

(`ENABLE_PHYSIM_TESTS` is off, so this does not pull in the physical-layer
simulators.)

> On a machine without AVX, `CMakeLists.txt` detects it and leaves the SIMDe
> AVX defines off, so SIMDe uses its portable implementation. If you somehow see
> `-Wpsabi` or `target specific option mismatch`, that detection was bypassed —
> check that `cmake` printed `AVX intrinsics are OFF`.

### 1.3 Making the build faster

In rough order of payoff on this VM:

**vCPUs and RAM.** `nproc` inside the VM is the ceiling — ninja defaults to
`nproc + 2` jobs. The comnetsemu Vagrantfile ships 2 vCPUs, which caps the whole
build. With the VM powered off, on the Windows host:

```
VBoxManage modifyvm comnetsemu --cpus 6 --memory 8192
```

(or raise `vb.cpus` / `vb.memory` in the Vagrantfile and `vagrant reload`). Keep
a couple of cores for Windows.

**Get the source off vboxsf.** Every `#include` is resolved against ~60 `-I`
paths, so a C build opens headers tens of thousands of times, and each open on a
shared folder is a round trip to the host filesystem. Copying to the VM's own
disk is often a 2-3x win on its own:

```bash
rsync -a --delete ~/comnetsemu/examples/5g/OAI_EWOC/ ~/OAI_EWOC/
```

then mount `~/OAI_EWOC:/oai-ran` instead. Re-run that `rsync` after any edit on
the Windows side; the build tree stays in the Docker volume either way, so this
does not reintroduce the exec problem from §1.1.

**Build only what is needed.** The seven targets in §1.2 cover all 23 changed
files. A bare `cmake --build /build` builds ~8000 targets — every softmodem,
simulator and tool — which is only worth it as a dress rehearsal for the image
build in stage 2.

**Drop the optimiser while iterating.** `-DCMAKE_BUILD_TYPE=Debug` removes `-O2
-funroll-loops` and the heavy inlining. Nothing here is performance-sensitive,
and `-Werror` behaves the same. Switch back to the default `RelWithDebInfo`
before the last run, since a few warnings only appear with optimisation on.

**Check ccache is actually hitting**: `ccache -s`. The default 5 GB ceiling is
small next to the generated ASN.1 code, so `ccache -M 20G` once is worth it.

What each one proves:

| Test | Covers |
|---|---|
| `e1ap_lib_test` | The new `nG-UL-UP-TNL-Information` in the E1 *PDU Session Resource To Modify* item survives encode → decode → copy → compare, **and** an item that carries only that endpoint and no DRB list — the shape a UPF change produces, and the one that used to dereference an absent list on decode. |
| `f1ap_lib_test` | The new **DRBs-ToBeModified** list on the F1 UE Context Modification Request: DRB ID and uplink endpoints round-trip. |
| `ngap_lib_test` | The **PDU Session Resource Modify Indication**'s mandatory IEs (downlink TNL and the associated QoS flow list) survive a roundtrip, and the **Modify Confirm** decoder picks up the uplink endpoint the core returns. |

Then the whole suite, which must not regress:

```bash
ninja tests && ctest --output-on-failure
```

(`ENABLE_PHYSIM_TESTS` is off, so this does not pull in the physical-layer
simulators.)

> `docker/Dockerfile.build.ubuntu` passes `-DCMAKE_C_FLAGS="-Werror"`, which a
> plain local build does not. That is why `-DCMAKE_C_FLAGS=-Werror` is on the
> `cmake` line above: a warning tolerated here would fail the image build in
> stage 2.

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

**(c) There is nowhere to send user-plane traffic.** Unlike the plain
`5g_rfsimulator` deployment, this one puts nothing on `traffic_net` except the
UPF, so a ping to `192.168.72.135` comes back as `Destination Host Unreachable`
*from the UPF itself* — which incidentally proves the uplink path works all the
way through GTP-U. A data-network host has to be added, both as a target and as
the iperf3 server §5 measures against.

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

    # 12.1.1.0/24 is the UE pool: without that route the uplink arrives here and
    # the reply never finds its way back through the UPF.
    oai-ext-dn:
        image: oaisoftwarealliance/trf-gen-cn5g:latest
        container_name: rfsim5g-oai-ext-dn
        privileged: true
        init: true
        entrypoint: /bin/bash -c \
              "iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE;"\
              "ip route add 12.1.1.0/24 via 192.168.72.134 dev eth0; sleep infinity"
        depends_on:
            - oai-upf
        networks:
            traffic_net:
                ipv4_address: 192.168.72.135
```

Bring it up — only the first DU and UE are needed, so start a subset:

```bash
cd ci-scripts/yaml_files/5g_rfsimulator_e1
C="docker compose -f docker-compose.yaml -f reloc-override.yaml"

$C up -d mysql oai-amf oai-smf oai-upf oai-ext-dn
$C ps -a                      # wait for healthy

$C up -d oai-cucp oai-cuup oai-cuup2 oai-du
docker logs rfsim5g-oai-cucp | grep -i "CU-UP"
# expect two: "Accepting new CU-UP ID 3584" (0xe00) and "ID 3585" (0xe01)

$C up -d oai-nr-ue
# -I matters: without it the ping leaves on the container's own eth0 instead of
# the PDU session, and tests nothing.
docker exec rfsim5g-oai-nr-ue ping -c 3 -I oaitun_ue1 192.168.72.135
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
sudo tcpdump -i rfsim5g-core -w n2.pcap sctp & 
sudo tcpdump -i rfsim5g-e1   -w e1.pcap sctp &
sudo tcpdump -i rfsim5g-f1c  -w f1c.pcap sctp & 
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
- `ngap_lib_test` failing to link on `asn_DEF_NR_*`: the two new codec functions
  live in `ngap_gNB_mobility_management.c`, the same object as
  `decode_ng_handover_request`, which references NR RRC ASN.1 definitions. The
  `ngap` library links only `asn1_nr_rrc_hdrs` (headers), so the softmodem gets
  those definitions via `nr_rrc` while the test never did. The patch adds
  `asn1_nr_rrc` to `openair3/NGAP/tests/CMakeLists.txt`. If `asn_DEF_LTE_*`
  symbols appear too, add `asn1_lte_rrc` the same way.
- `error: inlining failed ... target specific option mismatch` on `_mm256_*`:
  the machine has no AVX2. See §0.1 and §0.5 — this is an environment problem,
  not a code one.
