/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/InstanceBuilder>
#include <osgEarth/Kit>
#include <osgEarth/VirtualProgram>

#include <osg/Geode>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osgDB/ReadFile>

#include <array>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace osgEarth;

namespace
{
    osg::Geometry* makeUnitTriangle()
    {
        osg::ref_ptr<osg::Geometry> geometry = InstanceBuilder::createGeometry();
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
        vertices->push_back(osg::Vec3f(-0.5f, -0.5f, 0.0f));
        vertices->push_back(osg::Vec3f(0.5f, -0.5f, 0.0f));
        vertices->push_back(osg::Vec3f(0.0f, 0.5f, 1.0f));
        geometry->setVertexArray(vertices.get());
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        return geometry.release();
    }

    osg::Node* makeModel()
    {
        osg::ref_ptr<osg::Geode> geode = new osg::Geode();
        geode->addDrawable(makeUnitTriangle());
        return geode.release();
    }

    osg::Node* makeTexturedModel()
    {
        osg::ref_ptr<osg::Geode> geode = new osg::Geode();
        osg::ref_ptr<osg::Geometry> geometry = makeUnitTriangle();

        osg::ref_ptr<osg::Vec2Array> texcoords = new osg::Vec2Array();
        texcoords->push_back(osg::Vec2f(0.0f, 0.0f));
        texcoords->push_back(osg::Vec2f(1.0f, 0.0f));
        texcoords->push_back(osg::Vec2f(0.5f, 1.0f));
        geometry->setTexCoordArray(0u, texcoords.get());

        osg::ref_ptr<osg::Image> image = new osg::Image();
        image->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->data()[0] = 255u;
        image->data()[1] = 127u;
        image->data()[2] = 63u;
        image->data()[3] = 255u;
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image.get());
        geometry->getOrCreateStateSet()->setTextureAttributeAndModes(0u, texture.get());

        geode->addDrawable(geometry.get());
        return geode.release();
    }

    struct GeometryFinder : public osg::NodeVisitor
    {
        GeometryFinder() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { }
        void apply(osg::Geode& geode) override
        {
            for (unsigned i = 0u; i < geode.getNumDrawables(); ++i)
                if (auto* geometry = geode.getDrawable(i)->asGeometry())
                    geometries.push_back(geometry);
            traverse(geode);
        }
        std::vector<osg::Geometry*> geometries;
    };

    struct KitNodeFinder : public osg::NodeVisitor
    {
        KitNodeFinder() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { }
        void apply(osg::Group& group) override
        {
            if (auto* kitNode = dynamic_cast<KitNode*>(&group))
                nodes.push_back(kitNode);
            traverse(group);
        }
        std::vector<KitNode*> nodes;
    };

    struct GeocentricRangeVisitor : public osg::NodeVisitor
    {
        explicit GeocentricRangeVisitor(
            float distance = 100.0f,
            unsigned traversalNumber = 1u) :
            osg::NodeVisitor(TRAVERSE_ACTIVE_CHILDREN),
            distance(distance)
        {
            setVisitorType(CULL_VISITOR);
            setTraversalNumber(traversalNumber);
        }

        float getDistanceToEyePoint(const osg::Vec3&, bool) const override
        {
            return distance;
        }

        float getDistanceToViewPoint(const osg::Vec3&, bool) const override
        {
            return 26000000.0f;
        }

        float distance;
    };

    template<typename T>
    void writeBinaryValue(std::ostream& output, const T& value)
    {
        output.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }
}

