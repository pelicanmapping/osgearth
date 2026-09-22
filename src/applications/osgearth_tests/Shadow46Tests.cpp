/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include "Shadow46TestScene.h"
#include <osgEarth/MapNode>
#include <osgEarth/PagedNode>
#include <osgEarth/TerrainEngineNode>
#include <osg/NodeCallback>
#include <osg/Texture2D>

using namespace osgEarth;
using osgEarth::Util::ShadowCaster;

namespace
{
    //! Requires pixel equivalence while allowing depth-edge rounding between independently composed matrices.
    void equivalent(const std::vector<float>& reference, const std::vector<float>& actual)
    {
        REQUIRE(reference.size() == actual.size());
        double sum = 0.0;
        unsigned changed = 0;
        bool finite = true;
        for (unsigned i=0; i<reference.size(); ++i)
        {
            finite &= std::isfinite(actual[i]);
            double delta = std::abs(reference[i]-actual[i]);
            sum += delta;
            if (delta > 0.02) ++changed;
        }
        CHECK(sum/reference.size() < 0.001);
        CHECK(finite);
        CHECK(double(changed)/reference.size() < 0.005);
    }

    //! Represents application animation even when this particular test callback does not move a vertex.
    struct Animated : osg::NodeCallback
    {
        //! Continues traversal, exercising conservative cache bypass without introducing pixel differences.
        void operator()(osg::Node* node, osg::NodeVisitor* visitor) override { traverse(node,visitor); }
    };
}

