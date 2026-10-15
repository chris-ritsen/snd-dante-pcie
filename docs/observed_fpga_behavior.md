# Observed FPGA Behavior — Dante PCIe Card (PCI 0x1a39:0x0004)

Hardware observations in this document use debugfs register reads, /proc,
and standard ALSA interfaces. Expected encodings and unqualified behavior
are identified separately; a driver declaration is not independent hardware
proof. No proprietary source code was consulted.

## Device Under Test

- PCI Vendor: 0x1a39, Device: 0x0004
- PCI slot: 1b:00.0
- Card: Digigram LX-DANTE (discontinued)
- Firmware version register (BAR0+0x70): 0x20010010
- Dante sample rate configured via Dante Controller: 48000 Hz

## 1. Firmware Version Register (BAR0 + 0x70)

Reading BAR0 offset 0x70 returns a 32-bit value encoding the sample rate and firmware version.

Observed value: `0x20010010`

| Bits | Value | Meaning |
|------|-------|---------|
| 31-28 | 0x2 | Sample rate index |
| 27-0 | 0x0010010 | Firmware version identifier |

### Sample Rate Index Mapping

The original implementation had retained register evidence only for 48000 Hz;
its earlier claim that every rate had been measured was unsupported. Chris
confirmed that limitation on 2026-10-14. The subsequent real-audio validation
in section 17 measured all six valid encodings and their channel capacities.

| Index | Sample Rate (Hz) | Max Channels |
|-------|-----------------|--------------|
| 0 | 192000 | 64 |
| 1 | 96000 | 128 |
| 2 | 48000 | 128 |
| 3 | (invalid) | - |
| 4 | 176400 | 64 |
| 5 | 88200 | 128 |
| 6 | 44100 | 128 |

The channel capacities above were independently advertised by the card's
Dante diagnostic capabilities on 2026-10-14 and agree with the Digigram
product documentation. Section 17 separately records actual audio at each
maximum, FPGA register reads, and ALSA constraint checks.

## 2. Sample Counter (BAR0 + 0x74 / 0x78)

A 64-bit free-running counter at BAR0 offsets 0x74 (low) and 0x78 (high).

Measured tick rate: **3000 ticks/second** at 48000 Hz sample rate.

This corresponds to one tick per audio chunk (48000 / 16 frames per chunk = 3000).

The counter does not reset when the driver is loaded/unloaded. It appears to run continuously while the FPGA is powered.

## 3. Event Flags (BAR0 + 0x10)

During active streaming with both playback and capture enabled, the VDMA event register consistently reads `0x0000000c`, indicating bits 2 and 3 are set.

| Bit | Observed State | Purpose |
|-----|---------------|---------|
| 0 | 0 (toggles) | Chunk clock from FPGA. Asserted by FPGA once per chunk, acknowledged by sequencer. Not visible in polled reads due to speed. |
| 1 | 0 (toggles) | Period marker. Set by sequencer, cleared to trigger interrupt. Not visible in polled reads. |
| 2 | 1 (while TX active) | Outbound (playback) enable. Set by host driver when playback stream starts. Cleared when playback stops. |
| 3 | 1 (while RX active) | Inbound (capture) enable. Set by host driver when capture stream starts. Cleared when capture stops. |

Event set: write to BAR0 + 0x08
Event clear: write to BAR0 + 0x0C

## 4. WAIT Instruction Operand

The VDMA sequencer WAIT instruction observed in the running microcode uses operand `0x001001`.

Using the WAIT_EVENT encoding from vdma_seqcode.h:
```
VDMA_WAIT_EVENT(EVENT_EN, EVENT_STATE)
  bits 23-12 = EVENT_EN = 0x001
  bits 11-0  = EVENT_STATE = 0x001
```

This means: wait until event bit 0 (chunk clock) matches state 1 (asserted), with enable mask selecting only bit 0.

## 5. Transfer Control Words

Observed in descriptor RAM during active streaming:

| Word | Value | Interpretation |
|------|-------|---------------|
| Outbound (host→FPGA) | 0x00012040 | Using VDMA_LOAD_XFER_CTL encoding: R=_IM(0), bits include direction + length. Bit 17 set (direction), bit 16 set, bits 5:0 = 0x40 (64 bytes). |
| Inbound (FPGA→host) | 0x00010040 | Same but bit 17 clear (opposite direction). Bit 16 set, bits 5:0 = 0x40 (64 bytes). |

Note: These values are loaded into VDMA_XFER_CTL via the VDMA_LOAD_XFER_CTL instruction. The R field selects _IM (immediate/indexed mode). The transfer length of 0x40 = 64 bytes = one chunk.

## 6. Interrupt Path

The interrupt fires through external interrupt 1 on the GN4124 bridge.

Observed register values:

| Register | Offset | Value | Meaning |
|----------|--------|-------|---------|
| INT_CTRL | BAR4+0x810 | 0x00000002 | Bit 1 set: external interrupt 1 rising edge |
| GPIO_BYPASS | BAR4+0xA00 | 0x00000002 | Bit 1 set: GPIO pin 1 bypasses to external interrupt 1 |
| INT_STAT acknowledge bit | BAR4+0x814 | bit 5 | Write bit 5 to clear the period interrupt |
| INT_CFG routing | BAR4+0x820+4*N | bit 5 | Route bit 5 of INT_STAT to MSI vector N |

The driver allocates one MSI vector through the Linux PCI API and routes the period interrupt through INT_CFG0. All other INT_CFG routes are disabled. With one message enabled, the bridge uses the complete MSI data value programmed by Linux; the low bits of that value are not an INT_CFG slot index.

## 7. Audio Format

Observed via `arecord --dump-hw-params` and `/proc/asound/card0/pcm0c/sub0/hw_params`:

| Parameter | Value |
|-----------|-------|
| Access | MMAP_NONINTERLEAVED |
| Format | S32_LE |
| Subformat | STD |
| Channels | 1 to max_channels (rate-dependent) |
| Period size | Must be multiple of 16 frames |
| Periods | 2 to 512 |
| Buffer size | period_size * periods |

Audio data is 24-bit aligned to the most significant bits of a 32-bit word. The low 8 bits are zero-padded.

## 8. DMA Buffer Layout

Non-interleaved: each channel occupies a contiguous region.

```
[Channel 0: chunk0 chunk1 ... chunkN]
[Channel 1: chunk0 chunk1 ... chunkN]
...
[Channel 127: chunk0 chunk1 ... chunkN]
```

Each chunk: 16 frames × 4 bytes = 64 bytes.
Channel stride: chunks_per_channel × 64 bytes.
Total buffer: channel_count × channel_stride.

With 128 channels, 64 chunks: 128 × 64 × 64 = 524,288 bytes per direction.

## 9. Observed Descriptor RAM State During Active 128ch Streaming

```
Word 0x00 = 0x00000000  (constant zero)
Word 0x01 = 0x00000001  (constant one)
Word 0x02 = 0xffffffff  (constant negative one)
Word 0x03 = 0x00012040  (outbound transfer control)
Word 0x04 = 0x00010040  (inbound transfer control)
Word 0x05 = 0x00000040  (chunk size = 64 bytes)
Word 0x06 = 0x00000080  (outbound channels = 128)
Word 0x07 = 0x00000080  (inbound channels = 128)
Word 0x08 = 0x00000020  (periods per interrupt = 32)
Word 0x09 = (varies)    (period countdown, runtime)
Word 0x10 = 0x00000040  (outbound chunks per channel = 64)
Word 0x11 = 0x00000040  (inbound chunks per channel = 64)
Word 0x12 = 0x00001000  (outbound channel stride = 4096 bytes)
Word 0x13 = 0x00001000  (inbound channel stride = 4096 bytes)
Word 0x14 = (buffer addr lo)  (outbound DMA buffer physical address)
Word 0x15 = (buffer addr hi)
Word 0x16 = (buffer addr lo)  (inbound DMA buffer physical address)
Word 0x17 = (buffer addr hi)
Word 0x18 = (varies)    (outbound current chunk index, counts down)
Word 0x19 = (varies)    (inbound current chunk index, counts down)
Word 0x1a = (varies)    (outbound current base address lo)
Word 0x1b = (varies)    (outbound current base address hi)
Word 0x1c = (varies)    (inbound current base address lo)
Word 0x1d = (varies)    (inbound current base address hi)
```

## 10. Buffer Wrap Behavior

The chunk index (words 0x18/0x19) counts down from chunks_per_channel toward zero. The base address (words 0x1a-0x1d) advances by chunk_size (64 bytes) each iteration.

When the chunk index reaches zero, the sequencer resets it to chunks_per_channel and reloads the base address from the configured buffer start address (words 0x14-0x17). This wrap check occurs at the beginning of each period cycle before the WAIT instruction.

The driver's ALSA pointer callback reads the current base address (word 0x1a or 0x1c), subtracts the buffer start address, and applies modulo to get the frame position within the ring buffer.

## 11. MSI Allocation

The LX-DANTE used for validation advertises four MSI vectors in PCI configuration space and works with one allocated vector. The earlier claims that it advertised only one and required four enabled messages were incorrect.

The previous driver allocated one vector, then manually enabled four messages and changed an internal kernel MSI descriptor. That left the hardware and interrupt allocator inconsistent. Under QEMU/VFIO, the guest showed four enabled messages while the host allocated only one, and native JACK received no period interrupts.

Use `pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI)` so Linux owns the allocation and configuration. Install the handler before enabling INT_CFG0. A normal single-vector allocation restored native JACK processing without manually toggling MSI. Requesting four vectors is unnecessary and can fail on interrupt controllers that support only one MSI vector per device. The driver does not implement legacy INTx delivery.

## 12. RSTOUT Toggle Warning

**Do not toggle PCI_SYS_CFG_SYSTEM (BAR4+0x800) bits 15:14 (RSTOUT) while the driver is loaded.** This asserts/de-asserts the FPGA fabric reset. The FPGA may fail to recover — the firmware version register (BAR0+0x70) reads 0xFFFFFFFF indefinitely and the device becomes non-functional. A warm reboot does not fix this because the PCIe slot remains powered. Only a cold boot (full power off/on) forces the FPGA to reload its bitstream from flash and recover.

This was observed when attempting to clear stuck VDMA DMA state by resetting the FPGA. The FPGA sometimes recovered after 100ms, sometimes never recovered. On the second attempt in the same session, the PCI device disappeared from the bus entirely and could not be rescanned.

## 13. Sequencer Start and Stop (BAR0 + 0x30)

On the LX-DANTE, writing `1` starts the sequencer and writing `2` stops it. Writing `0` does not stop it: after both ALSA streams closed, the running bit, DMA pointers, and period interrupts continued advancing. Writing `2` cleared the running bit and left the pointers and interrupt count unchanged across a one-second observation. The stopped register can read `0` or `2`; bit 0 indicates whether it is running.

The driver uses the stop command before reprogramming descriptors, after the last stream stops or closes, and during removal and system shutdown. Shutdown also disables bus mastering. These operations stop the sequencer without toggling the FPGA fabric reset.

## 14. DMA Initialization After PCI Reset

The public GN412x reference sequence writes `0x1f` then `0x7` to DMA_CFG (BAR0 + 0x04); see `reference/gn4124-core/hdl/spec/sim/Test_Builder/cont_block_dma.c`. The driver now uses this sequence, with a readback between writes, after stopping the sequencer.

