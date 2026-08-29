#define STB_IMAGE_IMPLEMENTATION

#include "Dicom.h"
#include "..\Decompression\stb_image.h"

void Dicom::configure(const std::vector<utils::Structures::DicomTagInstruction>& instructions, const bool& allow_override) {
    if (!processed) {
        verifyInstructions(instructions, allow_override);
        processed = true;
    }
};

std::string Dicom::getInstructions() {
    std::string buffer;
    for (const auto& tmp : processed_instructions) {
        if (tmp.group == 0x7fe0 && tmp.element == 0x0010) continue;
        buffer += tmp.name + ",";
    }

    if (!buffer.empty())  buffer += "RawPath";

    return buffer;
};

bool Dicom::anonymizeStream(const std::filesystem::path& path) const {
    std::string map_err;
    auto mf = utils::Files::mapExistingFileWrite(path, map_err);

    if (!mf.data) return false;

    for (const auto& match : sensitive_offsets) {

        if (match.value_offset + match.length <= mf.size)
            std::memset(mf.data + match.value_offset, ' ', match.length);
        else {
            utils::Files::unmapFile(mf);
            return false;
        }
    }

    utils::Files::unmapFile(mf);
    return true;
}

Dicom::Dicom(const utils::Structures::MappedFile& mf) {
    if (!processed) {
        error = "Invalid instructions";
        return;
    }

    if (mf.size < 132 || mf.data == nullptr) {
        error = "Invalid file";
        return;
    }

    if (mf.data[128] != 'D' || mf.data[129] != 'I' || mf.data[130] != 'C' || mf.data[131] != 'M') {
        error = "Invalid Dicom Signature";
        return;
    }

    uint32_t instructions_helper = 0, sensitive_helper = 0, localTags_counter = mandatoryTags_counter;

    for (size_t i = 132; i + 8 <= mf.size;) {

        uint16_t current_tag = convertBytes<uint16_t>(mf, i);
        uint16_t next_tag = convertBytes<uint16_t>(mf, i + 2);
        uint32_t length = 0;
        unsigned short header_size = 8;

        if (!implicit_vr || current_tag == 0x0002) {
            if ((mf.data[i + 4] == 'O' && mf.data[i + 5] == 'B') ||
                (mf.data[i + 4] == 'O' && mf.data[i + 5] == 'W') ||
                (mf.data[i + 4] == 'S' && mf.data[i + 5] == 'Q') ||
                (mf.data[i + 4] == 'U' && mf.data[i + 5] == 'N') ||
                (mf.data[i + 4] == 'U' && mf.data[i + 5] == 'T') ||
                (mf.data[i + 4] == 'O' && mf.data[i + 5] == 'F')) {
                length = convertBytes<uint32_t>(mf, i + 8);
                header_size = 12;
            }
            else length = convertBytes<uint16_t>(mf, i + 6);
        }
        else length = convertBytes<uint32_t>(mf, i + 4);

        if (current_tag == 0x7fe0 && next_tag == 0x0010) {
            if (localTags_counter == 0) {
                // Essential size check for decompression
                if (rows == 0 || columns == 0 || bits_allocated == 0) {
                    error = "Missing image dimension tags (Rows/Columns/BitsAllocated)";
                    return;
                }

                pixeldata_offset = i + header_size;
                pixeldata_length = length;
                validity = true;
                error = "No error";
            }
            else error = "Missing " + std::to_string(localTags_counter) + " mandatory tag(s)";
            return;
        }

        if (advanceAndCheck(sensitive_helper, sensitive_tags, current_tag, next_tag, [](const std::pair<uint16_t, uint16_t>& p) { return p; }))
            sensitive_offsets.push_back({ sensitive_helper , i + header_size , length });

        if (advanceAndCheck(instructions_helper, processed_instructions, current_tag, next_tag,
            [](const utils::Structures::DicomTagInstruction& d) { return std::make_pair(d.group, d.element); })) {

            if (current_tag == 0x0002 && next_tag == 0x0010) {

                transfer_syntax_uid = readString(mf, i + header_size, length);
                while (!transfer_syntax_uid.empty() && (transfer_syntax_uid.back() == ' ' || transfer_syntax_uid.back() == '\0'))
                    transfer_syntax_uid.pop_back();

                if (transfer_syntax_uid == "1.2.840.10008.1.2") implicit_vr = true;
                else if (transfer_syntax_uid == "1.2.840.10008.1.2.2") big_endian = true;
            }

            if (current_tag == 0x0028) {
                if (next_tag == 0x0010) rows = convertBytes<uint16_t>(mf, i + header_size);
                else if (next_tag == 0x0011) columns = convertBytes<uint16_t>(mf, i + header_size);
                else if (next_tag == 0x0100) bits_allocated = convertBytes<uint16_t>(mf, i + header_size);
                else if (next_tag == 0x0002) samples_per_pixel = convertBytes<uint16_t>(mf, i + header_size);
            }

            instructions_offsets.push_back({ instructions_helper , i + header_size, length });
            if (processed_instructions[instructions_helper].mandatory) --localTags_counter;
        }

        i += header_size + length;
    }

    error = "Dicom text file";
}