// Exercises both GPU-counted APIs against the retained reference culler with multiple commands and empty cascades.
TEST_CASE("Shadow46 submissions preserve city pixels", "[shadows][shadow46][.gl]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires a 4.6 NVGL context"); return; }
    Shadow46Tests::Scene scene(1024,8,320,240);
    scene.buildings->setBirthday(-100.0);
    for (unsigned count : {1u,2u,3u,4u})
    {
        scene.shadows->setCascades(count,600);
        scene.shadows->setSubmission(ShadowCaster::LEGACY);
        scene.draw(); scene.draw();
        auto reference = scene.pixels();
        for (auto path : {ShadowCaster::AUTOMATIC,ShadowCaster::CORE})
        {
            scene.shadows->setSubmission(path);
            scene.draw(); scene.draw();
            equivalent(reference,scene.pixels());
        }
    }
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Mirrors GUI installation: shared classification preserves shadows through map and paging containers.
TEST_CASE("Shadow46 shared culling accepts map and paging containers", "[shadows][shadow46][.gl]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires a 4.6 NVGL context"); return; }
    Shadow46Tests::Scene scene(256,2,320,240);
    scene.buildings->setBirthday(-100.0);
    osg::ref_ptr<MapNode> map = new MapNode;
    REQUIRE(map->open());
    int unit = -1;
    REQUIRE(map->getTerrainEngine()->getResources()->reserveTextureImageUnit(unit,"Shadow test"));
    scene.shadows->setTextureImageUnit(unit);
    scene.shadows->addChild(map);
    scene.shadows->getShadowCastingGroup()->addChild(map->getTerrainEngine()->getNode());
    scene.shadows->getShadowCastingGroup()->addChild(map->getLayerNodeGroup());
    osg::ref_ptr<Util::PagingManager> manager = new Util::PagingManager("");
    osg::ref_ptr<Util::PagedNode2> page = new Util::PagedNode2;
    page->setMaxRange(FLT_MAX);
    page->addChild(scene.models);
    manager->addChild(page);
    scene.shadows->removeChild(scene.models);
    scene.shadows->getShadowCastingGroup()->removeChild(scene.models);
    scene.shadows->addChild(manager);
    scene.shadows->getShadowCastingGroup()->addChild(manager);
    scene.shadows->setSubmission(ShadowCaster::LEGACY);
    for (unsigned i=0; i<8; ++i) scene.draw();
    auto reference = scene.pixels();
    for (auto path : {ShadowCaster::AUTOMATIC,ShadowCaster::CORE})
    {
        scene.shadows->setSubmission(path);
        scene.draw(); scene.draw();
        CHECK(scene.shadows->getRenderedCascades() == 3);
        equivalent(reference,scene.pixels());
    }
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Covers map reuse, explicit invalidation, geometry/placement changes, and callbacks that prohibit caching.
TEST_CASE("Shadow46 static cache invalidates safely", "[shadows][shadow46][.gl]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires a 4.6 NVGL context"); return; }
    Shadow46Tests::Scene scene(256,2,320,240);
    scene.buildings->setBirthday(-100.0);
    scene.shadows->setCacheEnabled(true);
    scene.draw(); scene.draw(); scene.draw();
    CHECK(scene.shadows->getReusedCascades() == 3);
    auto reference = scene.pixels();
    scene.shadows->invalidateCache();
    scene.draw();
    CHECK(scene.shadows->getRenderedCascades() == 3);
    equivalent(reference,scene.pixels());
    scene.draw();
    CHECK(scene.shadows->getReusedCascades() == 3);
    scene.cityView(10.0);
    scene.draw();
    CHECK(scene.shadows->getRenderedCascades() == 3);
    scene.draw();
    CHECK(scene.shadows->getReusedCascades() == 3);
    scene.models->setMatrix(osg::Matrixd::translate(1,0,0)*scene.models->getMatrix());
    scene.draw();
    CHECK(scene.shadows->getRenderedCascades() == 3);
    scene.models->setUpdateCallback(new Animated);
    scene.draw(); scene.draw();
    CHECK(scene.shadows->getReusedCascades() == 0);
    scene.models->setUpdateCallback(nullptr);
    scene.draw(); scene.draw();
    CHECK(scene.shadows->getReusedCascades() == 3);
    scene.shadows->setTextureSize(512);
    scene.draw();
    CHECK(scene.shadows->getRenderedCascades() == 3);
    scene.draw();
    scene.buildings->setAlphaCutoff(0.25f);
    scene.draw();
    CHECK(scene.shadows->getRenderedCascades() == 3);
    scene.draw();
    CHECK(scene.shadows->getReusedCascades() == 3);
    scene.models->setNodeMask(0);
    scene.draw();
    CHECK(scene.shadows->getRenderedCascades() == 3);
    scene.models->setNodeMask(~0u);
    scene.sky->setEphemeris(new Sky2Tests::Sun(55.0));
    scene.draw();
    CHECK(scene.shadows->getRenderedCascades() == 3);
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Shared drawable placements and two primary cameras must not overwrite one another's compact visibility lists.
TEST_CASE("Shadow46 isolates views and parent transforms", "[shadows][shadow46][.gl]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires a 4.6 NVGL context"); return; }
    Shadow46Tests::Scene scene(256,4,320,240);
    scene.buildings->setBirthday(-100.0);
    osg::ref_ptr<osg::MatrixTransform> placement = new osg::MatrixTransform;
    placement->setMatrix(osg::Matrixd::rotate(0.4,osg::Vec3d(0,0,1))*osg::Matrixd::translate(100,30,0));
    placement->addChild(scene.buildings);
    scene.models->addChild(placement);
    osg::ref_ptr<osg::Camera> other = new osg::Camera;
    other->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
    other->setRenderOrder(osg::Camera::PRE_RENDER);
    other->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
    other->setViewport(0,0,160,120);
    other->setViewMatrixAsLookAt(osg::Vec3d(6378250,90,90),osg::Vec3d(6378137,0,0),osg::Vec3d(1,0,0));
    other->setProjectionMatrixAsPerspective(70,4.0/3.0,0.1,1000);
    other->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    other->addCullCallback(new InstallCameraUniform);
    osg::ref_ptr<osg::Texture2D> color = new osg::Texture2D;
    color->setTextureSize(160,120);
    color->setInternalFormat(GL_RGBA8);
    other->attach(osg::Camera::COLOR_BUFFER,color);
    other->attach(osg::Camera::DEPTH_BUFFER,GL_DEPTH_COMPONENT24);
    other->addChild(scene.sky);
    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->addChild(scene.sky);
    root->addChild(other);
    scene.viewer->setSceneData(root);
    scene.shadows->setSubmission(ShadowCaster::LEGACY);
    scene.draw(); scene.draw();
    auto reference = scene.pixels();
    for (auto path : {ShadowCaster::CORE,ShadowCaster::AUTOMATIC})
    {
        scene.shadows->setSubmission(path);
        scene.draw(); scene.draw();
        equivalent(reference,scene.pixels());
    }
    other->releaseGLObjects(scene.context->getState());
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Protects the pointer-sized GL copy ABI used to move GPU draw counts without CPU readback.
TEST_CASE("Shadow46 buffer allocation and counter copies preserve byte offsets", "[shadows][shadow46][.gl]")
{
    if (!Capabilities::get().supportsGLSL(460u)) { WARN("Requires a 4.6 context"); return; }
    SkyNode2::Options options;
    options.preset = SkyNode2::FLAT;
    Sky2Tests::Scene scene(new SkyNode2(options),64,64);
    auto& state = *scene.context->getState();
    auto source = GLBuffer::create(GL_SHADER_STORAGE_BUFFER,state);
    auto destination = GLBuffer::create(GL_SHADER_STORAGE_BUFFER,state);
    std::vector<GLuint> values{3,7,11,13};
    source->bind();
    source->uploadData(values);
    destination->bind();
    destination->uploadData(GLsizei(values.size()*sizeof(GLuint)),nullptr);
    source->copyBufferSubData(destination,sizeof(GLuint),0,2*sizeof(GLuint));
    source->copyBufferSubData(destination,0,2*sizeof(GLuint),sizeof(GLuint));
    GLuint result[3]{};
    destination->getBufferSubData(0,sizeof(result),result);
    CHECK(result[0] == 7);
    CHECK(result[1] == 11);
    CHECK(result[2] == 3);
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Shared page buffers contain multiple mesh index/base-vertex ranges; compact core draws must preserve each range.
TEST_CASE("Shadow46 compacts geometry page meshes without mixing ranges", "[shadows][shadow46][.gl]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires a 4.6 NVGL context"); return; }
    Shadow46Tests::Scene scene(0,1,320,240);
    ChonkFactory factory(scene.textures);
    auto builder = factory.createGeometryPageBuilder();
    for (unsigned i=0; i<8; ++i)
    {
        float height = 6.0f+float(i);
        osg::ref_ptr<osg::ShapeDrawable> box = new osg::ShapeDrawable(
            new osg::Box(osg::Vec3(0,0,height*0.5f),2,2,height));
        box->setColor(osg::Vec4(0.7,0.7,0.7,1));
        osg::ref_ptr<osg::Geometry> mesh = new osg::Geometry(*box,osg::CopyOp::SHALLOW_COPY);
        REQUIRE(builder.addMesh(mesh) == i);
    }
    auto page = builder.finish();
    REQUIRE(page != nullptr);
    for (unsigned i=0; i<1024; ++i)
        REQUIRE(scene.buildings->add(page,i%8,osg::Matrixf::translate(
            (float(i%32)-16)*5,(float(i/32)-16)*5,0)));
    scene.buildings->setBirthday(-100.0);
    scene.shadows->setCascades(4,600);
    scene.shadows->setSubmission(ShadowCaster::LEGACY);
    scene.draw(); scene.draw();
    auto reference = scene.pixels();
    for (auto path : {ShadowCaster::CORE,ShadowCaster::AUTOMATIC})
    {
        scene.shadows->setSubmission(path);
        scene.draw(); scene.draw();
        equivalent(reference,scene.pixels());
    }
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Submission and GPU-culling changes invalidate cached maps while preserving the rendered image.
TEST_CASE("Shadow46 cache tracks submission and GPU culling changes", "[shadows][shadow46][.gl]")
{
    if (!Capabilities::get().supportsNVGL()) { WARN("Requires a 4.6 NVGL context"); return; }
    Shadow46Tests::Scene scene(256,2,320,240);
    scene.buildings->setBirthday(-100.0);
    scene.shadows->setCacheEnabled(true);
    std::vector<float> reference;
    for (auto path : {ShadowCaster::LEGACY,ShadowCaster::CORE,ShadowCaster::AUTOMATIC,ShadowCaster::CORE})
    {
        scene.shadows->setSubmission(path);
        scene.draw();
        CHECK(scene.shadows->getRenderedCascades() == 3);
        if (reference.empty()) reference = scene.pixels();
        else equivalent(reference,scene.pixels());
        scene.draw(); scene.draw();
        CHECK(scene.shadows->getReusedCascades() == 3);
        equivalent(reference,scene.pixels());
    }
    for (bool enabled : {false,true})
    {
        scene.buildings->setUseGPUCulling(enabled);
        scene.draw();
        CHECK(scene.shadows->getRenderedCascades() == 3);
        equivalent(reference,scene.pixels());
        scene.draw();
        CHECK(scene.shadows->getReusedCascades() == 3);
    }
    REQUIRE(glGetError() == GL_NO_ERROR);
}

