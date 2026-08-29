#pragma once
#if defined(_WIN32)
#include <windows.h>
#include <stdlib.h> 
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#endif
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>

namespace utils {

    namespace Structures {

        struct DicomTagInstruction {
            uint16_t group;
            uint16_t element;
            std::string vr;
            std::string name;
            bool mandatory = false;
        };

        struct DicomFileEntry {
            std::filesystem::path full_path;
            std::filesystem::path relative_parent;
        };

        struct TagMatch {
            uint32_t instruction_index;
            size_t value_offset;
            uint32_t length;
        };

        struct Fragment {
            size_t offset;
            uint32_t length;
        };

        template <typename T>
        struct MappedFileBase {
            T* data = nullptr;
            size_t size = 0;

#if defined(_WIN32)
            void* hFile = nullptr;
            void* hMap = nullptr;
#else
            int fd = -1;
#endif
        };

        using MappedFile = MappedFileBase<const uint8_t>;
        using MappedFileWrite = MappedFileBase<uint8_t>;
    };

    namespace Files {

        inline std::string trim(const std::string& str) {
            size_t first = str.find_first_not_of(" \t\r\n");
            if (std::string::npos == first) return "";
            size_t last = str.find_last_not_of(" \t\r\n");
            return str.substr(first, (last - first + 1));
        };

        inline bool isValidHex(const std::string& str) {
            if (str.empty() || str.length() > 4) return false;
            for (char c : str) {
                if (!isxdigit(static_cast<unsigned char>(c))) return false;
            }
            return true;
        };

        inline std::vector<Structures::DicomTagInstruction> loadInstruction(const std::filesystem::path& file_path, std::string& error) {
            std::ifstream file(file_path);
            if (!file.is_open()) { //Check sul file
                error = "Unable to open the tag configuration file: " + file_path.string();
                return {};
            }

            std::string line;
            size_t line_number = 0;
            std::vector<Structures::DicomTagInstruction> buffer;

            while (std::getline(file, line)) {
                line_number++;

                std::string trimmed_line = trim(line);
                if (trimmed_line.empty() || trimmed_line[0] == '#' || trimmed_line[0] == '/') continue; // Ignore any comments marked with # or /

                std::stringstream ss(trimmed_line);
                std::string group_str, element_str, vr_str, name_str;

                //Format: Group,Element,VR,Description
                if (std::getline(ss, group_str, ',') && std::getline(ss, element_str, ',') && std::getline(ss, vr_str, ',') && std::getline(ss, name_str)) {
                    try {
                        Structures::DicomTagInstruction tmp;
                        tmp.vr = trim(vr_str);
                        tmp.name = trim(name_str);

                        std::string g_clean = trim(group_str), e_clean = trim(element_str); // Remove any leading and trailing spaces

                        if (!isValidHex(g_clean) || !isValidHex(e_clean) || tmp.vr.empty() || tmp.name.empty()) { // Validity check on the hexadecimal value and the presence of name and VR
                            error = "Syntax error at line " + std::to_string(line_number);
                            return {};
                        }

                        tmp.group = static_cast<uint16_t>(std::stoul(g_clean, nullptr, 16));
                        tmp.element = static_cast<uint16_t>(std::stoul(e_clean, nullptr, 16));

                        buffer.push_back(tmp);
                    }
                    catch (...) {
                        error = "Parsing error (invalid hexadecimal values) at line " + std::to_string(line_number);
                        return {};
                    }
                }
                else {
                    error = "Invalid format at line " + std::to_string(line_number);
                    return {};
                }
            }

            if (buffer.empty()) {
                error = "The configuration file is empty or does not contain valid tags.";
                return {};
            }

            return buffer;
        };

        inline std::vector<utils::Structures::DicomFileEntry> listDicomFiles(const std::filesystem::path& folder_path, std::string& error) {
            std::vector<utils::Structures::DicomFileEntry> result;
            std::error_code ec;

            if (!std::filesystem::exists(folder_path, ec) || ec) {
                error = "The path does not exist: " + folder_path.string();
                return {};
            }

            if (!std::filesystem::is_directory(folder_path, ec) || ec) {
                error = "The path is not a directory: " + folder_path.string();
                return {};
            }

            auto it = std::filesystem::recursive_directory_iterator(
                folder_path, std::filesystem::directory_options::skip_permission_denied, ec),
                end = std::filesystem::recursive_directory_iterator();

            for (; it != end; it.increment(ec)) {
                if (ec) {
                    error += "Warning during scanning: " + ec.message() + "; ";
                    ec.clear();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;

                std::string extension = it->path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                    [](unsigned char c) { return std::tolower(c); });

                if (extension == ".dicom" || extension == ".dcm" || extension == ".dic") {
                    std::filesystem::path full = it->path();

                    std::error_code rel_ec;
                    std::filesystem::path rel_parent = std::filesystem::relative(full.parent_path(), folder_path, rel_ec);
                    if (rel_ec || rel_parent == "." || rel_parent.empty()) {
                        rel_parent = "";
                    }

                    result.push_back({ full, rel_parent });
                }
            }

            if (result.empty()) {
                error = "No DICOM files found in the folder: " + folder_path.string();
                return {};
            }

            return result;
        };

