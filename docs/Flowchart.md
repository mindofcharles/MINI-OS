# MINI-OS Workflows

This document visualizes the core execution paths of MINI-OS using Mermaid diagrams, covering hardware drivers, filesystem internals, and kernel logic.

## 1. Boot and Kernel Initialization

This flow describes the transition from BIOS to the functional Shell.

```mermaid
graph TD
    A[Power On / Reset] --> B[BIOS Loads Boot Sector LBA 0 to 0x7C00]
    B --> C[Bootloader: Set 16-bit Segments & Stack]
    C --> MC{Firmware Reports Required<br/>Low and Extended RAM Spans?}
    MC -- No --> MF[Print M and Halt]
    MC -- Yes --> D{INT 13h EDD Available?}
    D -- Yes --> D1[Read One LBA Sector at a Time<br/>3 Attempts + Disk Reset]
    D1 -- EDD Failure --> D2[Restart Load with CHS Reads<br/>3 Attempts per Sector]
    D -- No --> D2
    D1 -- Complete --> A20E[Enable Fast A20 Gate]
    D2 -- Complete --> A20E
    D2 -- Failure --> DF[Print F and Halt]
    A20E --> A20V{Addresses One MiB Apart<br/>Remain Distinct?}
    A20V -- No --> A20F[Print A and Halt]
    A20V -- Yes --> E[Bootloader: Define GDT - Null/Code/Data]
    E --> F[Bootloader: Set CR0.PE & Far Jump to 0x08:init_pm]
    F --> KS[Kernel: Initialize Canary and Clear<br/>Complete Kernel Stack]
    KS --> G[Kernel: Initialize VGA 0xB8000 & Hardware Cursor]
    G --> LG{Clear Fixed Regions,<br/>Exercise Interrupt Stack,<br/>and Verify Canaries?}
    LG -- No --> LF[Print Memory-Guard Error and Halt]
    LG -- Yes --> IDT[Install Fatal Defaults,<br/>Exceptions 0..31, PIC IRQs,<br/>and int 0x80 Trap Gate]
    IDT --> RI[Detect CPUID and RDRAND]
    RI --> IRQ{Remap and Mask PIC,<br/>Run Dedicated-Stack IRQ/EOI Self-Test,<br/>Program PIT Channel 0?}
    IRQ -- No --> IRQF[Print Interrupt Initialization Error and Halt]
    IRQ -- Yes --> STI[Enable Maskable Interrupts]
    STI --> H[Kernel: Read Primary-Master LBA 0]
    H --> HI{Boot Code, 48-bit Image ID,<br/>and Kernel Sample Match?}
    HI -- No --> HF[Print Wrong-Device Error and Halt<br/>No Disk Writes]
    HI -- Yes --> I{Superblock Valid and<br/>Mutation Marker Clear?}
    I -- No --> IF[Print Filesystem-Unavailable Error and Halt<br/>No Automatic Format]
    I -- Yes --> K[Validate Root Inode @ Index 0]
    K -- Invalid --> IF
    K -- Valid --> L[Set CWD Inode = 0]
    L --> M[fs_rebuild_cwd_path: Generate / string]
    M --> N[Enter Shell REPL Loop]
```

## 2. Low-Level Disk I/O (ATA PIO LBA28)

This section details the synchronous primary-master handshake in `drivers.asm`.

Every polling loop is bounded and fails immediately on ATA `ERR` or `DF`.

```mermaid
flowchart TD
    Start([Call ATA I/O Routine]) --> LBA{Within LBA28 Range?}
    LBA -- No --> Fail([Return CF Set])
    LBA -- Yes --> WaitBSY1{ERR/DF Set?<br/>Timed Out?<br/>BSY == 0?}
    WaitBSY1 -- Error / Timeout --> Fail
    WaitBSY1 -- Busy --> WaitBSY1
    WaitBSY1 -- Yes (Ready) --> Program[Program Task File Registers:<br/>0x1F2: Sector Count = 1<br/>0x1F3: LBA Low<br/>0x1F4: LBA Mid<br/>0x1F5: LBA High<br/>0x1F6: Drive/Head Select]

    Program --> IssueCmd[Write Command to 0x1F7:<br/>Read: 0x20 / Write: 0x30]

    IssueCmd --> WaitDRQ{ERR/DF Set?<br/>Timed Out?<br/>DRQ == 1?}
    WaitDRQ -- Error / Timeout --> Fail
    WaitDRQ -- Not Ready --> WaitDRQ
    WaitDRQ -- Yes (Ready) --> OpType{Operation Type?}

    OpType -- Read (0x20) --> DataIn[REP INSW:<br/>Read 256 words from 0x1F0]
    DataIn --> WaitReadDone{ERR/DF Set?<br/>Timed Out?<br/>BSY == 0?}
    WaitReadDone -- Error / Timeout --> Fail
    WaitReadDone -- Busy --> WaitReadDone
    WaitReadDone -- Complete --> Finish([Return CF Clear])

    OpType -- Write (0x30) --> DataOut[REP OUTSW:<br/>Write 256 words to 0x1F0]
    DataOut --> Flush[Issue Cache Flush:<br/>Write 0xE7 to 0x1F7]
    Flush --> WaitBSY2{ERR/DF Set?<br/>Timed Out?<br/>BSY == 0?}
    WaitBSY2 -- Error / Timeout --> Fail
    WaitBSY2 -- Pending --> WaitBSY2
    WaitBSY2 -- Yes (Complete) --> Finish
```

