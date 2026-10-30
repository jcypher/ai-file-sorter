#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Cross-platform filesystem timestamp helpers used for preserve/recover flows.
 */
namespace FileTimestampUtils {

/**
 * @brief Platform capabilities for timestamp fields.
 */
struct FileTimestampCapabilities {
    bool birthtime{false};
    bool date_added{false};
};

/**
 * @brief Filesystem timestamps. Zero means unset/unknown.
 */
struct FileTimestamps {
    std::time_t birthtime{0};
    std::time_t mtime{0};
    std::time_t date_added{0};
};

/**
 * @brief Bitmask selecting which timestamp fields to write.
 */
enum class TimestampField : unsigned {
    None = 0,
    Birthtime = 1u << 0,
    Mtime = 1u << 1,
    DateAdded = 1u << 2,
    All = Birthtime | Mtime | DateAdded
};

inline TimestampField operator|(TimestampField a, TimestampField b)
{
    return static_cast<TimestampField>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

inline TimestampField operator&(TimestampField a, TimestampField b)
{
    return static_cast<TimestampField>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}

inline bool has_flag(TimestampField value, TimestampField flag)
{
    return (static_cast<unsigned>(value) & static_cast<unsigned>(flag)) != 0;
}

/**
 * @brief Returns timestamp capabilities for the current platform.
 */
FileTimestampCapabilities capabilities();

/**
 * @brief Reads filesystem timestamps for a path.
 * @param path Path to inspect.
 * @param out Filled on success.
 * @return True when at least mtime was read successfully.
 */
bool read_file_timestamps(const std::filesystem::path& path, FileTimestamps& out);

/**
 * @brief Writes selected filesystem timestamps (best-effort).
 * @param path Path to update.
 * @param times Values to apply.
 * @param fields Which fields to write.
 * @return True when every requested supported field was written.
 */
bool write_file_timestamps(const std::filesystem::path& path,
                           const FileTimestamps& times,
                           TimestampField fields = TimestampField::All);

/**
 * @brief Proposes a created time only when the candidate is strictly older than current.
 */
std::optional<std::time_t> propose_created(std::time_t current, std::time_t candidate);

/**
 * @brief Proposes a modified time when the candidate is valid and differs from current.
 */
std::optional<std::time_t> propose_modified(std::time_t current, std::time_t candidate);

/**
 * @brief Aggregates child timestamps for a newly created folder.
 * Created = min birthtime, Modified = max mtime, Date Added = min date_added (zeros ignored).
 */
FileTimestamps aggregate_folder_timestamps(const std::vector<FileTimestamps>& children);

/**
 * @brief Parses metadata date strings (YYYY-MM-DD, YYYY-MM, or YYYYMMDD) to time_t (UTC noon).
 */
std::optional<std::time_t> parse_metadata_date(const std::string& value);

/**
 * @brief Collects directory path segments that do not yet exist under @p directory.
 * @param directory Target directory that may need creation.
 * @param out Appends missing ancestors from shallowest to deepest (excluding existing parents).
 */
void collect_missing_directory_segments(const std::filesystem::path& directory,
                                        std::vector<std::filesystem::path>& out);

/**
 * @brief Stamps newly created directories from their immediate children (deepest first).
 * @param new_dirs Directories created during apply.
 * @return Number of directories successfully stamped.
 */
std::size_t stamp_new_directories_from_contents(std::vector<std::filesystem::path> new_dirs);

/**
 * @brief Writes recovered Created/Modified while re-applying preserved Date Added on macOS.
 */
bool apply_recovered_file_timestamps(const std::filesystem::path& path,
                                     std::optional<std::time_t> proposed_created,
                                     std::optional<std::time_t> proposed_modified,
                                     std::time_t preserved_date_added = 0);

} // namespace FileTimestampUtils