std::string Dicom::csvEscape(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) return value;

    std::string escaped = "\"";
    for (char c : value) {
        if (c == '"') escaped += "\"\"";
        else escaped += c;
    }
    escaped += "\"";
    return escaped;
}

std::string Dicom::convertValueToString(const utils::Structures::MappedFile& mf, const utils::Structures::TagMatch& match) const {
    const std::string& vr = processed_instructions[match.instruction_index].vr;

    if (vr == "US") return std::to_string(convertBytes<uint16_t>(mf, match.value_offset));
    if (vr == "UL") return std::to_string(convertBytes<uint32_t>(mf, match.value_offset));
    if (vr == "SS") return reinterpretAs<uint16_t, int16_t>(mf, match.value_offset);
    if (vr == "SL") return reinterpretAs<uint32_t, int32_t>(mf, match.value_offset);
    if (vr == "FL") return reinterpretAs<uint32_t, float>(mf, match.value_offset);
    if (vr == "FD") return reinterpretAs<uint64_t, double>(mf, match.value_offset);

    std::string text = readString(mf, match.value_offset, match.length);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\0'))
        text.pop_back();
    return text;
}

template<typename Origin, typename Target>
std::string Dicom::reinterpretAs(const utils::Structures::MappedFile& mf, size_t offset) const {
    Origin origin = convertBytes<Origin>(mf, offset);
    Target value;
    std::memcpy(&value, &origin, sizeof(value));
    return std::to_string(value);
}
template std::string Dicom::reinterpretAs<uint16_t, int16_t>(const utils::Structures::MappedFile&, size_t) const;
template std::string Dicom::reinterpretAs<uint32_t, int32_t>(const utils::Structures::MappedFile&, size_t) const;
template std::string Dicom::reinterpretAs<uint32_t, float>(const utils::Structures::MappedFile&, size_t) const;
template std::string Dicom::reinterpretAs<uint64_t, double>(const utils::Structures::MappedFile&, size_t) const;


std::string Dicom::getCSV(const utils::Structures::MappedFile& mf, const std::string& raw_file_path) const {
    std::string row;
    size_t offsets_ptr = 0; // instructions_offsets is already sorted in ascending order

    for (size_t i = 0; i < processed_instructions.size(); ++i) {
        if (processed_instructions[i].group == 0x7fe0 && processed_instructions[i].element == 0x0010)
            continue; // PixelData goes in .RAW

        if (offsets_ptr < instructions_offsets.size() && instructions_offsets[offsets_ptr].instruction_index == i) {

            row += csvEscape(convertValueToString(mf, instructions_offsets[offsets_ptr]));
            ++offsets_ptr;
        }

        row += ",";
    }

    row += csvEscape(raw_file_path);
    return row;
}

std::vector<utils::Structures::Fragment> Dicom::walkFragments(const utils::Structures::MappedFile& mf, size_t start_offset) const {
    std::vector<utils::Structures::Fragment> fragments;
    size_t pos = start_offset;
    bool first_item = true; // the first Item is the Basic Offset Table

    while (pos + 8 <= mf.size) {
        uint16_t item_group = mf.data[pos] | (mf.data[pos + 1] << 8);
        uint16_t item_element = mf.data[pos + 2] | (mf.data[pos + 3] << 8);
        uint32_t item_length = mf.data[pos + 4] | (mf.data[pos + 5] << 8) | (mf.data[pos + 6] << 16) | (uint32_t(mf.data[pos + 7]) << 24);
        pos += 8;

        if (item_group == 0xFFFE && item_element == 0xE0DD) break;
        if (item_group != 0xFFFE || item_element != 0xE000) break; // unexpected format, stopping for safety

        if (!first_item) fragments.push_back({ pos, item_length });
        first_item = false;

        pos += item_length;
    }
    return fragments;
}

