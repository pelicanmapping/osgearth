/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include <osgEarth/catch.hpp>

#include <iostream>

#include <osgEarth/FileUtils>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ConvertUTF>
#include <osgDB/fstream>
#include <cctype>
#include <thread>
#include <vector>

#if defined(_WIN32) && !defined(__CYGWIN__)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

using namespace osgEarth;

TEST_CASE( "getFullPath works" ) {    
    // Basic relative paths should should.
    std::string fullPath = osgEarth::Util::getFullPath("C:/images/vacation.jpg", "../models/model.obj");
    REQUIRE(fullPath == "C:/models/model.obj");

    // Single and double dots should be handled.
    fullPath = osgEarth::Util::getFullPath("C:/images/vacation.jpg", "./../models/model.obj");
    REQUIRE(fullPath == "C:/models/model.obj");

    // If an absolute path is passed in along with a relativeTo the absolute paths should be returned.
    fullPath = osgEarth::Util::getFullPath("C:/images/vacation.jpg", "c:/models/model2.obj");
    REQUIRE(fullPath == "c:/models/model2.obj");

    // If no relativeTo is passed in the unmodified path should be returned.
    fullPath = osgEarth::Util::getFullPath("", "../../images/vacation.jpg");
    REQUIRE(fullPath == "../../images/vacation.jpg");

    // Absolute paths with relative paths should get resolved
    fullPath = osgEarth::Util::getFullPath("", "c:/images/../models/model.obj");
    REQUIRE(fullPath == "c:/models/model.obj");

    // If just the relativeTo is passed in then that is what should be returned.
    fullPath = osgEarth::Util::getFullPath("c:/images/vacation.jpg", "");
    REQUIRE(fullPath == "c:/images/vacation.jpg");
}

TEST_CASE("stripRelativePaths works") {
    // Basic relative paths should should.
    std::string fullPath = osgEarth::Util::stripRelativePaths("http://server.com/files/1/2/3/../../../image.png");
    REQUIRE(fullPath == "http://server.com/files/image.png");

    fullPath = osgEarth::Util::stripRelativePaths("C:/files/1/2/3/../../../image.png");
    REQUIRE(fullPath == "C:/files/image.png");

    // If you pass in a relative path it should be returned unmodified.
    fullPath = osgEarth::Util::stripRelativePaths("../data/world.tif");
    REQUIRE(fullPath == "../data/world.tif");
}

TEST_CASE("getFullPath preserves URL and empty-input behavior", "[path]")
{
    REQUIRE(Util::getFullPath("", "") == "");
    REQUIRE(Util::getFullPath("", "../images/./tile.png") == "../images/./tile.png");
    REQUIRE(Util::getFullPath("relative/world.earth", "") == "relative/world.earth");
    REQUIRE(Util::getFullPath("https://example.com/maps/world.earth", "../images/tile.png") ==
        "https://example.com/images/tile.png");
    REQUIRE(Util::getFullPath("https://example.com/maps/world.earth", "/images/./tile.png") ==
        "https://example.com/images/tile.png");
    REQUIRE(Util::getFullPath("https://example.com/maps/world.earth?token=base&z=1",
        "../images/tile.png?token=target&a=2") ==
        "https://example.com/images/tile.png?a=2&token=base&z=1");
    REQUIRE(Util::getFullPath("https://example.com/maps/world.earth?token=base",
        "https://other.example.com/a/../tile.png?x=2") ==
        "https://other.example.com/tile.png?token=base&x=2");
    REQUIRE(Util::getFullPath("https://example.com/world.earth?token=base", "") ==
        "https://example.com/world.earth?token=base");
}

TEST_CASE("getFullPath matches legacy local path resolution", "[path]")
{
    const std::string directory = osgDB::getCurrentWorkingDirectory();
    const std::vector<std::string> referrers = {
        osgDB::concatPaths(directory, "../CMakeLists.txt"),
        osgDB::concatPaths(directory, "missing.earth"),
        osgDB::concatPaths(directory, "missing-directory/missing.earth"),
        "../CMakeLists.txt", "./missing.earth", "missing.earth"
    };
    const std::vector<std::string> targets = {
        "tile.png", "../images/tile.png", "./images/../tile.png",
        "images/", "..", ".", "archive.zip/images/tile.png"
    };
    for (const auto& referrer : referrers)
    {
        for (const auto& target : targets)
        {
            INFO(referrer << " + " << target);
            const auto expected = Util::stripRelativePaths(osgDB::concatPaths(
                osgDB::getFilePath(osgDB::getRealPath(referrer)), target));
            REQUIRE(Util::getFullPath(referrer, target) == expected);
        }
    }
}

