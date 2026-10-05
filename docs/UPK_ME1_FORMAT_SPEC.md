# Mirror's Edge (UE3 v536 / Lic 43) Package Format Specification
**Binary Package (`.u`, `.upk`, `.me1`) & Asset Serialization Specification**  
*Reverse-engineered for clean-room macOS playable runtime in `mierrorsedgere`.*  
*Target Data Source: `/Users/tomnom/mirrorsedge/TdGame/CookedPC/`*

---

## 1. Executive Summary & File Formats

Mirror's Edge uses a tailored variant of **Unreal Engine 3**:
- **Engine FileVersion**: `536` (`0x0218`)
- **Licensee Version**: `43` (`0x002B`)
- **Engine Version**: `3716` (`0x0E84`)
- **Cooker Version**: `60` (`0x003C`)
- **Byte Order**: Little-Endian (`<`)

Three file extensions are present in `CookedPC`:
1. `.u`: UnrealScript bytecode packages (`Core.u`, `Engine.u`, `TdGame.u`, etc.)
2. `.upk`: Shared resource and asset packages (`EditorMeshes.upk`, `A_Bodyfalls.upk`, `Maps/Entry.upk`, etc.)
3. `.me1`: Map packages (`Tutorial_p.me1`, `Edge_p.me1`, `Edge_Pt1.me1`, etc.)

All three share the **exact same binary container format** and can be compressed (LZO1X or ZLIB) or uncompressed.

---

## 2. Package Binary Header Layout

The package header is located at offset `0x00` in the file.

| Byte Offset | Type | Field Name | Description |
|:---|:---|:---|:---|
| `0x00` | `uint32` | `Tag` | Magic constant `0x9E2A83C1` (`\xC1\x83\x2A\x9E`) |
| `0x04` | `uint16` | `FileVersion` | `536` |
| `0x06` | `uint16` | `LicenseeVersion` | `43` |
| `0x08` | `int32` | `TotalHeaderSize` | Size in bytes of uncompressed package header up to first export data |
| `0x0C` | `FString` | `FolderName` | Length-prefixed string (typically `5, "None\0"`, taking 9 bytes: `0x0C..0x15`) |
| Variable | `uint32` | `PackageFlags` | Bitfield flags (e.g., `PKG_Cooked = 0x02000000`, `PKG_ServerSideOnly`, etc.) |
| Variable | `int32` | `NameCount` | Number of entries in Name Table |
| Variable | `int32` | `NameOffset` | Byte offset to Name Table (in uncompressed stream) |
| Variable | `int32` | `ExportCount` | Number of entries in Export Table |
| Variable | `int32` | `ExportOffset` | Byte offset to Export Table (in uncompressed stream) |
| Variable | `int32` | `ImportCount` | Number of entries in Import Table |
| Variable | `int32` | `ImportOffset` | Byte offset to Import Table (in uncompressed stream) |
| Variable | `int32` | `DependsOffset` | Byte offset to Depends Table (in uncompressed stream) |
| Variable | `uint8[16]` | `GUID` | 128-bit unique identifier for package |
| Variable | `int32` | `GenerationCount` | Number of generation entries |
| Variable | `FGenerationInfo[]` | `Generations` | Array of `(int32 ExportCount, int32 NameCount, int32 NetObjectCount)` (12 bytes each) |
| Variable | `int32` | `EngineVersion` | Engine build version: `3716` |
| Variable | `int32` | `CookerVersion` | Cooker build version: `60` |
| Variable | `uint32` | `CompressionFlags` | Compression method: `0x00` None, `0x01` ZLIB, `0x02` LZO |
| Variable | `int32` | `CompressedChunkCount` | Number of compressed chunk descriptors (`0` if uncompressed) |
| Variable | `FCompressedChunk[]` | `CompressedChunks` | Array of chunk descriptors (16 bytes each) |
| Variable | `uint32` | `PackageSource` | Checksum / CRC hash of package source (only if `CompressedChunkCount > 0`) |
| Variable | `TArray<FString>` | `AdditionalPackagesToCook`| List of streaming / linked sub-level packages (e.g. `Tutorial_Art`, `Tutorial_Spt`) |

### 2.1 Compressed Chunk Descriptor (`FCompressedChunk`)
Each chunk descriptor in the header array is 16 bytes:
```cpp
struct FCompressedChunk {
    int32 UncompressedOffset;  // Target offset in the fully uncompressed package buffer
    int32 UncompressedSize;    // Size of this chunk when uncompressed
    int32 CompressedOffset;    // File offset where the compressed chunk block header begins
    int32 CompressedSize;      // Total size of compressed block on disk (including headers)
};
```