bool Dicom::getRAW(const utils::Structures::MappedFile& mf, const std::filesystem::path& raw_path, const uint8_t& num_threads) {
    size_t expected_size = static_cast<size_t>(rows) * columns * (bits_allocated / 8) * samples_per_pixel;

    // RLE Lossless
    if (transfer_syntax_uid == "1.2.840.10008.1.2.5") {
        auto fragments = walkFragments(mf, pixeldata_offset);
        if (fragments.empty()) { raw_error = "No RLE fragment found"; return false; }

        auto decoded = Compression::decodeRLE(mf.data + fragments[0].offset, fragments[0].length, expected_size);
        if (decoded.empty()) { raw_error = "RLE decode failed"; return false; }

        std::string map_err;
        utils::Structures::MappedFileWrite mf_out = utils::Files::createMappedFile(raw_path, decoded.size(), map_err);
        if (!mf_out.data) { raw_error = map_err; return false; }

        std::memcpy(mf_out.data, decoded.data(), decoded.size());
        utils::Files::unmapFile(mf_out);
        return true;
    }

    // JPEG Baseline / Extended
    if (transfer_syntax_uid == "1.2.840.10008.1.2.4.50" ||
        transfer_syntax_uid == "1.2.840.10008.1.2.4.51") {
        auto fragments = walkFragments(mf, pixeldata_offset);
        if (fragments.empty()) { raw_error = "No JPEG fragment found"; return false; }

        int width = 0, height = 0, channels = 0;
        unsigned char* decoded = stbi_load_from_memory(
            mf.data + fragments[0].offset, static_cast<int>(fragments[0].length),
            &width, &height, &channels, 0);

        if (!decoded) {
            raw_error = std::string("stb_image decode failed: ") + stbi_failure_reason();
            return false;
        }

        size_t decoded_size = static_cast<size_t>(width) * height * channels;

        std::string map_err;
        auto mf_out = utils::Files::createMappedFile(raw_path, decoded_size, map_err);
        if (!mf_out.data) {
            stbi_image_free(decoded);
            raw_error = map_err;
            return false;
        }

        std::memcpy(mf_out.data, decoded, decoded_size);
        utils::Files::unmapFile(mf_out);
        stbi_image_free(decoded);
        return true;
    }

    // JPEG 2000
    if (transfer_syntax_uid == "1.2.840.10008.1.2.4.90" ||
        transfer_syntax_uid == "1.2.840.10008.1.2.4.91") {
        auto fragments = walkFragments(mf, pixeldata_offset);
        if (fragments.empty()) { raw_error = "No JPEG2000 fragment found"; return false; }

        auto decoded = Compression::decodeJPEG2000(
            mf.data + fragments[0].offset, fragments[0].length, bits_allocated, num_threads);
        if (decoded.empty()) { raw_error = "JPEG2000 decode failed"; return false; }

        std::string map_err;
        auto mf_out = utils::Files::createMappedFile(raw_path, decoded.size(), map_err);
        if (!mf_out.data) { raw_error = map_err; return false; }

        std::memcpy(mf_out.data, decoded.data(), decoded.size());
        utils::Files::unmapFile(mf_out);
        return true;
    }

    // JPEG-LS
    if (transfer_syntax_uid == "1.2.840.10008.1.2.4.80" ||
        transfer_syntax_uid == "1.2.840.10008.1.2.4.81") {
        auto fragments = walkFragments(mf, pixeldata_offset);
        if (fragments.empty()) { raw_error = "No JPEG-LS fragment found"; return false; }

        auto decoded = Compression::decodeJPEGLS(mf.data + fragments[0].offset, fragments[0].length);
        if (decoded.empty()) { raw_error = "JPEG-LS decode failed"; return false; }
        if (decoded.size() != expected_size) {
            raw_error = "JPEG-LS decoded size mismatch";
            return false;
        }

        std::string map_err;
        auto mf_out = utils::Files::createMappedFile(raw_path, decoded.size(), map_err);
        if (!mf_out.data) { raw_error = map_err; return false; }

        std::memcpy(mf_out.data, decoded.data(), decoded.size());
        utils::Files::unmapFile(mf_out);
        return true;
    }

    if (transfer_syntax_uid == "1.2.840.10008.1.2.4.57" ||
        transfer_syntax_uid == "1.2.840.10008.1.2.4.70") {
        raw_error = "JPEG Lossless (non-hierarchical) not yet supported";
        return false;
    }

    // Native, not compressed
    if (pixeldata_length == 0xFFFFFFFF || pixeldata_offset + pixeldata_length > mf.size) {
        raw_error = "Invalid image size";
        return false;
    }

    std::string map_err;
    auto mf_out = utils::Files::createMappedFile(raw_path, pixeldata_length, map_err);
    if (!mf_out.data) {
        raw_error = map_err;
        return false;
    }

    std::memcpy(mf_out.data, mf.data + pixeldata_offset, pixeldata_length);
    utils::Files::unmapFile(mf_out);

    return true;
};

