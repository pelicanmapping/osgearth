/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/PBRMaterial>
#include <osgEarth/ImageUtils>
#include <osgEarth/JsonUtils>
#include <osgEarth/ShaderLoader>
#include <osgEarth/Shaders>
#include <osgDB/ConvertBase64>
#include <osgDB/FileNameUtils>
#include <fstream>
#include "ChonkTestUtils.h"

using namespace osgEarth;

namespace osgEarth { namespace Tests { extern std::string executablePath; } }

namespace
{
    osg::ref_ptr<osg::Image> image(const osg::Vec4& color)
    {
        osg::ref_ptr<osg::Image> result = new osg::Image();
        result->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        for (unsigned y = 0; y < 2; ++y)
            for (unsigned x = 0; x < 2; ++x) result->setColor(color, x, y);
        return result;
    }

    osg::Vec4 pixel(osg::Texture* texture)
    {
        REQUIRE(texture != nullptr);
        REQUIRE(texture->getImage(0) != nullptr);
        osg::Vec4 value;
        ImageUtils::PixelReader(texture->getImage(0))(value, 0, 0);
        return value;
    }

    struct GeometryVisitor : osg::NodeVisitor
    {
        std::vector<osg::ref_ptr<osg::Geometry>> geometries;
        GeometryVisitor() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { }
        void apply(osg::Geometry& geometry) override { geometries.push_back(&geometry); }
    };

    struct GLTFFixture
    {
        Util::Json::Value json;
        std::string path = Util::getTempName("pbr-material-", ".png");

        GLTFFixture()
        {
            REQUIRE(osgDB::writeImageFile(*image(osg::Vec4(64/255.0f,128/255.0f,192/255.0f,1)), path));
            const float positions[] = { -2,-2,0, 2,-2,0, 0,2,0 };
            const unsigned short indices[] = { 0,1,2 };
            std::string data(reinterpret_cast<const char*>(positions), sizeof(positions));
            data.append(reinterpret_cast<const char*>(indices), sizeof(indices));
            std::string encoded;
            osgDB::Base64encoder().encode(data.data(), static_cast<int>(data.size()), encoded);
            encoded.erase(std::remove(encoded.begin(), encoded.end(), '\n'), encoded.end());
            encoded.erase(std::remove(encoded.begin(), encoded.end(), '\r'), encoded.end());
            const std::string document = R"({
                "asset":{"version":"2.0"},
                "buffers":[{"byteLength":42}],
                "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},
                    {"buffer":0,"byteOffset":36,"byteLength":6}],
                "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
                    "min":[-2,-2,0],"max":[2,2,0]},
                    {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
                "images":[{}], "samplers":[{"wrapS":10497,"wrapT":33071}],
                "textures":[{"source":0,"sampler":0}],
                "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},
                    "metallicRoughnessTexture":{"index":0},"roughnessFactor":0.5,"metallicFactor":0.25},
                    "normalTexture":{"index":0,"scale":0.7},"occlusionTexture":{"index":0,"strength":0.6}}],
                "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0},
                    {"attributes":{"POSITION":0},"indices":1,"material":0}]}],
                "nodes":[{"mesh":0}], "scenes":[{"nodes":[0]}], "scene":0
            })";
            REQUIRE(Util::Json::Reader().parse(document, json));
            json["buffers"][0u]["uri"] = "data:application/octet-stream;base64," + encoded;
            json["images"][0u]["uri"] = osgDB::convertFileNameToUnixStyle(path);
        }

        ~GLTFFixture() { std::remove(path.c_str()); }

        std::vector<osg::ref_ptr<osg::Geometry>> load(const std::string& options = "")
        {
            auto* reader = osgDB::Registry::instance()->getReaderWriterForExtension("gltf");
            REQUIRE(reader != nullptr);
            std::istringstream stream(Util::Json::FastWriter().write(json));
            osg::ref_ptr<osgDB::Options> readOptions = new osgDB::Options("gltfZUp " + options);
            auto result = reader->readNode(stream, readOptions);
            INFO(result.message());
            REQUIRE(result.validNode());
            GeometryVisitor visitor;
            result.getNode()->accept(visitor);
            REQUIRE(visitor.geometries.size() == 2);
            return visitor.geometries;
        }
    };

    PBRTexture* material(osg::Geometry* geometry)
    {
        auto* result = dynamic_cast<PBRTexture*>(geometry->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
        REQUIRE(result != nullptr);
        return result;
    }
}