Writing only `0x7` left DMA stalled after a guest PCI reset: the sample counter advanced, but the descriptor pointers stayed at their initial addresses and no period interrupts arrived. Loading the driver with the complete initialization sequence restored streaming on the same card without restarting the VM.

## 15. Native JACK Failure and Validation, 2026-10-14

### Original report

The reporter used a Four Audio Dante PCI Express card on what he described as the latest Ubuntu Studio. He reported PCI identity `1a39:0004` and this probe message on 2026-10-12:

```text
snd-dante-pcie 0000:03:00.0: firmware 0x20010010, rate 48000 Hz, max 128 channels
```

His initial symptom was a two-channel limit in JACK. In the follow-up supplied on 2026-10-14, he said disabling PipeWire's JACK emulation removed that limit and the `Unknown "alsa" driver!!!` error. Native JACK then selected 128 capture and 128 playback channels but could not accept a client. The command below is preserved exactly as supplied:

```sh
jackd -R -d hw:0 -r 48000 -p 1024 -i 128 -o 128
```

That transcription omits the explicit `-d alsa` backend selector used in the local reproduction. The supplied server log nevertheless confirms that the ALSA backend was active; the omission is not evidence that it caused the failure:

```text
Acquire audio card Audio0
creating alsa driver ... hw:0|1024|2|48000|128|128|nomon|swmeter|-|32bit
configuring for 48000Hz, period = 1024 frames (21.3 ms), buffer = 2 periods
ALSA: final selected sample format for capture: 32 bit integer little-endian
ALSA: use 2 period for capture
ALSA: final selected sample format for playback: 32 bit integer little-endian
ALSA: use 2 period for playback
```

`jack_lsp` waited a few seconds and failed:

```text
Cannot open lsp client
Error: cannot connect to JACK, jack_client_open() failed, status=0x21
```

The server logged:

```text
Driver is not running
Cannot create new client
```

He also reported failures with periods of 512 and 2048 frames and channel counts of 16 and 64. His exact Ubuntu release, kernel version, JACK package version, PCI subsystem ID and loaded module source revision were not supplied. No Four Audio card was available for this validation. Its probe message proves recognition and advertised limits, not successful multichannel streaming.

### Local hardware and software

| Component | Configuration used |
| --- | --- |
| Physical card | Digigram LX-DANTE, Gennum GN4124, PCI `1a39:0004`, subsystem `1a39:e04c` |
| Host PCI address | `0000:1b:00.0`, alone in IOMMU group 20 |
| Host | x86-64 Arch Linux, kernel `7.2.2-arch1-1` |
| Guest | Full Ubuntu Studio 26.04.1 desktop installation, kernel `7.0.0-38-generic` |
| Virtualization | QEMU/KVM, q35, host CPU model, 6 vCPUs, 12 GiB RAM, real PCI card assigned through VFIO |
| Guest PCI address | `0000:01:00.0`, behind the `pciecard` root port |
| Guest disk | Writable qcow2 overlay over the retained 2026-10-12 Ubuntu Studio installation |
| Audio stack | Native JACK 1.9.22, ALSA `hw:DantePCIe,0`, not `plughw:` or PipeWire-JACK |
| Audio parameters | 48,000 Hz, `S32_LE`, `MMAP_NONINTERLEAVED`, two periods, equal input/output channel counts |
| Probe | Native libjack client named `pcie-validation`; all physical capture ports connected to its inputs and silent outputs connected to all physical playback ports |
| Firmware register | `0x20000010` and `0x20010010` observed; no firmware was flashed |

The VM hardware arguments were:

```sh
qemu-system-x86_64 \
  -name dante-jack-stall-fix -machine q35,accel=kvm -cpu host -smp 6 -m 12G \
  -drive file=ubuntu-studio.qcow2,if=none,id=system,format=qcow2 \
  -device virtio-blk-pci,drive=system,serial=UBSTUDIO-PCIE-TEST,bootindex=1 \
  -device pcie-root-port,id=pciecard,chassis=1,slot=1 \
  -device vfio-pci,host=0000:1b:00.0,id=dantecard,bus=pciecard \
  -netdev user,id=network,hostfwd=tcp:127.0.0.1:2226-:22 \
  -device virtio-net-pci,netdev=network \
  -vga virtio -display none -vnc 127.0.0.1:16 \
  -qmp unix:qmp.sock,server=on,wait=off \
  -chardev socket,id=serial0,path=serial.sock,server=on,wait=off,logfile=guest-serial.log \
  -serial chardev:serial0 -monitor none \
  -device qemu-xhci -device usb-tablet -boot order=c
```

Host audio was stopped through its owning service controller before assignment, with the original module and audio state saved for restoration. These are historical setup details, not instructions to assign a card still in use by host audio. No host reboot was performed. The RSTOUT reset in section 12 was not used.

Inside the guest, `/usr/sbin/ubuntustudio-pwjack-config disable` selected native JACK. The studio user's `pipewire.service`, `pipewire.socket`, `pipewire-pulse.service`, `pipewire-pulse.socket` and `wireplumber.service` were stopped and masked for the experiment. SDDM and its greeter session were also stopped when their open ALSA control device prevented module removal. The underlying installation remained a full desktop installation; the native JACK experiments ran with those competing audio consumers inactive.

### Driver identities and method

| Revision | Identity | Scope |
| --- | --- | --- |
| Original | Commit `cc7ab1981e8163fadc81b5b16b6654a7cb50c65f`; module srcversion `BBFC87C91DB51145F7E2B88` | Failed locally before the repair and again after reloading it following a successful patched run |
| Hardware-validated repair | Module srcversion `6BCE360C2499C99DF5DFFA4`; source SHA-256 `eed27aaf32705d0e27b2c6b42960da07df7001c1cf07b8ea398431c063113540` | Single MSI, actual sequencer stop command and complete DMA_CFG initialization; tested in the guest and then installed on the host |
| Subsequent source audit | See section 16 | Build validation only; not included in the hardware results below |

