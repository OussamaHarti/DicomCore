#include "Memory/Dicom.h"

namespace entry {
    inline void handleFailure(
        std::ofstream& log_out,
        std::mutex& io_mutex,
        const std::string& error_msg,
        std::atomic_size_t& skipped_count,
        std::atomic_size_t& processed,
        const std::string& discard_path,
        const std::filesystem::path* src,
        utils::Structures::MappedFile* mf) {
            {
                std::lock_guard<std::mutex> lock(io_mutex);
                log_out << error_msg;

                if (mf != nullptr) utils::Files::unmapFile(*mf);

                if (src != nullptr) {
                    if (utils::Files::moveFile(*src, std::filesystem::path(discard_path) / src->filename()))
                        log_out << "[FILE MOVE ERROR]\n";
                }
            }

            ++skipped_count;
            processed.fetch_add(1, std::memory_order_relaxed);
    };

    inline void progressTask(const size_t total_files, std::atomic_size_t& processed) {
        while (true) {
            size_t completed = processed.load(std::memory_order_relaxed);

            if (total_files > 0) {
                double percent = (static_cast<double>(completed) / total_files) * 100.0;
                std::cout << "\r[PROCESSING] " << completed << " / " << total_files
                    << " (" << static_cast<int>(percent) << "%)" << std::flush;
            }

            if (completed >= total_files) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    };

    void run(const int num_threads, const int decomp_threads) {

        std::string source_path, output_path;

        std::cout << "Path to the DICOM files folder: ";
        std::getline(std::cin, source_path);
        std::cout << "Output folder path (CSV, log, RAW): ";
        std::getline(std::cin, output_path);
        if (source_path.empty() || output_path.empty()) {
            std::cerr << "[ERROR] Path not specified." << std::endl;
            return;
        }

        auto global_start = std::chrono::high_resolution_clock::now();

        if (utils::Files::createFolder(output_path)) return;

        std::string discarded_path = output_path + "/discarded";

        if (utils::Files::createFolder(output_path + "/raw")
            || utils::Files::createFolder(discarded_path)) return;

        std::ofstream csv_out(output_path + "/dataset.csv"), log_out(output_path + "/errors.log");
        if (!csv_out.is_open() || !log_out.is_open()) {
            std::cerr << "[ERROR] Unable to open output files in " << output_path << std::endl;
            return;
        }

        std::string instructions_error;
        auto instructions = utils::Files::loadInstruction(
            (source_path + "/tags_config.txt").c_str(), instructions_error);
        if (instructions.empty()) {
            std::cerr << "[CONFIG INSTRUCTIONS ERROR] " << instructions_error << std::endl;
            return;
        }

        std::string dcmList_error;
        auto dcm_list = utils::Files::listDicomFiles(source_path.c_str(), dcmList_error);
        if (dcm_list.empty()) {
            std::cerr << "[CONFIG LIST ERROR] " << dcmList_error << std::endl;
            return;
        }

        Dicom::configure(instructions, false);
        csv_out << Dicom::getInstructions() << "\n";

        std::unordered_set<std::string> unique_directories;
        for (const auto& entry : dcm_list) {
            std::filesystem::path raw_dir = std::filesystem::path(output_path) / "raw" / entry.relative_parent;
            unique_directories.insert(raw_dir.string());
        }

        for (const auto& dir : unique_directories) {
            if (utils::Files::createFolder(dir))
                std::cerr << "[WARNING] Unable to create or verify the directory: " << dir << std::endl;
        }

        unique_directories.clear();

        std::mutex io_mutex;
        std::atomic_size_t processed{ 0 }, valid_count{ 0 }, skipped_count{ 0 }, next_file{ 0 };

        std::thread progress_thread(
            progressTask, dcm_list.size(), std::ref(processed));

        auto worker = [&]() {
            std::string local_csv_buffer;
            local_csv_buffer.reserve(1024 * 50);

            while (true) {
                size_t idx = next_file.fetch_add(1, std::memory_order_relaxed);
                if (idx >= dcm_list.size()) break;

                const auto& entry = dcm_list[idx];

                std::string mf_error;
                utils::Structures::MappedFile mf = utils::Files::mapFile(entry.full_path, mf_error);

                if (!mf.data) {
                    handleFailure(log_out, io_mutex,
                        "[READ ERROR] " + entry.full_path.generic_string() + " - " + mf_error + "\n",
                        skipped_count, processed, discarded_path, &entry.full_path, &mf);
                    continue;
                }

                Dicom myDicom(mf);
                if (!myDicom.isValid()) {
                    handleFailure(log_out, io_mutex,
                        "[INVALID FILE] " + entry.full_path.generic_string() + " - " + myDicom.getError() + "\n",
                        skipped_count, processed, discarded_path, &entry.full_path, &mf);
                    continue;
                }

                const std::filesystem::path raw_directory =
                    std::filesystem::path(output_path) / "raw" / entry.relative_parent;
                const std::filesystem::path raw_path =
                    raw_directory / (entry.full_path.stem().string() + ".raw");

                if (!myDicom.getRAW(mf, raw_path, decomp_threads)) {
                    {
                        std::lock_guard<std::mutex> lock(io_mutex);
                        log_out << "[INVALID RAW] " + entry.full_path.generic_string()
                            << " - " << myDicom.getRAWError() << "\n";
                        if (utils::Files::moveFile(entry.full_path, std::filesystem::path(discarded_path) / entry.full_path.filename()))
                            log_out << "[FILE MOVE ERROR]\n";
                    }
                    ++skipped_count;
                    processed.fetch_add(1, std::memory_order_relaxed);
                    utils::Files::unmapFile(mf);
                    continue;
                }

                local_csv_buffer += myDicom.getCSV(mf, raw_path.generic_string()) + "\n";
                ++valid_count;
                processed.fetch_add(1, std::memory_order_relaxed);

                utils::Files::unmapFile(mf);

                if (!myDicom.anonymizeStream(entry.full_path)) {
                    std::lock_guard<std::mutex> lock(io_mutex);
                    log_out << "[ANONYMIZATION ERROR] " << entry.full_path.generic_string()
                        << " - Unable to overwrite sensitive tags\n";
                }

                if (local_csv_buffer.size() >= 16 * 1024) {
                    std::lock_guard<std::mutex> lock(io_mutex);
                    csv_out << local_csv_buffer;
                    local_csv_buffer.clear();
                }
            }

            if (!local_csv_buffer.empty()) {
                std::lock_guard<std::mutex> lock(io_mutex);
                csv_out << local_csv_buffer;
            }
            };

        int n = (std::max)(1, (std::min)(num_threads,
            static_cast<int>(std::thread::hardware_concurrency())));

        std::vector<std::thread> pool;
        pool.reserve(n);
        for (int i = 0; i < n; ++i)
            pool.emplace_back(worker);

        for (auto& t : pool) t.join();

        auto global_end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = global_end - global_start;

        if (progress_thread.joinable()) progress_thread.join();
        std::cout << "\r                                                                           \r";

        log_out << "\nCompleted: " << valid_count.load() << " valid files, " << skipped_count.load() << " skipped.\n"
            << "Execution time: " << elapsed.count() << " seconds.\n";

        std::cout << "Completed in " << elapsed.count() << " seconds: "
            << valid_count.load() << " valid, " << skipped_count.load() << " skipped.\n"
            << "Details in " << output_path + "/errors.log" << std::endl;
    }
}