### 2.2 Compressed Chunk Block Header (On Disk at `CompressedOffset`)
At `CompressedOffset`, the block header is laid out as follows:
```cpp
struct FChunkBlockHeader {
    uint32 Magic;              // 0x9E2A83C1
    uint32 BlockSize;          // Sub-block slice size (always 131072 = 128 KB)
    uint32 TotalCompressedSize;
    uint32 TotalUncompressedSize;
    // Followed by num_blocks = ceil(TotalUncompressedSize / BlockSize) sub-block pairs:
    struct {
        uint32 SubBlockCompressedSize;
        uint32 SubBlockUncompressedSize;
    } SubBlocks[num_blocks];
};
```
Directly after the `SubBlocks` table, each sub-block's compressed payload follows sequentially.
Total block size on disk equals `16 + num_blocks * 8 + TotalCompressedSize`, matching `CompressedSize` exactly.

---

## 3. LZO1X-1 Decompression Specification

Mirror's Edge uses **LZO1X-1** for all compressed cooked packages (`CompressionFlags = 0x02`).
A clean-room implementation requires no external dependencies. The byte-level state machine is specified below:

### 3.1 State Machine
1. **Initial Literal Run**:
   - Read first byte `b = src[ip++]`.
   - If `b > 17`:
     - Copy `t = b - 17` literal bytes from `src` to `dst`.
     - Read next byte `b = src[ip++]`, set `state = 0`.
   - Else: fall through to main loop with `state = 0`.

2. **Instruction Processing**:
   - If `b < 16`:
     - **If `state == 0` (Literal Run)**:
       - Length `t = b`.
       - If `t == 0`: while `src[ip] == 0`: `t += 255; ip++`; `t += 15 + src[ip++]`.
       - Copy `t + 3` literal bytes from `src` to `dst`.
       - Read `b = src[ip++]`.
       - If `b < 16`:
         - **Short Match (M1)**:
           - Distance `m_dist = 1 + (b >> 2) + (src[ip++] << 2)`.
           - Copy 2 bytes from `dst[len - m_dist]`.
           - Trailing literals count `trailing = b & 3`.
           - If `trailing > 0`: copy `trailing` bytes from `src` to `dst`.
           - Set `state = trailing`.
           - Read `b = src[ip++]`, continue.
         - Else: set `state = 0` and fall through to match handler.
     - **If `state != 0` (Short Match M1)**:
       - Distance `m_dist = 1 + (b >> 2) + (src[ip++] << 2)`.
       - Copy 2 bytes from `dst[len - m_dist]`.
       - Trailing literals count `trailing = b & 3`.
       - If `trailing > 0`: copy `trailing` bytes from `src` to `dst`.
       - Set `state = trailing`.
       - Read `b = src[ip++]`, continue.

3. **Match Instructions**:
   - **M2 Match (`b >= 64`)**:
     - Length `m_len = ((b >> 5) - 1) + 2`.
     - Distance `m_dist = 1 + ((b >> 2) & 7) + (src[ip++] << 3)`.
     - Trailing literals `trailing = b & 3`.
   - **M3 Match (`32 <= b < 64`)**:
     - Length `m_len = b & 31`.
     - If `m_len == 0`: while `src[ip] == 0`: `m_len += 255; ip++`; `m_len += 31 + src[ip++]`.
     - `m_len += 2`.
     - Read two bytes: `lo = src[ip++]`, `hi = src[ip++]`.
     - Distance `m_dist = 1 + (lo >> 2) + (hi << 6)`.
     - Trailing literals `trailing = lo & 3`.
   - **M4 Match (`16 <= b < 32`)**:
     - Length `m_len = b & 7`.
     - If `m_len == 0`: while `src[ip] == 0`: `m_len += 255; ip++`; `m_len += 7 + src[ip++]`.
     - `m_len += 2`.
     - High distance bit `dist_high = (b & 8) << 11`.
     - Read two bytes: `lo = src[ip++]`, `hi = src[ip++]`.
     - Offset `m_off = (lo >> 2) | (hi << 6)`.
     - If `m_off == 0`: **EOF Marker** (decompression complete).
     - Distance `m_dist = 0x4000 + dist_high + m_off`.
     - Trailing literals `trailing = lo & 3`.