The equivalent native JACK command used for the 128-channel failure was:

```sh
jackd -R -d alsa -d hw:DantePCIe,0 -r 48000 -p 1024 -n 2 -i 128 -o 128
```

Each case launched JACK in a transient user systemd unit with `TimeoutStopSec=5`. After one second, `timeout 10 jack_lsp` attempted a client connection. The probe then ran as `timeout 25 /home/studio/jack-probe CHANNELS 15`. It registered one input and one silent output per channel, connected all physical ports, counted process callbacks, frames and xruns, and recorded capture peak/RMS/nonzero counts. The server was stopped at the end of each case.

The record includes both ALSA directions' `hw_params` and `status` before, during and after the probe; `/proc/interrupts`; PCI MSI configuration and allocated IRQs; FPGA sample-counter and descriptor-pointer reads; and the JACK journal. After stopping the server, both PCM streams had to be closed, CSR bit 0 clear, and descriptor pointers and IRQ count unchanged over one second. An open port list alone was not accepted as streaming evidence. Successful probe results were checked against elapsed audio frames, because a client that processes only one block can otherwise exit successfully.

### Original failure reproduced

| Original-driver case | `jack_lsp` elapsed time | Result |
| --- | ---: | --- |
| 128 input/output channels, 1024-frame period | 4.749 s | Exit 1, zero ports, `status = 0x21`, server `Driver is not running` / `Cannot create new client` |
| 16 input/output channels, 512-frame period | 5.174 s | Same client/server failure |
| Original reloaded after a successful patched run, 128 channels / 1024 frames | 5.173 s | Same failure returned |

In the failing runs ALSA accepted the requested format and channel count, and DMA descriptor pointers advanced, but the driver's interrupt count stayed at zero. Guest PCI configuration showed four enabled MSI messages while VFIO had allocated only one physical vector. Removing the manual four-message override and using a normal one-vector allocation restored interrupts without a manual MSI disable/re-enable operation. That operation had been used in the separate 2026-10-12 investigation; it was not used in these 2026-10-14 baseline or repaired-driver runs.

This reproduces the reporter's observable JACK failure on a different physical card through VFIO. It establishes the local MSI defect, not the cause of the failure on his native Four Audio system.

### Results for the hardware-validated repair

Every row used 48 kHz, two periods and a 15-second silent probe. The frames column counts audio frames per channel, not samples summed across channels. Every row exposed exactly twice the channel count as physical JACK ports, kept both ALSA directions running, advanced interrupts, and passed the stopped-DMA observation described above.

| Channels each direction | Period, frames | Process callbacks | Frames processed | Xruns |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 512 | 1406 | 719872 | 0 |
| 16 | 1024 | 703 | 719872 | 0 |
| 16 | 2048 | 351 | 718848 | 0 |
| 64 | 512 | 1407 | 720384 | 0 |
| 64 | 1024 | 703 | 719872 | 0 |
| 64 | 2048 | 351 | 718848 | 0 |
| 128 | 512 | 1408 | 720896 | 0 |
| 128 | 1024 | 704 | 720896 | 0 |
| 128 | 2048 | 351 | 718848 | 0 |

`jack_lsp` completed in 0.160–0.171 seconds in these cases, including command transport overhead. Two warm **guest** reboots followed by 128-channel / 1024-frame runs also passed. Together with the initial repaired-driver reload, there were 12 successful final-revision guest runs and 12 successful stopped-DMA observations. Those guest reboots did not reboot the host.

After shutting down the guest and releasing VFIO, the same source was installed through DKMS on the host. A separate native JACK run used 128 input/output channels, 48 kHz, 512-frame periods and two periods for 30 seconds: 2,818 callbacks, 1,442,816 frames and zero xruns. The host's normal audio services and saved player state were then restored. The restored production JACK instance reported four startup xruns, unchanged across subsequent readbacks; that is separate from the zero-xrun isolated probes.

The repair compiled with `W=1` against guest kernel `7.0.0-38-generic`, host kernel `7.2.2-arch1-1` and RT headers `7.1.12-rt1-arch1-1-rt`. Only the normal host kernel's module was installed on the host. The guest build reported a compiler executable-name difference and skipped BTF generation because `vmlinux` was unavailable; compilation and linking succeeded.

Intermediate results are retained rather than counted as passes: requesting four MSI vectors failed with `ENOSPC`; MSI-only and stop-only candidates still stalled DMA after a guest reset; one earlier guest shutdown hit a VFS `iput` BUG whose cause was not established. The two reboots on the final hardware-validated revision did not reproduce that BUG. Runs accidentally made without a loaded module were excluded.

All output from the probe was silence. These runs establish negotiation, callbacks, DMA and interrupt progress, plus shutdown observations. They do not establish audible output, new Dante network subscriptions, channel identity/crosstalk, long-duration reliability, other sample rates or Four Audio interoperability. No host reboot, FPGA RSTOUT toggle, firmware flash or Dante routing change was part of this validation.

### Retained evidence

The local evidence directory is `/home/chris/Documents/Codex/2026-10-14/dante-jack-stall-fix`. It contains:

- `results.json`: versioned case results, final validation summary, excluded failures and host restoration evidence.
- `guest-qemu-arguments.json`, `run-cases.py`, `jack-probe.c`: exact VM arguments, case orchestration and probe source.
- `original-cases.json`, `original-recheck-cases.json`, and their `*-ports.txt`, `*-journal.txt`, `*-dma.txt`, `*-pci.txt` and PCM/IRQ snapshots: failing baseline and original-driver recheck.
- `release-matrix-cases.json`, `init-reboot-*`, `final-reboot2-*`, and per-case `*-probe.txt` / `*-idle.txt`: repaired-driver streaming and stop evidence.
- `host-probe-case.json`, `host-probe.txt`, `host-final-readback.json`: direct host run and restoration readback.
- `guest-shutdown-failure-serial.txt`: the unexplained intermediate guest shutdown crash.
- `driver-fix.patch` and `snd-dante-pcie-fixed-source.tar.gz`: the exact hardware-validated source, before the subsequent audit edits.

The source archive SHA-256 is `dd1b6c8ebd02e698e7148f7fe0b655a315829565163bd15dd76bae7547be87fc`. The repair patch SHA-256 is `c23299de3799be607e0386328114853502097cd04d8ee0b0b526b0114aff3248`. Later working-tree edits do not inherit these hardware validation results.

## 16. Subsequent Source Audit, 2026-10-14

The audit changes below initially received isolated build checks only. At
that point the host remained on the revision in section 15. Section 17
records the subsequent live validation, including failures found in this
audit candidate and their corrections.

| Defect | Source change | Validation boundary |
| --- | --- | --- |
| Diagnostic mmap passed a DMA/bus address to `remap_pfn_range` as a CPU physical address | Use `snd_dma_buffer_mmap`, which selects the coherent DMA mapping implementation for these buffers | Builds pass; IOMMU-backed device mmap still needs a hardware run |
| Diagnostic files/mappings were not tied to ALSA card lifetime; the normal managed-buffer setup could fall back to smaller allocations and replace buffers later | Require a fixed 4 MiB buffer per direction, check allocation failure, and track diagnostic files with `snd_card_file_add/remove` so mappings retain the card | Builds pass; retained mappings across PCM close/reconfigure and device removal still need validation |
| Diagnostic device defaulted to mode `0666` and allowed writable mappings of audio memory | Default to root-only read access, reject write opens and writable/executable mappings, and prevent later permission escalation with `mprotect` | Source/build check only |
| Interrupt handler read stream pointers without the writer's lock, and ALSA had no IRQ number for stop synchronization | Snapshot pointers under the driver lock, release it before `snd_pcm_period_elapsed`, and set `card->sync_irq` | Builds pass; concurrent stop/close stress not performed |
| Capture and playback could negotiate different periods despite one shared hardware countdown | Declare joint duplex and reject conflicting period sizes in `hw_params` | Builds pass; mismatched-period and recovery scenarios not run on hardware |
| `hw_free` left diagnostic metadata valid; readers assembled pointers and sizes from independent reads | Clear metadata on `hw_free` and serialize diagnostic reads, configuration and disconnect with a mutex | Builds pass; hardware concurrency remains unvalidated |
| Probe failure could leave debugfs/misc interfaces behind; failed misc registration was deregistered unconditionally | Register cleanup actions, track successful misc registration, quiesce before probe unwind, and use `snd_card_free_on_error` | Builds pass; allocation-failure injection not performed |
| BAR lengths and unusable firmware reads were not rejected before register writes; allocation/constraint errors were ignored; `id=` was ignored | Validate resource sizes and firmware reads, propagate setup errors, and honor `id=` | Builds pass; non-Dante GN4124 identity matching remains an open issue |
| Counter low/high reads could combine opposite sides of a low-word rollover | Read high/low/high and retry on a changed high word | Source/build check only; rollover not induced on hardware |
| Dashboard used one direction's stride for both, retained stale mapping lengths, and overflowed `abs(INT32_MIN)` | Derive each stride from that direction's size/channel count, remap when the active size changes, and widen peak arithmetic | Python syntax and synthetic calculations/regular-file mapping checks pass; no live dashboard validation of the new module |

These changes build without warnings with `W=1` against `7.2.2-arch1-1` and `7.1.12-rt1-arch1-1-rt`. The candidate module srcversion is `F4D24E136F55FDB68824D0C`, and its C source SHA-256 is `417d31f9d1bbe044a9d59a085667fee9091c4ff28c8598a58f804c29933d92b1`. The audit directory `/home/chris/Documents/Codex/2026-10-14/dante-driver-audit` retains the pre-audit source, separate patches, kernel-source reference manifest, build logs and synthetic dashboard observations. No repository tests were added or changed.

Remaining work requires a separate validation step. `prepare` still stops and restarts the shared sequencer even if the opposite direction is running. Independent direction stop/restart also needs verification: the microcode visits directions based on channel counts, while trigger STOP clears event bits and only stops the whole sequencer when both directions are idle. This audit does not establish that those event bits alone suppress all DMA for the stopped direction. The wildcard GN4124 PCI match also needs a supported-board identification policy without guessing Four Audio subsystem IDs. Suspend/resume and firmware sample-rate changes while bound are unqualified. None of these gaps is claimed fixed by the nine-case duplex matrix.

## 17. Real Audio and Sample Rates, 2026-10-14

This later validation used the native host kernel `7.2.2-arch1-1`, the same
physical LX-DANTE, and real music sent through ALSA playback, the card's
temporary Dante self subscriptions, and ALSA capture. There was no JACK
software return connection. External receivers of the card were temporarily
disconnected, and the service owners held audio consumers and routing
enforcement. The host retained boot ID
`43c355a7-d676-4b39-b706-aae4ced832d2`; no PCI reset, RSTOUT operation, VFIO
assignment, or host reboot was used in this validation.