TEST_CASE("Kit reads compact binary city batches")
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "osgearth_kit_test.kitcityb";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        const std::array<char, 8> magic = { 'O', 'E', 'K', 'I', 'T', 'B', '0', '1' };
        output.write(magic.data(), magic.size());
        writeBinaryValue(output, std::uint32_t(0x01020304u));
        writeBinaryValue(output, std::uint32_t(1u));
        writeBinaryValue(output, std::uint64_t(2u));
        const std::string model = "wall1";
        writeBinaryValue(output, static_cast<std::uint32_t>(model.size()));
        output.write(model.data(), model.size());
        const std::array<float, 7> rotationAndScale = {
            0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 2.0f, 2.0f
        };
        output.write(
            reinterpret_cast<const char*>(rotationAndScale.data()),
            sizeof(float) * rotationAndScale.size());
        writeBinaryValue(output, std::uint64_t(2u));
        const std::array<float, 6> positions = {
            1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f
        };
        output.write(
            reinterpret_cast<const char*>(positions.data()),
            sizeof(float) * positions.size());
    }

    osg::ref_ptr<osg::Node> node = osgDB::readRefNodeFile(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    KitNode* city = dynamic_cast<KitNode*>(node.get());
    REQUIRE(city != nullptr);
    REQUIRE(city->getNumInstances() == 2u);
    REQUIRE(city->getInstanceBatches().size() == 1u);
    CHECK(city->getInstanceBatches().front().model == "wall1");
    CHECK(city->getInstanceBatches().front().positions.size() == 2u);
    CHECK(city->getInstances()[0].model == "wall1");
    CHECK(city->getInstances()[0].position == osg::Vec3f(1.0f, 2.0f, 3.0f));
    CHECK(city->getInstances()[1].position == osg::Vec3f(4.0f, 5.0f, 6.0f));
    CHECK(city->getInstances()[1].scale == osg::Vec3f(2.0f, 2.0f, 2.0f));
    CHECK(city->getInstances()[0].minRange == 0.0f);
    CHECK(city->getInstances()[0].maxRange == std::numeric_limits<float>::max());
}

TEST_CASE("Kit reads version two binary instance ranges")
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "osgearth_kit_lod_test.kitcityb";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        const std::array<char, 8> magic = { 'O', 'E', 'K', 'I', 'T', 'B', '0', '2' };
        output.write(magic.data(), magic.size());
        writeBinaryValue(output, std::uint32_t(0x01020304u));
        writeBinaryValue(output, std::uint32_t(1u));
        writeBinaryValue(output, std::uint64_t(1u));
        const std::string model = "window";
        writeBinaryValue(output, static_cast<std::uint32_t>(model.size()));
        output.write(model.data(), model.size());
        const std::array<float, 9> batchValues = {
            0.0f, 0.0f, 0.0f, 1.0f,
            1.0f, 1.0f, 1.0f,
            25.0f, 750.0f
        };
        output.write(
            reinterpret_cast<const char*>(batchValues.data()),
            sizeof(float) * batchValues.size());
        writeBinaryValue(output, std::uint64_t(1u));
        const std::array<float, 3> position = { 1.0f, 2.0f, 3.0f };
        output.write(
            reinterpret_cast<const char*>(position.data()),
            sizeof(float) * position.size());
    }

    osg::ref_ptr<osg::Node> node = osgDB::readRefNodeFile(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    KitNode* city = dynamic_cast<KitNode*>(node.get());
    REQUIRE(city != nullptr);
    REQUIRE(city->getNumInstances() == 1u);
    CHECK(city->getInstances()[0].model == "window");
    CHECK(city->getInstances()[0].minRange == 25.0f);
    CHECK(city->getInstances()[0].maxRange == 750.0f);
}

TEST_CASE("Kit reads text instance ranges")
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "osgearth_kit_lod_test.kitcity";
    {
        std::ofstream output(path, std::ios::trunc);
        REQUIRE(output.good());
        output << "kitcity 2\n";
        output << "instance \"window\" 1 2 3 0 0 0 1 1 1 1 25 750\n";
    }

    osg::ref_ptr<osg::Node> node = osgDB::readRefNodeFile(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    REQUIRE(node.valid());
    KitNodeFinder finder;
    node->accept(finder);
    REQUIRE(finder.nodes.size() == 1u);
    REQUIRE(finder.nodes.front()->getNumInstances() == 1u);
    const auto& value = finder.nodes.front()->getInstances().front();
    CHECK(value.model == "window");
    CHECK(value.minRange == 25.0f);
    CHECK(value.maxRange == 750.0f);
}