4. **Copy Match & Trailing Literals**:
   - Copy `m_len` bytes from `dst[len - m_dist]` to `dst`.
   - If `trailing > 0`: copy `trailing` bytes from `src` to `dst`.
   - Set `state = trailing`.
   - Read next byte `b = src[ip++]`.

---

## 4. Tables Binary Layout

### 4.1 Name Table (`FNameEntry`)
Located at `NameOffset`, containing `NameCount` entries:
```cpp
struct FNameEntry {
    int32 StringLength;  // > 0: ASCII (StringLength bytes including null)
                         // < 0: UTF-16LE (-StringLength * 2 bytes including null)
    char StringData[];   // Null-terminated string
    uint64 Flags;        // 8 bytes object/name flags
};
```

### 4.2 Import Table (`FObjectImport`)
Located at `ImportOffset`, containing `ImportCount` entries (fixed 28 bytes each):
```cpp
struct FObjectImport {
    FName ClassPackage;  // 8 bytes: int32 NameIndex, int32 Number
    FName ClassName;     // 8 bytes: int32 NameIndex, int32 Number
    int32 OuterIndex;    // Package index to Outer (0 = None, >0 = Export, <0 = Import)
    FName ObjectName;    // 8 bytes: int32 NameIndex, int32 Number
};
```
Total size: `ImportCount * 28` bytes. Exactly aligns with `ExportOffset`.

### 4.3 Export Table (`FObjectExport`)
Located at `ExportOffset`, containing `ExportCount` entries:
```cpp
struct FObjectExport {
    int32 ClassIndex;                    // >0: Export, <0: Import, 0: Class
    int32 SuperIndex;                    // Superclass index
    int32 OuterIndex;                    // Outer container index
    FName ObjectName;                    // 8 bytes: int32 NameIndex, int32 Number
    int32 Archetype;                     // Archetype object index
    uint64 ObjectFlags;                  // 64-bit object flags
    int32 SerialSize;                    // Size of serialized object data
    int32 SerialOffset;                  // Offset to object data in uncompressed package
    int32 ComponentMapCount;             // Number of components
    struct {
        FName ComponentName;             // 8 bytes
        int32 ExportIndex;               // Export index of component
    } ComponentMap[ComponentMapCount];   // 12 bytes per component
    uint32 ExportFlags;                  // EF_ForcedExport, etc.
    int32 GenerationNetObjectCountCount; // Number of generation net counts
    int32 GenerationNetObjectCount[];    // 4 bytes * count
    uint8 PackageGuid[16];               // 16 bytes GUID
    uint32 PackageFlags;                 // 4 bytes package flags
};
```
Base size when counts are 0 is 72 bytes. Total table size matches `DependsOffset - ExportOffset` exactly.

### 4.4 Depends Table
Located at `DependsOffset`, containing `ExportCount` entries:
```cpp
struct FDependsEntry {
    int32 DependencyCount;
    int32 DependencyIndices[DependencyCount]; // 1-based package indices
};
```

---

## 5. UObject Property Stream (`FPropertyTag`)

An object's data stream starts at `SerialOffset`.
For actors, an engine prefix of 32 bytes precedes properties. For components, an 8-byte prefix precedes properties. For meshes/textures, a 4-byte NetIndex precedes properties.

Properties are serialized consecutively until an `FName` with string `"None"` is encountered.

### 5.1 Tag Header
```cpp
struct FPropertyTag {
    FName Name;        // 8 bytes: NameIndex, Number
    // If Name == "None": terminator, property stream ends
    FName Type;        // 8 bytes: NameIndex, Number
    int32 Size;        // Payload size in bytes
    int32 ArrayIndex;  // Array index (0 for scalars)
    
    // Type-specific header fields:
    union {
        FName StructName;  // If Type == "StructProperty": 8 bytes struct type name
        int32 BoolVal;     // If Type == "BoolProperty": 4 bytes integer boolean value (Size == 0)
        FName EnumName;    // If Type == "ByteProperty": 8 bytes enum name (if enum-backed)
    };
    uint8 Data[Size];      // Payload bytes
};
```

