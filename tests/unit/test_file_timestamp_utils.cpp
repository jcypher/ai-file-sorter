#include <catch2/catch_test_macros.hpp>

#include "FileTimestampUtils.hpp"
#include "LocalFsProvider.hpp"
#include "Utils.hpp"

#include <chrono>
#include <fstream>
#include <filesystem>
#include <thread>

namespace {

std::filesystem::path make_temp_dir(const std::string& label)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("aifs_ts_" + label + "_" + std::to_string(std::time(nullptr)));
    std::filesystem::create_directories(root);
    return root;
}

std::filesystem::path write_temp_file(const std::filesystem::path& dir, const std::string& name)
{
    const auto path = dir / name;
    std::ofstream out(path);
    out << "timestamp-test\n";
    return path;
}

} // namespace

TEST_CASE("propose_created only accepts strictly older candidates")
{
    CHECK_FALSE(FileTimestampUtils::propose_created(100, 0).has_value());
    CHECK_FALSE(FileTimestampUtils::propose_created(100, 100).has_value());
    CHECK_FALSE(FileTimestampUtils::propose_created(100, 150).has_value());
    const auto proposed = FileTimestampUtils::propose_created(100, 50);
    REQUIRE(proposed.has_value());
    CHECK(*proposed == 50);
    const auto when_unknown_current = FileTimestampUtils::propose_created(0, 50);
    REQUIRE(when_unknown_current.has_value());
    CHECK(*when_unknown_current == 50);
}

TEST_CASE("propose_modified accepts differing valid candidates")
{
    CHECK_FALSE(FileTimestampUtils::propose_modified(100, 0).has_value());
    CHECK_FALSE(FileTimestampUtils::propose_modified(100, 100).has_value());
    const auto older = FileTimestampUtils::propose_modified(100, 80);
    REQUIRE(older.has_value());
    CHECK(*older == 80);
    const auto newer = FileTimestampUtils::propose_modified(100, 120);
    REQUIRE(newer.has_value());
    CHECK(*newer == 120);
}

TEST_CASE("aggregate_folder_timestamps uses min created, max mtime, min date_added")
{
    using FileTimestampUtils::FileTimestamps;
    const std::vector<FileTimestamps> children = {
        FileTimestamps{.birthtime = 30, .mtime = 10, .date_added = 40},
        FileTimestamps{.birthtime = 10, .mtime = 50, .date_added = 20},
        FileTimestamps{.birthtime = 0, .mtime = 0, .date_added = 0},
    };
    const auto aggregated = FileTimestampUtils::aggregate_folder_timestamps(children);
    CHECK(aggregated.birthtime == 10);
    CHECK(aggregated.mtime == 50);
    CHECK(aggregated.date_added == 20);
}

TEST_CASE("parse_metadata_date accepts YYYY-MM and YYYY-MM-DD")
{
    const auto month = FileTimestampUtils::parse_metadata_date("2014-03");
    REQUIRE(month.has_value());
    const auto day = FileTimestampUtils::parse_metadata_date("2014-03-10");
    REQUIRE(day.has_value());
    CHECK(*day >= *month);
    CHECK_FALSE(FileTimestampUtils::parse_metadata_date("not-a-date").has_value());
}

TEST_CASE("LocalFsProvider move preserves timestamps")
{
    const auto root = make_temp_dir("move");
    const auto source = write_temp_file(root, "source.txt");
    const auto destination = root / "dest.txt";

    FileTimestampUtils::FileTimestamps original;
    REQUIRE(FileTimestampUtils::read_file_timestamps(source, original));
    REQUIRE(original.mtime > 0);

    // Give clocks a moment so a failed preserve would likely differ.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    LocalFsProvider provider;
    const auto result =
        provider.move_entry(Utils::path_to_utf8(source), Utils::path_to_utf8(destination));
    REQUIRE(result.success);

    FileTimestampUtils::FileTimestamps after;
    REQUIRE(FileTimestampUtils::read_file_timestamps(destination, after));
    CHECK(after.mtime == original.mtime);
    if (original.birthtime > 0 && FileTimestampUtils::capabilities().birthtime) {
        CHECK(after.birthtime == original.birthtime);
    }
#if defined(__APPLE__)
    if (original.date_added > 0) {
        CHECK(after.date_added == original.date_added);
    }
#endif

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("stamp_new_directories_from_contents sets aggregate folder times")
{
    const auto root = make_temp_dir("stamp");
    const auto folder = root / "NewCategory";
    std::filesystem::create_directories(folder);

    const auto older = write_temp_file(folder, "older.txt");
    const auto newer = write_temp_file(folder, "newer.txt");

    FileTimestampUtils::FileTimestamps older_times{.birthtime = 1'000, .mtime = 2'000, .date_added = 1'500};
    FileTimestampUtils::FileTimestamps newer_times{.birthtime = 3'000, .mtime = 4'000, .date_added = 3'500};
    FileTimestampUtils::write_file_timestamps(older, older_times, FileTimestampUtils::TimestampField::All);
    FileTimestampUtils::write_file_timestamps(newer, newer_times, FileTimestampUtils::TimestampField::All);

    const auto stamped =
        FileTimestampUtils::stamp_new_directories_from_contents({folder});
    REQUIRE(stamped == 1);

    FileTimestampUtils::FileTimestamps folder_times;
    REQUIRE(FileTimestampUtils::read_file_timestamps(folder, folder_times));
    if (FileTimestampUtils::capabilities().birthtime) {
        CHECK(folder_times.birthtime == 1'000);
    }
    CHECK(folder_times.mtime == 4'000);
#if defined(__APPLE__)
    if (FileTimestampUtils::capabilities().date_added) {
        CHECK(folder_times.date_added == 1'500);
    }
#endif

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
