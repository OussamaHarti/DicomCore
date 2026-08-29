#pragma once
#include "../Utils/utils.h"
#include "charls/charls.h"
#include "openjpeg.h"

namespace Compression {

    struct MemoryStreamState {
        const uint8_t* data;
        OPJ_SIZE_T size;
        OPJ_SIZE_T offset;
    };

    inline OPJ_SIZE_T memoryRead(void* buffer, OPJ_SIZE_T requested, void* user_data) {
        auto* state = static_cast<MemoryStreamState*>(user_data);
        OPJ_SIZE_T remaining = state->size - state->offset;
        if (remaining == 0) return static_cast<OPJ_SIZE_T>(-1);
        OPJ_SIZE_T to_read = (requested < remaining) ? requested : remaining;
        std::memcpy(buffer, state->data + state->offset, to_read);
        state->offset += to_read;
        return to_read;
    };

    inline OPJ_OFF_T memorySkip(OPJ_OFF_T bytes, void* user_data) {
        auto* state = static_cast<MemoryStreamState*>(user_data);
        OPJ_OFF_T new_offset = static_cast<OPJ_OFF_T>(state->offset) + bytes;
        if (new_offset < 0) new_offset = 0;
        if (static_cast<OPJ_SIZE_T>(new_offset) > state->size) new_offset = static_cast<OPJ_OFF_T>(state->size);
        state->offset = static_cast<OPJ_SIZE_T>(new_offset);
        return bytes;
    };

    inline OPJ_BOOL memorySeek(OPJ_OFF_T bytes, void* user_data) {
        auto* state = static_cast<MemoryStreamState*>(user_data);
        if (bytes < 0 || static_cast<OPJ_SIZE_T>(bytes) > state->size) return OPJ_FALSE;
        state->offset = static_cast<OPJ_SIZE_T>(bytes);
        return OPJ_TRUE;
    };

    inline std::vector<uint8_t> decodeJPEG2000(const uint8_t* compressed, size_t length, uint16_t bits_allocated, const uint8_t& num_threads) {
        MemoryStreamState state{ compressed, static_cast<OPJ_SIZE_T>(length), 0 };

        opj_stream_t* stream = opj_stream_default_create(OPJ_TRUE);
        if (!stream) return {};
        opj_stream_set_read_function(stream, memoryRead);
        opj_stream_set_skip_function(stream, memorySkip);
        opj_stream_set_seek_function(stream, memorySeek);
        opj_stream_set_user_data(stream, &state, nullptr);
        opj_stream_set_user_data_length(stream, length);

        opj_codec_t* codec = opj_create_decompress(OPJ_CODEC_J2K);
        if (!codec) { opj_stream_destroy(stream); return {}; }
        opj_codec_set_threads(codec, num_threads);

        opj_dparameters_t params;
        opj_set_default_decoder_parameters(&params);
        opj_setup_decoder(codec, &params);

        opj_image_t* image = nullptr;
        bool ok = opj_read_header(stream, codec, &image) && opj_decode(codec, stream, image);
        if (ok) opj_end_decompress(codec, stream);

        std::vector<uint8_t> output;
        if (ok && image && image->numcomps > 0) {
            size_t width = image->comps[0].w;
            size_t height = image->comps[0].h;
            size_t num_pixels = width * height;
            size_t bytes_per_sample = (bits_allocated > 8) ? 2 : 1;

            output.resize(num_pixels * bytes_per_sample);

            if (image->numcomps == 1) {
                if (bytes_per_sample == 2) {
                    const int32_t* src_ptr = image->comps[0].data;
                    uint16_t* dst_ptr = reinterpret_cast<uint16_t*>(output.data());
                    for (size_t p = 0; p < num_pixels; ++p) {
                        dst_ptr[p] = static_cast<uint16_t>(src_ptr[p]);
                    }
                }
                else {
                    const int32_t* src_ptr = image->comps[0].data;
                    uint8_t* dst_ptr = output.data();
                    for (size_t p = 0; p < num_pixels; ++p) {
                        dst_ptr[p] = static_cast<uint8_t>(src_ptr[p]);
                    }
                }
            }
            else {
                size_t num_components = image->numcomps;
                for (size_t c = 0; c < num_components; c++) {
                    for (size_t p = 0; p < num_pixels; p++) {
                        int32_t sample = image->comps[c].data[p];
                        size_t dst = (p * num_components + c) * bytes_per_sample;
                        if (bytes_per_sample == 2) {
                            output[dst] = static_cast<uint8_t>(sample & 0xFF);
                            output[dst + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
                        }
                        else {
                            output[dst] = static_cast<uint8_t>(sample);
                        }
                    }
                }
            }
        }

        if (image) opj_image_destroy(image);
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return output;
    };

    inline std::vector<uint8_t> decodeJPEGLS(const uint8_t* compressed, size_t length) {
        try {
            charls::jpegls_decoder decoder;
            decoder.source(compressed, length);
            decoder.read_header();
            std::vector<uint8_t> destination(decoder.destination_size());
            decoder.decode(destination);

            return destination;
        }
        catch (...) { return {}; }
    };

    inline std::vector<uint8_t> decodeRLE(const uint8_t* compressed, size_t compressed_size, size_t expected_output_size) {
        if (compressed_size < 4 || expected_output_size == 0) return {};

        uint32_t num_segments = 0;
        std::memcpy(&num_segments, compressed, 4);

        if (num_segments == 0 || expected_output_size % num_segments != 0) return {};
        if (compressed_size < 4 + (size_t)num_segments * 4) return {};

        std::vector<uint32_t> offsets(num_segments);
        for (uint32_t s = 0; s < num_segments; s++) {
            std::memcpy(&offsets[s], compressed + 4 + (s * 4), 4);
            if (offsets[s] >= compressed_size) return {};
        }

        std::vector<uint8_t> output(expected_output_size, 0);
        size_t bytes_per_segment = expected_output_size / num_segments;

        for (uint32_t s = 0; s < num_segments; s++) {
            size_t src = offsets[s];
            size_t src_end = (s + 1 < num_segments) ? offsets[s + 1] : compressed_size;
            size_t dst = 0;
            size_t base_dst = s * bytes_per_segment;

            while (src < src_end && dst < bytes_per_segment) {
                int8_t control = static_cast<int8_t>(compressed[src++]);

                if (control >= 0) {
                    size_t count = static_cast<size_t>(control) + 1;
                    for (size_t i = 0; i < count && src < src_end && dst < bytes_per_segment; i++)
                        output[base_dst + dst++] = compressed[src++];
                }
                else if (control != -128) {
                    size_t count = static_cast<size_t>(1 - control);
                    if (src < src_end) {
                        uint8_t value = compressed[src++];
                        for (size_t i = 0; i < count && dst < bytes_per_segment; i++)
                            output[base_dst + dst++] = value;
                    }
                }
            }
        }
        return output;
    };

}