The reference was a quiet excerpt from an existing local music file, with
different offsets per channel and deterministic low-bit markers. The
recorded return was aligned by those markers and compared as signed 24-bit
samples. Every settled sample was compared, not only a peak or checksum.
Startup and the final one second were excluded from comparison. An isolated
software qualification first checked the recorder/analyzer; deliberate
sample corruption and swapped channels in copied data were detected.
Those software checks are not counted as hardware audio evidence.

### Failures caught and corrected

The section 16 candidate `F4D24E136F55FDB68824D0C` failed probe with `ENOMEM`
when it required two 4 MiB allocations on the long-running host. The card
remained stopped and responsive. An intermediate balanced-allocation
candidate obtained 1 MiB per direction, but its diagnostic mmap rejected a
valid read-only shared mapping with `EINVAL`. Linux clears `VM_SHARED` for
an `O_RDONLY` file while retaining `VM_MAYSHARE`; the sharing-mode check now
uses the latter. Neither failed candidate is counted as a successful run.

The final C source SHA-256 is
`769e7d119ff5ae5581805a2a60f7dea67d0c5ddddefeee8259390230250b96b5`, with module
srcversion `E6F60C4233CEE0BBCD6BF99`. Its changes include:

- Card-owned coherent buffers remain allocated for the card lifetime,
  including retained diagnostic mappings. Allocation tries balanced pairs
  with the supported DMA masks, then smaller sizes if necessary. ALSA
  constraints reflect the allocation actually obtained. This host obtained
  4 MiB per direction using a 32-bit coherent DMA mask.
- Read-only shared diagnostic mappings use the DMA mapping API and remain
  readable after the original file descriptor and PCM streams close.
  Write opens and writable mappings were rejected in the live checks, and
  inactive-stream addresses, lengths, and timing metadata cleared on close.
- Idle PCM opens and diagnostic reads obtain the rate and channel capacity
  from the FPGA. `hw_params` and `prepare` reject a stale configuration.
  All sample-rate transitions below occurred with PCM closed and without
  reloading the module.
- `prepare` rejects reinitialization while either direction is running.
  Preparation leaves the sequencer stopped until START. START and STOP
  update the direction's actual microcode channel-count gate; event bits
  alone are no longer used to suppress its transfers.

The source builds without warnings with `W=1` against both the running
kernel and `7.1.12-rt1-arch1-1-rt`. Only the running kernel was used for
these audio measurements. The matching source and module srcversion were
installed through DKMS; installed and running identities were read back.

### Measured sample-rate results

Each maximum-channel run below used 512-frame periods, two periods, and
15 seconds of recording. All had zero xruns, recorder overruns, and sample
differences. Fresh ALSA opens reported the measured rate and maximum in
both directions and rejected a channel count one above that maximum.

| Rate, Hz | Channels each way | FPGA firmware word | Compared samples |
| ---: | ---: | --- | ---: |
| 44100 | 128 | `0x60010010` | 68,459,008 |
| 48000 | 128 | `0x20010010` | 74,498,048 |
| 88200 | 128 | `0x50010010` | 136,983,552 |
| 96000 | 128 | `0x10010010` | 149,061,632 |
| 176400 | 64 | `0x40010010` | 136,852,480 |
| 192000 | 64 | `0x00010010` | 149,291,008 |

A 16-channel, 192 kHz run with 1024-frame periods and three periods also
passed, comparing 27,959,296 samples. The prior hardware repair `6BCE...`
provided a separate 30-second real-audio baseline at 48 kHz, 128 channels,
512-frame periods and two periods: 166,641,664 samples matched exactly.
Its measured return delay and the new driver's equivalent configuration
were both 1569 frames. This work establishes reliability changes, not a
latency improvement.

The 176.4-to-96 kHz transition exposed a NetAudio consistency defect:
its setter reported success and 96 kHz, but the first ordinary device read
still reported 64 RX/TX channels while the FPGA and ALSA exposed 128.
The NetAudio developer corrected channel-count/map refresh and publication,
deployed the change, and independently inspected its readback. A repeated
192-to-48 kHz transition then returned coherent 64-to-128 counts on the
first ordinary reads, without a manual refresh. This is backend metadata
evidence, not a claim about every browser or mobile rendering path.

Three final 48 kHz runs swapped adjacent Dante return channels and passed
at 16 channels / 256 frames / two periods, 64 / 1024 / three, and
128 / 2048 / three. The last configuration used a 3 MiB active buffer per
direction. A subsequent three-minute run at 128 / 512 / two retained the
swapped returns and compared 1,088,274,432 samples with zero differences,
xruns, or recorder overruns. Its diagnostic mappings also passed the normal
PCM-close lifetime check. Across the final source's eleven real-audio runs,
1,922,329,088 samples matched exactly. The proposed larger matrix was not run.

Original receiver assignments and 48 kHz / 128-channel operation were
restored after validation. The raw captures and comparison reports remain
available independently of the restored live audio services.

Evidence is retained under
`/home/chris/Documents/Codex/2026-10-14/dante-real-audio-validation`:
`audio-probe.c`, `analyze-audio.py`, reference provenance, raw captures,
per-case analysis and PCM/IRQ/idle observations, failed candidate logs,
the `rate-*-ordinary-read.json` and `*-alsa.json` records, DKMS readback,
and the delegated NetAudio fix's source/deployment evidence. No repository
tests were added or changed in this driver project.

This is digital self-subscription evidence on this LX-DANTE. It does not
qualify external Dante network transport, analog sound, Four Audio boards,
suspend/resume, hot removal, fault recovery, or a clock change during active
PCM. The active-opposite-direction prepare rejection and independent
direction shutdown still need their own hardware qualification; the normal
duplex runs do not substitute for those scenarios.

## 18. Independent streams and linked duplex (2026-10-15)

