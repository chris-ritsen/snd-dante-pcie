# GN4124 PCIe-to-Local-Bus Bridge: Clean Room Hardware Specification

**For Dante PCIe Audio Card Linux ALSA Driver Development**

**Document Version:** 1.0
**Date:** 2026-03-23
**Status:** Clean Room -- derived exclusively from public datasheets and open-source headers

## Sources

| ID | Document | Doc Number | Description |
|----|----------|------------|-------------|
| [REF] | GN412x PCI Express Family Reference Manual | 52624-0, June 2009 | Full register map, interrupt controller, GPIO, local bus protocol, 169 pages |
| [DS] | GN4124 x4 Lane PCI Express to Local Bridge Data Sheet | 48407-1, May 2009 | Pinout, electrical specs, feature overview, 31 pages |
| [APP] | Implementing Multi-channel DMA with the GN412x IP | 53715-0, December 2009 | VDMA sequencer programming, scatter-gather, multi-channel DMA, 36 pages |
| [HDR] | vdma_seqcode.h (CERN gn4124-core, LGPL v2.1) | N/A | FlexDMA sequencer instruction encoding macros |

---

## Section 1 -- PCI Device

### 1.1 Device Identification

| Field | Value | Source |
|-------|-------|--------|
| PCI Vendor ID | `0x1A39` (Gennum Corporation) | [REF] p89, PCI_VENDOR register at 0x000 |
| PCI Device ID | `0x0004` (GN4124) | [REF] p89, PCI_DEVICE register at 0x002 |
| Subsystem Vendor ID | `0x1A39` (default, FR) | [REF] p98, PCI_SUB_VENDOR register at 0x02C |
| Subsystem ID | `0x04` (default, FR) | [REF] p99, PCI_SUB_SYS register at 0x02E |
| Revision ID | `0x00` (default) | [REF] p92, PCI_REVISION register at 0x008 |
| Class Code | Configurable via FR | [REF] p92, PCI_CLASS_CODE register at 0x009 |

**Note:** The Subsystem Vendor ID and Subsystem ID are set by the board manufacturer (Audinate) via the 2-wire EEPROM or local processor during initialization. The actual values on a Dante PCIe card will differ from the Gennum defaults and must be determined by hardware observation.

*Source: [REF] Section 10.4.1, p89-99*

### 1.2 BAR Layout

The GN4124 provides three Base Address Registers mapping different address spaces:

| BAR | PCI Config Offset | Purpose | Default Size | Address Space |
|-----|-------------------|---------|-------------|---------------|
| BAR0 | 0x010/0x014 | Local Bus Window 0 | Configurable (1MB-4GB) | FPGA address space (application registers + VDMA) |
| BAR2 | 0x018/0x01C | Local Bus Window 1 | Configurable (1MB-4GB) | Second FPGA address space |
| BAR4 | 0x020/0x024 | GN4124 Internal Registers | 4096 bytes (fixed) | Bridge configuration, interrupt, GPIO, I2C, FCL registers |

*Source: [REF] Section 10.4.1, p94-98*

#### BAR0 (Local Bus Window 0)
- Maps to the FPGA's local bus address space
- PCIe target reads/writes to BAR0 are translated to local bus transactions
- Lower bits of PCI_BAR0_LOW (address 0x010): TYPE field (bits 2:1) selects 32-bit or 64-bit; PREFETCH (bit 3) is hardwired to 0; IO (bit 0) is hardwired to 0 (memory space)
- Size controlled by PCI_BAR_CONFIG.SIZE0 field

*Source: [REF] p94-95*

#### BAR2 (Local Bus Window 1)
- Second local bus window, independent of BAR0
- Same structure as BAR0
- Size controlled by PCI_BAR_CONFIG.SIZE2 field

*Source: [REF] p96-97*

#### BAR4 (Internal Register Space)
- Maps GN4124 internal configuration registers
- Fixed at 4096 bytes (PREFETCH bit hardwired to 0, TYPE bits "00" for 32-bit when aperture is 4KB)
- Bit 0 (IO) hardwired to 0 (memory mapped)
- **Must not be prefetched** -- accesses to BAR4 for more than one DW will trigger ALI2 (BAR4 Target Error) interrupt
- The only exception allowing burst writes is the FCL data FIFO

*Source: [REF] p97-98*

### 1.3 BAR Sizing via PCI_BAR_CONFIG

The `PCI_BAR_CONFIG` register at BAR4 offset `0x80C` controls BAR0 and BAR2 aperture sizes. These values should only be set by factory reset or local processor before BIOS enumeration.

| Bits | Mnemonic | Description |
|------|----------|-------------|
| 15:12 | ROM_SIZE | ROM aperture size (0="Disabled", 1="2KB", ..., 14="16MB") |
| 11:8 | SIZE2 | BAR2 aperture size |
| 7:4 | RESERVED | |
| 3:0 | SIZE0 | BAR0 aperture size |

**SIZE0 and SIZE2 encoding:**

| Value | Size | Valid BASE bits (32-bit) | Valid BASE bits (64-bit) |
|-------|------|------------------------|------------------------|
| 0x0 | 1MB | 31:20 | 63:20 |
| 0x1 | 2MB | 31:21 | 63:21 |
| 0x2 | 4MB | 31:22 | 63:22 |
| 0x3 | 8MB | 31:23 | 63:23 |
| 0x4 | 16MB | 31:24 | 63:24 |
| 0x5 | 32MB | 31:25 | 63:25 |
| 0x6 | 64MB | 31:26 | 63:26 |
| 0x7 | 128MB | 31:27 | 63:27 |
| 0x8 | 256MB | 31:28 | 63:28 |
| 0x9 | 512MB | 31:29 | 63:29 |
| 0xA | 1GB | 31:30 | 63:30 |
| 0xB | 2GB | 31 | 63:31 |
| 0xC | 4GB | -- | 63:32 |

*Source: [REF] Section 10.4.7, p128-130*

### 1.4 PCIe Link Capabilities

| Parameter | GN4124 Value | Source |
|-----------|-------------|--------|
| Maximum Link Width | x4 lanes | [REF] p113, PCIE_LINK_CAP.MAX_LINK_WIDTH = 0x4 |
| Maximum Link Speed | 2.5 Gb/s per lane (Gen1) | [REF] p113, PCIE_LINK_CAP.MAX_LINK_SPEED = 0x1 |
| Negotiated Width | 1, 2, or 4 lanes | [REF] p115, PCIE_LSR.LINK_WIDTH |
| Negotiated Speed | 2.5 Gb/s | [REF] p115, PCIE_LSR.LINK_SPEED |
| PCIe Capability Version | 0x2 | [REF] p109, PCIE_CAPABILITY.VERSION |
| Device/Port Type | 0x0 (Endpoint) | [REF] p110, PCIE_CAPABILITY.TYPE |
| Max Payload Supported | 128 bytes (default) | [REF] p110, PCIE_DEVICE_CAP.MAX_PAYLOAD |
| Max Payload Configured | Set by host (128/256/512) | [REF] p111, PCIE_DCR.MAX_PAYLOAD |
| Max Read Request Size | Set by host (128-4096) | [REF] p111, PCIE_DCR.MAX_READ_SIZE |
| 64-bit addressing | Supported | [REF] p107, MSI_CONTROL.64BIT = 1 |
| MSI capable | Yes | [REF] p106 |

*Source: [REF] Section 10.4.4, p109-115; [DS] Section 2, p4*

---

## Section 2 -- GN4124 Internal Registers (BAR4)

All registers in this section are accessed through BAR4. Offsets are relative to BAR4 base. Only single DW (32-bit) accesses are permitted to BAR4 (burst accesses trigger ALI2 interrupt), except for the FCL FIFO data register.

*Source: [REF] Section 10, p82-164*

### 2.1 Register Map Summary

#### 2.1.1 PCI Type 0 Configuration Header (0x000 - 0x03F)

| Offset | Size | Register | Reset | Description |
|--------|------|----------|-------|-------------|
| 0x000 | 16-bit | PCI_VENDOR | 0x1A39 | Vendor ID (Gennum) |
| 0x002 | 16-bit | PCI_DEVICE | 0x0004 | Device ID (GN4124) |
| 0x004 | 16-bit | PCI_CMD | 0x0000 | Command register |
| 0x006 | 16-bit | PCI_STAT | 0x0010 | Status register |
| 0x008 | 8-bit | PCI_REVISION | 0x00 | Revision ID |
| 0x009 | 24-bit | PCI_CLASS_CODE | FR | Class code |
| 0x00C | 8-bit | PCI_CACHE | 0x00 | Cache line size |
| 0x00D | 8-bit | PCI_LATENCY | 0x00 | Latency timer |
| 0x00E | 8-bit | PCI_HEADER | 0x00 | Header type (single-function) |
| 0x00F | 8-bit | PCI_BIST | 0x00 | BIST |
| 0x010 | 32-bit | PCI_BAR0_LOW | 0x00000000 | BAR0 lower 32 bits |
| 0x014 | 32-bit | PCI_BAR0_HIGH | 0x00000000 | BAR0 upper 32 bits |
| 0x018 | 32-bit | PCI_BAR2_LOW | 0x00000000 | BAR2 lower 32 bits |
| 0x01C | 32-bit | PCI_BAR2_HIGH | 0x00000000 | BAR2 upper 32 bits |
| 0x020 | 32-bit | PCI_BAR4_LOW | 0x00000000 | BAR4 lower 32 bits |
| 0x024 | 32-bit | PCI_BAR4_HIGH | 0x00000000 | BAR4 upper 32 bits |
| 0x028 | 32-bit | PCI_CIS | 0x00000000 | Cardbus CIS pointer |
| 0x02C | 16-bit | PCI_SUB_VENDOR | 0x1A39 (FR) | Subsystem vendor ID |
| 0x02E | 16-bit | PCI_SUB_SYS | 0x04 (FR) | Subsystem ID |
| 0x030 | 32-bit | PCI_ROM_BASE | 0x00000000 | Expansion ROM base |
| 0x034 | 8-bit | PM_CAP_POINTER | 0x40 | Capability pointer |
| 0x03C | 8-bit | PCI_INT_LINE | 0x00 | Interrupt line |
| 0x03D | 8-bit | PCI_INT_PIN | FR | Interrupt pin |
| 0x03E | 8-bit | PCI_MIN_GNT | 0x00 | Min grant (legacy, unused) |
| 0x03F | 8-bit | PCI_MAX_LAT | 0x00 | Max latency (legacy, unused) |

*Source: [REF] Section 10.4.1, p89-101*

#### 2.1.2 Power Management Capability (0x040 - 0x047)

| Offset | Size | Register | Reset | Description |
|--------|------|----------|-------|-------------|
| 0x040 | 8-bit | PM_CAP_ID | 0x01 | PM capability ID |
| 0x041 | 8-bit | PM_NEXT_ID | 0x48 or 0x58 | Next capability pointer (0x48 if MSI enabled, else 0x58) |
| 0x042 | 16-bit | PM_CAP | 0x0003 | PM capabilities (version 1.2) |
| 0x044 | 16-bit | PM_CSR | 0x0000 | PM control/status |
| 0x046 | 8-bit | PM_CSR_BSE | 0x00 | PM bridge support extensions |
| 0x047 | 8-bit | PM_DATA | 0x00 | PM data |

*Source: [REF] Section 10.4.2, p102-105*

#### 2.1.3 MSI Capability (0x048 - 0x054)

Present in the capability chain only when PCI_SYS_CFG_SYSTEM.MSI_EN = 1.

| Offset | Size | Register | Reset | Description |
|--------|------|----------|-------|-------------|
| 0x048 | 8-bit | MSI_CAP_ID | 0x05 | MSI capability ID |
| 0x049 | 8-bit | MSI_NEXT_ID | 0x58 | Next capability (PCIe capability) |
| 0x04A | 16-bit | MSI_CONTROL | 0x0080 | MSI control (64-bit capable, bit 7=1) |
| 0x04C | 32-bit | MSI_ADDRESS_LOW | 0x00 | MSI address low (bits 31:2) |
| 0x050 | 32-bit | MSI_ADDRESS_HIGH | 0x00 | MSI address high (bits 63:32) |
| 0x054 | 16-bit | MSI_DATA | 0x00 | MSI message data |