TEST_CASE("Kit batches one model across lightweight nodes and parent transforms")
{
    osg::ref_ptr<Kit> kit = new Kit();
    REQUIRE(kit->addModel("wall1", makeModel()));

    osg::ref_ptr<osg::Group> source = new osg::Group();
    osg::ref_ptr<KitNode> first = new KitNode();
    first->addInstance("wall1", osg::Vec3f(1.0f, 2.0f, 3.0f));
    source->addChild(first.get());

    osg::ref_ptr<osg::MatrixTransform> parent = new osg::MatrixTransform(
        osg::Matrix::translate(100.0f, 0.0f, 0.0f));
    osg::ref_ptr<KitNode> second = new KitNode();
    const osg::Quat secondRotation(osg::PI_4, osg::Vec3f(0.0f, 0.0f, 1.0f));
    const osg::Vec3f secondScale(2.0f, 3.0f, 4.0f);
    second->addInstance(
        "wall1", osg::Vec3f(4.0f, 5.0f, 6.0f),
        secondRotation, secondScale);
    parent->addChild(second.get());
    source->addChild(parent.get());

    Kit::BuildStats stats;
    osg::ref_ptr<osg::Group> result = kit->createInstancedNode(source.get(), &stats);
    CHECK(stats.instances == 2u);
    CHECK(stats.batches == 1u);
    CHECK(stats.drawables == 1u);
    CHECK(stats.missingModels == 0u);
    CHECK(result->getNumChildren() == 0u);

    GeometryFinder submissions;
    result->accept(submissions);
    CHECK(submissions.geometries.empty());

    GeometryFinder renderer;
    kit->getRenderNode()->accept(renderer);
    REQUIRE(renderer.geometries.size() == 1u);
    osg::Geometry* geometry = renderer.geometries.front();
    CHECK_FALSE(geometry->getUseVertexArrayObject());
    REQUIRE(geometry->getNumPrimitiveSets() == 1u);
    // The cull collector sets this immediately before the persistent drawable
    // renders; compiled city graphs never own or mutate a primitive set.
    CHECK(geometry->getPrimitiveSet(0)->getNumInstances() == 0u);
    CHECK(result->getBound().valid());
    CHECK(result->getBound().center().x() + result->getBound().radius() > 100.0f);
}

TEST_CASE("Kit instance node filters compact batches by eye range")
{
    osg::ref_ptr<Kit> kit = new Kit();
    kit->setInstanceChunkSize(100.0f);
    REQUIRE(kit->addModel("window", makeModel()));

    osg::ref_ptr<KitNode> source = new KitNode();
    source->addInstance("window", osg::Vec3f(1.0f, 0.0f, 0.0f));
    source->addInstance(
        "window",
        osg::Vec3f(2.0f, 0.0f, 0.0f),
        osg::Quat(),
        osg::Vec3f(1.0f, 1.0f, 1.0f),
        25.0f,
        750.0f);

    Kit::BuildStats stats;
    osg::ref_ptr<osg::Group> result =
        kit->createInstancedNode(source.get(), &stats);
    CHECK(stats.instances == 2u);
    CHECK(stats.batches == 2u);
    CHECK(stats.drawables == 1u);

    GeometryFinder finder;
    result->accept(finder);
    CHECK(finder.geometries.empty());
    CHECK(result->getNumChildren() == 0u);
    GeocentricRangeVisitor nearVisitor(100.0f, 1u);
    result->accept(nearVisitor);
    CHECK(kit->getNumCollectedInstances(nullptr) == 2u);
    GeocentricRangeVisitor farVisitor(1000.0f, 2u);
    result->accept(farVisitor);
    CHECK(kit->getNumCollectedInstances(nullptr) == 1u);
    CHECK(kit->getNumRenderDrawables() == 1u);
}

TEST_CASE("Kit instance LOD uses camera eye in a geocentric transform")
{
    osg::ref_ptr<Kit> kit = new Kit();
    kit->setInstanceChunkSize(100.0f);
    REQUIRE(kit->addModel("window", makeModel()));

    osg::ref_ptr<KitNode> source = new KitNode();
    source->addInstance(
        "window", osg::Vec3f(2.0f, 0.0f, 0.0f), osg::Quat(),
        osg::Vec3f(1.0f, 1.0f, 1.0f), 25.0f, 750.0f);
    osg::ref_ptr<osg::Group> result = kit->createInstancedNode(source.get());

    GeocentricRangeVisitor visitor;
    result->accept(visitor);
    CHECK(kit->getNumCollectedInstances(nullptr) == 1u);
}