Stock PipeWire exposed the active-opposite-direction `prepare` restriction
described above. A second direction could not be prepared while the first
was running. The driver now programs only the inactive direction's ring,
preserving the shared sequencer, interrupt countdown, and active ring.
ALSA period negotiation also exposes the already-configured direction's
period size instead of discovering a conflict only in `hw_params`.

The sequencer acknowledges the playback and capture channel gates in DRAM
words `0x0a` and `0x0b`. It skips inactive ring wrap checks and acknowledges
a cleared gate before that direction's descriptors are reused. START waits
for the enabled gate to be observed. It also retires a pending STOP left by
an older module: writing STOP to an already-halted engine can leave a command
that halts the next RUN after one instruction. The driver no longer queues
that unnecessary STOP.

An intermediate candidate passed short audio comparisons but failed native
JACK at 128 channels, 48 kHz, 2048-frame periods and two periods. The capture
position was 16 frames ahead of playback in the retained failure snapshots.
Kernel traces exposed repeated hardware restarts before the client could
activate, including shorter recovery bursts in some apparently passing
sessions. Waiting for each direction's START separately had allowed the
first direction to advance before the second was enabled.

The final implementation handles ALSA-linked streams as one trigger group,
enabling both gates before RUN and acknowledging both. Unlinked streams
retain independent operation. The exact failing JACK case then passed from
the first start. Every subsequent native JACK session required exactly one
successful grouped START before shutdown, stable hardware trigger times,
and zero client xruns; eventual recovery was not accepted as a clean start.

The final candidate's module source identity was `4FE4760889C950A39D0CD58`,
built from source SHA-256
`d5791dfc6f7b027367efcab243c43a5c98740badc504885c0534b08eadf15539`.
Both Linux `7.2.2-arch1-1` and `7.1.12-rt1-arch1-1-rt` builds completed with
`W=1` and no warnings. Hardware checks used the former kernel, the LX-DANTE,
firmware `0x20010010`, native JACK 1.9.22, PipeWire 1.6.9, and WirePlumber
0.5.18. PipeWire used its stock Pro Audio behavior in an isolated session;
no ALSA startup workarounds were supplied.

The 13-minute final run passed:

- 60 opposite-direction close/reopen cycles while the other direction
  continued, varying 16/64/128 channels and two/three/four-period buffers.
- Rejection of a conflicting period size without restarting the active
  direction, plus duplex operation at 256/512/1024/2048-frame periods.
- Eleven fresh native JACK sessions with 128 capture and playback channels,
  covering those four period sizes and two/three-period buffers. All had
  zero client xruns and no hidden hardware restart.
- Ten fresh PipeWire sessions with 128-channel ALSA capture and playback,
  alternating capture-first and playback-first applications. The eight
  restart cases included 24 application stop/reopen cycles. Hardware trigger
  times remained unchanged after initial duplex setup, frame progress and
  capture writes continued, and the retained logs contained no stream errors.

Quiet music with different deterministic sample markers on channels 125
and 126 was sent through two temporary card self-subscriptions. Capture was
compared as exact signed 24-bit samples after alignment, excluding the first
four seconds and last second. The comparator separately rejected a changed
bit and a repeated frame in offline checks.

| Capture | Compared samples | Different samples |
|---------|-----------------:|------------------:|
| Short native JACK | 681,216 | 0 |
| Short PipeWire | 695,552 | 0 |
| Five-minute native JACK | 28,330,240 | 0 |
| Five-minute PipeWire | 29,056,256 | 0 |
| **Total** | **58,763,264** | **0** |

JACK reported zero xruns and recorder overruns. The final kernel log had no
driver timeout, failed trigger, warning, or fault. Both PCMs closed, module
references and process holders cleared, and interrupts and DMA pointers
stayed stopped before ordinary module unload. The exact released module was
restored, both temporary routes were removed, and the complete route readback
matched the pre-run state. This trial did not persistently install or publish
the candidate, and no repository tests were added or changed.

Evidence is retained under
`/home/chris/Documents/Codex/2026-10-15/dante-pipewire-fix`:
`candidate-06-manifest.json`, `reliability-06-suite.json`,
`reliability-06-kernel.log`, and `reliability-06/qualification-summary.json`,
with per-case trigger logs, hardware snapshots, raw captures, comparison
reports, and route restoration readbacks. The failed intermediate run is
retained separately under `reliability/` and `reliability-suite.json`.

These checks qualify the changed stream lifecycle at 48 kHz on this host
and card. The six-rate audio results in section 17 used the earlier source;
that rate matrix was not repeated here. The new media evidence covers two
digital self-return channels, not external network transport, analog sound,
other boards, suspend/resume, hot removal, or active clock changes.

## 19. Ubuntu Studio desktop and VM audio policy (2026-10-15)

The section 18 host results were not Ubuntu validation. The same candidate
was therefore built and loaded in the existing Ubuntu Studio VM, using a
disposable overlay of its original installation and ordinary PCI passthrough
of the physical LX-DANTE. No new installation or package upgrade was used.

| Component | Observed version |
|-----------|------------------|
| Ubuntu Studio | 26.04.1 |
| Kernel | 7.0.0-38-generic |
| PipeWire, PulseAudio compatibility, PipeWire-JACK | 1.6.2-1ubuntu1.1 |
| WirePlumber | 0.5.13-1ubuntu1 |
| VLC | 3.0.23 |
| qpwgraph | 0.9.9 |

The normal logged-in desktop PipeWire, pipewire-pulse and WirePlumber
services were used. Pro Audio exposed 128 capture, 128 playback and 128
monitor ports. Ubuntu Studio's 1024-frame/48-kHz quantum was retained.
The candidate source identity remained `4FE4760889C950A39D0CD58`; its
Ubuntu kernel module built with `W=1` without source warnings.