**MSI_CONTROL bit fields:**

| Bits | Mnemonic | Type | Description |
|------|----------|------|-------------|
| 15:8 | RESERVED | R | |
| 7 | 64BIT | R | 64-bit address capable (hardwired to 1) |
| 6:4 | MUL_MSG_EN | RW | Multiple message enable (set by host: 0-7 = 1 to 128 vectors) |
| 3:1 | MUL_MSG_CAP | FR | Multiple message capable (requested vectors) |
| 0 | MSI_EN | RW | MSI enable (1=MSI, 0=legacy INTx) |

*Source: [REF] Section 10.4.3, p106-108*

#### 2.1.4 PCIe Capability (0x058 - 0x06A)

| Offset | Size | Register | Reset | Description |
|--------|------|----------|-------|-------------|
| 0x058 | 8-bit | PCIE_CAP_ID | 0x10 | PCIe capability ID |
| 0x059 | 8-bit | PCIE_NEXT_ID | 0x00 | Next capability (end of standard list) |
| 0x05A | 16-bit | PCIE_CAPABILITY | 0x0002 | PCIe capabilities (version 2, endpoint) |
| 0x05C | 32-bit | PCIE_DEVICE_CAP | see text | Device capabilities |
| 0x060 | 16-bit | PCIE_DCR | see text | Device control register |
| 0x062 | 16-bit | PCIE_DSR | 0x0000 | Device status register |
| 0x064 | 32-bit | PCIE_LINK_CAP | see text | Link capabilities |
| 0x068 | 16-bit | PCIE_LCR | 0x0000 | Link control register |
| 0x06A | 16-bit | PCIE_LSR | 0x0011 | Link status register |

*Source: [REF] Section 10.4.4, p109-115*

**PCIE_DCR (0x060) -- Device Control Register:**

| Bits | Mnemonic | Type | Reset | Description |
|------|----------|------|-------|-------------|
| 15 | BRIDGE_RETRY | FR | 0x0 | Not used, must be 0 |
| 14:12 | MAX_READ_SIZE | RW | 0x2 | Max read request (0=128, 1=256, 2=512, 3=1024, 4=2048, 5=4096) |
| 11 | NO_SNOOP | RW | 0x1 | Enable no-snoop |
| 10 | AUX_PWR_PM_EN | R | 0x0 | Auxiliary power PM (not used) |
| 9 | PHANTOM_EN | R | 0x0 | Phantom functions (not used) |
| 8 | TAG_SIZE | FR | 0x0 | Extended tag field (0=5-bit, 1=8-bit) |
| 7:5 | MAX_PAYLOAD | RW | 0x0 | Max payload size (0=128, 1=256, 2=512) |
| 4 | RELAX | RW | 0x1 | Enable relaxed ordering |
| 3 | UNSUPPORT_EN | RW | 0x0 | Unsupported request reporting |
| 2 | ERR_FATAL_EN | RW | 0x0 | Fatal error reporting enable |
| 1 | ERR_NONFATAL_EN | RW | 0x0 | Non-fatal error reporting enable |
| 0 | ERR_COR_EN | RW | 0x0 | Correctable error reporting enable |

*Source: [REF] p111-112*

**PCIE_DSR (0x062) -- Device Status Register:**

| Bits | Mnemonic | Type | Reset | Description |
|------|----------|------|-------|-------------|
| 15:6 | RESERVED | R | 0x0 | |
| 5 | PENDING | R | 0x0 | Transactions pending (non-posted requests outstanding) |
| 4 | AUX_PWR_DET | R | -- | Reflects AUX_PWR_DET in PCI_SYS_CFG_SYSTEM |
| 3 | UNSUPPORT_DET | RW | 0x0 | Unsupported request detected (write 1 to clear) |
| 2 | ERR_FATAL_DET | RW | 0x0 | Fatal error detected |
| 1 | ERR_NONFATAL_DET | RW | 0x0 | Non-fatal error detected |
| 0 | ERR_COR_DET | RW | 0x0 | Correctable error detected |

*Source: [REF] p112*

**PCIE_LSR (0x06A) -- Link Status Register:**

| Bits | Mnemonic | Type | Reset | Description |
|------|----------|------|-------|-------------|
| 15:14 | RESERVED | R | 0x0 | |
| 13 | DLL_ACTIVE | R | 0x0 | Data link layer active (not used by endpoint) |
| 12 | CLOCK_MODE | FR | 0x1 | Slot clock configuration |
| 11 | LINK_TRAIN | R | 0x0 | Link training (hardwired 0 for endpoint) |
| 10 | LINK_ERROR | R | 0x0 | Link error (deprecated) |
| 9:4 | LINK_WIDTH | R | 0x0 | Negotiated link width (1=x1, 2=x2, 4=x4) |
| 3:0 | LINK_SPEED | R | 0x1 | Negotiated link speed (1=2.5GT/s) |

*Source: [REF] p115*

#### 2.1.5 Device Serial Number Capability (0x100 - 0x108)

Present when PCI_SYS_CFG_SYSTEM.SN_EN = 1.

| Offset | Size | Register | Reset | Description |
|--------|------|----------|-------|-------------|
| 0x100 | 32-bit | DSN_CAP | see text | Extended capability header (ID=0x0003, version=1) |
| 0x104 | 32-bit | DSN_LOW | 0x0 (FR) | Serial number lower 32 bits |
| 0x108 | 32-bit | DSN_HIGH | 0x0 (FR) | Serial number upper 32 bits |

*Source: [REF] Section 10.4.5, p116-117*

#### 2.1.6 Virtual Channel Capability (0x400 - 0x426)

Present when PCI_SYS_CFG_SYSTEM.VC_EN = 1. Aliased to 0x100 when SN capability is disabled.

| Offset | Size | Register | Description |
|--------|------|----------|-------------|
| 0x400 | 32-bit | VC_CAP | VC capability header (ID=0x0002, version=1) |
| 0x404 | 32-bit | VC_PORT_CAP_1 | Port VC capability register 1 |
| 0x408 | 32-bit | VC_PORT_CAP_2 | Port VC capability register 2 |
| 0x40C | 16-bit | VC_PCR | Port control register |
| 0x40E | 16-bit | VC_PSR | Port status register |
| 0x410 | 32-bit | VC_RESOURCE_CAP0 | VC0 resource capability |
| 0x414 | 32-bit | VC_RESOURCE_CR0 | VC0 resource control |
| 0x41A | 16-bit | VC_RESOURCE_SR0 | VC0 resource status |
| 0x41C | 32-bit | VC_RESOURCE_CAP1 | VC1 resource capability |
| 0x420 | 32-bit | VC_RESOURCE_CR1 | VC1 resource control |
| 0x426 | 16-bit | VC_RESOURCE_SR1 | VC1 resource status |

*Source: [REF] Section 10.4.6, p118-124*

#### 2.1.7 System Registers (0x800 - 0x854)

| Offset | Size | Register | Reset | Description |
|--------|------|----------|-------|-------------|
| 0x800 | 32-bit | PCI_SYS_CFG_SYSTEM | see text | System configuration |
| 0x804 | 32-bit | LB_CTL | see text | Local bus control |
| 0x808 | 32-bit | CLK_CSR | see text | Clock status/control |
| 0x80C | 16-bit | PCI_BAR_CONFIG | 0x0000 | BAR size configuration |
| 0x810 | 32-bit | INT_CTRL | 0x00000000 | Interrupt control |
| 0x814 | 32-bit | INT_STAT | 0x00000000 | Interrupt status |
| 0x818 | 32-bit | PEX_ERROR_STAT | 0x00000000 | PCIe error status |
| 0x820 | 32-bit | INT_CFG0 | 0x00000000 | Interrupt config for MSI vector 0 |
| 0x824 | 32-bit | INT_CFG1 | 0x00000000 | Interrupt config for MSI vector 1 |
| 0x828 | 32-bit | INT_CFG2 | 0x00000000 | Interrupt config for MSI vector 2 |
| 0x82C | 32-bit | INT_CFG3 | 0x00000000 | Interrupt config for MSI vector 3 |
| 0x830 | 32-bit | INT_CFG4 | 0x00000000 | Interrupt config for GPIO vector 0 |
| 0x834 | 32-bit | INT_CFG5 | 0x00000000 | Interrupt config for GPIO vector 1 |
| 0x838 | 32-bit | INT_CFG6 | 0x00000000 | Interrupt config for GPIO vector 2 |
| 0x83C | 32-bit | INT_CFG7 | 0x00000000 | Interrupt config for GPIO vector 3 |
| 0x840 | 32-bit | PCI_TO_ACK_TIME | 0x00000000 | PME TO_ACK timeout |
| 0x844 | 32-bit | PEX_CDN_CFG1 | see text | PEX configuration (DO NOT CHANGE) |
| 0x848 | 32-bit | PEX_CDN_CFG2 | see text | PEX configuration 2 |
| 0x84C | 32-bit | PHY_TEST_CONTROL | 0x00000000 | PHY test (set to 0 for normal use) |
| 0x850 | 32-bit | PHY_CONTROL | 0x00000000 | PHY control (Tx drive, de-emphasis) |
| 0x854 | 32-bit | CDN_LOCK | 0x00000000 | Lock for PEX_CDN_CFG1/2 write access |

*Source: [REF] Section 10.4.7, p125-138*

#### 2.1.8 I2C (2-Wire) Interface Registers (0x900 - 0x928)

| Offset | Size | Register | Reset | Description |
|--------|------|----------|-------|-------------|
| 0x900 | 16-bit | TWI_CTRL | 0x0000 | I2C control |
| 0x904 | 16-bit | TWI_STATUS | 0x0000 | I2C status |
| 0x908 | 16-bit | TWI_ADDRESS | 0x0000 | I2C address |
| 0x90C | 16-bit | TWI_DATA | 0x0000 | I2C data |
| 0x910 | 16-bit | TWI_IRT_STATUS | 0x0000 | I2C interrupt status (clear on read) |
| 0x914 | 8-bit | TWI_TR_SIZE | 0x00 | I2C transfer size |
| 0x918 | 8-bit | TWI_SLV_MON | 0x00 | I2C slave monitor pause |
| 0x91C | 16-bit | TWI_TO | 0x001F | I2C timeout |
| 0x920 | 16-bit | TWI_IR_MASK | 0x02FF | I2C interrupt mask |
| 0x924 | 16-bit | TWI_IR_EN | WO | I2C interrupt enable (clears mask bits) |
| 0x928 | 16-bit | TWI_IR_DIS | WO | I2C interrupt disable (sets mask bits) |

*Source: [REF] Section 10.4.8, p139-145*

#### 2.1.9 GPIO Registers (0xA00 - 0xA2C)

| Offset | Size | Register | Reset | Description |
|--------|------|----------|-------|-------------|
| 0xA00 | 32-bit | GPIO_BYPASS_MODE | 0x0000 | Bypass mode (1=bypass, 0=GPIO per pin) |
| 0xA04 | 32-bit | GPIO_DIRECTION_MODE | 0x0000 | Direction (1=input, 0=output). **Output inhibits GPIO interrupts** |
| 0xA08 | 32-bit | GPIO_OUTPUT_ENABLE | 0x0000 | Output driver enable (1=enabled) |
| 0xA0C | 32-bit | GPIO_OUTPUT_VALUE | 0x0000 | Output data value |
| 0xA10 | 32-bit | GPIO_INPUT_VALUE | 0x0000 | Input data (read-only, regardless of pin mode) |
| 0xA14 | 32-bit | GPIO_INT_MASK | 0x0000 | Interrupt mask (1=masked/disabled) |
| 0xA18 | 32-bit | GPIO_INT_MASK_CLR | WO | Write 1 to clear mask bits (enable interrupts) |
| 0xA1C | 32-bit | GPIO_INT_MASK_SET | WO | Write 1 to set mask bits (disable interrupts) |
| 0xA20 | 32-bit | GPIO_INT_STATUS | 0x0000 | Interrupt status (cleared on read) |
| 0xA24 | 32-bit | GPIO_INT_TYPE | 0x0000 | Interrupt type (1=level, 0=edge) |
| 0xA28 | 32-bit | GPIO_INT_VALUE | 0x0000 | Interrupt polarity (1=high/rising, 0=low/falling) |
| 0xA2C | 32-bit | GPIO_INT_ON_ANY | 0x0000 | Edge on any (1=both edges, ignored if INT_TYPE=level) |