## 3. Path Resolution (`fs_resolve_path`)

The mechanism used by `cd`, `ls`, `cat`, etc., to locate an object.

```mermaid
graph TD
    A[Start Path String] --> B{Starts with '/'?}
    B -- Yes --> C[Current Inode = 0 Root]
    B -- No --> D[Current Inode = CWD]
    C & D --> E[Extract next component via '/' delimiter]
    E --> F{Component Empty?}
    F -- Yes --> G[Return Current Inode Index]
    F -- No --> H{Is '.'?}
    H -- Yes --> E
    H -- No --> I{Is '..'?}
    I -- Yes --> J[Read Inode Metadata -> Get .parent]
    J --> K[Current Inode = Parent Inode] --> E
    I -- No --> L[fs_find_entry_in_dir: Linear scan of data blocks]
    L --> M{Match Found?}
    M -- Yes --> N[Current Inode = Entry.inode_idx] --> E
    M -- No --> O[Return -1 Error]
```

## 4. File Creation (`touch`)

Metadata allocation and directory entry synchronization.

Every return below passes through mutation completion: validation failures that occur before an operation write clear the marker, while any failure after an operation write keeps it set and disables further writes.

```mermaid
graph TD
    A[Persist Mutation Marker = 1] --> B0[fs_split_parent_name]
    B0 --> B{Parent Resolved?}
    B -- No --> C[Return -5 Invalid Path]
    B -- Yes --> D[fs_validate_name: No . or ..]
    D -- Invalid --> C
    D -- Valid --> E[fs_alloc_inode: Scan Inode Bitmap LBA 102]
    E -- Full --> F[Return -6 No Inode]
    E -- Success --> G[Initialize 64-byte Inode Buffer]
    G --> K[fs_write_inode: Commit to LBA 119+]
    K --> H[fs_find_free_entry_in_dir]
    H -- Success --> I[Write 32-byte Dir Entry to Parent Data Block]
    H -- Full --> J[fs_expand_dir: Alloc new data block]
    J --> I
    I --> L[Persist Mutation Marker = 0]
    L --> M[Return 0 Success]
    A -. I/O Failure .-> X[Keep Marker = 1;<br/>Disable Writes for This Boot]
```

## 5. Rename and Move Logic (`mv`)

The most complex operation in the filesystem (`fs_rename_path`).

```mermaid
graph TD
    A[Persist Mutation Marker = 1] --> A1[Resolve Source Path]
    A1 --> B[Resolve Destination Parent]
    B --> C{Dest exists?}
    C -- Yes --> D[Return -2 Exists]
    C -- No --> E{Source is Dir?}
    E -- Yes --> F[fs_parent_contains_inode: Prevent recursive move]
    F -- Cycle --> G[Return -3 Invalid Move]
    F -- Safe --> H[Write New Dir Entry at Destination]
    E -- No --> H
    H --> I[Read Inode Metadata]
    I --> J[Update Inode.parent and Inode.name]
    J --> K[fs_write_inode: Commit changes]
    K --> L[Clear Old Dir Entry at Source]
    L --> M[Persist Mutation Marker = 0]
    M --> N[Return Success]
```

## 6. Removal Logic (`rm`)

Cleanup of data blocks and metadata.

```mermaid
graph TD
    A[Persist Mutation Marker = 1] --> A1[Resolve Target Path]
    A1 --> B{Is Root 0?}
    B -- Yes --> C[Return -3 Deny]
    B -- No --> D{Is Directory?}
    D -- Yes --> E[fs_is_dir_empty: Scan for entries]
    E -- Not Empty --> F[Return -2 Not Empty]
    E -- Empty --> G[Proceed]
    D -- No --> G
    G --> H[fs_free_inode_data_blocks:<br/>Validate and Clear FAT Chain LBA 103-118]
    H --> I[Clear Inode Bitmap bit]
    I --> J[Zero Inode Entry on disk]
    J --> K[Clear Dir Entry in Parent]
    K --> L[Persist Mutation Marker = 0]
    L --> M[Return Success]
```

## 7. CWD Path Rebuilding

How the shell prompt (e.g., `/docs/work/`) is generated.

```mermaid
graph TD
    A[Start with cwd_inode] --> B{Is Root 0?}
    B -- Yes --> C[Path = /]
    B -- No --> D[Collect inode index into stack/array]
    D --> E[Read Inode -> Get Parent]
    E --> F[Inode = Parent]
    F --> G{Reached Root?}
    G -- No --> D
    G -- Yes --> H[Iterate collected indices in reverse]
    H --> I[Read Inode -> Append .name to string]
    I --> J[Append / to string]
    J --> K[Finalize cwd_path buffer]
```

## 8. File Editing (`edit`)

The shell editor accepts at most 510 bytes and therefore writes at most one data block.

General file I/O through the syscall ABI supports multi-block FAT chains.

```mermaid
graph TD
    A[Resolve File Inode] --> B[kbd_read_text: Polling for ESC]
    B --> B1[Persist Mutation Marker = 1]
    B1 --> C{Has Data Block?}
    C -- No --> D[fs_alloc_data_block: Allocate FAT Entry & Update Inode]
    C -- Yes --> E[Prepare 512-byte Sector Buffer]
    D --> E
    E --> F[Copy Input to Buffer]
    F --> G[ata_write_sector_lba28: Commit to Disk]
    G --> H[Update Inode.size]
    H --> I[fs_write_inode: Commit Metadata]
    I --> J[Persist Mutation Marker = 0]
```