        inline std::error_code createFolder(const std::string& path) {
            std::error_code fs_ec;
            std::filesystem::create_directories(path, fs_ec);
            if (fs_ec) std::cerr << "[ERROR] Unable to create output folder : " << path << " - " << fs_ec.message() << std::endl;

            return fs_ec;
        };

        inline std::error_code moveFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
            std::error_code ec;

            std::filesystem::rename(source, destination, ec);

            return ec;
        };

        inline Structures::MappedFile mapFile(const std::filesystem::path& file_path, std::string& error) {
            Structures::MappedFile mf;

#if defined(_WIN32)
            mf.hFile = CreateFileW(
                file_path.wstring().c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr
            );

            if (mf.hFile == INVALID_HANDLE_VALUE) {
                error = "Unable to open the file (Win32).";
                return mf;
            }

            LARGE_INTEGER file_size;
            if (!GetFileSizeEx(static_cast<HANDLE>(mf.hFile), &file_size)) {
                error = "Unable to read the size (Win32).";
                CloseHandle(static_cast<HANDLE>(mf.hFile));
                return mf;
            }
            mf.size = static_cast<size_t>(file_size.QuadPart);

            mf.hMap = CreateFileMappingW(static_cast<HANDLE>(mf.hFile), nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!mf.hMap) {
                error = "File mapping creation failed (Win32).";
                CloseHandle(static_cast<HANDLE>(mf.hFile));
                return mf;
            }

            mf.data = static_cast<const uint8_t*>(MapViewOfFile(static_cast<HANDLE>(mf.hMap), FILE_MAP_READ, 0, 0, 0));
            if (!mf.data) {
                error = "File view mapping failed (Win32).";
                CloseHandle(static_cast<HANDLE>(mf.hMap));
                CloseHandle(static_cast<HANDLE>(mf.hFile));
                mf.data = nullptr;
            }

            WIN32_MEMORY_RANGE_ENTRY range;
            range.VirtualAddress = const_cast<void*>(static_cast<const void*>(mf.data));
            range.NumberOfBytes = mf.size;

            PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
#else
            // Implementazione POSIX per Linux / macOS
            mf.fd = open(file_path.string().c_str(), O_RDONLY);
            if (mf.fd == -1) {
                error = "Unable to open the file (POSIX).";
                return mf;
            }

            struct stat sb;
            if (fstat(mf.fd, &sb) == -1) {
                error = "Unable to read the file information (POSIX).";
                close(mf.fd);
                return mf;
            }
            mf.size = static_cast<size_t>(sb.st_size);

            void* ptr = mmap(nullptr, mf.size, PROT_READ, MAP_SHARED, mf.fd, 0);
            if (ptr == MAP_FAILED) {
                error = "mmap failed (POSIX).";
                close(mf.fd);
                return mf;
            }
            madvise(ptr, mf.size, MADV_SEQUENTIAL);
            mf.data = static_cast<const uint8_t*>(ptr);

            madvise(const_cast<void*>(static_cast<const void*>(mf.data)), mf.size, MADV_WILLNEED);
#endif
            return mf;
        };