All GPIO registers use bits [15:0] for GPIO pins 0-15. Bits [31:16] are reserved.

*Source: [REF] Section 10.4.9, p146-157*

#### 2.1.10 FPGA Configuration Loader (FCL) Registers (0xB00 - 0xB30, 0xE00)

| Offset | Size | Register | Reset | Description |
|--------|------|----------|-------|-------------|
| 0xB00 | 16-bit | FCL_CTRL | 0x0000 | FCL control |
| 0xB04 | 16-bit | FCL_STATUS | 0x0000 | FCL status (SPRI pin states) |
| 0xB08 | 16-bit | FCL_IODATA_IN | 0x0000 | FCL GPIO input |
| 0xB0C | 16-bit | FCL_IODATA_OUT | 0x0004 | FCL GPIO output |
| 0xB10 | 16-bit | FCL_EN | 0x0017 | FCL GPIO output enable |
| 0xB14 | 16-bit | FCL_TIMER_0 | 0x0000 | FCL timer lower 16 bits |
| 0xB18 | 16-bit | FCL_TIMER_1 | 0x0000 | FCL timer upper 16 bits |
| 0xB1C | 16-bit | FCL_CLK_DIV | 0x0000 | FCL clock divider (divides 125MHz PCLK) |
| 0xB20 | 16-bit | FCL_IRQ | 0x0000 | FCL interrupt request (cleared on read) |
| 0xB24 | 16-bit | FCL_TIMER_CTRL | 0x0000 | FCL timer control (GO, RESET) |
| 0xB28 | 16-bit | FCL_IM | 0x0000 | FCL interrupt mask |
| 0xB2C | 16-bit | FCL_TIMER2_0 | 0x0000 | Generic timer2 lower |
| 0xB30 | 16-bit | FCL_TIMER2_1 | 0x0000 | Generic timer2 upper |
| 0xE00 | 32-bit | FCL_FIFO_DATA | 0x0000 | FCL data FIFO (burst-write capable) |

*Source: [REF] Section 10.4.10, p158-164*

### 2.2 Key Register Details

#### PCI_SYS_CFG_SYSTEM (0x800)

| Bits | Mnemonic | Type | Reset | Description |
|------|----------|------|-------|-------------|
| 31:21 | RESERVED | RO | 0x0 | |
| 20 | AUX_PWR_DET | RW | 0x0 | AUX power detected (controls PCIE_DSR.AUX_PWR_DET) |
| 19 | VC_EN | RW | 0x0 | Virtual channel capability enable |
| 18 | SN_EN | RW | 0x0 | Serial number capability enable |
| 17 | MSI_EN | RW | 0x0 | MSI capability enable (adds MSI to capability chain) |
| 16 | RESERVED | RO | 0x0 | |
| 15:14 | RSTOUT | RW | 0x0 | Reset output control (01=de-assert RSTOUT33, 00=assert) |
| 13:12 | LB_EN | RW | 0x0 | Local bus enable (01=enable, 00=disable/high-Z) |
| 11:8 | RESERVED | RO | 0x0 | |
| 7:6 | CFG_RETRY | FR | 0x1 | Configuration retry (01=CRS to host, 00=normal) |
| 5:0 | RESERVED | RO | 0x0 | |

*Source: [REF] p125-126*

#### CLK_CSR (0x808) -- Clock Status/Control

| Bits | Mnemonic | Type | Reset | Description |
|------|----------|------|-------|-------------|
| 31 | L_LOCK | RO | 0x0 | LCLK PLL lock status (1=locked) |
| 30 | P2L_LOCK | RO | 0x0 | P2L DLL lock status |
| 29 | L2P_LOCK | RO | 0x0 | L2P DLL lock status |
| 28 | FB | RW | 0x0 | Test feature (leave at 0) |
| 27 | RST | RW | 0x0 | PLL reset (1=reset mode, 0=normal) |
| 26:20 | DIVIN | RW | 0x0 | Input divider ratio |
| 19 | L2P_DLL_RST | RO | 0x0 | Reserved, set to 0 |
| 18:12 | DIVFB | RW | 0x13 | Feedback divider (clock multiplier) |
| 11:10 | RESERVED | RO | 0x0 | |
| 9:4 | DIVOT | RW | 0x4 | Output divider ratio |
| 3 | FS_EN | RW | 0x1 | PLL internal feedback path enable (set to 1) |
| 2:0 | L_PLL_RANGE | RW | 0x4 | PLL filter range |

**L_PLL_RANGE values:**

| Value | Filter Range |
|-------|-------------|
| 0x0 | Bypass |
| 0x1 | 5-10 MHz |
| 0x2 | 8-16 MHz |
| 0x3 | 13-26 MHz |
| 0x4 | 21-42 MHz |
| 0x5 | 34-68 MHz |
| 0x6 | 54-108 MHz |
| 0x7 | 88-200 MHz |

*Source: [REF] p128*

#### LB_CTL (0x804) -- Local Bus Control

| Bits | Mnemonic | Type | Reset | Description |
|------|----------|------|-------|-------------|
| 31:17 | RESERVED | RO | 0x0 | |
| 16 | TWI_HOST_MODE_EN | RW | 0x0 | Enable TWI host mode |
| 15:12 | P2L_VC_ARB | RW | 0x4 | PCIe-to-Local VC arbitration (0=VC1 high, 4=round-robin, 8=VC0 high) |
| 11:10 | RESERVED | RO | 0x0 | |
| 9 | P_RD_CREDIT1 | RW | 0x1 | Outstanding read credit for VC1 (0=1 outstanding, 1=up to 3) |
| 8 | RD_MASK_EN1 | RW | 0x0 | Read masking enable for VC1 |
| 7:2 | RESERVED | RO | 0x0 | |
| 1 | P_RD_CREDIT0 | RW | 0x1 | Outstanding read credit for VC0 |
| 0 | RD_MASK_EN0 | RW | 0x0 | Read masking enable for VC0 |

*Source: [REF] p127*

---

## Section 3 -- Interrupt Architecture

### 3.1 INT_STAT (0x814) -- Interrupt Source Bits

The INT_STAT register contains all interrupt sources. Bits marked RW are write-1-to-clear. Bits marked RO are status-only (clearing happens at the source).

| Bit | Mnemonic | Type | Description |
|-----|----------|------|-------------|
| 15 | GPIO | RO | GPIO block interrupt (read does NOT clear; must handle in GPIO_INT_STATUS) |
| 14 | ALI6 | RW | Local bus receive error (RX_ERROR asserted by external circuit) |
| 13 | ALI5 | RW | Local bus transmit error (TX_ERROR from L2P FIFO overflow or malformed packet) |
| 12 | ALI4 | RW | P2L FIFO overflow error for VC1 |
| 11 | ALI3 | RW | P2L FIFO overflow error for VC0 |
| 10 | ALI2 | RW | BAR4 target error (multi-DW access to BAR4) |
| 9 | ALI1 | RW | PCIe TX controller error (rejected request, debug only) |
| 8 | ALI0 | RW | PCIe controller internal error (malformed TLPs, timeouts; see PEX_ERROR_STAT) |
| 7 | INT3 | RW | External interrupt 3 (active level/edge per INT_CTRL) |
| 6 | INT2 | RW | External interrupt 2 |
| 5 | INT1 | RW | External interrupt 1 |
| 4 | INT0 | RW | External interrupt 0 |
| 3 | SWI1 | RW | Software interrupt 1 (write 1 to assert, write 0 to clear) |
| 2 | SWI0 | RW | Software interrupt 0 |
| 1 | FCLI | RO | FCL interrupt (status only; read does not clear) |
| 0 | TWI | RO | I2C interrupt (status only; read does not clear) |

*Source: [REF] p131-132*

### 3.2 INT_CTRL (0x810) -- Edge vs Level Triggering

Controls whether external interrupts INT0-INT3 use edge or level triggering:

| Bit | Mnemonic | Description |
|-----|----------|-------------|
| 3 | I3_IN_EDGE | 0=high-level sensitive, 1=rising edge on INT3 |
| 2 | I2_IN_EDGE | 0=high-level sensitive, 1=rising edge on INT2 |
| 1 | I1_IN_EDGE | 0=high-level sensitive, 1=rising edge on INT1 |
| 0 | I0_IN_EDGE | 0=high-level sensitive, 1=rising edge on INT0 |

*Source: [REF] p130-131*

### 3.3 INT_CFG0-7 (0x820 - 0x83C) -- Interrupt Routing to MSI Vectors

There are 8 interrupt configuration registers. Each has the same bit layout as INT_STAT, acting as an enable mask. When an interrupt source fires and the corresponding bit is set in an INT_CFGx register, MSI vector x is generated.

**INT_CFG0-3** route to MSI vectors 0-3.
**INT_CFG4-7** route to GPIO vectors 0-3 (directly to the GPIO block, not MSI).

Each INT_CFGx register bit layout:

| Bit | Mnemonic | Type | Description |
|-----|----------|------|-------------|
| 15 | GPIO | RW | GPIO interrupt enable (1=enabled, 0=disabled) |
| 14 | ALI6 | RW | ALI6 enable |
| 13 | ALI5 | RW | ALI5 enable |
| 12 | ALI4 | RW | ALI4 enable |
| 11 | ALI3 | RW | ALI3 enable |
| 10 | ALI2 | RW | ALI2 enable |
| 9 | ALI1 | RW | ALI1 enable |
| 8 | ALI0 | RW | ALI0 enable |
| 7 | INT3 | RW | INT3 enable |
| 6 | INT2 | RW | INT2 enable |
| 5 | INT1 | RW | INT1 enable |
| 4 | INT0 | RW | INT0 enable |
| 3 | SWI1 | RW | SWI1 enable |
| 2 | SWI0 | RW | SWI0 enable |
| 1 | FCLI | RW | FCL interrupt enable |
| 0 | TWI | RW | I2C interrupt enable |

*Source: [REF] p133*

### 3.4 GPIO-to-Interrupt Path

GPIO interrupts reach the main interrupt controller via a multi-stage path:

1. **GPIO_BYPASS_MODE** (0xA00): Pin must be in GPIO mode (bit=0), not bypass mode
2. **GPIO_DIRECTION_MODE** (0xA04): Pin must be set to input (bit=1). **Setting to output inhibits GPIO interrupts.**
3. **GPIO_INT_TYPE** (0xA24): Select level (1) or edge (0) triggering per pin
4. **GPIO_INT_VALUE** (0xA28): Select polarity -- high/rising (1) or low/falling (0)
5. **GPIO_INT_ON_ANY** (0xA2C): If edge mode (INT_TYPE=0) and this bit=1, trigger on both edges
6. **GPIO_INT_MASK** (0xA14): Active-high mask (1=masked/disabled). Use GPIO_INT_MASK_CLR (0xA18) to enable, GPIO_INT_MASK_SET (0xA1C) to disable. The set/clear mechanism prevents race conditions.
7. **GPIO_INT_STATUS** (0xA20): Latched interrupt status per pin. **Cleared on read.**
8. When any unmasked GPIO interrupt fires, INT_STAT bit 15 (GPIO) is asserted
9. GPIO bit is routed through INT_CFG0-7 to generate MSI or reach GPIO block