TEST_CASE("Kit can split instanced batches into spatial chunks")
{
    osg::ref_ptr<Kit> kit = new Kit();
    kit->setInstanceChunkSize(100.0f);
    REQUIRE(kit->addModel("wall1", makeModel()));

    osg::ref_ptr<KitNode> source = new KitNode();
    source->addInstance("wall1", osg::Vec3f(10.0f, 0.0f, 0.0f));
    source->addInstance("wall1", osg::Vec3f(40.0f, 0.0f, 0.0f));
    source->addInstance("wall1", osg::Vec3f(160.0f, 0.0f, 0.0f));

    Kit::BuildStats stats;
    osg::ref_ptr<osg::Group> result =
        kit->createInstancedNode(source.get(), &stats);
    CHECK(stats.instances == 3u);
    CHECK(stats.batches == 2u);
    CHECK(stats.drawables == 1u);

    GeometryFinder finder;
    result->accept(finder);
    CHECK(finder.geometries.empty());
    CHECK(result->getNumChildren() == 0u);
    CHECK(kit->getNumRenderDrawables() == 1u);
}

TEST_CASE("InstanceBuilder computes bounds for every transformed corner")
{
    osg::ref_ptr<osg::Geometry> geometry = makeUnitTriangle();
    osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array();
    positions->push_back(osg::Vec3f(-10.0f, 0.0f, 0.0f));
    positions->push_back(osg::Vec3f(20.0f, 0.0f, 0.0f));

    osg::ref_ptr<osg::Vec3Array> scales = new osg::Vec3Array();
    scales->push_back(osg::Vec3f(2.0f, 2.0f, 2.0f));
    scales->push_back(osg::Vec3f(4.0f, 4.0f, 4.0f));

    osg::ref_ptr<osg::Vec4Array> rotations = new osg::Vec4Array();
    rotations->push_back(osg::Vec4f(0.0f, 0.0f, 0.0f, 1.0f));
    rotations->push_back(osg::Vec4f(0.0f, 0.0f, 0.0f, 1.0f));

    InstanceBuilder builder;
    builder.setPositions(positions.get());
    builder.setRotations(rotations.get());
    builder.setScales(scales.get());
    REQUIRE(builder.compressInstanceAttributes());
    builder.installInstancing(geometry.get());

    const osg::BoundingBox bounds = geometry->getBoundingBox();
    CHECK(geometry->getComputeBoundingBoxCallback() == nullptr);
    CHECK(std::abs(bounds.xMin() - (-11.0f)) < 1e-5f);
    CHECK(std::abs(bounds.xMax() - 22.0f) < 1e-5f);
    CHECK(std::abs(bounds.zMax() - 4.0f) < 1e-5f);
    geometry->dirtyBound();
    const osg::BoundingBox cachedBounds = geometry->getBoundingBox();
    CHECK(cachedBounds.xMin() == bounds.xMin());
    CHECK(cachedBounds.xMax() == bounds.xMax());
    CHECK(cachedBounds.zMax() == bounds.zMax());
}

TEST_CASE("Kit city graphs share one persistent model renderer")
{
    osg::ref_ptr<Kit> kit = new Kit();
    REQUIRE(kit->addModel("wall1", makeModel()));

    osg::ref_ptr<KitNode> firstSource = new KitNode();
    firstSource->addInstance("wall1", osg::Vec3f(1.0f, 0.0f, 0.0f));
    osg::ref_ptr<KitNode> secondSource = new KitNode();
    secondSource->addInstance("wall1", osg::Vec3f(2.0f, 0.0f, 0.0f));

    osg::ref_ptr<osg::Group> first = kit->createInstancedNode(firstSource.get());
    osg::ref_ptr<osg::Group> second = kit->createInstancedNode(secondSource.get());
    GeometryFinder firstFinder;
    GeometryFinder secondFinder;
    first->accept(firstFinder);
    second->accept(secondFinder);
    CHECK(firstFinder.geometries.empty());
    CHECK(secondFinder.geometries.empty());

    GeometryFinder rendererFinder;
    kit->getRenderNode()->accept(rendererFinder);
    REQUIRE(rendererFinder.geometries.size() == 1u);
    CHECK(kit->getNumRenderDrawables() == 1u);
    const VirtualProgram* program =
        VirtualProgram::get(kit->getRenderNode()->getStateSet());
    REQUIRE(program != nullptr);
    CHECK(program->getName() == "KitCollectedInstancing");
}

