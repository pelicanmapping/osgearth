/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */

#include <osgEarth/catch.hpp>
#include <osgEarth/FileUtils>
#include <osgEarth/Registry>
#include <osgEarth/URI>
#include <osg/Geode>
#include <osg/Geometry>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ObjectWrapper>
#include <osgDB/Registry>
#include <osgDB/fstream>
#include <cstdint>
#include <sstream>
#include <vector>

using namespace osgEarth;

namespace
{
    struct LocalFiles
    {
        std::string directory = osgDB::concatPaths(osgDB::getCurrentWorkingDirectory(),
            Util::getTempName("Compressed URI Tests ", ""));
        bool created = Util::makeDirectory(directory);

        ~LocalFiles()
        {
            if (created)
                Util::removeDirectory(directory);
        }

        std::string write(const std::string& name, const std::string& data) const
        {
            const auto path = osgDB::concatPaths(directory, name);
            osgDB::ofstream output(path.c_str(), std::ios::binary);
            output.write(data.data(), data.size());
            output.close();
            REQUIRE(output.good());
            return path;
        }
    };

    std::string gzip(const std::string& data)
    {
        osg::ref_ptr<osgDB::BaseCompressor> compressor =
            osgDB::Registry::instance()->getObjectWrapperManager()->findCompressor("zlib");
        REQUIRE(compressor.valid());
        std::ostringstream output;
        REQUIRE(compressor->compress(output, data));
        return output.str();
    }

    void appendUInt32(std::string& data, std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            data.push_back(static_cast<char>((value >> shift) & 0xffu));
    }

    std::string triangleBuffer()
    {
        // Three positions as little-endian IEEE floats, followed by indices.
        std::string result;
        for (auto bits : { 0u, 0u, 0u, 0x3f800000u, 0u, 0u, 0u, 0x3f800000u, 0u })
            appendUInt32(result, bits);
        result.append("\0\0\1\0\2\0", 6);
        return result;
    }

    std::string triangleJSON(bool binary)
    {
        return std::string(R"({"asset":{"version":"2.0"},"scene":0,
            "scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],
            "buffers":[{)") + (binary ? "" : "\"uri\":\"mesh.bin\",") + R"("byteLength":42}],
            "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},
                           {"buffer":0,"byteOffset":36,"byteLength":6,"target":34963}],
            "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
                          "min":[0,0,0],"max":[1,1,0]},
                         {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";
    }

    std::string triangleGLB()
    {
        auto json = triangleJSON(true);
        while (json.size() % 4 != 0)
            json += ' ';
        auto buffer = triangleBuffer();
        while (buffer.size() % 4 != 0)
            buffer += '\0';
        std::string result;
        appendUInt32(result, 0x46546c67u); // glTF
        appendUInt32(result, 2u);
        appendUInt32(result, static_cast<std::uint32_t>(28 + json.size() + buffer.size()));
        appendUInt32(result, static_cast<std::uint32_t>(json.size()));
        appendUInt32(result, 0x4e4f534au); // JSON
        result += json;
        appendUInt32(result, static_cast<std::uint32_t>(buffer.size()));
        appendUInt32(result, 0x004e4942u); // BIN
        result += buffer;
        return result;
    }

    struct GeometryCounts : osg::NodeVisitor
    {
        unsigned vertices = 0;
        unsigned indices = 0;

        GeometryCounts() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { }

        void apply(osg::Geode& geode) override
        {
            for (unsigned i = 0; i < geode.getNumDrawables(); ++i)
            {
                auto geometry = geode.getDrawable(i)->asGeometry();
                if (geometry && geometry->getVertexArray())
                {
                    vertices += geometry->getVertexArray()->getNumElements();
                    for (unsigned p = 0; p < geometry->getNumPrimitiveSets(); ++p)
                        indices += geometry->getPrimitiveSet(p)->getNumIndices();
                }
            }
            traverse(geode);
        }
    };
}