*Source: [REF] p146-157*

### 3.5 Interrupt Acknowledge Sequence

1. Read INT_STAT (0x814) to identify active sources
2. For RW bits (ALI0-ALI6, INT0-INT3, SWI0-SWI1): Write 1 to the asserted bits in INT_STAT to clear them
3. For RO bits:
   - GPIO (bit 15): Read GPIO_INT_STATUS (0xA20) to identify and clear GPIO sources
   - FCLI (bit 1): Read FCL_IRQ (0xB20) to identify and clear FCL sources
   - TWI (bit 0): Handle via TWI_IRT_STATUS (0x910, cleared on read)
4. If using MSI with multiple vectors, the vector number identifies which INT_CFGx register matched

*Source: [REF] p131-133*

---

## Section 4 -- Local Bus Protocol

### 4.1 Signal Groups

The local bus uses DDR (Double Data Rate) SSTL signaling with a 16-bit data path. There are two independent interfaces:

#### P2L Interface (PCIe-to-Local, Inbound Data)

| Signal | Direction | Width | Description |
|--------|-----------|-------|-------------|
| P2L_CLKp/n | GN4124->FPGA | 1 diff | Source synchronous clock for P2L data |
| P2L_DATA[15:0] | GN4124->FPGA | 16 | DDR data bus |
| P2L_DFRAME | GN4124->FPGA | 1 | Data frame (asserted during header and data phases) |
| P2L_VALID | GN4124->FPGA | 1 | Data valid (qualifies data on P2L_DATA) |
| P_WR_REQ[1:0] | GN4124->FPGA | 2 | Write request (one per VC) |
| P_WR_RDY[1:0] | FPGA->GN4124 | 2 | Write ready (flow control, one per VC) |
| P2L_RDY | FPGA->GN4124 | 1 | P2L ready (general flow control) |
| P_RD_D_RDY[1:0] | FPGA->GN4124 | 2 | Read data ready (one per VC) |

*Source: [DS] p26, Figure 4-2; [REF] Section 5.2, p43*

#### L2P Interface (Local-to-PCIe, Outbound Data)

| Signal | Direction | Width | Description |
|--------|-----------|-------|-------------|
| L2P_CLKp/n | FPGA->GN4124 | 1 diff | Source synchronous clock for L2P data |
| L2P_DATA[15:0] | FPGA->GN4124 | 16 | DDR data bus |
| L2P_DFRAME | FPGA->GN4124 | 1 | Data frame |
| L2P_VALID | FPGA->GN4124 | 1 | Data valid |
| L2P_EDB | FPGA->GN4124 | 1 | End of data burst / error |
| L_WR_RDY[1:0] | GN4124->FPGA | 2 | Write ready (Tx buffer space available) |
| L2P_RDY | GN4124->FPGA | 1 | L2P ready |
| VC_RDY[1:0] | GN4124->FPGA | 2 | Virtual channel ready |
| TX_ERROR | GN4124->FPGA | 1 | Transmit error |
| RX_ERROR | FPGA->GN4124 | 1 | Receive error |

*Source: [DS] p26, Figure 4-2; [REF] Section 5.3, p45*

#### Other Local Bus Signals

| Signal | Direction | Description |
|--------|-----------|-------------|
| LCLK/LCLKn | GN4124->FPGA | Primary local bus clock (differential SSTL) |
| LB_REF_CLK_MI/MO | Xtal->GN4124 | Low-frequency reference clock oscillator |
| LCLK_MODE[3:0] | Input | Clock mode select pins |
| GPIO[15:0] | Bidirectional | General purpose I/O |
| RSTOUT33/RSTOUT18 | GN4124->FPGA | Reset outputs |

*Source: [DS] p26, Figure 4-2*

### 4.2 Local Bus Clock Configuration

Three local clocks are used:

| Clock | Source | Description |
|-------|--------|-------------|
| LCLK/LCLKn | GN4124 output | Primary clock to FPGA, generated by PLL or derived from PCIe 125MHz |
| P2L_CLKp/n | GN4124 output | Source synchronous clock for inbound data, same source as LCLK |
| L2P_CLKp/n | FPGA output | Source synchronous clock for outbound data, derived from LCLK by FPGA |

**LCLK_MODE pin settings** (active during reset):

| LCLK_MODE[2] | LCLK_MODE[1] | LCLK_MODE[0] | Description |
|--------------|--------------|--------------|-------------|
| 0 | X | 0 | LCLK from PLL with LB_REF_CLK input (recommended) |
| 0 | X | 1 | LCLK from PLL with 125MHz PCIe clock |
| 1 | X | X | LCLK = 125MHz directly from PCIe link (PLL bypass) |

LCLK_MODE[1] controls the PLL test clock divider (set to 0 for normal operation).

*Source: [DS] p24, Table 3-12; [REF] Section 3.4, p27*

### 4.3 Transaction Types

The local bus supports four transaction types:

| Transaction | Direction | Description |
|-------------|-----------|-------------|
| Target Write | PCIe -> Local | Host writes to BAR0/BAR2, data sent to FPGA via P2L |
| Target Read | PCIe -> Local | Host reads from BAR0/BAR2, FPGA returns data via L2P |
| Master Write | Local -> PCIe | FPGA writes to host memory via L2P (DMA write) |
| Master Read | Local -> PCIe | FPGA reads from host memory; data returned via P2L (DMA read) |

*Source: [REF] Chapter 5, p40-56*

### 4.4 Packet Header Format

All local bus transactions begin with a 2-DW (64-bit) header transmitted on the 16-bit DDR bus (4 clock edges):

**Header Word 0 (first 32 bits on P2L_DATA or L2P_DATA):**

| Bits | Field | Description |
|------|-------|-------------|
| 31:28 | TYPE | Transaction type |
| 27:24 | FBE | First byte enable |
| 23:20 | LBE | Last byte enable |
| 19:10 | LENGTH | Transfer length in DWs (0=1DW for some types) |
| 9:8 | V/B | V=valid bit, B=burst bit |
| 7:4 | CID | Channel ID |
| 3:0 | STAT | Status / error code |

**Header Word 1 (next 32 bits):** Target address (32-bit) or system address lower bits

**TYPE field encoding** (from [REF] Section 5):

| TYPE[3:0] | Direction | Description |
|-----------|-----------|-------------|
| 0x0 | P2L | Target write (host writes to FPGA) |
| 0x2 | P2L | Target read (host reads from FPGA, request phase) |
| 0x4 | L2P | Target read completion (FPGA returns data) |
| 0x8 | L2P | Master write (FPGA writes to host memory) |
| 0xA | L2P | Master read (FPGA requests read from host memory) |
| 0xC | P2L | Master read completion (host returns data to FPGA) |

*Source: [REF] Section 5, p46-56*

### 4.5 Address Decoding

- BAR0 accesses map directly to local bus addresses. The local bus address is the offset within the BAR0 aperture.
- BAR2 accesses similarly map to local bus addresses through the second window.
- The FPGA's address decoder uses the local bus address to determine which internal register or memory block is being accessed.
- For DMA master operations, the FPGA provides PCIe system addresses in the L2P packet headers.

*Source: [REF] Section 5.1, p40-42*

### 4.6 Flow Control

**Write flow control:**
- `P_WR_RDY[x]` (FPGA to GN4124): FPGA asserts when ready to accept P2L write data for VC x
- `L_WR_RDY[x]` (GN4124 to FPGA): GN4124 asserts when its internal Tx buffers have space for L2P data

**Read flow control:**
- Outstanding read credits controlled by `LB_CTL.P_RD_CREDIT0/1`:
  - When 0: Only 1 outstanding read at a time per VC (wait for completion before next read)
  - When 1: Up to 3 outstanding reads per VC
- `P_RD_D_RDY[x]`: FPGA asserts when read data is ready to be sent back

**General flow control:**
- `P2L_RDY`: FPGA can deassert to stall all P2L transactions
- `L2P_RDY`: GN4124 can deassert to stall all L2P transactions
- `VC_RDY[x]`: Per-VC ready signals from GN4124

*Source: [REF] Section 5.2-5.3, p43-46; [REF] LB_CTL p127*

### 4.7 DMA Master Mode

For DMA transfers (audio data streaming), the FPGA IP core generates master read/write transactions on the L2P bus:

- **Master Write (L2P):** FPGA sends audio data to host memory. FPGA provides the 64-bit system address and data payload in L2P packets.
- **Master Read (L2P request, P2L completion):** FPGA requests data from host memory. Response comes back on P2L as master read completion.
- Maximum payload per transaction is governed by PCIE_DCR.MAX_PAYLOAD (128, 256, or 512 bytes)
- Maximum read request size governed by PCIE_DCR.MAX_READ_SIZE (128 to 4096 bytes)

*Source: [REF] Section 5.3, p45-46; Section 6, p54-56*

---

## Section 5 -- VDMA Sequencer (FlexDMA)

### 5.1 Architecture Overview

The VDMA (Virtual DMA) sequencer is a programmable microcontroller implemented in the **FPGA IP core** (Gennum FPGA IP), **not** in the GN4124 silicon itself. It executes microcode from a descriptor RAM to orchestrate DMA transfers between local bus (FPGA) and PCIe host memory.

Key characteristics:
- Runs in the FPGA, driven by the local bus clock
- Default descriptor RAM: 2K words (8KB) of 32-bit words. Some implementations have 4K words (16KB).
- Descriptor RAM is divided into code space and data space
- Supports multiple DMA channels through time-division multiplexing
- Can generate interrupts to the host via the EVENT mechanism

*Source: [APP] Section 2, p5-8; Section 3, p9-15*

### 5.2 Descriptor RAM Organization

The descriptor RAM holds both microcode (instructions) and data (scatter-gather lists, constants, channel state):

```
+---------------------------+
| Code Region               |  Address 0x0000 and up
| (VDMA instructions)       |
+---------------------------+
| ...                       |
+---------------------------+
| Data Region               |  Address set by data_address counter
| (SG lists, constants,     |
|  channel state variables)  |
+---------------------------+
```

The code region starts at address 0x0000 (set by `vdma_org()`). The data region address is set separately. Both share the same physical RAM.

*Source: [APP] Section 5, p25-34*

### 5.3 VDMA Instruction Set

All instructions are 32-bit words. The opcode is encoded in the upper bits.

*Source: [HDR] vdma_seqcode.h; [APP] Section 3, p9-15*

#### 5.3.1 Register Select Codes

| Code | Value | Description |
|------|-------|-------------|
| `_IM` | 0 | Immediate (value from descriptor RAM at ADDR) |
| `_RA` | 2 | Register A |
| `_RB` | 3 | Register B |

*Source: [HDR] lines 23-25*

#### 5.3.2 Complete Instruction Reference

**NOP -- No Operation**
```
Encoding: 0x00000000
Macro:    VDMA_NOP()
Opcode:   0x0 (bits 31:28)
```
Does nothing. Used for padding or delay.

*Source: [HDR] lines 64-65*

---

**LOAD_SYS_ADDR -- Load System Address Register**
```
Encoding: 0x40000000 | (R << 24) | (ADDR & 0xFFFF)
Macro:    VDMA_LOAD_SYS_ADDR(R, ADDR)
Opcode:   0x4 (bits 31:28)
```
Loads the 64-bit system address register pair (SYS_ADDR_H and SYS_ADDR_L) from descriptor RAM. R selects the source: `_IM` reads from descriptor RAM at ADDR, `_RB` uses RB as index into descriptor RAM. The system address is always 64 bits (two consecutive words: lower then upper).

*Source: [HDR] lines 67-71; [APP] p13*

---

