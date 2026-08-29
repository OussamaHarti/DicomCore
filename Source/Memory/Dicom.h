#pragma once
#include "../Decompression/algorithms.h"

class Dicom {
private:
    static inline bool processed = false;

    static inline std::vector<std::pair<uint16_t, uint16_t>> sensitive_tags = { // Vector sorted by group and element
        {0x0008, 0x0020}, // Study Date
        {0x0008, 0x0050}, // Accession Number
        {0x0008, 0x0080}, // Institution Name
        {0x0008, 0x0081}, // Institution Address
        {0x0008, 0x0090}, // Referring Physician's Name
        {0x0008, 0x1010}, // Station Name
        {0x0008, 0x1050}, // Performing Physician's Name
        {0x0008, 0x1070}, // Operators' Name
        {0x0010, 0x0010}, // Patient's Name
        {0x0010, 0x0020}, // Patient ID
        //{0x0010, 0x0030}, Ammissibile per alcuni studi
        {0x0010, 0x1000}, // Other Patient IDs
        {0x0010, 0x1001}, // Other Patient Names
        {0x0010, 0x1040}, // Patient's Address
        {0x0010, 0x2154}, // Patient's Telephone Numbers
        {0x0010, 0x4000}, // Patient's Comments
        {0x0018, 0x1000}, // Device Serial Number
        {0x0020, 0x4000}  // Image Comments
    };

    static inline std::vector<utils::Structures::DicomTagInstruction> processed_instructions = {
        {0x0002, 0x0010, "UI", "TransferSyntax",           true},
        {0x0028, 0x0002, "US", "SamplesPerPixel",          true},
        {0x0028, 0x0004, "CS", "PhotometricInterpretation", true},
        {0x0028, 0x0006, "US", "PlanarConfiguration",      false},
        {0x0028, 0x0008, "IS", "NumberOfFrames",           false},
        {0x0028, 0x0010, "US", "Rows",                     true},
        {0x0028, 0x0011, "US", "Columns",                  true},
        {0x0028, 0x0100, "US", "BitsAllocated",            true},
        {0x0028, 0x0101, "US", "BitsStored",               true},
        {0x0028, 0x0103, "US", "PixelRepresentation",      true}
    };

    static inline uint32_t mandatoryTags_counter = 0;

    bool big_endian = false;
    bool validity = false;
    std::string error = "Unchecked file";
    std::string raw_error = "Extraction error";
    bool implicit_vr = false;
    std::string transfer_syntax_uid;

    std::vector<utils::Structures::TagMatch> instructions_offsets; // instruction offset, Dicom offset, Lenght
    std::vector<utils::Structures::TagMatch> sensitive_offsets;    // instruction offset, Dicom offset, Lenght
    size_t pixeldata_offset = 0;
    uint32_t pixeldata_length = 0;
    uint16_t rows = 0, columns = 0, bits_allocated = 0, samples_per_pixel = 0;

    static void verifyInstructions(const std::vector<utils::Structures::DicomTagInstruction>& instructions, const bool& allow_override);

    template<typename Container, typename KeyFn>
    static bool advanceAndCheck(uint32_t& helper, const Container& sorted, uint16_t group, uint16_t element, KeyFn key);

    template<typename T>
    T convertBytes(const utils::Structures::MappedFile& mf, const size_t i) const;

    template<typename Raw, typename Target>
    std::string reinterpretAs(const utils::Structures::MappedFile& mf, size_t offset) const;

    std::string readString(const utils::Structures::MappedFile& mf, const size_t i, const size_t length) const;

    std::string convertValueToString(const utils::Structures::MappedFile& mf, const utils::Structures::TagMatch& match) const;

    static std::string csvEscape(const std::string& value);

    std::vector<utils::Structures::Fragment> walkFragments(const utils::Structures::MappedFile& mf, size_t start_offset) const;

public:

    Dicom(const utils::Structures::MappedFile& mf);
    Dicom() = delete;
    ~Dicom() = default;

    static void configure(const std::vector<utils::Structures::DicomTagInstruction>& instructions, const bool& allow_override);

    static std::string getInstructions();

    bool anonymizeStream(const std::filesystem::path& path) const;

    std::string getCSV(const utils::Structures::MappedFile& mf, const std::string& raw_file_path) const;

    bool getRAW(const utils::Structures::MappedFile& mf, const std::filesystem::path& raw_path, const uint8_t& num_threads);

    bool isValid() const { return validity; }

    std::string getError() const { return error; }

    std::string getRAWError() const { return raw_error; };
};