TEST_CASE("URI readString preserves uncompressed local bytes", "[uri][gzip]")
{
    LocalFiles files;
    REQUIRE(files.created);
    const std::vector<std::string> contents = {
        "", "x", std::string("\x1f", 1), std::string("\x1f\0", 2),
        std::string("binary\0data\xff", 12), "plain text\nwith whitespace\n"
    };
    for (unsigned i = 0; i < contents.size(); ++i)
    {
        const auto path = files.write(std::to_string(i) + ".dat", contents[i]);
        const auto result = URI(path).readString();
        REQUIRE(result.succeeded());
        REQUIRE(result.getString() == contents[i]);
        REQUIRE(result.lastModifiedTime() == Util::getLastModifiedTime(path));
    }
}

TEST_CASE("URI readString detects gzip from its header", "[uri][gzip]")
{
    LocalFiles files;
    REQUIRE(files.created);
    std::string payload;
    // Cross the decompressor's chunk boundary and include every byte value.
    for (unsigned i = 0; i < 100000; ++i)
        payload.push_back(static_cast<char>(i & 0xffu));
    const auto compressed = gzip(payload);
    for (const auto& name : { "payload.bin", "payload.bin.gz" })
    {
        const auto path = files.write(name, compressed);
        const auto result = URI(path).readString();
        REQUIRE(result.succeeded());
        REQUIRE(result.getString() == payload);
        REQUIRE(result.lastModifiedTime() == Util::getLastModifiedTime(path));
    }
    // A complete gzip member containing an empty DEFLATE block.
    const unsigned char emptyGzip[] = {
        0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 3,
        3, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    const auto emptyPath = files.write("empty.gz", std::string(
        reinterpret_cast<const char*>(emptyGzip), sizeof(emptyGzip)));
    const auto emptyResult = URI(emptyPath).readString();
    REQUIRE(emptyResult.succeeded());
    REQUIRE(emptyResult.getString().empty());
}

TEST_CASE("URI readString rejects damaged gzip without returning partial data", "[uri][gzip]")
{
    LocalFiles files;
    REQUIRE(files.created);
    const auto compressed = gzip("a compressed payload");
    auto corrupt = compressed;
    corrupt[corrupt.size() - 8] ^= 1; // Damage the gzip CRC.
    const std::vector<std::string> damaged = {
        compressed.substr(0, 2), compressed.substr(0, compressed.size() - 1), corrupt
    };
    for (unsigned i = 0; i < damaged.size(); ++i)
    {
        const auto name = std::to_string(i) + ".gz";
        const auto path = files.write(name, damaged[i]);
        const auto result = URI(path).readString();
        REQUIRE(result.code() == ReadResult::RESULT_READER_ERROR);
        REQUIRE(result.failed());
        REQUIRE(result.empty());
        REQUIRE_FALSE(result.errorDetail().empty());
        // A corrupt file must not be blacklisted as if it did not exist.
        files.write(name, compressed);
        REQUIRE(URI(path).getString() == "a compressed payload");
    }
}

TEST_CASE("glTF loader reads plain and gzip-compressed local models", "[uri][gzip][gltf]")
{
    Registry::instance();
    REQUIRE(osgDB::Registry::instance()->getReaderWriterForExtension("gltf") != nullptr);
    for (const auto& extension : { ".gltf", ".glb", ".gltf.gz", ".glb.gz" })
    {
        for (bool compressed : { false, true })
        {
            if (!compressed && osgDB::getFileExtension(extension) == "gz")
                continue;
            SECTION(std::string(extension) + (compressed ? " gzip" : " plain"))
            {
                LocalFiles files;
                REQUIRE(files.created);
                const bool binary = std::string(extension).find(".glb") == 0;
                const auto data = binary ? triangleGLB() : triangleJSON(false);
                const auto buffer = triangleBuffer();
                files.write("mesh.bin", compressed ? gzip(buffer) : buffer);
                const auto path = files.write(std::string("triangle") + extension,
                    compressed ? gzip(data) : data);
                const auto result = URI(path).readNode();
                INFO(result.errorDetail());
                REQUIRE(result.succeeded());
                REQUIRE(result.getNode() != nullptr);
                GeometryCounts counts;
                result.getNode()->accept(counts);
                REQUIRE(counts.vertices == 3);
                REQUIRE(counts.indices == 3);
            }
        }
    }
}