        inline Structures::MappedFileWrite createMappedFile(const std::filesystem::path& file_path, size_t size, std::string& error) {
            Structures::MappedFileWrite mf;
            mf.size = size;

            if (size == 0) {
                error = "File size cannot be zero.";
                return mf;
            }

#if defined(_WIN32)
            // CREATE_ALWAYS forza la creazione di un nuovo file (o la sovrascrittura)
            mf.hFile = CreateFileW(
                file_path.wstring().c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (mf.hFile == INVALID_HANDLE_VALUE) {
                error = "Unable to create the output file (Win32).";
                return mf;
            }

            LARGE_INTEGER liSize;
            liSize.QuadPart = size;

            // PAGE_READWRITE abilita la scrittura sulla cache
            mf.hMap = CreateFileMappingW(static_cast<HANDLE>(mf.hFile), nullptr, PAGE_READWRITE, liSize.HighPart, liSize.LowPart, nullptr);
            if (!mf.hMap) {
                error = "File mapping creation failed (Win32).";
                CloseHandle(static_cast<HANDLE>(mf.hFile));
                return mf;
            }

            // FILE_MAP_WRITE concede al puntatore i privilegi di modifica
            mf.data = static_cast<uint8_t*>(MapViewOfFile(static_cast<HANDLE>(mf.hMap), FILE_MAP_WRITE, 0, 0, size));
            if (!mf.data) {
                error = "File view mapping failed (Win32).";
                CloseHandle(static_cast<HANDLE>(mf.hMap));
                CloseHandle(static_cast<HANDLE>(mf.hFile));
                mf.data = nullptr;
            }
#else
            // O_RDWR | O_CREAT | O_TRUNC apre in scrittura e sovrascrive se esiste
            mf.fd = open(file_path.string().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (mf.fd == -1) {
                error = "Unable to create the output file (POSIX).";
                return mf;
            }

            // ftruncate allarga fisicamente il file sul disco prima del mapping
            if (ftruncate(mf.fd, size) == -1) {
                error = "Unable to set output file size (POSIX).";
                close(mf.fd);
                return mf;
            }

            // PROT_WRITE mappa in scrittura
            void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, mf.fd, 0);
            if (ptr == MAP_FAILED) {
                error = "mmap failed (POSIX).";
                close(mf.fd);
                return mf;
            }
            mf.data = static_cast<uint8_t*>(ptr);
#endif
            return mf;
        };

        inline Structures::MappedFileBase<uint8_t> mapExistingFileWrite(const std::filesystem::path& file_path, std::string& error) {
            Structures::MappedFileBase<uint8_t> mf;

#if defined(_WIN32)
            // OPEN_EXISTING apre il file solo se esiste, senza cancellarne il contenuto
            mf.hFile = CreateFileW(file_path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (mf.hFile == INVALID_HANDLE_VALUE) {
                error = "Unable to open existing file for modification.";
                return mf;
            }

            LARGE_INTEGER size;
            GetFileSizeEx(mf.hFile, &size);
            mf.size = size.QuadPart;

            mf.hMap = CreateFileMappingW(static_cast<HANDLE>(mf.hFile), nullptr, PAGE_READWRITE, 0, 0, nullptr);
            if (!mf.hMap) {
                error = "File mapping creation failed.";
                CloseHandle(static_cast<HANDLE>(mf.hFile));
                return mf;
            }

            mf.data = static_cast<uint8_t*>(MapViewOfFile(static_cast<HANDLE>(mf.hMap), FILE_MAP_WRITE, 0, 0, mf.size));
            if (!mf.data) {
                error = "File view mapping failed.";
                CloseHandle(static_cast<HANDLE>(mf.hMap));
                CloseHandle(static_cast<HANDLE>(mf.hFile));
            }
#else
            // O_RDWR apre in lettura e scrittura senza troncare
            mf.fd = open(file_path.string().c_str(), O_RDWR);
            if (mf.fd == -1) {
                error = "Unable to open existing file for modification (POSIX).";
                return mf;
            }

            struct stat sb;
            fstat(mf.fd, &sb);
            mf.size = sb.st_size;

            void* ptr = mmap(nullptr, mf.size, PROT_READ | PROT_WRITE, MAP_SHARED, mf.fd, 0);
            if (ptr == MAP_FAILED) {
                error = "mmap failed (POSIX).";
                close(mf.fd);
                return mf;
            }
            mf.data = static_cast<uint8_t*>(ptr);
#endif
            return mf;
        };

        template <typename T>
        inline void unmapFile(Structures::MappedFileBase<T>& mf) {
            if (mf.data) {
#if defined(_WIN32)
                UnmapViewOfFile(mf.data);
#else
                void* ptr = const_cast<void*>(static_cast<const void*>(mf.data));

                if constexpr (!std::is_const_v<T>)
                    msync(ptr, mf.size, MS_SYNC);

                munmap(ptr, mf.size);
#endif
                mf.data = nullptr;
            }
#if defined(_WIN32)
            if (mf.hMap && mf.hMap != INVALID_HANDLE_VALUE) {
                CloseHandle(static_cast<HANDLE>(mf.hMap));
                mf.hMap = nullptr;
            }
            if (mf.hFile && mf.hFile != INVALID_HANDLE_VALUE) {
                CloseHandle(static_cast<HANDLE>(mf.hFile));
                mf.hFile = INVALID_HANDLE_VALUE; 
        }
#else
            if (mf.fd != -1) {
                close(mf.fd);
                mf.fd = -1;
            }
#endif
            mf.size = 0;
        }
    };
}