template<typename Container, typename KeyFn>
bool Dicom::advanceAndCheck(uint32_t& helper, const Container& sorted, uint16_t group, uint16_t element, KeyFn key) {
    while (helper < sorted.size()) {
        const auto [g, e] = key(sorted[helper]);
        if (g < group || (g == group && e < element)) {
            ++helper;
            continue;
        }
        break;
    }
    if (helper >= sorted.size()) return false;
    const auto [g, e] = key(sorted[helper]);
    return g == group && e == element;
}

void Dicom::verifyInstructions(const std::vector<utils::Structures::DicomTagInstruction>& instructions, const bool& allow_override) { // Function executed only once during the entire program
    // Security filter for jailbreak and sensitive data exposure. Checks specific tags and odd tag groups used for private data

    std::unordered_set<uint32_t> existing_tags;
    for (const auto& existing : processed_instructions)
        existing_tags.insert((static_cast<uint32_t>(existing.group) << 16) | existing.element);

    for (const auto& inst : instructions) {

        if (existing_tags.count((static_cast<uint32_t>(inst.group) << 16) | inst.element)) continue;

        bool jail_break = false;

        if (!allow_override) {
            if (inst.group % 2 != 0) continue;

            for (const auto& sens : sensitive_tags) {
                if (inst.group == sens.first && inst.element == sens.second) {
                    jail_break = true;
                    break;
                }
            }
        }

        if (!jail_break) {
            processed_instructions.push_back(inst);
            existing_tags.insert((static_cast<uint32_t>(inst.group) << 16) | inst.element);
        }
    }

    // Sorting to improve constructor execution efficiency
    std::sort(processed_instructions.begin(), processed_instructions.end(),
        [](const utils::Structures::DicomTagInstruction& a, const utils::Structures::DicomTagInstruction& b) {
            if (a.group == b.group) return a.element < b.element;

            return a.group < b.group;
        });

    for (const auto& instr : processed_instructions) if (instr.mandatory) mandatoryTags_counter++;
};

template<typename T>
T Dicom::convertBytes(const utils::Structures::MappedFile& mf, const size_t i) const {
    if (i + sizeof(T) > mf.size) return T{};

    T value;
    std::memcpy(&value, mf.data + i, sizeof(T));

    if (!big_endian)  return value;

    if constexpr (sizeof(T) == 2) {
#if defined(_WIN32)
        return _byteswap_ushort(value);
#else
        return __builtin_bswap16(value);
#endif
    }
    else if constexpr (sizeof(T) == 4) {
#if defined(_WIN32)
        return _byteswap_ulong(value);
#else
        return __builtin_bswap32(value);
#endif
    }
    else if constexpr (sizeof(T) == 8) {
#if defined(_WIN32)
        return _byteswap_uint64(value);
#else
        return __builtin_bswap64(value);
#endif
    }

    return value;
};
template uint16_t Dicom::convertBytes<uint16_t>(const utils::Structures::MappedFile& mf, const size_t i) const;
template uint32_t Dicom::convertBytes<uint32_t>(const utils::Structures::MappedFile& mf, const size_t i) const;

std::string Dicom::readString(const utils::Structures::MappedFile& mf, const size_t i, const size_t length) const {
    if (i + length > mf.size) return "";

    return std::string(reinterpret_cast<const char*>(mf.data + i), length);
};