### 5.2 Supported Property Payloads
- `IntProperty`: 4-byte `int32`
- `FloatProperty`: 4-byte IEEE-754 `float32`
- `BoolProperty`: Value stored in `BoolVal` tag header (`0` or `1`), `Size = 0`
- `NameProperty`: 8-byte `FName` (NameIndex, Number)
- `ObjectProperty`: 4-byte `int32` PackageIndex
- `StrProperty`: `FString` (int32 length + characters)
- `ByteProperty`: 1-byte unsigned integer or 8-byte enum `FName`
- `ArrayProperty`: `int32 Count`, followed by raw serialized items
- `StructProperty`:
  - `Vector`: `float32 X, Y, Z` (12 bytes)
  - `Rotator`: `int32 Pitch, Yaw, Roll` (12 bytes; 65536 units = 360°)
  - `Color`: `uint8 R, G, B, A` (4 bytes)
  - `LinearColor`: `float32 R, G, B, A` (16 bytes)
  - `Matrix`: `float32 M[4][4]` (64 bytes)
  - `Guid`: `uint8 Data[16]` (16 bytes)
  - `Box`: `FVector Min, FVector Max, uint8 IsValid` (25 bytes)
  - `LightingChannelContainer`: 36 or 64 bytes flags

---

## 6. Actor & Asset Serialization Formats

### 6.1 Actors (`AActor` and subclasses)
Actors begin with a 32-byte header:
1. `ClassIndex` (`int32`): Class package index
2. `SuperIndex` (`int32`): Superclass index
3. `StateExecution` (`uint8[16]`): Internal script execution and state frame
4. `NetIndex` (`int32`): Network replication index
5. `LevelActorIndex` (`int32`): Unique placed index within the level

#### Placed Level Actors in Mirror's Edge:
- `TdPlayerStart` / `PlayerStart`:
  - `Location` (`Vector`): Spawn position
  - `Rotation` (`Rotator`): Initial view direction
  - `CylinderComponent`: Collision capsule
- `StaticMeshActor`:
  - Component Map references `StaticMeshComponent0`
  - `StaticMeshComponent0` holds `StaticMesh` (ObjectRef), `Materials`, `LightingChannels`
- `InterpActor`:
  - Dynamic matinee/mover actor (doors, runner bags `S_Bag`, zipline anchors, cranes)
- `Trigger`:
  - Interaction and checkpoint trigger volumes
- `TdPlaceableCheckpoint` / `TdCheckpointVolume`:
  - Checkpoint spatial trigger and sequence links
- `DirectionalLight` / `PointLight`:
  - `LightComponent` with `Brightness`, `LightColor`, `Radius`
- Parkour Traversal Volumes:
  - `TdLedgeWalkVolume`: Ledge grab and wall-run constraints
  - `TdLadderVolume`: Pipe and ladder climbing constraints
  - `TdBalanceWalkVolume`: Catwalk / pipe balancing
  - `TdSwingVolume`: Bar swinging
  - `TdZiplineVolume`: Zipline sliding
  - `TdKillVolume` / `TdFallHeightVolume`: Lethal fall bounds
  - `BlockingVolume`: Invisible collision barriers

### 6.2 `ULevel`
- Properties (`None`)
- `TArray<int32> Actors`: Count followed by 1-based package indices of all actors in the level
- `FURL URL`: Length-prefixed map name URL (e.g. `unreal`)
- Model / BSP surface geometries

### 6.3 `UStaticMesh`
- `NetIndex` (`int32`, 4 bytes)
- Properties: `bNeedsCPUAccess`, `UseSimpleRigidBodyCollision`, `UseSimpleBoxCollision`, etc.
- `FBoxSphereBounds` (28 bytes):
  - `FVector Origin` (12 bytes)
  - `FVector BoxExtent` (12 bytes)
  - `float32 SphereRadius` (4 bytes)
- `BodySetup` (`int32` PackageIndex, 4 bytes)
- `kDOPTreeCompact`: Collision bounding volume hierarchy nodes and leaf triangles
- `TArray<FStaticMeshLODModel>`:
  - `IndexBuffer`: Triangle indices (`uint16` / `uint32`)
  - `PositionVertexBuffer`: Array of `FVector` vertex positions
  - `StaticMeshVertexBuffer`: Tangent X/Y/Z vectors, UV texture coordinates

### 6.4 `USoundNodeWave` (Embedded Ogg Vorbis Audio)
- Properties: `Duration`, `NumChannels`, `SampleRate`, `Volume`
- `FByteBulkData` (16 bytes header):
  - `BulkDataFlags` (`uint32`)
  - `ElementCount` (`int32`): Length of audio stream in bytes
  - `BulkDataSizeOnDisk` (`int32`): Stored size on disk
  - `BulkDataOffsetInFile` (`int32`): Absolute file or decompressed buffer offset