// Exercises the portable path without Chonk or NV extensions, including draw ranges and ordinary instance IDs.
TEST_CASE("Shadow46 preserves ordinary instanced geometry", "[shadows][shadow46][.gl]")
{
    if (!Capabilities::get().supportsGLSL(460u)) { WARN("Requires a 4.6 context"); return; }
    Sky2Tests::Scene scene(new SkyNode2(Shadow46Tests::Scene::flatOptions()),320,240);
    scene.models->removeChildren(0,scene.models->getNumChildren());
    osg::ref_ptr<osg::ShapeDrawable> floor = new osg::ShapeDrawable(new osg::Box(osg::Vec3(0,0,-1),400,400,2));
    floor->setColor(osg::Vec4(0.7,0.7,0.7,1));
    floor->setUseDisplayList(false);
    floor->setUseVertexBufferObjects(true);
    scene.models->addChild(floor);
    osg::ref_ptr<osg::Geometry> roof = new osg::Geometry;
    roof->setName("Ordinary instanced roof");
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
    for (unsigned i=0; i<6; ++i) vertices->push_back(osg::Vec3(0,0,0)); // Nonzero DrawArrays first.
    for (auto p : {osg::Vec3(-6,-5,12),osg::Vec3(6,-5,12),osg::Vec3(6,5,12),
        osg::Vec3(-6,-5,12),osg::Vec3(6,5,12),osg::Vec3(-6,5,12)}) vertices->push_back(p);
    roof->setVertexArray(vertices);
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    normals->push_back(osg::Vec3(0,0,1));
    roof->setNormalArray(normals,osg::Array::BIND_OVERALL);
    roof->setUseDisplayList(false);
    roof->setUseVertexBufferObjects(true);
    roof->setCullingActive(false); // Shader displacements extend beyond the source bounds.
    auto vp = VirtualProgram::getOrCreate(roof->getOrCreateStateSet());
    ShaderLoader::load(vp,R"(
        #version 460
        #pragma vp_function shadowTestInstances, vertex_model, 0.5
        out vec2 shadowTestUV;
        // Must see 0,1,2 independently in every cascade, with the same placement as the main camera.
        void shadowTestInstances(inout vec4 vertex)
        {
            shadowTestUV = vertex.xy;
            vertex.x += float(gl_InstanceID-1)*25.0;
        }
        [break]
        #pragma vp_function shadowTestCutout, fragment_coloring, 0.5
        in vec2 shadowTestUV;
        // Keep application fragment processing active in the depth pass as well as the visible pass.
        void shadowTestCutout(inout vec4 color)
        {
            if (abs(shadowTestUV.x)<1.5 && abs(shadowTestUV.y)<3.0) discard;
            color = vec4(0.7,0.7,0.7,1);
        }
    )");
    scene.models->addChild(roof);
    osg::ref_ptr<ShadowCaster> shadows = new ShadowCaster;
    shadows->setTextureSize(1024);
    shadows->setLight(scene.sky->getSunLight());
    shadows->getShadowCastingGroup()->addChild(scene.models);
    scene.sky->removeChild(scene.models);
    shadows->addChild(scene.models);
    scene.sky->addChild(shadows);
    scene.viewer->getCamera()->setViewMatrixAsLookAt(osg::Vec3d(6378217,-80,-90),
        osg::Vec3d(6378137,0,0),osg::Vec3d(1,0,0));
    std::array<std::vector<float>,4> reference;
    for (unsigned kind=0; kind<4; ++kind)
    {
        osg::ref_ptr<osg::PrimitiveSet> primitive;
        osg::ref_ptr<osg::DrawElementsUByte> prefix = new osg::DrawElementsUByte(GL_TRIANGLES);
        prefix->resize(128,0); // Forces a nonzero, aligned offset inside a shared EBO.
        osg::ref_ptr<osg::ElementBufferObject> ebo = new osg::ElementBufferObject;
        prefix->setElementBufferObject(ebo);
        if (kind == 0) primitive = new osg::DrawArrays(GL_TRIANGLES,6,6,3);
        else
        {
            osg::ref_ptr<osg::DrawElements> elements = kind == 1 ?
                static_cast<osg::DrawElements*>(new osg::DrawElementsUByte(GL_TRIANGLES)) : kind == 2 ?
                static_cast<osg::DrawElements*>(new osg::DrawElementsUShort(GL_TRIANGLES)) :
                static_cast<osg::DrawElements*>(new osg::DrawElementsUInt(GL_TRIANGLES));
            for (unsigned i=6; i<12; ++i) elements->addElement(i);
            elements->setNumInstances(3);
            elements->setElementBufferObject(ebo);
            primitive = elements;
        }
        roof->setPrimitiveSetList(osg::Geometry::PrimitiveSetList{primitive});
        for (unsigned count : {1u,2u,3u,4u})
        {
            INFO("primitive " << kind << ", cascades " << count);
            shadows->setCascades(count,400);
            scene.draw(); scene.draw();
            if (kind == 0) reference[count-1] = scene.pixels();
            if (kind != 0)
            {
                auto buffer = primitive->getDrawElements()->getGLBufferObject(scene.context->getState()->getContextID());
                REQUIRE(buffer != nullptr);
                CHECK(buffer->getOffset(primitive->getBufferIndex()) != 0);
            }
            if (kind == 0 && count == 4)
            {
                shadows->setEnabled(false);
                scene.draw();
                auto lit = scene.pixels();
                double difference = 0.0;
                for (unsigned i=0; i<lit.size(); ++i) difference += std::abs(lit[i]-reference[count-1][i]);
                CHECK(difference/lit.size() > 0.0001); // Ensure equivalence is comparing visible, nonempty shadows.
                shadows->setEnabled(true);
            }
            else if (kind != 0) equivalent(reference[count-1],scene.pixels());
            CHECK(primitive->getNumInstances() == 3);
            CHECK(roof->getPrimitiveSet(0) == primitive.get());
        }
    }
    shadows->setCacheEnabled(true);
    scene.draw(); scene.draw();
    CHECK(shadows->getReusedCascades() == 4);
    roof->getPrimitiveSet(0)->setNumInstances(2);
    scene.draw();
    CHECK(shadows->getRenderedCascades() == 4);
    for (auto& p : *vertices) p.z() += 2.0f;
    vertices->dirty();
    scene.draw();
    CHECK(shadows->getRenderedCascades() == 4);
    scene.draw();
    CHECK(shadows->getReusedCascades() == 4);
    auto edited = scene.pixels();
    shadows->setCacheEnabled(false);
    scene.draw();
    equivalent(edited,scene.pixels());
    REQUIRE(glGetError() == GL_NO_ERROR);
}