TEST_CASE("getFullPath resolves concurrent cache hits and misses", "[path]")
{
    const std::string referrer = "https://example.com/concurrent/world.earth";
    std::vector<std::thread> workers;
    std::vector<unsigned> failures(4u, 0u);
    for (unsigned worker = 0; worker < failures.size(); ++worker)
    {
        workers.emplace_back([&, worker]()
        {
            // Exceed the 20,000-entry cache while other threads are reading it.
            for (unsigned i = 0; i < 6000u; ++i)
            {
                const auto target = "tiles/" +
                    std::to_string(i * failures.size() + worker) + ".png";
                for (unsigned repeat = 0; repeat < 2u; ++repeat)
                {
                    if (Util::getFullPath(referrer, target) !=
                        "https://example.com/concurrent/" + target)
                        ++failures[worker];
                }
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    for (auto count : failures)
        REQUIRE(count == 0u);
}

TEST_CASE("getFullPath cache distinguishes delimiter-containing inputs", "[path]")
{
    REQUIRE(Util::getFullPath("https://example.com/cache/a&b", "c") ==
        "https://example.com/cache/c");
    REQUIRE(Util::getFullPath("https://example.com/cache/a", "b&c") ==
        "https://example.com/cache/b&c");
}

namespace
{
    struct PathTestDirectory
    {
        std::string path = osgDB::concatPaths(osgDB::getCurrentWorkingDirectory(),
            Util::getTempName("Path Tests ", ""));
        bool created = Util::makeDirectory(path);

        ~PathTestDirectory()
        {
            if (created)
                Util::removeDirectory(path);
        }
    };

}

#if defined(_WIN32) && !defined(__CYGWIN__)
TEST_CASE("getFullPath resolves Windows paths without filesystem lookups", "[path]")
{
    REQUIRE(Util::getFullPath("c:\\Missing Folder\\maps\\world.earth", "../tile.png") ==
        "C:/Missing Folder/tile.png");
    REQUIRE(Util::getFullPath("c:/Missing Folder/maps/./world.earth", "tile.png") ==
        "C:/Missing Folder/maps/tile.png");
    REQUIRE(Util::getFullPath("\\\\unreachable-server\\share\\maps\\world.earth", "../tile.png") ==
        "//unreachable-server/share/tile.png");
    REQUIRE(Util::getFullPath("//unreachable-server/share/maps/world.earth", "tile.png") ==
        "//unreachable-server/share/maps/tile.png");
    REQUIRE(Util::getFullPath("c:/maps/", "tile.png") == "C:/maps/tile.png");
    REQUIRE(Util::getFullPath("c:/world.earth", "tile.png") == "C:/tile.png");
    REQUIRE(Util::getFullPath("\\\\?\\C:\\maps\\world.earth", "tile.png") ==
        "//?/C:/maps/tile.png");
    REQUIRE(Util::getFullPath("\\\\?\\UNC\\server\\share\\maps\\world.earth", "tile.png") ==
        "//?/UNC/server/share/maps/tile.png");
    REQUIRE(Util::getFullPath("\\\\.\\C:\\maps\\world.earth", "tile.png") ==
        "//C:/maps/tile.png");

    const auto cwd = osgDB::convertFileNameToUnixStyle(osgDB::getCurrentWorkingDirectory());
    REQUIRE(cwd.size() >= 3u);
    REQUIRE(Util::getFullPath(cwd.substr(0, 2) + "world.earth", "drive-tile.png") ==
        cwd + "/drive-tile.png");
    REQUIRE(Util::getFullPath("\\maps\\world.earth", "root-tile.png") ==
        cwd.substr(0, 2) + "/maps/root-tile.png");

    std::string longDirectory = "C:/";
    for (unsigned i = 0; i < 30u; ++i)
        longDirectory += "long-directory-name/";
    REQUIRE(Util::getFullPath(longDirectory + "world.earth", "tile.png") ==
        longDirectory + "tile.png");
    for (std::size_t length : { 260u, 261u, 262u })
    {
        const auto directory = "C:/" + std::string(100u, 'a') + '/' +
            std::string(length - 116u, 'b') + '/';
        REQUIRE(directory.size() + std::string("world.earth").size() == length);
        REQUIRE(Util::getFullPath(directory + "world.earth", "tile.png") ==
            directory + "tile.png");
    }
    REQUIRE(Util::getFullPath(std::string(33000u, 'a'), "fallback.png") == "fallback.png");

#ifdef OSG_USE_UTF8_FILENAME
    REQUIRE(Util::getFullPath(u8"c:/donn\u00e9es/\u5730\u56fe/world.earth", u8"../\u5f71\u50cf.png") ==
        u8"C:/donn\u00e9es/\u5f71\u50cf.png");
    // UTF-16 length, rather than UTF-8 byte length, determines the buffer size.
    std::string unicodeDirectory = "C:/";
    for (unsigned i = 0; i < 140u; ++i)
        unicodeDirectory += u8"\u5730\u56fe/";
    REQUIRE(Util::getFullPath(unicodeDirectory + "world.earth", "tile.png") ==
        unicodeDirectory + "tile.png");
#endif
}

TEST_CASE("getFullPath preserves Windows casing and short directory names", "[path]")
{
    PathTestDirectory temporary;
    REQUIRE(temporary.created);
    const auto source = osgDB::concatPaths(temporary.path, "World File.earth");
    const auto target = osgDB::concatPaths(temporary.path, "tile.txt");
    { osgDB::ofstream file(source.c_str()); file << "referrer"; REQUIRE(file.good()); }
    { osgDB::ofstream file(target.c_str()); file << "same target"; REQUIRE(file.good()); }

    auto checkTarget = [&](const std::string& referrer)
    {
        const auto resolved = Util::getFullPath(referrer, "tile.txt");
        const auto legacy = Util::stripRelativePaths(osgDB::concatPaths(
            osgDB::getFilePath(osgDB::getRealPath(referrer)), "tile.txt"));
        osgDB::ifstream file(resolved.c_str());
        std::string content;
        std::getline(file, content);
        REQUIRE(content == "same target");
        osgDB::ifstream legacyFile(legacy.c_str());
        std::string legacyContent;
        std::getline(legacyFile, legacyContent);
        REQUIRE(content == legacyContent);
        return resolved;
    };

    auto differentlyCased = osgDB::convertToLowerCase(source);
    auto expected = osgDB::convertFileNameToUnixStyle(osgDB::getFilePath(differentlyCased));
    expected[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(expected[0])));
    REQUIRE(checkTarget(differentlyCased) == expected + "/tile.txt");

    const auto wideSource = osgDB::convertUTF8toUTF16(source);
    wchar_t shortPath[MAX_PATH + 1] = {};
    const auto length = GetShortPathNameW(wideSource.c_str(), shortPath, MAX_PATH + 1);
    REQUIRE(length > 0u);
    REQUIRE(length < MAX_PATH + 1u);
    const auto shortSource = osgDB::convertUTF16toUTF8(shortPath);
    const auto shortDirectory = osgDB::convertFileNameToUnixStyle(osgDB::getFilePath(shortSource));
    REQUIRE(checkTarget(shortSource) == shortDirectory + "/tile.txt");
    if (shortDirectory == osgDB::convertFileNameToUnixStyle(temporary.path))
        WARN("This volume did not supply an 8.3 directory alias; spelling preservation still checked");
}
#else
TEST_CASE("getFullPath retains POSIX symbolic-link resolution", "[path]")
{
    PathTestDirectory temporary;
    REQUIRE(temporary.created);
    const auto actual = osgDB::concatPaths(temporary.path, "actual");
    REQUIRE(Util::makeDirectory(actual));
    const auto source = osgDB::concatPaths(actual, "world.earth");
    { osgDB::ofstream file(source.c_str()); file << "referrer"; REQUIRE(file.good()); }
    const auto link = osgDB::concatPaths(temporary.path, "link.earth");
    REQUIRE(::symlink(source.c_str(), link.c_str()) == 0);
    const auto expected = Util::stripRelativePaths(osgDB::concatPaths(
        osgDB::getFilePath(osgDB::getRealPath(source)), "tile.png"));
    REQUIRE(Util::getFullPath(link, "tile.png") == expected);
    // Remove the link explicitly before the recursive fixture cleanup.
    REQUIRE(::unlink(link.c_str()) == 0);
}
#endif