TEST_CASE("PBR materials prefer supplied images and produce valid defaults", "[pbr]")
{
    PBRMaterial description;
    description.colorImage = image(osg::Vec4(.2f,.4f,.6f,.8f));
    REQUIRE(description.isSimple());
    description.normalImage = image(osg::Vec4(.5f,.5f,1,1));
    REQUIRE_FALSE(description.isSimple());
    description.color() = URI("missing-pbr-color.png");
    description.normal() = URI("missing-pbr-normal.png");
    PBRTexture texture;
    REQUIRE(texture.load(description).isOK());
    REQUIRE(texture.normal->getImage(0) == description.normalImage);
    REQUIRE(std::abs((pixel(texture.albedo).a()) - (.8f)) <= (1/255.0f));
    auto dram = pixel(texture.pbr);
    REQUIRE(dram.r() == 0.0f);
    REQUIRE(std::abs((dram.g()) - (.5f)) <= (1/255.0f));
    REQUIRE(dram.b() == 1.0f);
    REQUIRE(dram.a() == 0.0f);
    REQUIRE(texture.load(PBRMaterial()).isOK());
    REQUIRE(pixel(texture.normal).b() == 1.0f);
}

TEST_CASE("glTF converts channels and factors to shared PBR materials", "[pbr][gltf]")
{
    GLTFFixture fixture;
    auto geometries = fixture.load();
    auto* texture = material(geometries[0]);
    REQUIRE(material(geometries[1]) == texture);
    REQUIRE(geometries[0]->getStateSet()->getUniform("oe_gltf_pbr_flags") == nullptr);
    VirtualProgram::ShaderMap shaders;
    VirtualProgram::get(geometries[0]->getStateSet())->getShaderMap(shaders);
    for (const auto& shader : shaders)
        REQUIRE(shader.second._shader->getName().find("oe_gltf_") == std::string::npos);
    REQUIRE(texture->albedo->getInternalFormat() == GL_SRGB8_ALPHA8);
    REQUIRE(texture->pbr->getWrap(osg::Texture::WRAP_S) == osg::Texture::REPEAT);
    REQUIRE(pixel(texture->albedo).r() == Approx(64/255.0f));
    const auto dram = pixel(texture->pbr);
    REQUIRE(dram.r() == 0.0f);
    REQUIRE(std::abs((dram.g()) - (128/255.0f*.5f)) <= (1/255.0f));
    REQUIRE(std::abs((dram.b()) - (1+.6f*(64/255.0f-1))) <= (1/255.0f));
    REQUIRE(std::abs((dram.a()) - (192/255.0f*.25f)) <= (1/255.0f));
    osg::Vec3 expected((64/255.0f*2-1)*.7f, -(128/255.0f*2-1)*.7f, 192/255.0f*2-1);
    expected.normalize();
    auto normal = pixel(texture->normal);
    REQUIRE(std::abs((normal.r()) - (expected.x()*.5f+.5f)) <= (1/255.0f));
    REQUIRE(std::abs((normal.g()) - (expected.y()*.5f+.5f)) <= (1/255.0f));
    REQUIRE(std::abs((normal.b()) - (expected.z()*.5f+.5f)) <= (1/255.0f));

    auto again = fixture.load();
    REQUIRE(material(again[0]) == texture);
    auto reload = fixture.load("gltfForceReload");
    REQUIRE(material(reload[0]) != texture);
    REQUIRE(material(fixture.load()[0]) == material(reload[0]));
    fixture.json["materials"][0u]["pbrMetallicRoughness"]["roughnessFactor"] = .75;
    REQUIRE(material(fixture.load()[0]) != material(reload[0]));

    auto skip = fixture.load("gltfSkipPBRTextures");
    REQUIRE(material(skip[0])->albedo.valid());
    REQUIRE_FALSE(material(skip[0])->normal.valid());
    REQUIRE_FALSE(material(skip[0])->pbr.valid());
}

