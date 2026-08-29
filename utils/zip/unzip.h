#pragma once

#include <string>
#include <functional>
#include <vector>
#include <atomic>
#include <unordered_map>

// Callback for progress: (current_file, total_files, current_bytes, total_bytes)
using UnzipProgressCallback = std::function<void(int, int, uint64_t, uint64_t)>;

// Unzip a file to a destination folder
// If flatten is true, extract all files directly to dest without preserving directory structure
// If error_msg is not nullptr, it will be filled with error details on failure
// If only_names is non-empty, only entries whose relative path (preferred) or basename
// matches one of the given names are extracted (delta/partial extraction).
// If cancel is not nullptr and becomes true, the extraction is aborted and the helper
// process is killed -- without this a 44GB extraction cannot be stopped once started.
bool unzip_file(const std::string& zip_path,
                const std::string& dest_path,
                bool flatten = false,
                UnzipProgressCallback progress_callback = nullptr,
                std::string* error_msg = nullptr,
                const std::vector<std::string>& only_names = {},
                const std::atomic<bool>* cancel = nullptr);

// Check if a file is a ZIP file
bool is_zip_file(const std::string& path);

// List the uncompressed size of every file entry in a ZIP, keyed by lowercase
// basename. Cached per zip_path (invalidated when the zip's mtime changes) so
// repeated calls (e.g. from a polling validation loop) don't re-open the
// archive / re-spawn a helper process every time.
std::unordered_map<std::string, uint64_t> get_zip_entry_sizes(const std::string& zip_path);