TEST_CASE("Kit draw count is independent of separately compiled city layout")
{
    osg::ref_ptr<Kit> kit = new Kit();
    REQUIRE(kit->addModel("wall1", makeModel()));

    osg::ref_ptr<osg::Group> scene = new osg::Group();
    unsigned totalInstances = 0u;
    for (unsigned group = 0u; group < 4u; ++group)
    {
        osg::ref_ptr<KitNode> source = new KitNode();
        for (unsigned i = 0u; i < 250u; ++i)
        {
            source->addInstance(
                "wall1",
                osg::Vec3f(static_cast<float>(i), static_cast<float>(group), 0.0f));
        }
        Kit::BuildStats stats;
        scene->addChild(kit->createInstancedNode(source.get(), &stats));
        totalInstances += stats.instances;
        CHECK(stats.drawables == 1u);
    }
    scene->addChild(kit->getRenderNode());

    GeometryFinder finder;
    scene->accept(finder);
    REQUIRE(finder.geometries.size() == 1u);
    CHECK(totalInstances == 1000u);
    CHECK(kit->getNumRenderDrawables() == 1u);
}

TEST_CASE("Kit instancing preserves model texture shader state")
{
    osg::ref_ptr<Kit> kit = new Kit();
    REQUIRE(kit->addModel("textured", makeTexturedModel()));

    GeometryFinder preparedFinder;
    kit->getModel("textured")->accept(preparedFinder);
    REQUIRE(preparedFinder.geometries.size() == 1u);
    osg::Geometry* preparedGeometry = preparedFinder.geometries.front();
    REQUIRE(preparedGeometry->getTexCoordArray(0u) != nullptr);
    REQUIRE(preparedGeometry->getStateSet() != nullptr);
    REQUIRE(preparedGeometry->getStateSet()->getTextureAttribute(
        0u, osg::StateAttribute::TEXTURE) != nullptr);
    const VirtualProgram* modelProgram =
        VirtualProgram::get(preparedGeometry->getStateSet());
    REQUIRE(modelProgram != nullptr);

    osg::ref_ptr<KitNode> source = new KitNode();
    source->addInstance("textured", osg::Vec3f(1.0f, 2.0f, 3.0f));
    osg::ref_ptr<osg::Group> result = kit->createInstancedNode(source.get());

    REQUIRE(kit->getRenderNode()->getStateSet() != nullptr);
    const VirtualProgram* instancingProgram =
        VirtualProgram::get(kit->getRenderNode()->getStateSet());
    REQUIRE(instancingProgram != nullptr);
    CHECK(instancingProgram->getName() == "KitCollectedInstancing");
    CHECK(instancingProgram != modelProgram);

    GeometryFinder batchFinder;
    kit->getRenderNode()->accept(batchFinder);
    REQUIRE(batchFinder.geometries.size() == 1u);
    osg::Geometry* batchGeometry = batchFinder.geometries.front();
    CHECK(batchGeometry->getTexCoordArray(0u) ==
        preparedGeometry->getTexCoordArray(0u));
    REQUIRE(batchGeometry->getStateSet() != nullptr);
    CHECK(batchGeometry->getStateSet()->getTextureAttribute(
        0u, osg::StateAttribute::TEXTURE) ==
        preparedGeometry->getStateSet()->getTextureAttribute(
            0u, osg::StateAttribute::TEXTURE));
    CHECK(VirtualProgram::get(batchGeometry->getStateSet()) == modelProgram);
}

TEST_CASE("Kit reports unresolved model names without emitting a batch")
{
    osg::ref_ptr<KitNode> source = new KitNode();
    source->addInstance("missing", osg::Vec3f());
    osg::ref_ptr<Kit> kit = new Kit();
    Kit::BuildStats stats;
    osg::ref_ptr<osg::Group> result = kit->createInstancedNode(source.get(), &stats);
    CHECK(stats.instances == 1u);
    CHECK(stats.missingModels == 1u);
    CHECK(result->getNumChildren() == 0u);
}
