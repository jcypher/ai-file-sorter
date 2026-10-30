#include "FileTimestampUtils.hpp"

#include "Utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/attr.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/stat.h>
#endif
#endif

namespace FileTimestampUtils {
namespace {

bool is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

std::optional<std::tm> parse_ymd_parts(int year, int month, int day)
{
    if (year < 1900 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
        return std::nullopt;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    tm.tm_isdst = 0;
    return tm;
}

std::time_t time_from_utc_tm(std::tm tm)
{
#if defined(_WIN32)
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

#if defined(_WIN32)
FILETIME time_t_to_filetime(std::time_t value)
{
    ULARGE_INTEGER large;
    large.QuadPart = (static_cast<ULONGLONG>(value) + 11644473600ULL) * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = large.LowPart;
    ft.dwHighDateTime = large.HighPart;
    return ft;
}

std::time_t filetime_to_time_t(const FILETIME& ft)
{
    ULARGE_INTEGER large;
    large.LowPart = ft.dwLowDateTime;
    large.HighPart = ft.dwHighDateTime;
    if (large.QuadPart == 0) {
        return 0;
    }
    return static_cast<std::time_t>((large.QuadPart / 10000000ULL) - 11644473600ULL);
}
#endif

#if defined(__APPLE__)
bool read_macos_timestamps(const std::filesystem::path& path, FileTimestamps& out)
{
    struct attrlist request {};
    request.bitmapcount = ATTR_BIT_MAP_COUNT;
    request.commonattr = ATTR_CMN_CRTIME | ATTR_CMN_MODTIME | ATTR_CMN_ADDEDTIME;

    struct ResponseBuf {
        u_int32_t length;
        timespec crtime;
        timespec mtime;
        timespec added;
    } __attribute__((aligned(4), packed));

    ResponseBuf response {};
    const std::string utf8 = Utils::path_to_utf8(path);
    if (getattrlist(utf8.c_str(), &request, &response, sizeof(response), 0) != 0) {
        return false;
    }

    out.birthtime = response.crtime.tv_sec;
    out.mtime = response.mtime.tv_sec;
    out.date_added = response.added.tv_sec;
    return out.mtime > 0 || out.birthtime > 0;
}

bool write_macos_field(const std::filesystem::path& path, attrgroup_t attr, std::time_t value)
{
    if (value <= 0) {
        return false;
    }
    struct attrlist request {};
    request.bitmapcount = ATTR_BIT_MAP_COUNT;
    request.commonattr = attr;

    timespec ts {};
    ts.tv_sec = value;
    ts.tv_nsec = 0;

    const std::string utf8 = Utils::path_to_utf8(path);
    return setattrlist(utf8.c_str(), &request, &ts, sizeof(ts), 0) == 0;
}
#endif

} // namespace

FileTimestampCapabilities capabilities()
{
    FileTimestampCapabilities caps;
#if defined(__APPLE__)
    caps.birthtime = true;
    caps.date_added = true;
#elif defined(_WIN32)
    caps.birthtime = true;
    caps.date_added = false;
#else
    caps.birthtime = true; // readable often; writable rarely
    caps.date_added = false;
#endif
    return caps;
}

bool read_file_timestamps(const std::filesystem::path& path, FileTimestamps& out)
{
    out = {};
#if defined(__APPLE__)
    if (read_macos_timestamps(path, out)) {
        return true;
    }
#endif

#if defined(_WIN32)
    const std::wstring wide = path.wstring();
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    out.birthtime = filetime_to_time_t(data.ftCreationTime);
    out.mtime = filetime_to_time_t(data.ftLastWriteTime);
    return out.mtime > 0 || out.birthtime > 0;
#else
    struct stat st {};
    if (stat(Utils::path_to_utf8(path).c_str(), &st) != 0) {
        return false;
    }
#if defined(__APPLE__)
    out.birthtime = st.st_birthtimespec.tv_sec;
    out.mtime = st.st_mtimespec.tv_sec;
#elif defined(__linux__)
    out.mtime = st.st_mtime;
#if defined(STATX_BTIME)
    struct statx stx {};
    if (statx(AT_FDCWD, Utils::path_to_utf8(path).c_str(), AT_SYMLINK_NOFOLLOW, STATX_BTIME | STATX_MTIME, &stx) ==
        0) {
        if (stx.stx_mask & STATX_BTIME) {
            out.birthtime = static_cast<std::time_t>(stx.stx_btime.tv_sec);
        }
        if (stx.stx_mask & STATX_MTIME) {
            out.mtime = static_cast<std::time_t>(stx.stx_mtime.tv_sec);
        }
    }
#else
    out.birthtime = 0;
#endif
#else
    out.mtime = st.st_mtime;
    out.birthtime = 0;
#endif
    return out.mtime > 0 || out.birthtime > 0;
#endif
}

bool write_file_timestamps(const std::filesystem::path& path,
                           const FileTimestamps& times,
                           TimestampField fields)
{
    bool ok = true;
    const auto caps = capabilities();

#if defined(__APPLE__)
    if (has_flag(fields, TimestampField::Birthtime) && caps.birthtime && times.birthtime > 0) {
        ok = write_macos_field(path, ATTR_CMN_CRTIME, times.birthtime) && ok;
    }
    if (has_flag(fields, TimestampField::Mtime) && times.mtime > 0) {
        ok = write_macos_field(path, ATTR_CMN_MODTIME, times.mtime) && ok;
    }
    if (has_flag(fields, TimestampField::DateAdded) && caps.date_added && times.date_added > 0) {
        ok = write_macos_field(path, ATTR_CMN_ADDEDTIME, times.date_added) && ok;
    }
    return ok;
#elif defined(_WIN32)
    const std::wstring wide = path.wstring();
    HANDLE handle = CreateFileW(wide.c_str(),
                                FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    FILETIME creation = {};
    FILETIME last_write = {};
    FILETIME* creation_ptr = nullptr;
    FILETIME* write_ptr = nullptr;
    if (has_flag(fields, TimestampField::Birthtime) && caps.birthtime && times.birthtime > 0) {
        creation = time_t_to_filetime(times.birthtime);
        creation_ptr = &creation;
    }
    if (has_flag(fields, TimestampField::Mtime) && times.mtime > 0) {
        last_write = time_t_to_filetime(times.mtime);
        write_ptr = &last_write;
    }
    const BOOL success = SetFileTime(handle, creation_ptr, nullptr, write_ptr);
    CloseHandle(handle);
    return success == TRUE;
#else
    if (has_flag(fields, TimestampField::Mtime) && times.mtime > 0) {
        timespec ts[2];
        ts[0].tv_sec = 0;
        ts[0].tv_nsec = UTIME_OMIT;
        ts[1].tv_sec = times.mtime;
        ts[1].tv_nsec = 0;
        if (utimensat(AT_FDCWD, Utils::path_to_utf8(path).c_str(), ts, 0) != 0) {
            ok = false;
        }
    }
    (void)caps;
    // Birthtime is typically immutable on Linux; Date Added unsupported.
    return ok;
#endif
}

std::optional<std::time_t> propose_created(std::time_t current, std::time_t candidate)
{
    if (candidate <= 0) {
        return std::nullopt;
    }
    if (current > 0 && candidate >= current) {
        return std::nullopt;
    }
    return candidate;
}

std::optional<std::time_t> propose_modified(std::time_t current, std::time_t candidate)
{
    if (candidate <= 0) {
        return std::nullopt;
    }
    if (current > 0 && candidate == current) {
        return std::nullopt;
    }
    return candidate;
}

FileTimestamps aggregate_folder_timestamps(const std::vector<FileTimestamps>& children)
{
    FileTimestamps result;
    for (const auto& child : children) {
        if (child.birthtime > 0) {
            if (result.birthtime == 0 || child.birthtime < result.birthtime) {
                result.birthtime = child.birthtime;
            }
        }
        if (child.mtime > 0) {
            if (result.mtime == 0 || child.mtime > result.mtime) {
                result.mtime = child.mtime;
            }
        }
        if (child.date_added > 0) {
            if (result.date_added == 0 || child.date_added < result.date_added) {
                result.date_added = child.date_added;
            }
        }
    }
    return result;
}

std::optional<std::time_t> parse_metadata_date(const std::string& value)
{
    std::string digits;
    digits.reserve(value.size());
    for (char ch : value) {
        if (is_digit(ch)) {
            digits.push_back(ch);
        }
    }
    if (digits.size() < 4) {
        return std::nullopt;
    }

    int year = 0;
    int month = 1;
    int day = 1;
    try {
        year = std::stoi(digits.substr(0, 4));
        if (digits.size() >= 6) {
            month = std::stoi(digits.substr(4, 2));
        }
        if (digits.size() >= 8) {
            day = std::stoi(digits.substr(6, 2));
        }
    } catch (...) {
        return std::nullopt;
    }

    const auto tm = parse_ymd_parts(year, month, day);
    if (!tm) {
        return std::nullopt;
    }
    const std::time_t converted = time_from_utc_tm(*tm);
    if (converted < 0) {
        return std::nullopt;
    }
    return converted;
}

void collect_missing_directory_segments(const std::filesystem::path& directory,
                                        std::vector<std::filesystem::path>& out)
{
    if (directory.empty()) {
        return;
    }

    std::error_code ec;
    const auto normalized = directory.lexically_normal();
    if (std::filesystem::exists(normalized, ec)) {
        return;
    }

    std::vector<std::filesystem::path> missing;
    auto current = normalized;
    while (!current.empty()) {
        ec.clear();
        if (std::filesystem::exists(current, ec)) {
            break;
        }
        missing.push_back(current);
        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    std::reverse(missing.begin(), missing.end());
    for (const auto& path : missing) {
        const auto already = std::find(out.begin(), out.end(), path);
        if (already == out.end()) {
            out.push_back(path);
        }
    }
}

bool apply_recovered_file_timestamps(const std::filesystem::path& path,
                                     std::optional<std::time_t> proposed_created,
                                     std::optional<std::time_t> proposed_modified,
                                     std::time_t preserved_date_added)
{
    FileTimestamps current;
    read_file_timestamps(path, current);

    FileTimestamps times = current;
    TimestampField fields = TimestampField::None;
    if (proposed_created && *proposed_created > 0) {
        times.birthtime = *proposed_created;
        fields = fields | TimestampField::Birthtime;
    }
    if (proposed_modified && *proposed_modified > 0) {
        times.mtime = *proposed_modified;
        fields = fields | TimestampField::Mtime;
    }
    if (fields == TimestampField::None) {
        return true;
    }

    bool ok = write_file_timestamps(path, times, fields);
    if (capabilities().date_added) {
        const std::time_t date_added =
            preserved_date_added > 0 ? preserved_date_added : current.date_added;
        if (date_added > 0) {
            FileTimestamps added_only;
            added_only.date_added = date_added;
            ok = write_file_timestamps(path, added_only, TimestampField::DateAdded) && ok;
        }
    }
    return ok;
}

std::size_t stamp_new_directories_from_contents(std::vector<std::filesystem::path> new_dirs)
{
    std::sort(new_dirs.begin(), new_dirs.end(), [](const auto& a, const auto& b) {
        const auto a_len = std::distance(a.begin(), a.end());
        const auto b_len = std::distance(b.begin(), b.end());
        if (a_len != b_len) {
            return a_len > b_len;
        }
        return a.native() > b.native();
    });

    std::size_t stamped = 0;
    for (const auto& dir : new_dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec) || ec) {
            continue;
        }

        std::vector<FileTimestamps> children;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            FileTimestamps child_times;
            if (read_file_timestamps(entry.path(), child_times)) {
                children.push_back(child_times);
            }
        }
        if (children.empty()) {
            continue;
        }

        const FileTimestamps aggregated = aggregate_folder_timestamps(children);
        TimestampField fields = TimestampField::None;
        if (aggregated.birthtime > 0) {
            fields = fields | TimestampField::Birthtime;
        }
        if (aggregated.mtime > 0) {
            fields = fields | TimestampField::Mtime;
        }
        if (aggregated.date_added > 0 && capabilities().date_added) {
            fields = fields | TimestampField::DateAdded;
        }
        if (fields == TimestampField::None) {
            continue;
        }
        if (write_file_timestamps(dir, aggregated, fields)) {
            ++stamped;
        }
    }
    return stamped;
}

} // namespace FileTimestampUtils