**STORE_SYS_ADDR -- Store System Address Register**
```
Encoding: 0x50000000 | (R << 24) | (ADDR & 0xFFFF)
Macro:    VDMA_STORE_SYS_ADDR(R, ADDR)
Opcode:   0x5 (bits 31:28)
```
Stores the 64-bit system address to descriptor RAM. R=`_IM` stores to fixed ADDR, R=`_RB` stores at index RB. Both SYS_ADDR_H and SYS_ADDR_L are written (STORE_SYS_ADDR is 64-bit, so it also zeroes the high word since there's no 32-bit store).

*Source: [HDR] lines 73-77; [APP] p22-23*

---

**ADD_SYS_ADDR -- Add Immediate to System Address**
```
Encoding: 0x60000000 | (DATA & 0xFFFF)
Macro:    VDMA_ADD_SYS_ADDR(DATA)
Opcode:   0x6 (bits 31:28)
```
Adds a 16-bit unsigned immediate value to the 64-bit system address register. Used to advance the DMA target address between transfers.

*Source: [HDR] lines 79-82*

---

**ADD_SYS_ADDR_I -- Add Indirect to System Address**
```
Encoding: 0xE0000000 | (ADDR & 0xFFFF)
Macro:    VDMA_ADD_SYS_ADDR_I(ADDR)
Opcode:   0xE (bits 31:28)
```
Adds the 32-bit value at descriptor RAM address ADDR to the system address. Allows variable-size address increments.

*Source: [HDR] lines 84-87*

---

**LOAD_XFER_CTL -- Load Transfer Control and Initiate DMA**
```
Encoding: 0xF0000000 | (R << 24) | (ADDR & 0xFFFF)
Macro:    VDMA_LOAD_XFER_CTL(R, ADDR)
Opcode:   0xF (bits 31:28)
```
Loads the XFER_CTL register from descriptor RAM (R=`_IM` at ADDR, R=`_RB` indexed by RB) and **initiates a DMA transfer**. This is the instruction that actually starts hardware DMA. The C (carry) bit in the hardware is updated when the transfer completes.

**VDMA_XFER_CTL format:**

| Bits | Field | Description |
|------|-------|-------------|
| 31 | C | Carry/interrupt flag -- set by hardware on completion |
| 30:24 | STREAM_ID[7:0] | Stream identifier for the transfer |
| 23:13 | (reserved) | |
| 12 | D | Direction (implementation-defined) |
| 11:0 | CNT[11:0] | Transfer byte count (0x000 = 4096 bytes, otherwise byte count) |

When XFER_CTL is zero, no DMA transfer is performed. This is used to mark retired scatter-gather entries.

*Source: [HDR] lines 89-93; [APP] p14-15, p30-31*

---

**LOAD_RA -- Load Register A**
```
Encoding: 0x20000000 | (ADDR & 0xFFFF)
Macro:    VDMA_LOAD_RA(ADDR)
Opcode:   0x20 (bits 31:24)
```
Loads RA from descriptor RAM at ADDR.

*Source: [HDR] lines 95-98*

---

**ADD_RA -- Add to Register A**
```
Encoding: 0x21000000 | (ADDR & 0xFFFF)
Macro:    VDMA_ADD_RA(ADDR)
Opcode:   0x21 (bits 31:24)
```
Adds the value at descriptor RAM ADDR to RA.

*Source: [HDR] lines 100-103*

---

**LOAD_RB -- Load Register B**
```
Encoding: 0x24000000 | (ADDR & 0xFFFF)
Macro:    VDMA_LOAD_RB(ADDR)
Opcode:   0x24 (bits 31:24)
```
Loads RB from descriptor RAM at ADDR.

*Source: [HDR] lines 105-108*

---

**ADD_RB -- Add to Register B**
```
Encoding: 0x25000000 | (ADDR & 0xFFFF)
Macro:    VDMA_ADD_RB(ADDR)
Opcode:   0x25 (bits 31:24)
```
Adds the value at descriptor RAM ADDR to RB.

*Source: [HDR] lines 110-113*

---

**STORE_RA -- Store Register A**
```
Encoding: 0xA2000000 | (ADDR & 0xFFFF)
Macro:    VDMA_STORE_RA(ADDR)
Opcode:   0xA2 (bits 31:24)
```
Stores RA to descriptor RAM at ADDR.

*Source: [HDR] lines 115-118*

---

**STORE_RB -- Store Register B**
```
Encoding: 0xA3000000 | (ADDR & 0xFFFF)
Macro:    VDMA_STORE_RB(ADDR)
Opcode:   0xA3 (bits 31:24)
```
Stores RB to descriptor RAM at ADDR.

*Source: [HDR] lines 120-123*

---

**JMP -- Conditional Jump**
```
Encoding: 0x10000000 | (C << 24) | (EXT_COND << 16) | (ADDR & 0xFFFF)
Macro:    VDMA_JMP(C, EXT_COND, ADDR)
Opcode:   0x1 (bits 31:28)
```
Conditional jump to ADDR based on condition code C and optional external condition EXT_COND.

*Source: [HDR] lines 125-130*

---

**SIG_EVENT -- Signal Event**
```
Encoding: 0x80000000 | (S << 27) | (A << 26) | (EVENT_EN & 0xFFFF)
Macro:    VDMA_SIG_EVENT(S, A, EVENT_EN)
Opcode:   0x8 (bits 31:28)
```
Signals events to the host:
- `S` (bit 27): When 1, set the events specified by EVENT_EN mask. When 0, clear them.
- `A` (bit 26): When 1, also acknowledge/clear the C (carry) bit from the last LOAD_XFER_CTL.
- `EVENT_EN` (bits 15:0): Bitmask of events to set or clear.

Events can generate interrupts to the host CPU. This is the mechanism by which the VDMA sequencer notifies the driver that a DMA transfer (or batch of transfers) has completed.

*Source: [HDR] lines 132-137; [APP] p14*

---

**WAIT_EVENT -- Wait for Event**
```
Encoding: 0x90000000 | ((EVENT_EN & 0xFFF) << 12) | (EVENT_STATE & 0xFFF)
Macro:    VDMA_WAIT_EVENT(EVENT_EN, EVENT_STATE)
Opcode:   0x9 (bits 31:28)
```
Stalls the sequencer until the specified events match the desired state:
- `EVENT_EN` (bits 23:12): 12-bit mask selecting which event bits to monitor
- `EVENT_STATE` (bits 11:0): Required state of the selected event bits

The sequencer resumes when `(EVENT & EVENT_EN) == EVENT_STATE`.

*Source: [HDR] lines 139-143*

### 5.4 JMP Condition Codes

| Code | Value | Binary | Description |
|------|-------|--------|-------------|
| `_RA_NEQZ` | 0x0 | 0000 | Jump if RA != 0 |
| `_RB_NEQZ` | 0x1 | 0001 | Jump if RB != 0 |
| `_NEVER` | 0x2 | 0010 | Never jump |
| `_C_LO` | 0x3 | 0011 | Jump if C (carry) == 0 |
| `_PDM_CMD_QUEUE_FULL_LO` | 0x4 | 0100 | Jump if PDM command queue not full |
| `_LDM_CMD_QUEUE_FULL_LO` | 0x5 | 0101 | Jump if LDM command queue not full |
| `_EXT_COND_LO` | 0x7 | 0111 | Jump if external condition == 0 |
| `_RA_EQZ` | 0x8 | 1000 | Jump if RA == 0 |
| `_RB_EQZ` | 0x9 | 1001 | Jump if RB == 0 |
| `_ALWAYS` | 0xA | 1010 | Always jump |
| `_C_HI` | 0xB | 1011 | Jump if C (carry) == 1 |
| `_PDM_CMD_QUEUE_FULL_HI` | 0xC | 1100 | Jump if PDM command queue full |
| `_LDM_CMD_QUEUE_FULL_HI` | 0xD | 1101 | Jump if LDM command queue full |
| `_EXT_COND_HI` | 0xF | 1111 | Jump if external condition == 1 |

**Note:** Condition codes with bit 3 set are the logical inverse of the code without bit 3. This provides a consistent complement pattern.

*Source: [HDR] lines 28-41*

### 5.5 External Condition Select Codes

Used as the EXT_COND parameter to JMP when using `_EXT_COND_HI` or `_EXT_COND_LO`:

| Code | Value | Description |
|------|-------|-------------|
| `_PDM_IDLE` | 32 | PCIe-to-local DMA engine idle |
| `_LDM_IDLE` | 33 | Local-to-PCIe DMA engine idle |
| `_EXT_COND_0` | 34 | External condition 0 (application-defined) |
| `_EXT_COND_1` | 35 | External condition 1 |
| `_EXT_COND_2` | 36 | External condition 2 |
| `_EXT_COND_3` | 37 | External condition 3 |
| `_EXT_COND_4` | 38 | External condition 4 |
| `_EXT_COND_5` | 39 | External condition 5 |
| `_EXT_COND_6` | 40 | External condition 6 |
| `_EXT_COND_7` | 41 | External condition 7 |
| `_EXT_COND_8` | 42 | External condition 8 |
| `_EXT_COND_9` | 43 | External condition 9 |
| `_EXT_COND_10` | 44 | External condition 10 |
| `_EXT_COND_11` | 45 | External condition 11 |
| `_EXT_COND_12` | 46 | External condition 12 |
| `_EXT_COND_13` | 47 | External condition 13 |
| `_EXT_COND_14` | 48 | External condition 14 |
| `_EXT_COND_15` | 49 | External condition 15 |

External conditions 0-15 are defined by the FPGA application. For audio applications, these may indicate buffer-ready states, sample clock events, or other application-specific signals.

*Source: [HDR] lines 44-61*

### 5.6 VDMA Hardware Registers

These registers are part of the FPGA IP core, accessed through the local bus (BAR0). Their exact offsets depend on the FPGA design's address map.

| Register | Description |
|----------|-------------|
| EVENT_SET | Write to set event bits |
| EVENT_CLR | Write to clear event bits |
| EVENT | Current event state (read-only) |
| EVENT_EN | Event interrupt enable mask |
| SYS_ADDR | Current 64-bit system address (read-only) |
| DPTR | Descriptor pointer (program counter) |
| XFER_CTL | Current transfer control word |
| RA | General purpose register A |
| RB | General purpose register B |
| CSR | Control/status register (start, stop, reset) |

*Source: [APP] Section 3.2, p11-12*

### 5.7 Scatter-Gather DMA Programming

#### 5.7.1 Descriptor Formats

**3-DW Descriptor** (basic, no repeat count):

| Word | Content |
|------|---------|
| 0 | VDMA_SYS_ADDR_L -- System address low 32 bits |
| 1 | VDMA_SYS_ADDR_H -- System address high 32 bits |
| 2 | VDMA_XFER_CTL -- Transfer control |

**4-DW Descriptor** (with repeat count):

| Word | Content |
|------|---------|
| 0 | VDMA_SYS_ADDR_L -- System address low 32 bits |
| 1 | VDMA_SYS_ADDR_H -- System address high 32 bits |
| 2 | VDMA_XFER_CTL -- Transfer control |
| 3 | VDMA_XFER_RPT -- Repeat count (up to 64K repetitions) |

With repeat count, the same DMA can transfer up to 64K x 4KB = 256MB over a contiguous block. The system address is automatically incremented by the transfer size after each repetition.

*Source: [APP] Section 4.1, p16-18; Section 5.1-5.2, p25-29*

#### 5.7.2 Channel State Variables in Descriptor RAM

Each DMA channel uses these variables stored in the data region of descriptor RAM:

| Variable | Description |
|----------|-------------|
| chanx_SIZE | Number of entries in the SG list (constant after init) |
| chanx_BASE | Base address of the SG list in descriptor RAM (constant after init) |
| chanx_INDEX | Current list pointer (dynamically updated by sequencer) |
| chanx_CNT | List counter for wrap detection (dynamically updated) |
| chanx_RPT | Repeat counter storage (4-DW format only) |
| chanx_SYSA | System address storage (4-DW format only, 64-bit, 2 words) |

*Source: [APP] Section 5.1, p25; Section 5.2, p27*

#### 5.7.3 Descriptor Write-Back (Dynamic List Management)

For dynamic SG lists, the sequencer clears processed entries by writing zero to VDMA_XFER_CTL (and VDMA_SYS_ADDR_H due to 64-bit store semantics). The host driver monitors these zeroed entries to know which descriptors can be reused.

Typical writeback microcode sequence:
```
VDMA_LOAD_SYS_ADDR(R=_RB, 0)       // Load sys addr from descriptor at RB index
VDMA_LOAD_XFER_CTL(R=_RB, 2)       // Load XFER_CTL and start DMA
VDMA_LOAD_SYS_ADDR(R=_IM, ZERO)    // Load zero into sys addr
VDMA_STORE_SYS_ADDR(R=_RB, 1)      // Store zero to clear XFER_CTL in descriptor
```

When XFER_CTL is zero, no DMA is performed. The host replenishes entries by writing new non-zero XFER_CTL values.

*Source: [APP] Section 4.3.1.2, p22-23*

### 5.8 Multi-Channel Scheduling

The sequencer supports multiple DMA channels through a main loop that services each channel in turn:

```
MAIN:
    vdma_channel_service(CHAN0)    // Service channel 0
    vdma_channel_service(CHAN1)    // Service channel 1
    ...
    vdma_jmp(_ALWAYS, 0, "MAIN")  // Loop forever
```

**Scheduling approaches:**
- **Round-robin:** Each channel gets one descriptor serviced per iteration (default)
- **Priority:** Certain channels can be serviced multiple times or checked first
- **Conditional:** Use EXT_COND signals to skip channels that don't need servicing

The channel service function checks if the L2P command queue is full before attempting a DMA. If full, it skips to the next channel.

*Source: [APP] Section 4.3, p21-24; Section 5, p25-34*

### 5.9 Transfer Size Constraints

- Maximum single DMA transfer: 4096 bytes (XFER_CTL.CNT = 0x000 means 4096)
- Transfers must not cross a 4KB page boundary (PCIe requirement)
- With repeat count (4-DW format): up to 64K repetitions of a single descriptor
- Descriptor RAM capacity: ~600 list entries (3-DW) in default 2K-word RAM, or ~500 entries (4-DW)
- Total queued data: 600 entries x 4KB = 2.4MB for unattended operation

*Source: [APP] Section 4.3, p23-24*

### 5.10 Interrupt Generation from Sequencer

The sequencer generates interrupts via the SIG_EVENT instruction:

1. Sequencer executes `VDMA_SIG_EVENT(S=1, A=1, EVENT_EN=mask)` after DMA completion
2. This sets event bits specified by the mask and acknowledges the C (carry) bit
3. Event bits are visible in the EVENT register
4. If corresponding EVENT_EN bits are set, an interrupt is generated
5. This interrupt typically routes to one of the external interrupt inputs (INT0-INT3) of the GN4124
6. The GN4124's INT_STAT register captures it, which routes through INT_CFG to generate MSI

The C bit in XFER_CTL (bit 31) is set by the board designer to control which transfers trigger interrupts. Only descriptors with C=1 will cause SIG_EVENT to fire (the microcode checks C after each DMA).

*Source: [APP] Section 4.2, p19-20; Section 5, p26, p29; [HDR] lines 132-137*

---

## Section 6 -- FPGA-Specific Registers (Application-Dependent)

### 6.1 Scope

Registers accessible through BAR0 (and BAR2) are defined by the FPGA design loaded onto the Audinate FPGA, **not** by the GN4124 silicon. The GN4124 reference manual and datasheet do not document these registers. The VDMA sequencer block occupies a portion of the BAR0 space, but the rest is entirely application-defined.

### 6.2 Expected FPGA Register Categories

For a Dante PCIe audio card, the FPGA is expected to contain:

| Category | Expected Functionality |
|----------|----------------------|
| Firmware Version | Read-only register identifying FPGA firmware version/revision |
| Audio Clock / Sample Counter | Registers reflecting the current audio sample position, clock source, sample rate |
| DMA Channel Configuration | Registers controlling DMA channel enable, buffer addresses, transfer parameters |
| VDMA Descriptor RAM | Memory-mapped region for writing microcode and scatter-gather lists |
| VDMA Control/Status | CSR to start/stop/reset the VDMA sequencer, read DPTR, RA, RB |
| VDMA Event Registers | EVENT, EVENT_SET, EVENT_CLR, EVENT_EN for interrupt generation |
| Audio Stream Control | Registers for enabling/disabling audio streams, channel count, format |
| External Condition Flags | Status bits connected to VDMA EXT_COND inputs (buffer ready, clock event) |
| Dante Network Interface | Registers for Dante network configuration (these may be on a separate bus) |

### 6.3 Determining FPGA Registers

Since these registers are not documented in any Gennum PDF, they must be determined by:
1. Hardware probing: Reading BAR0 at various offsets to discover register patterns
2. Firmware analysis: If FPGA bitstream documentation is available from Audinate
3. Protocol observation: Monitoring DMA transactions and interrupt patterns during normal operation
4. Comparison with known Gennum FPGA IP core reference designs

The VDMA registers (CSR, DPTR, RA, RB, EVENT, etc.) will be at a base offset within BAR0 that depends on the FPGA address decoder. A common convention from the Gennum reference design is to place VDMA registers at BAR0 + 0x4000.

*Source: [APP] p33 -- vdma_process(BAR0_BASE + 0x4000) suggests VDMA base at BAR0+0x4000*

---

## Section 7 -- Driver Implementation Guide

### 7.1 PCI Probe Sequence

```
1. pci_enable_device(pdev)
2. pci_set_master(pdev)          // Enable bus mastering for DMA
3. pci_set_dma_mask(pdev, DMA_BIT_MASK(64))   // 64-bit DMA capable
4. pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64))
5. bar0 = pci_iomap(pdev, 0, 0) // Map BAR0 (FPGA local bus window)
6. bar2 = pci_iomap(pdev, 2, 0) // Map BAR2 (second local bus window, if needed)
7. bar4 = pci_iomap(pdev, 4, 0) // Map BAR4 (GN4124 internal registers)
8. pci_alloc_irq_vectors(pdev, 1, N, PCI_IRQ_MSI | PCI_IRQ_INTX)
9. request_irq(pci_irq_vector(pdev, 0), irq_handler, IRQF_SHARED, ...)
```

*Source: Standard Linux PCI driver practice; [REF] Chapter 10 for register access requirements*

### 7.2 Bridge Initialization

After mapping BARs:

1. **Verify device identity:** Read PCI_VENDOR (BAR4+0x000) and PCI_DEVICE (BAR4+0x002)
2. **Check link status:** Read PCIE_LSR (BAR4+0x06A) for negotiated width and speed
3. **Enable local bus:** Write PCI_SYS_CFG_SYSTEM (BAR4+0x800) with LB_EN=0x01 (bits 13:12)
4. **De-assert FPGA reset:** Write PCI_SYS_CFG_SYSTEM.RSTOUT = 0x01 (bits 15:14)
5. **Configure clock:** Set CLK_CSR (BAR4+0x808) for desired LCLK frequency (verify L_LOCK bit 31)
6. **Wait for PLL lock:** Poll CLK_CSR.L_LOCK until set
7. **Disable configuration retry:** Write PCI_SYS_CFG_SYSTEM.CFG_RETRY = 0x00 (bits 7:6) if not already done
8. **Enable MSI:** Ensure PCI_SYS_CFG_SYSTEM.MSI_EN is set (the Linux PCI subsystem handles MSI_CONTROL)

*Source: [REF] Section 3.1, p20-26; Section 10.4.7, p125-128*

### 7.3 Interrupt Setup

1. **Configure INT_CTRL** (BAR4+0x810): Set edge/level for external interrupts as needed
2. **Configure INT_CFG0** (BAR4+0x820): Enable desired interrupt sources for MSI vector 0. For audio: enable INT0 (or whichever external interrupt the VDMA EVENT maps to) and optionally GPIO.
3. **Clear pending interrupts:** Read and write-back INT_STAT (BAR4+0x814)
4. **For GPIO interrupts:** Configure GPIO_DIRECTION_MODE, GPIO_INT_TYPE, GPIO_INT_VALUE, clear GPIO_INT_MASK via GPIO_INT_MASK_CLR

*Source: [REF] Section 8, p57-66; p130-133; p146-157*

### 7.4 VDMA Initialization

1. **Halt the sequencer:** Write the stop command to VDMA CSR (`2` on the observed LX-DANTE; writing `0` does not stop execution)
2. **Upload microcode:** Write instruction words to descriptor RAM via BAR0 (starting at VDMA base)
3. **Upload scatter-gather lists:** Write initial SG entries to the data region
4. **Upload constants:** Write constants (ZERO, MINUS1, THREE, FOUR, etc.) to their assigned addresses
5. **Initialize channel state:** Set chanx_SIZE, chanx_BASE, chanx_INDEX, chanx_CNT for each channel
6. **Configure events:** Set EVENT_EN to enable desired event interrupts
7. **Start the sequencer:** Write to VDMA CSR to begin execution from address 0x0000

*Source: [APP] Section 4.3, p21-24; Section 5, p33-34*

### 7.5 Audio Stream Lifecycle (ALSA PCM)

| ALSA Callback | Action |
|---------------|--------|
| `open` | Allocate per-stream state. Set hardware constraints. |
| `hw_params` | Allocate DMA buffer (`snd_pcm_lib_malloc_pages`). Size according to period/buffer settings. |
| `prepare` | Build scatter-gather list in descriptor RAM. Program channel state variables. Upload microcode if not already loaded. |
| `trigger START` | Enable the VDMA channel (set EXT_COND or event flag to allow sequencer to service this channel). |
| `trigger STOP` | Disable the VDMA channel. Wait for current transfer to complete. |
| `pointer` | Read current DMA position from VDMA hardware (DPTR or application-specific position register). Return as ALSA frame offset. |
| `close` | Free per-stream state. Remove channel from sequencer schedule. |

### 7.6 DMA Buffer Requirements

- **Allocation:** Use `dma_alloc_coherent()` or `snd_pcm_lib_malloc_pages()` for DMA-capable memory
- **Alignment:** Transfers must not cross 4KB page boundaries (PCIe TLP requirement)
- **4GB boundary:** 32-bit addressing mode limits DMA to below 4GB. Use 64-bit mode for full address range.
- **Scatter-gather:** Physical pages may be non-contiguous. Build SG list entries for each page. For contiguous buffers, use 4-DW descriptors with repeat count for efficiency.
- **Coherency:** Semaphore locations used for position tracking must be in non-cached/coherent memory

*Source: [APP] Section 4.2.1, p19-21; Section 5.2.1, p29-31*

### 7.7 Interrupt Handler Pattern

```c
irqreturn_t irq_handler(int irq, void *dev_id) {
    u32 int_stat = ioread32(bar4 + 0x814);  // Read INT_STAT
    if (!int_stat)
        return IRQ_NONE;  // Not our interrupt

    // Handle GPIO interrupts
    if (int_stat & (1 << 15)) {
        u32 gpio_stat = ioread32(bar4 + 0xA20);  // Read & clear GPIO_INT_STATUS
        // Process GPIO events (e.g., VDMA event interrupt)
    }

    // Handle external interrupts (VDMA completion)
    if (int_stat & (1 << 4)) {  // INT0
        // Read VDMA EVENT register, update ALSA pointer
        // snd_pcm_period_elapsed(substream);
    }

    // Handle error interrupts
    if (int_stat & 0x7F00) {  // ALI0-ALI6
        // Log error, read PEX_ERROR_STAT if ALI0
    }

    // Clear handled RW interrupt bits
    iowrite32(int_stat & 0x7EFC, bar4 + 0x814);  // Write 1 to clear RW bits

    return IRQ_HANDLED;
}
```

*Source: [REF] p131-133 for INT_STAT bit definitions and clear semantics*

### 7.8 Shutdown Sequence

1. **Stop ALSA streams:** Trigger STOP on all active substreams
2. **Halt VDMA sequencer:** Write the stop command (`2` on the observed LX-DANTE) to VDMA CSR
3. **Poll for idle:** Wait for sequencer to stop (check CSR status)
4. **Disable interrupts:** Write 0x0000 to all INT_CFG0-7 registers
5. **Clear pending interrupts:** Read and clear INT_STAT
6. **Disable local bus:** Write PCI_SYS_CFG_SYSTEM.LB_EN = 0x00
7. **Free IRQ:** `free_irq()`
8. **Unmap BARs:** `pci_iounmap()`
9. **Disable device:** `pci_disable_device()`

### 7.9 Error Recovery

If the VDMA sequencer or DMA engine enters an error state:

1. **Halt the engine:** Write the stop command (`2` on the observed LX-DANTE) to VDMA CSR
2. **Poll CSR:** Wait for the sequencer to report stopped state
3. **Clear error conditions:** Read and clear INT_STAT error bits, clear PEX_ERROR_STAT
4. **Reset VDMA:** Toggle reset bit in VDMA CSR if available
5. **Reinitialize:** Re-upload microcode and SG lists
6. **Restart:** Write CSR to begin execution

If the PCIe link itself has errors (ALI0, check PEX_ERROR_STAT at BAR4+0x818), more drastic recovery may be needed including a full device reset via PCI_SYS_CFG_SYSTEM.RSTOUT.

---

## Section 8 -- Appendices

### Appendix A -- Complete BAR4 Register Offset Table

| Offset | Register | Size | Section |
|--------|----------|------|---------|
| 0x000 | PCI_VENDOR | 16-bit | PCI Config |
| 0x002 | PCI_DEVICE | 16-bit | PCI Config |
| 0x004 | PCI_CMD | 16-bit | PCI Config |
| 0x006 | PCI_STAT | 16-bit | PCI Config |
| 0x008 | PCI_REVISION | 8-bit | PCI Config |
| 0x009 | PCI_CLASS_CODE | 24-bit | PCI Config |
| 0x00C | PCI_CACHE | 8-bit | PCI Config |
| 0x00D | PCI_LATENCY | 8-bit | PCI Config |
| 0x00E | PCI_HEADER | 8-bit | PCI Config |
| 0x00F | PCI_BIST | 8-bit | PCI Config |
| 0x010 | PCI_BAR0_LOW | 32-bit | PCI Config |
| 0x014 | PCI_BAR0_HIGH | 32-bit | PCI Config |
| 0x018 | PCI_BAR2_LOW | 32-bit | PCI Config |
| 0x01C | PCI_BAR2_HIGH | 32-bit | PCI Config |
| 0x020 | PCI_BAR4_LOW | 32-bit | PCI Config |
| 0x024 | PCI_BAR4_HIGH | 32-bit | PCI Config |
| 0x028 | PCI_CIS | 32-bit | PCI Config |
| 0x02C | PCI_SUB_VENDOR | 16-bit | PCI Config |
| 0x02E | PCI_SUB_SYS | 16-bit | PCI Config |
| 0x030 | PCI_ROM_BASE | 32-bit | PCI Config |
| 0x034 | PM_CAP_POINTER | 8-bit | PCI Config |
| 0x03C | PCI_INT_LINE | 8-bit | PCI Config |
| 0x03D | PCI_INT_PIN | 8-bit | PCI Config |
| 0x03E | PCI_MIN_GNT | 8-bit | PCI Config |
| 0x03F | PCI_MAX_LAT | 8-bit | PCI Config |
| 0x040 | PM_CAP_ID | 8-bit | Power Mgmt |
| 0x041 | PM_NEXT_ID | 8-bit | Power Mgmt |
| 0x042 | PM_CAP | 16-bit | Power Mgmt |
| 0x044 | PM_CSR | 16-bit | Power Mgmt |
| 0x046 | PM_CSR_BSE | 8-bit | Power Mgmt |
| 0x047 | PM_DATA | 8-bit | Power Mgmt |
| 0x048 | MSI_CAP_ID | 8-bit | MSI |
| 0x049 | MSI_NEXT_ID | 8-bit | MSI |
| 0x04A | MSI_CONTROL | 16-bit | MSI |
| 0x04C | MSI_ADDRESS_LOW | 32-bit | MSI |
| 0x050 | MSI_ADDRESS_HIGH | 32-bit | MSI |
| 0x054 | MSI_DATA | 16-bit | MSI |
| 0x058 | PCIE_CAP_ID | 8-bit | PCIe Cap |
| 0x059 | PCIE_NEXT_ID | 8-bit | PCIe Cap |
| 0x05A | PCIE_CAPABILITY | 16-bit | PCIe Cap |
| 0x05C | PCIE_DEVICE_CAP | 32-bit | PCIe Cap |
| 0x060 | PCIE_DCR | 16-bit | PCIe Cap |
| 0x062 | PCIE_DSR | 16-bit | PCIe Cap |
| 0x064 | PCIE_LINK_CAP | 32-bit | PCIe Cap |
| 0x068 | PCIE_LCR | 16-bit | PCIe Cap |
| 0x06A | PCIE_LSR | 16-bit | PCIe Cap |
| 0x100 | DSN_CAP | 32-bit | Serial Number |
| 0x104 | DSN_LOW | 32-bit | Serial Number |
| 0x108 | DSN_HIGH | 32-bit | Serial Number |
| 0x400 | VC_CAP | 32-bit | Virtual Channel |
| 0x404 | VC_PORT_CAP_1 | 32-bit | Virtual Channel |
| 0x408 | VC_PORT_CAP_2 | 32-bit | Virtual Channel |
| 0x40C | VC_PCR | 16-bit | Virtual Channel |
| 0x40E | VC_PSR | 16-bit | Virtual Channel |
| 0x410 | VC_RESOURCE_CAP0 | 32-bit | Virtual Channel |
| 0x414 | VC_RESOURCE_CR0 | 32-bit | Virtual Channel |
| 0x41A | VC_RESOURCE_SR0 | 16-bit | Virtual Channel |
| 0x41C | VC_RESOURCE_CAP1 | 32-bit | Virtual Channel |
| 0x420 | VC_RESOURCE_CR1 | 32-bit | Virtual Channel |
| 0x426 | VC_RESOURCE_SR1 | 16-bit | Virtual Channel |
| 0x800 | PCI_SYS_CFG_SYSTEM | 32-bit | System |
| 0x804 | LB_CTL | 32-bit | System |
| 0x808 | CLK_CSR | 32-bit | System |
| 0x80C | PCI_BAR_CONFIG | 16-bit | System |
| 0x810 | INT_CTRL | 32-bit | Interrupt |
| 0x814 | INT_STAT | 32-bit | Interrupt |
| 0x818 | PEX_ERROR_STAT | 32-bit | Interrupt |
| 0x820 | INT_CFG0 | 32-bit | Interrupt |
| 0x824 | INT_CFG1 | 32-bit | Interrupt |
| 0x828 | INT_CFG2 | 32-bit | Interrupt |
| 0x82C | INT_CFG3 | 32-bit | Interrupt |
| 0x830 | INT_CFG4 | 32-bit | Interrupt |
| 0x834 | INT_CFG5 | 32-bit | Interrupt |
| 0x838 | INT_CFG6 | 32-bit | Interrupt |
| 0x83C | INT_CFG7 | 32-bit | Interrupt |
| 0x840 | PCI_TO_ACK_TIME | 32-bit | System |
| 0x844 | PEX_CDN_CFG1 | 32-bit | PEX Config |
| 0x848 | PEX_CDN_CFG2 | 32-bit | PEX Config |
| 0x84C | PHY_TEST_CONTROL | 32-bit | PHY |
| 0x850 | PHY_CONTROL | 32-bit | PHY |
| 0x854 | CDN_LOCK | 32-bit | PEX Config |
| 0x900 | TWI_CTRL | 16-bit | I2C |
| 0x904 | TWI_STATUS | 16-bit | I2C |
| 0x908 | TWI_ADDRESS | 16-bit | I2C |
| 0x90C | TWI_DATA | 16-bit | I2C |
| 0x910 | TWI_IRT_STATUS | 16-bit | I2C |
| 0x914 | TWI_TR_SIZE | 8-bit | I2C |
| 0x918 | TWI_SLV_MON | 8-bit | I2C |
| 0x91C | TWI_TO | 16-bit | I2C |
| 0x920 | TWI_IR_MASK | 16-bit | I2C |
| 0x924 | TWI_IR_EN | 16-bit | I2C |
| 0x928 | TWI_IR_DIS | 16-bit | I2C |
| 0xA00 | GPIO_BYPASS_MODE | 32-bit | GPIO |
| 0xA04 | GPIO_DIRECTION_MODE | 32-bit | GPIO |
| 0xA08 | GPIO_OUTPUT_ENABLE | 32-bit | GPIO |
| 0xA0C | GPIO_OUTPUT_VALUE | 32-bit | GPIO |
| 0xA10 | GPIO_INPUT_VALUE | 32-bit | GPIO |
| 0xA14 | GPIO_INT_MASK | 32-bit | GPIO |
| 0xA18 | GPIO_INT_MASK_CLR | 32-bit | GPIO |
| 0xA1C | GPIO_INT_MASK_SET | 32-bit | GPIO |
| 0xA20 | GPIO_INT_STATUS | 32-bit | GPIO |
| 0xA24 | GPIO_INT_TYPE | 32-bit | GPIO |
| 0xA28 | GPIO_INT_VALUE | 32-bit | GPIO |
| 0xA2C | GPIO_INT_ON_ANY | 32-bit | GPIO |
| 0xB00 | FCL_CTRL | 16-bit | FCL |
| 0xB04 | FCL_STATUS | 16-bit | FCL |
| 0xB08 | FCL_IODATA_IN | 16-bit | FCL |
| 0xB0C | FCL_IODATA_OUT | 16-bit | FCL |
| 0xB10 | FCL_EN | 16-bit | FCL |
| 0xB14 | FCL_TIMER_0 | 16-bit | FCL |
| 0xB18 | FCL_TIMER_1 | 16-bit | FCL |
| 0xB1C | FCL_CLK_DIV | 16-bit | FCL |
| 0xB20 | FCL_IRQ | 16-bit | FCL |
| 0xB24 | FCL_TIMER_CTRL | 16-bit | FCL |
| 0xB28 | FCL_IM | 16-bit | FCL |
| 0xB2C | FCL_TIMER2_0 | 16-bit | FCL |
| 0xB30 | FCL_TIMER2_1 | 16-bit | FCL |
| 0xE00 | FCL_FIFO_DATA | 32-bit | FCL |

### Appendix B -- VDMA Sequencer Instruction Encoding Reference

| Instruction | Opcode Bits [31:24] | Encoding | Parameters |
|-------------|-------------------|----------|------------|
| NOP | 0x00 | `0x00000000` | None |
| JMP | 0x10 + C | `0x10000000 \| (C<<24) \| (EXT<<16) \| ADDR` | C=condition, EXT=ext_cond, ADDR=target |
| LOAD_RA | 0x20 | `0x20000000 \| ADDR` | ADDR=descriptor RAM address |
| ADD_RA | 0x21 | `0x21000000 \| ADDR` | ADDR=descriptor RAM address |
| LOAD_RB | 0x24 | `0x24000000 \| ADDR` | ADDR=descriptor RAM address |
| ADD_RB | 0x25 | `0x25000000 \| ADDR` | ADDR=descriptor RAM address |
| LOAD_SYS_ADDR | 0x40 + R | `0x40000000 \| (R<<24) \| ADDR` | R=reg select, ADDR=source |
| STORE_SYS_ADDR | 0x50 + R | `0x50000000 \| (R<<24) \| ADDR` | R=reg select, ADDR=dest |
| ADD_SYS_ADDR | 0x60 | `0x60000000 \| DATA` | DATA=16-bit immediate |
| SIG_EVENT | 0x80 | `0x80000000 \| (S<<27) \| (A<<26) \| EVENT_EN` | S=set/clr, A=ack, EVENT_EN=mask |
| WAIT_EVENT | 0x90 | `0x90000000 \| (EN<<12) \| STATE` | EN=12-bit mask, STATE=12-bit match |
| STORE_RA | 0xA2 | `0xA2000000 \| ADDR` | ADDR=descriptor RAM address |
| STORE_RB | 0xA3 | `0xA3000000 \| ADDR` | ADDR=descriptor RAM address |
| ADD_SYS_ADDR_I | 0xE0 | `0xE0000000 \| ADDR` | ADDR=indirect address |
| LOAD_XFER_CTL | 0xF0 + R | `0xF0000000 \| (R<<24) \| ADDR` | R=reg select, ADDR=source. **Initiates DMA.** |

### Appendix C -- Condition Code Reference

| Name | Value | Hex | Complement | Test |
|------|-------|-----|-----------|------|
| _RA_NEQZ | 0 | 0x0 | _RA_EQZ | RA != 0 |
| _RB_NEQZ | 1 | 0x1 | _RB_EQZ | RB != 0 |
| _NEVER | 2 | 0x2 | _ALWAYS | Never |
| _C_LO | 3 | 0x3 | _C_HI | C == 0 |
| _PDM_CMD_QUEUE_FULL_LO | 4 | 0x4 | _PDM_CMD_QUEUE_FULL_HI | PDM queue not full |
| _LDM_CMD_QUEUE_FULL_LO | 5 | 0x5 | _LDM_CMD_QUEUE_FULL_HI | LDM queue not full |
| _EXT_COND_LO | 7 | 0x7 | _EXT_COND_HI | External condition == 0 |
| _RA_EQZ | 8 | 0x8 | _RA_NEQZ | RA == 0 |
| _RB_EQZ | 9 | 0x9 | _RB_NEQZ | RB == 0 |
| _ALWAYS | 10 | 0xA | _NEVER | Always |
| _C_HI | 11 | 0xB | _C_LO | C == 1 |
| _PDM_CMD_QUEUE_FULL_HI | 12 | 0xC | _PDM_CMD_QUEUE_FULL_LO | PDM queue full |
| _LDM_CMD_QUEUE_FULL_HI | 13 | 0xD | _LDM_CMD_QUEUE_FULL_LO | LDM queue full |
| _EXT_COND_HI | 15 | 0xF | _EXT_COND_LO | External condition == 1 |

**External Condition Select:**

| Name | Value | Description |
|------|-------|-------------|
| _PDM_IDLE | 32 (0x20) | P2L DMA engine idle |
| _LDM_IDLE | 33 (0x21) | L2P DMA engine idle |
| _EXT_COND_0 | 34 (0x22) | Application-defined |
| _EXT_COND_1 | 35 (0x23) | Application-defined |
| _EXT_COND_2 through _EXT_COND_15 | 36-49 (0x24-0x31) | Application-defined |

### Appendix D -- Recommended C Constant Definitions

```c
#define GN4124_VENDOR_ID        0x1A39
#define GN4124_DEVICE_ID        0x0004

#define GN4124_PCI_VENDOR       0x000
#define GN4124_PCI_DEVICE       0x002
#define GN4124_PCI_CMD          0x004
#define GN4124_PCI_STAT         0x006
#define GN4124_PCI_REVISION     0x008
#define GN4124_PCI_CLASS_CODE   0x009

#define GN4124_SYS_CFG_SYSTEM   0x800
#define GN4124_LB_CTL           0x804
#define GN4124_CLK_CSR          0x808
#define GN4124_BAR_CONFIG       0x80C

#define GN4124_INT_CTRL         0x810
#define GN4124_INT_STAT         0x814
#define GN4124_PEX_ERROR_STAT   0x818
#define GN4124_INT_CFG0         0x820
#define GN4124_INT_CFG1         0x824
#define GN4124_INT_CFG2         0x828
#define GN4124_INT_CFG3         0x82C
#define GN4124_INT_CFG4         0x830
#define GN4124_INT_CFG5         0x834
#define GN4124_INT_CFG6         0x838
#define GN4124_INT_CFG7         0x83C

#define GN4124_PCIE_DCR         0x060
#define GN4124_PCIE_DSR         0x062
#define GN4124_PCIE_LINK_CAP    0x064
#define GN4124_PCIE_LCR         0x068
#define GN4124_PCIE_LSR         0x06A

#define GN4124_MSI_CONTROL      0x04A
#define GN4124_MSI_ADDR_LOW     0x04C
#define GN4124_MSI_ADDR_HIGH    0x050
#define GN4124_MSI_DATA         0x054

#define GN4124_TWI_CTRL         0x900
#define GN4124_TWI_STATUS       0x904
#define GN4124_TWI_ADDRESS      0x908
#define GN4124_TWI_DATA         0x90C

#define GN4124_GPIO_BYPASS_MODE     0xA00
#define GN4124_GPIO_DIRECTION_MODE  0xA04
#define GN4124_GPIO_OUTPUT_ENABLE   0xA08
#define GN4124_GPIO_OUTPUT_VALUE    0xA0C
#define GN4124_GPIO_INPUT_VALUE     0xA10
#define GN4124_GPIO_INT_MASK        0xA14
#define GN4124_GPIO_INT_MASK_CLR    0xA18
#define GN4124_GPIO_INT_MASK_SET    0xA1C
#define GN4124_GPIO_INT_STATUS      0xA20
#define GN4124_GPIO_INT_TYPE        0xA24
#define GN4124_GPIO_INT_VALUE       0xA28
#define GN4124_GPIO_INT_ON_ANY      0xA2C

#define GN4124_FCL_CTRL         0xB00
#define GN4124_FCL_STATUS       0xB04
#define GN4124_FCL_IODATA_IN    0xB08
#define GN4124_FCL_IODATA_OUT   0xB0C
#define GN4124_FCL_EN           0xB10
#define GN4124_FCL_CLK_DIV      0xB1C
#define GN4124_FCL_IRQ          0xB20
#define GN4124_FCL_IM           0xB28
#define GN4124_FCL_FIFO_DATA    0xE00

#define GN4124_INT_STAT_TWI     BIT(0)
#define GN4124_INT_STAT_FCLI    BIT(1)
#define GN4124_INT_STAT_SWI0    BIT(2)
#define GN4124_INT_STAT_SWI1    BIT(3)
#define GN4124_INT_STAT_INT0    BIT(4)
#define GN4124_INT_STAT_INT1    BIT(5)
#define GN4124_INT_STAT_INT2    BIT(6)
#define GN4124_INT_STAT_INT3    BIT(7)
#define GN4124_INT_STAT_ALI0    BIT(8)
#define GN4124_INT_STAT_ALI1    BIT(9)
#define GN4124_INT_STAT_ALI2    BIT(10)
#define GN4124_INT_STAT_ALI3    BIT(11)
#define GN4124_INT_STAT_ALI4    BIT(12)
#define GN4124_INT_STAT_ALI5    BIT(13)
#define GN4124_INT_STAT_ALI6    BIT(14)
#define GN4124_INT_STAT_GPIO    BIT(15)

#define GN4124_INT_STAT_W1C_MASK  0x7EFC

#define GN4124_SYS_CFG_MSI_EN      BIT(17)
#define GN4124_SYS_CFG_SN_EN       BIT(18)
#define GN4124_SYS_CFG_VC_EN       BIT(19)
#define GN4124_SYS_CFG_LB_EN_SHIFT 12
#define GN4124_SYS_CFG_LB_EN_MASK  (0x3 << 12)
#define GN4124_SYS_CFG_RSTOUT_SHIFT 14
#define GN4124_SYS_CFG_RSTOUT_MASK (0x3 << 14)
#define GN4124_SYS_CFG_CFG_RETRY_SHIFT 6
#define GN4124_SYS_CFG_CFG_RETRY_MASK  (0x3 << 6)

#define GN4124_CLK_CSR_L_LOCK      BIT(31)
#define GN4124_CLK_CSR_P2L_LOCK    BIT(30)
#define GN4124_CLK_CSR_L2P_LOCK    BIT(29)
#define GN4124_CLK_CSR_RST         BIT(27)
#define GN4124_CLK_CSR_FS_EN       BIT(3)

#define GN4124_PCIE_LSR_LINK_SPEED_MASK  0x000F
#define GN4124_PCIE_LSR_LINK_WIDTH_MASK  0x03F0
#define GN4124_PCIE_LSR_LINK_WIDTH_SHIFT 4

#define VDMA_SEL_IM     0
#define VDMA_SEL_RA     2
#define VDMA_SEL_RB     3

#define VDMA_CC_RA_NEQZ                    0x0
#define VDMA_CC_RB_NEQZ                    0x1
#define VDMA_CC_NEVER                      0x2
#define VDMA_CC_C_LO                       0x3
#define VDMA_CC_PDM_CMD_QUEUE_FULL_LO      0x4
#define VDMA_CC_LDM_CMD_QUEUE_FULL_LO      0x5
#define VDMA_CC_EXT_COND_LO               0x7
#define VDMA_CC_RA_EQZ                     0x8
#define VDMA_CC_RB_EQZ                     0x9
#define VDMA_CC_ALWAYS                     0xA
#define VDMA_CC_C_HI                       0xB
#define VDMA_CC_PDM_CMD_QUEUE_FULL_HI      0xC
#define VDMA_CC_LDM_CMD_QUEUE_FULL_HI      0xD
#define VDMA_CC_EXT_COND_HI               0xF

#define VDMA_EXT_PDM_IDLE       32
#define VDMA_EXT_LDM_IDLE       33
#define VDMA_EXT_COND(n)        (34 + (n))

#define VDMA_NOP()                      0x00000000u
#define VDMA_JMP(c, ext, addr)          (0x10000000u | (((c) & 0xF) << 24) | \
                                         (((ext) & 0xFF) << 16) | ((addr) & 0xFFFF))
#define VDMA_LOAD_RA(addr)              (0x20000000u | ((addr) & 0xFFFF))
#define VDMA_ADD_RA(addr)               (0x21000000u | ((addr) & 0xFFFF))
#define VDMA_LOAD_RB(addr)              (0x24000000u | ((addr) & 0xFFFF))
#define VDMA_ADD_RB(addr)               (0x25000000u | ((addr) & 0xFFFF))
#define VDMA_LOAD_SYS_ADDR(r, addr)     (0x40000000u | (((r) & 0x3) << 24) | \
                                         ((addr) & 0xFFFF))
#define VDMA_STORE_SYS_ADDR(r, addr)    (0x50000000u | (((r) & 0x3) << 24) | \
                                         ((addr) & 0xFFFF))
#define VDMA_ADD_SYS_ADDR(data)         (0x60000000u | ((data) & 0xFFFF))
#define VDMA_SIG_EVENT(s, a, en)        (0x80000000u | (((s) & 0x1) << 27) | \
                                         (((a) & 0x1) << 26) | ((en) & 0xFFFF))
#define VDMA_WAIT_EVENT(en, state)      (0x90000000u | \
                                         ((((en) & 0xFFF) << 12)) | ((state) & 0xFFF))
#define VDMA_STORE_RA(addr)             (0xA2000000u | ((addr) & 0xFFFF))
#define VDMA_STORE_RB(addr)             (0xA3000000u | ((addr) & 0xFFFF))
#define VDMA_ADD_SYS_ADDR_I(addr)       (0xE0000000u | ((addr) & 0xFFFF))
#define VDMA_LOAD_XFER_CTL(r, addr)     (0xF0000000u | (((r) & 0x3) << 24) | \
                                         ((addr) & 0xFFFF))

#define VDMA_XFER_CTL_C         BIT(31)
#define VDMA_XFER_CTL_STREAM_ID_SHIFT  24
#define VDMA_XFER_CTL_STREAM_ID_MASK   (0x7F << 24)
#define VDMA_XFER_CTL_D        BIT(12)
#define VDMA_XFER_CTL_CNT_MASK 0x0FFF
```

---

*End of Clean Room Specification*