- Payload: **Raw Ogg Vorbis stream** starting with magic header `OggS` (`0x4F 0x67 0x67 0x53`).

---

## 7. Empirical Verification & Test Results

The parser tool (`tools/upk_inspector.py`) was verified across real stock packages in `/Users/tomnom/mirrorsedge/TdGame/CookedPC/`:

| Package File | Format | Compression | Names | Imports | Exports | Verified Assets & Output |
|:---|:---|:---|---:|---:|---:|:---|
| `Core.u` | Script Bytecode | LZO (1 chunk) | 660 | 19 | 1,274 | Table offsets match `d_off=108343` to the exact byte. |
| `Engine.u` | Script Bytecode | LZO (5 chunks) | 15,856 | 163 | 25,282 | Table offsets match `d_off=2296525` to the exact byte. |
| `TdGame.u` | Game Bytecode | LZO (68 chunks) | 20,592 | 260 | 52,410 | Identified all 18 Mirror's Edge custom traversal volume classes. |
| `EditorMeshes.upk` | Asset UPK | Uncompressed | 44 | 14 | 11 | 8 StaticMeshes (`TexPropPlane` extent 160x160, `TexPropCube` extent 128x128). |
| `Maps/Entry.upk` | Map Package | LZO (1 chunk) | 107 | 32 | 35 | Extracted 6 level actors: `PlayerStart0` at `(-941.49, 430.94, 208.00)`, `DirectionalLight0` at `(-1596.53, -68.18, 0.00)`. |
| `Audio/A_Bodyfalls.upk` | Sound Package | Uncompressed | 134 | 16 | 76 | Extracted 31 embedded `SoundNodeWave` Ogg Vorbis audio streams. |
| `Maps/SP00/Tutorial_p.me1`| Master Map | LZO (106 chunks)| 4,147 | 833 | 15,339 | Extracted 2,280 level actors, 1,063 StaticMeshActors, 187 StaticMeshes, 411 SoundNodeWaves. |
| `Maps/SP00/Tutorial_Spt.me1`| Gameplay Sub-Map| LZO (2 chunks) | 480 | 185 | 370 | Extracted 39 gameplay actors, Matinee `InterpActor` with `MatineeCam_SM`, `Trigger`, `PointLight`. |
| `Maps/SP01/Edge_p.me1` | Chapter 1 Map | LZO (14 chunks) | 1,467 | 454 | 1,495 | Extracted 101 level actors including courier runner bags (`InterpActor` with `S_Bag`). |

---

## 8. CLI Tool Usage (`tools/upk_inspector.py`)

The companion tool is located at `/Users/tomnom/git/mierrorsedgere/tools/upk_inspector.py`:

```bash
# Dump package header and compression information
tools/upk_inspector.py header /Users/tomnom/mirrorsedge/TdGame/CookedPC/Core.u

# List Name Table entries
tools/upk_inspector.py names /Users/tomnom/mirrorsedge/TdGame/CookedPC/Maps/Entry.upk --limit 20

# List Import Table entries
tools/upk_inspector.py imports /Users/tomnom/mirrorsedge/TdGame/CookedPC/Maps/Entry.upk

# List Export Table entries (with class filter)
tools/upk_inspector.py exports /Users/tomnom/mirrorsedge/TdGame/CookedPC/Maps/SP00/Tutorial_p.me1 --class StaticMeshActor --limit 10

# Extract Level Actor Placements (positions, rotations, scales, meshes)
tools/upk_inspector.py actors /Users/tomnom/mirrorsedge/TdGame/CookedPC/Maps/Entry.upk

# Inspect StaticMesh geometry bounds and properties
tools/upk_inspector.py mesh /Users/tomnom/mirrorsedge/TdGame/CookedPC/EditorMeshes.upk TexPropCube

# Dump object properties
tools/upk_inspector.py dump /Users/tomnom/mirrorsedge/TdGame/CookedPC/Maps/Entry.upk PlayerStart0

# Extract embedded Ogg Vorbis streams from SoundNodeWave
tools/upk_inspector.py audio /Users/tomnom/mirrorsedge/TdGame/CookedPC/Audio/A_Bodyfalls.upk --out /tmp/me_audio
```
