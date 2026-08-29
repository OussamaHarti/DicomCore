# DicomCore

## Table of Contents
- [Description](#description)
- [Key Features](#key-features)
- [Processing Pipeline](#processing-pipeline)
- [Operating Principle](#operating-principle)   
  - [1. Memory-Mapped I/O](#1-memory-mapped-io)   
  - [2. Offset-Based Parsing & Two-Pointer Algorithm](#2-offset-based-parsing--two-pointer-algorithm)   
  - [3. In-Place Anonymization](#3-in-place-anonymization)
- [Output Structure](#output-structure)
- [Performance Benchmarks](#performance-benchmarks)
- [Third-Party Libraries & Acknowledgments](#third-party-libraries--acknowledgments)
- [License](#license)

---

## Description
`DicomCore` is a lightweight, high-performance C++ DICOM parser developed to overcome the traditional bottlenecks of interpreted data science pipelines (such as Python's `pydicom`). By eliminating excessive RAM consumption and I/O overhead, it provides rapid extraction of raw pixel data and secure in-place anonymization for large-scale medical datasets.

Its primary goal is academic and professional: bridging the gap between clinical PACS storage infrastructure and AI research workflows, ensuring strict data privacy compliance. DicomCore is designed to process large DICOM repositories while isolating invalid files and preserving detailed processing diagnostics.

---

## Key Features

- **High-performance C++17 processing** for large-scale DICOM datasets.
- **Memory-Mapped I/O** to minimize memory usage and unnecessary data copies.
- **Offset-based parsing** without instantiating complete DICOM object representations.
- **Two-pointer tag matching** for efficient selective metadata extraction.
- **Multithreaded processing** with shared read-only resources and controlled concurrent access.
- **DICOM validation** with mandatory-tag checks and automatic isolation of invalid or non-conforming files.
- **Fault isolation**: corrupted or problematic files are discarded without contaminating the generated CSV.
- **Detailed error logging** with file-specific error descriptions.
- **RAW pixel-data extraction and decompression** for supported Transfer Syntaxes.
- **Explicit and Implicit VR** handling.
- **Little Endian and Big Endian** handling.
- **Private and sensitive tag management** with configurable privacy restrictions and authorized overrides.
- **In-place anonymization** to avoid unnecessary file rewriting.
- **Structured output** separating valid extracted data, RAW files, discarded DICOM files, and processing logs.

---

## Processing Pipeline

DicomCore processes each DICOM independently while maintaining a clean separation between valid output and processing errors.

```text
Input DICOM Dataset
        │
        ▼
   File Mapping
        │
        ▼
   DICOM Parsing
        │
        ├───────────────┐
        │               │
        ▼               ▼
   Validation       Parsing Error
        │               │
        │               ▼
        │          Discarded File
        │          + Error Log
        │
        ▼
 Tag Extraction
        │
        ├───────────────┐
        │               │
        ▼               ▼
   Metadata        Pixel Data
        │               │
        ▼               ▼
       CSV          Decompression
                        │
                        ▼
                      RAW
        │
        ▼
 In-Place Anonymization
```

Invalid, corrupted, or non-conforming files are isolated and moved to a dedicated directory instead of contaminating the generated CSV. Processing continues with the remaining files, while detailed errors are recorded in the processing log.

---

## Operating Principle

### 1. Memory-Mapped I/O
Traditional I/O models copy data twice (kernel space to user space). `DicomCore` uses **Memory-Mapped I/O** (`CreateFileMappingW`/`MapViewOfFile` on Windows, `mmap` on POSIX) to map files directly into the process's virtual address space. 
- **Demand Paging**: Only the accessed parts are loaded into RAM.
- Since DICOM pixel data (typically >90% of file size) sits at the end of the binary stream, the parser bypasses it entirely during metadata extraction, saving massive amounts of memory.

### 2. Offset-Based Parsing & Two-Pointer Algorithm
Instead of instantiating heavy wrapper objects for every single DICOM tag, the parser scans the binary stream and records only lightweight `TagMatch` structures (instruction index, value offset, and length). 
Leveraging the strictly ascending order of DICOM tags, a **two-pointer algorithm** runs in linear time ( $O(n + m)$ instead of $O(n \cdot m)$ ), ensuring maximum scanning efficiency. Mandatory tags (like `Rows`, `Columns`) are validated on the fly, and malformed files are automatically filtered out.

### 3. In-Place Anonymization
Sensitive clinical attributes (e.g., Patient Name, ID) defined in DICOM PS3.15 are located instantly via pre-calculated offsets and directly overwritten with padding spaces (`0x20`). This avoids the heavy performance penalty of fully rewriting files to disk.

---

## Output Structure

After processing, DicomCore generates a structured output directory:

```text
output/
│
├── discarded/
│   ├── invalid_file_001.dcm
│   ├── corrupted_file_002.dcm
│   └── ...
│
├── raw/
│   ├── image_001.raw
│   ├── image_002.raw
│   └── ...
│
├── extracted.csv
│
└── log.txt
```

- **`discarded/`** — contains DICOM files that could not be safely processed or did not pass validation.
- **`raw/`** — contains extracted and decompressed pixel data.
- **`extracted.csv`** — contains the metadata requested through `tags_config.txt`.
- **`log.txt`** — contains detailed processing errors and diagnostic information.

This separation ensures that invalid or corrupted files do not contaminate the generated dataset while still preserving them for inspection and debugging.

---

## Performance Benchmarks

Tested on a real-world dataset of **33,026 DICOM files (21.6 GB)**:

- **Speed**: Up to **$\times 7.57$ faster** than Python (`pydicom` with `ProcessPoolExecutor`), peaking at 4 threads (142s for C++ vs 1075s for Python).
- **Memory Footprint**: Maintains a constant **17.8 MB** footprint regardless of thread count, whereas Python scales dynamically according to:
  $$\text{RAM}_{Python}(N) = 154.1 + 26.8 \cdot N \quad [\text{MB}]$$
  At 4 threads, Python consumes **261.3 MB** compared to DicomCore's **17.8 MB** ($\times 14.7$ reduction).

> ![Graph](https://i.ibb.co/HTvcnj8W/graph.png)

---

## How to Use

### Prerequisites
* A C++ compiler supporting **C++17** or later (e.g., MSVC, GCC, Clang).
* Visual Studio (recommended for Windows) or any standard development environment.

### Getting Started & Configuration
1. **Configuration File:**
To execute the search and extraction correctly, you must place a text file named **`tags_config.txt`** inside the input folder.
Inside this file, list the DICOM tags of interest (which can be found on [DICOM Library - DICOM Tags](https://www.dicomlibrary.com/dicom/dicom-tags/)) strictly following this format:
```text
Group,Element,VR,Description
```

* **`tags_config.txt` Example:**
```text
0010,0010,PN,Patient's Name
0010,0020,LO,Patient ID
0008,0060,CS,Modality
```
2. **Execution & Paths Setup:**
When launching the program, the application will prompt you interactively via console to specify:
* **Input Directory**: The source path containing your DICOM files.
* **Output Directory**: The destination path where processed raw data and extracted CSV metrics will be saved.

3. **Example Integration (`main.cpp`):**
```cpp
#include "entry.hpp"

int main() {
    // Initializes the engine, reads configuration, 
    // and prompts for input/output paths at runtime.
    // First parameter: number of threads for DICOM processing.
    // Second parameter: number of threads used by OpenJPEG for decompression
    entry::run(4, 4); 
    return 0;
}
```

---

## Third-Party Libraries & Acknowledgments

DicomCore integrates specialized open-source libraries for advanced compression formats:
- **[OpenJPEG](https://github.com/uclouvain/openjpeg)** – JPEG 2000 decoding (2-Clause BSD License).
- **[CharLS](https://github.com/team-charls/charls)** – JPEG-LS decoding (3-Clause BSD License).
- **[stb](https://github.com/nothings/stb)** (by Sean Barrett) – Image decoding header-only libraries (Public Domain / MIT License).

*Note: Complete license texts are available inside the `Licenses/` directory.*

---

## License

Distributed under the **MIT License**. See the [LICENSE](LICENSE) file for details.