TEST_CASE("glTF factor-only and plain materials preserve their defaults", "[pbr][gltf]")
{
    GLTFFixture fixture;
    auto& source = fixture.json["materials"][0u];
    source = Util::Json::Value(Util::Json::objectValue);
    source["pbrMetallicRoughness"]["metallicFactor"] = .25;
    source["pbrMetallicRoughness"]["roughnessFactor"] = .75;
    auto geometries = fixture.load();
    auto* texture = material(geometries[0]);
    REQUIRE(pixel(texture->albedo) == osg::Vec4(1,1,1,1));
    REQUIRE_FALSE(texture->normal.valid());
    REQUIRE(std::abs((pixel(texture->pbr).g()) - (.75f)) <= (1/255.0f));
    REQUIRE(std::abs((pixel(texture->pbr).a()) - (.25f)) <= (1/255.0f));
    source = Util::Json::Value(Util::Json::objectValue);
    auto plain = fixture.load();
    REQUIRE_FALSE(material(plain[0])->pbr.valid());
}

TEST_CASE("glTF embedded images remain local to each loaded model", "[pbr][gltf]")
{
    GLTFFixture fixture;
    std::ifstream input(fixture.path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::string encoded;
    osgDB::Base64encoder().encode(bytes.data(), static_cast<int>(bytes.size()), encoded);
    encoded.erase(std::remove(encoded.begin(), encoded.end(), '\n'), encoded.end());
    encoded.erase(std::remove(encoded.begin(), encoded.end(), '\r'), encoded.end());
    fixture.json["images"][0u]["uri"] = "data:image/png;base64," + encoded;
    auto a = fixture.load();
    auto b = fixture.load();
    REQUIRE(material(a[0]) == material(a[1]));
    REQUIRE(material(a[0]) != material(b[0]));
    REQUIRE(pixel(material(a[0])->pbr) == pixel(material(b[0])->pbr));
}

TEST_CASE("ShaderGenerator retains PBR descriptors and stable component bindings", "[pbr]")
{
    GLTFFixture fixture;
    auto loaded = fixture.load();
    auto geometry = ChonkTest::mesh();
    geometry->setUserValue(SHADERGEN_HINT_LINEAR_COLOR, true);
    geometry->getOrCreateStateSet()->setTextureAttributeAndModes(0, material(loaded[0]));
    auto* texture = material(geometry);
    osg::ref_ptr<osg::StateSet> original = geometry->getStateSet();
    osg::ref_ptr<osg::Texture2D> unrelated = new osg::Texture2D(image(osg::Vec4(1,1,1,1)));
    original->setTextureAttributeAndModes(1, unrelated);
    ShaderGenerator().run(geometry);
    auto* generated = geometry->getStateSet();
    REQUIRE(generated != original.get());
    REQUIRE(material(geometry) == texture);
    REQUIRE(generated->getTextureAttribute(1, osg::StateAttribute::TEXTURE) == unrelated);
    REQUIRE(original->getUniform("oe_sg_pbr_0_0") == nullptr);
    int first = -1;
    REQUIRE(generated->getUniform("oe_sg_pbr_0_0")->get(first));
    REQUIRE(first > 1);
    REQUIRE(generated->getTextureAttribute(first, osg::StateAttribute::TEXTURE) == texture->albedo);
    ShaderGenerator().run(geometry);
    int second = -1;
    REQUIRE(geometry->getStateSet()->getUniform("oe_sg_pbr_0_0")->get(second));
    REQUIRE(second == first);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    ChonkFactory factory(arena);
    auto chonk = factory.getOrCreateChonk(geometry);
    REQUIRE(chonk);
    REQUIRE(chonk->_vbo_store.front().pbr_index >= 0);
    REQUIRE(chonk->_vbo_store.front().albedo_index >= 0);
    REQUIRE(chonk->_vbo_store.front().normalmap_index >= 0);
    REQUIRE(chonk->_vbo_store.front().color_is_linear == 1);
}

TEST_CASE("PBR rendering uses ordinary and Chonk shaders", "[pbr][gpu]")
{
    // Chonk retains shared GL objects across viewer lifetimes. Use a fresh
    // process so this comparison does not reuse a prior GPU test's context ID.
    const auto& executable = osgEarth::Tests::executablePath;
#ifdef _WIN32
    const std::string command = "\"\"" + executable + "\" \"[.pbr-render-worker]\"\"";
#else
    std::string quoted = "'";
    for (char c : executable) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    const std::string command = quoted + "' '[.pbr-render-worker]'";
#endif
    REQUIRE(std::system(command.c_str()) == 0);
}

TEST_CASE("PBR material rendering comparison", "[.pbr-render-worker]")
{
    ChonkTest::Renderer renderer;
    REQUIRE(renderer.initialize());
    GLTFFixture fixture;
    auto loaded = fixture.load();
    auto geometry = ChonkTest::mesh();
    geometry->setUserValue(SHADERGEN_HINT_LINEAR_COLOR, true);
    geometry->getOrCreateStateSet()->setTextureAttributeAndModes(0, material(loaded[0]));
    ShaderGenerator().run(geometry);
    auto* rootVP = VirtualProgram::getOrCreate(renderer.root->getOrCreateStateSet());
    Util::Shaders shaders;
    shaders.load(rootVP, shaders.PBR);
    const std::string declarations = R"(
        struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;
        in vec3 vp_Normal;
    )";
    renderer.viewer.getCamera()->setViewMatrixAsLookAt(osg::Vec3d(0,0,5), osg::Vec3d(), osg::Vec3d(0,1,0));
    osg::ref_ptr<ChonkDrawable> drawable;
    if (Capabilities::get().supportsNVGL())
    {
        auto chonk = renderer.factory->getOrCreateChonk(geometry);
        REQUIRE(chonk);
        drawable = new ChonkDrawable();
        drawable->add(chonk, osg::Matrixf());
    }
    auto dram = pixel(material(loaded[0])->pbr);
    auto n = pixel(material(loaded[0])->normal);
    osg::Vec3 normal(n.r()*2-1, n.g()*2-1, n.b()*2-1);
    normal.normalize();
    auto decode = [](float c) { return c <= .04045f ? c/12.92f : std::pow((c+.055f)/1.055f,2.4f); };
    auto encode = [](float c) { return c <= .0031308f ? c*12.92f : 1.055f*std::pow(c,1/2.4f)-.055f; };
    std::vector<osg::Vec4> expected = {
        osg::Vec4(dram.g(),dram.b(),dram.a(),1),
        osg::Vec4(encode(decode(64/255.0f)*.2f), encode(decode(128/255.0f)*.6f), 192/255.0f, 1),
        osg::Vec4(normal.x()*.5f+.5f, normal.y()*.5f+.5f, normal.z()*.5f+.5f, 1) };
    const std::vector<std::string> bodies = {
        "c = vec4(oe_pbr.roughness, oe_pbr.ao, oe_pbr.metal, 1);",
        "", "c = vec4(normalize(vp_Normal)*.5+.5,1);" };
    for (unsigned mode = 0; mode < bodies.size(); ++mode)
    {
        INFO("Diagnostic mode: " << mode);
        rootVP->setFunction("pbr_test_values", declarations +
            "void pbr_test_values(inout vec4 c) { " + bodies[mode] + " }",
            VirtualProgram::LOCATION_FRAGMENT_LIGHTING);
        renderer.setScene(geometry);
        renderer.frame(); renderer.frame();
        auto ordinary = renderer.pixels()->getColor(128,128);
        for (unsigned c = 0; c < 3; ++c)
            REQUIRE(std::abs((ordinary[c]) - (expected[mode][c])) <= (3/255.0f));
        REQUIRE(glGetError() == GL_NO_ERROR);
        if (drawable)
        {
            renderer.setScene(drawable);
            renderer.frame(); renderer.frame();
            auto packed = renderer.pixels()->getColor(128,128);
            for (unsigned c = 0; c < 3; ++c)
                REQUIRE(std::abs((packed[c]) - (ordinary[c])) <= (3/255.0f));
            REQUIRE(glGetError() == GL_NO_ERROR);
        }
    }
}