Unmodified VM policy failed both capture and VLC playback. Capture wrote
zero bytes, its ALSA application pointer stayed at zero, and repeated
hardware trigger times showed recovery loops despite continuing interrupts.
VLC selected its default `pulse` output through pipewire-pulse. PipeWire
opened capture alongside playback even without a recording application;
both directions then repeatedly restarted. Driver trigger acknowledgements
completed successfully. A graph containing ports and apparently running
PCMs was therefore not evidence of working audio.

The installed, unmodified WirePlumber
[`alsa-vm.conf`](https://github.com/PipeWire/wireplumber/blob/0.5.13/src/config/wireplumber.conf.d/alsa-vm.conf)
applies a 1024-frame period and 2048-frame headroom to PCI audio nodes in
QEMU. PipeWire 1.6.2's
[ALSA implementation](https://github.com/PipeWire/pipewire/blob/1.6.2/spa/plugins/alsa/alsa-pcm.c)
chooses three periods in Pro Audio's interrupt-driven mode. The observed
buffer was therefore 3072 frames, while capture waited for quantum plus
headroom, also 3072 frames. This left no margin before overrun.

An independent control in the same VM, with the physical Dante card absent
and only emulated Intel HDA present, reproduced zero-byte capture and
repeated restarts with those defaults. After normal PCM closure, changing
only runtime headroom to zero produced 1,916,928 bytes of stereo S32 capture
in five seconds with a stable trigger time and approximately 48,000 frames
per second. An earlier attempt to change headroom while the PCM remained
open did not fix it; that failed control is also retained. This establishes
a generic VM-policy failure that does not require the Dante driver.

With only runtime `api.alsa.headroom=0` applied to the Dante playback and
capture nodes before opening the PCMs, the unchanged candidate then passed
the two-minute direct PipeWire media run. All 11,300,608 compared signed
24-bit samples matched exactly. Playback joined an already-running capture
stream without resetting capture's trigger time. A playback-first run and
three capture-application stop/reopen cycles also preserved continuous
playback and stable hardware timing. ALSA remained at 128 channels each way,
48 kHz, S32_LE non-interleaved, 1024-frame periods and a 3072-frame buffer.

VLC's normal PulseAudio output ran for 40 seconds with continuous capture,
stable trigger times and normal frame progress. Comparing its recording
against the unprocessed WAV initially failed: 1,799,396 of 1,804,032 samples
differed, with maximum errors of 692 and 664 signed 24-bit LSBs. The wrapper
stopped at that comparison and returned the card; it did not run the planned
final Ubuntu PipeWire-JACK client probe. That failed automatic-suite result
is preserved, rather than relabeled as a complete pass.

Offline analysis explained the VLC difference exactly. Its log selected the
`samplerate` module; VLC 3.0.23's
[resampler source](https://github.com/videolan/vlc/blob/3.0.23/modules/audio_filter/resampler/src.c)
defaults to `SRC_SINC_FASTEST`. Passing the same WAV through libsamplerate
0.2.2 with that converter and a ratio of 1, then applying the float-to-S32
conversion, reproduced all 1,804,032 compared captured samples with zero
differences. No fitted gain or filter was used. Thus the discrepancy from
the original WAV was the application's documented processing, not corrupted
transport in this recording. The direct PipeWire comparison used the
original unprocessed sample reference and also matched exactly.

The two comparisons excluded startup and the final second: six seconds
at the start for direct PipeWire, and 22 seconds for VLC, whose file included
a ten-second silent lead-in for routing. These are two marked channels
through physical card self-subscriptions, not an external converter or
analog listening test. The Sound panel, qpwgraph and running VLC desktop
were inspected visually. The panel displayed 0% device levels while the
manually routed streams carried audio; its speaker Test button was not
exercised. No guest volume or scheduler changes were made.

The final guest kernel trace had no failed driver trigger or driver timeout.
The VM shut down cleanly, the exact released host module was returned with
both PCMs closed and stable stopped DMA/interrupt state, and removal of the
two temporary returns restored the complete 29-route baseline. This run
does not qualify unmodified VM audio policy, and the new Ubuntu
PipeWire-JACK probe remains unrun. Section 18's native JACK qualification
still applies to the same candidate on the Arch host.

Normal host consumers were subsequently restored and all task guards and
the remaining NetAudio hold were released. The 35 active units, eight player
sessions, 120 JACK rules, canonical shared state and ten volume hashes
matched the baseline. All native graph connections outside the meter
collector matched; twelve volume outputs had different collector input
numbers. One previously empty playlist title was populated, with other
recorded player state unchanged. JACK accumulated three xruns while its
consumers returned, then stayed at three during the owner's 20-second
stability check. Restoration is therefore not reported as a zero-xrun run.

Evidence is retained under
`/home/chris/Documents/Codex/2026-10-15/dante-ubuntu-pipewire-current`:
`attempt-02-capture-failure/`, `attempt-03-default-vlc-failure/`,
`hda-policy/`, `hda-policy-after-close/`, and
`vm-default-policy-evidence.txt`, together with `ubuntu-passthrough-media/`,
`ubuntu-passthrough-cycles/`, `ubuntu-passthrough-vlc/`,
`vlc-resampler-analysis.json`, the offline analyzer, live desktop screenshots,
`window-result.json`, `ubuntu-qualification-summary.json`,
`root-restoration-comparison.json`, and the module/route restoration readbacks.
The initial attempt failed before audio
because graphical login was sent before the greeter was ready; its evidence
is retained separately. None of these failed attempts is a passing result.

This is the retained Ubuntu installation and the available LX-DANTE, not
Simone's Four Audio hardware. His exact Ubuntu and package versions were
not supplied. The VM-specific failure does not establish the cause of his
native-machine report.
