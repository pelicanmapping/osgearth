/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/ChonkRenderPass>
#include <osgEarth/ShaderLoader>
#include "ChonkTestUtils.h"
#include <osg/Texture2DArray>
#include <osg/FrameBufferObject>
#include <osg/UserDataContainer>
#include <set>
#include <tuple>
#include <iostream>

using namespace osgEarth;
namespace
{
    using Pair = std::tuple<unsigned,unsigned,unsigned>; // source, LOD, view

    //! Exposes completed GPU results only to tests; production submission never reads counters back.
    struct InspectableViews : ChonkDrawable
    {
        struct Snapshot
        {
            std::set<Pair> pairs;
            unsigned commands = 0, instances = 0;
            std::uint64_t serial = 0;
        };

        //! Reads the selected view group's commands and verifies every visible record has a distinct bounded slot.
        Snapshot snapshot(osg::State& state, const ChonkRenderPass::Batch& packet) const
        {
            auto& objects = GLObjects::get(_globjects,state);
            Snapshot result;
            for (const auto& batch : objects._viewBatches)
            {
                if (batch.viewID != packet.parameters.viewID) continue;
                result.serial = batch.batchSerial;
                auto ext = state.get<osg::GLExtensions>();
                ext->glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                GLint previous = 0;
                glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING,&previous);
                unsigned lists = packet.parameters.output == ChonkRenderPass::PER_VIEW ? packet.parameters.count : 1;
                Chonk::DrawCommands commands(lists*objects._commands.size());
                std::vector<VisibleInstance> visible(batch.visible->size()/sizeof(VisibleInstance));
                std::vector<GLuint> counts(ChonkRenderPass::MAX_VIEWS*(1+objects._drawGroups.size()));
                batch.commands->bind();
                batch.commands->getBufferSubData(0,commands.size()*sizeof(Chonk::DrawCommand),commands.data());
                batch.visible->bind();
                batch.visible->getBufferSubData(0,visible.size()*sizeof(VisibleInstance),visible.data());
                batch.counts->bind();
                batch.counts->getBufferSubData(0,counts.size()*sizeof(GLuint),counts.data());
                ext->glBindBuffer(GL_SHADER_STORAGE_BUFFER,previous);
                for (unsigned view=0; view<lists; ++view) result.commands += counts[view];
                std::set<unsigned> occupied;
                for (const auto& command : commands)
                for (unsigned i=0; i<command.cmd.instanceCount; ++i)
                {
                    unsigned slot = command.cmd.baseInstance+i;
                    REQUIRE(slot < visible.size());
                    REQUIRE(occupied.insert(slot).second);
                    const auto& record = visible[slot];
                    REQUIRE(record.sourceIndex < objects._all_instances.size());
                    CHECK(std::isfinite(record.fade));
                    result.pairs.emplace(record.sourceIndex,record.lod & 0xffffu,record.lod >> 16u);
                    ++result.instances;
                }
                CHECK(result.pairs.size() == result.instances);
                return result;
            }
            FAIL("Expected multi-view GPU storage");
            return result;
        }
    };

    //! Application-owned six-view renderer: uses no ShadowCaster, shadow fitting, or shadow shader functions.
    struct ExternalViews : ChonkTest::Renderer
    {
        osg::ref_ptr<InspectableViews> drawable = new InspectableViews;
        osg::ref_ptr<osg::Texture2DArray> color = new osg::Texture2DArray;
        osg::ref_ptr<osg::Texture2DArray> depth = new osg::Texture2DArray;
        osg::ref_ptr<osg::Camera> renderCamera = new osg::Camera;
        ChonkRenderPass::Parameters parameters;
        osg::ref_ptr<const ChonkRenderPass::Batch> batch;
        mutable unsigned errors = 0;
        using DebugCallback = void(GL_APIENTRY *)(GLenum,GLenum,GLuint,GLenum,GLsizei,const GLchar*,const void*);
        void(GL_APIENTRY * debugCallback)(DebugCallback,const void*) = nullptr;

        //! Makes driver errors actionable instead of letting OSG consume them before the final error assertion.
        static void GL_APIENTRY debug(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* text, const void* data)
        {
            if (type == GL_DEBUG_TYPE_ERROR)
            {
                std::cerr << "Multi-view GL error: " << text << '\n';
                ++static_cast<const ExternalViews*>(data)->errors;
            }
        }

        //! Creates mixed perspective/orthographic target volumes and two independently indexed meshes.
        bool setup()
        {
            if (!initialize()) return false;
            osg::setGLExtensionFuncPtr(debugCallback,"glDebugMessageCallback");
            if (debugCallback)
            {
                debugCallback(debug,this);
                glEnable(GL_DEBUG_OUTPUT);
                glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            }
            drawable->setBirthday(-100);
            drawable->setUseGPUCulling(true);
            drawable->setCullingActive(false);
            for (unsigned mesh=0; mesh<2; ++mesh)
            {
                auto geometry = ChonkTest::mesh(mesh+1);
                auto chonk = factory->getOrCreateChonk(geometry);
                for (unsigned instance=0; instance<3; ++instance)
                    drawable->add(chonk,osg::Matrixf::translate(float(instance)*12-12,float(mesh)*8-4,0));
            }
            renderCamera->addChild(drawable);
            setScene(renderCamera);
            for (auto texture : {color.get(),depth.get()})
            {
                texture->setTextureSize(256,256,6);
                texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST);
                texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::NEAREST);
            }
            color->setInternalFormat(GL_RGBA8);
            color->setSourceFormat(GL_RGBA);
            color->setSourceType(GL_UNSIGNED_BYTE);
            depth->setInternalFormat(GL_DEPTH_COMPONENT24);
            depth->setSourceFormat(GL_DEPTH_COMPONENT);
            depth->setSourceType(GL_UNSIGNED_INT);
            auto camera = renderCamera.get();
            camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
            camera->setViewMatrix(viewer.getCamera()->getViewMatrix());
            camera->setProjectionMatrix(viewer.getCamera()->getProjectionMatrix());
            camera->setViewport(0,0,256,256);
            camera->setRenderOrder(osg::Camera::PRE_RENDER);
            camera->setClearColor(osg::Vec4(0,0,0,1));
            camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            camera->addCullCallback(new InstallCameraUniform);
            camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
            camera->setImplicitBufferAttachmentMask(0,0);
            camera->setDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
            camera->setReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
            parameters.viewID = ChonkRenderPass::createViewID();
            parameters.count = 6;
            parameters.activeViews = 0x3f;
            parameters.frameNumber = 7; // Intentionally fixed: new batches within a frame must invalidate prior results.
            parameters.lodView = camera->getViewMatrix();
            parameters.lodProjection = camera->getProjectionMatrix();
            parameters.lodViewport.set(256,256);
            auto matrices = new osg::Uniform(osg::Uniform::FLOAT_MAT4,"test_view_clip",8);
            osg::Matrixd undo = osg::Matrixd::inverse(camera->getViewMatrix()*camera->getProjectionMatrix());
            for (unsigned i=0; i<6; ++i)
            {
                double x = i == 5 ? 200.0 : 8.0*(double(i)-2.0);
                osg::Matrixd view = osg::Matrixd::lookAt(osg::Vec3d(x,0,100),osg::Vec3d(x,0,0),osg::Vec3d(0,1,0));
                osg::Matrixd projection = (i%2) ? osg::Matrixd::perspective(20,1,1,200) :
                    osg::Matrixd::ortho(-18,18,-18,18,1,200);
                parameters.clipFromWorld[i] = view*projection;
                matrices->setElement(i,undo*parameters.clipFromWorld[i]);
            }
            auto state = root->getOrCreateStateSet();
            state->addUniform(matrices);
            state->addUniform(new osg::Uniform("test_merged",false));
            ShaderLoader::load(VirtualProgram::getOrCreate(state),R"(
                #version 460
                #extension GL_ARB_shader_viewport_layer_array : require
                #pragma vp_function external_project, vertex_clip, 1.0
                uniform mat4 test_view_clip[8];
                uniform bool test_merged;
                int oe_chonk_view_index;
                // The application decides how a visible view index maps to a projection and framebuffer layer.
                void external_project(inout vec4 vertex)
                {
                    vertex = test_view_clip[oe_chonk_view_index]*vertex;
                    gl_Layer = test_merged ? oe_chonk_view_index : 0;
                }
            )");
            return true;
        }

        //! Publishes a fresh immutable submission, sharing its GPU classification across separate passes.
        void begin(ChonkRenderPass::Output output, bool core)
        {
            parameters.output = output;
            parameters.coreDraws = core;
            batch = ChonkRenderPass::createBatch(parameters);
            REQUIRE(batch);
            root->getOrCreateStateSet()->getUniform("test_merged")->set(output == ChonkRenderPass::MERGED);
        }

        //! Selects the target attachment; zero is the only submission index for a merged batch.
        void draw(unsigned view = 0)
        {
            auto camera = renderCamera.get();
            auto layer = parameters.output == ChonkRenderPass::MERGED ?
                osg::Camera::FACE_CONTROLLED_BY_GEOMETRY_SHADER : view;
            camera->attach(osg::Camera::COLOR_BUFFER,color,0,layer);
            camera->attach(osg::Camera::DEPTH_BUFFER,depth,0,layer);
            camera->dirtyAttachmentMap();
            osg::ref_ptr<ChonkRenderPass> pass = new ChonkRenderPass(batch,view,camera->getViewMatrix());
            ChonkRenderPass::set(camera->getOrCreateStateSet(),pass);
            frame();
            REQUIRE(errors == 0);
        }

        //! Copies every target layer after GPU completion, preserving OSG's texture state cache.
        std::vector<unsigned char> pixels()
        {
            GLint previous = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY,&previous);
            glBindTexture(GL_TEXTURE_2D_ARRAY,color->getTextureObject(context->getState()->getContextID())->id());
            std::vector<unsigned char> result(256*256*6*4);
            glGetTexImage(GL_TEXTURE_2D_ARRAY,0,GL_RGBA,GL_UNSIGNED_BYTE,result.data());
            glBindTexture(GL_TEXTURE_2D_ARRAY,previous);
            return result;
        }

        //! Releases shared render-bin programs while their owning context is still current.
        ~ExternalViews()
        {
            if (!context) return;
            viewer.stopThreading();
            context->makeCurrent();
            viewer.getCamera()->releaseGLObjects(context->getState());
            root->releaseGLObjects(context->getState());
            // Chonk's specialized release method handles its buffers; release its shared shader StateSet as well.
            drawable->getStateSet()->releaseGLObjects(context->getState());
            ChonkRenderBin::releaseSharedGLObjects(context->getState());
            if (debugCallback) debugCallback(nullptr,nullptr);
            context->releaseContext();
        }
    };
}

// Checks public contract validation, immutable snapshots, inheritance and preservation of application user data.
TEST_CASE("Chonk multi-view packets preserve application state", "[chonk][chonk-multiview]")
{
    ChonkRenderPass::Parameters p;
    CHECK_FALSE(ChonkRenderPass::createBatch(p));
    p.viewID = ChonkRenderPass::createViewID();
    CHECK(p.viewID != ChonkRenderPass::createViewID());
    p.count = 6;
    p.activeViews = 63;
    auto batch = ChonkRenderPass::createBatch(p);
    REQUIRE(batch);
    p.activeViews = 0;
    CHECK(batch->parameters.activeViews == 63);
    auto second = ChonkRenderPass::createBatch(p);
    REQUIRE(second);
    CHECK(second->serial != batch->serial);
    p.activeViews = 64;
    CHECK_FALSE(ChonkRenderPass::createBatch(p));
    p.activeViews = 0;
    p.count = 9;
    CHECK_FALSE(ChonkRenderPass::createBatch(p));
    p.count = 6;
    p.lodViewport.x() = 0;
    CHECK_FALSE(ChonkRenderPass::createBatch(p));
    osg::ref_ptr<ChonkRenderPass> pass = new ChonkRenderPass(batch,3,osg::Matrixd::identity());
    REQUIRE(pass->getBatch());
    osg::ref_ptr<ChonkRenderPass> invalid = new ChonkRenderPass(batch,6,osg::Matrixd::identity());
    CHECK_FALSE(invalid->getBatch());
    invalid = new ChonkRenderPass(batch,0,osg::Matrixd::scale(0,0,0));
    CHECK_FALSE(invalid->getBatch());
    osg::ref_ptr<osg::StateSet> parent = new osg::StateSet, child = new osg::StateSet;
    osg::ref_ptr<osg::Object> marker = new osg::StateSet;
    parent->setUserData(marker);
    osg::ref_ptr<osg::StateSet> copy = new osg::StateSet(*parent,osg::CopyOp::SHALLOW_COPY);
    ChonkRenderPass::set(parent,pass);
    CHECK(parent->getUserData() == marker);
    CHECK(copy->getUserData() == marker);
    CHECK(copy->getUserDataContainer()->getUserObject("osgEarth.ChonkRenderPass") == nullptr);
    osg::ref_ptr<osg::State> state = new osg::State;
    state->pushStateSet(parent);
    REQUIRE(ChonkRenderPass::find(*state));
    CHECK(ChonkRenderPass::find(*state)->getIndex() == 3);
    state->pushStateSet(child);
    REQUIRE(ChonkRenderPass::find(*state));
    ChonkRenderPass::set(child,nullptr);
    CHECK_FALSE(ChonkRenderPass::find(*state));
    state->popStateSet();
    CHECK(ChonkRenderPass::find(*state)->getBatch() == batch);
    state->popStateSet();
}

// Verifies actual command reduction and exact pixels using only the public renderer contract, including >4 views.
TEST_CASE("Chonk merges mesh commands for independent multi-view renderers", "[chonk][chonk-multiview][.gl]")
{
    if (!Capabilities::get().supportsNVGL() || !Capabilities::get().supportsShaderViewportLayerArray())
    { WARN("Requires NVGL and ARB_shader_viewport_layer_array"); return; }
    ExternalViews scene;
    REQUIRE(scene.setup());
    auto& state = *scene.context->getState();
    scene.begin(ChonkRenderPass::PER_VIEW,true);
    for (unsigned i=0; i<6; ++i) scene.draw(i);
    auto reference = scene.pixels();
    auto separate = scene.drawable->snapshot(state,*scene.batch);
    CHECK(separate.commands == 10); // Two mesh/LOD commands in five occupied views; sixth is empty.
    CHECK(separate.instances > 6);
    for (bool core : {true,false})
    {
        scene.begin(ChonkRenderPass::MERGED,core);
        scene.draw();
        auto merged = scene.drawable->snapshot(state,*scene.batch);
        CHECK(merged.serial == scene.batch->serial);
        CHECK(merged.commands == 2);
        CHECK(merged.pairs == separate.pairs);
        auto actual = scene.pixels();
        for (unsigned view=0; view<6; ++view)
        {
            unsigned changed = 0, actualCoverage = 0, referenceCoverage = 0;
            for (unsigned pixel=view*256*256*4; pixel<(view+1)*256*256*4; pixel+=4)
            {
                changed += actual[pixel] != reference[pixel];
                actualCoverage += actual[pixel] != 0;
                referenceCoverage += reference[pixel] != 0;
            }
            INFO("core=" << core << ", view=" << view << ", actual coverage=" << actualCoverage <<
                ", reference coverage=" << referenceCoverage);
            CHECK((referenceCoverage > 0) == (view != 5));
            CHECK(changed == 0);
        }
    }
    // A second batch in the same frame must reclassify, including zero-active-view submissions.
    for (unsigned mask : {0x15u,0u})
    {
        scene.parameters.activeViews = mask;
        scene.begin(ChonkRenderPass::MERGED,true);
        scene.draw();
        auto merged = scene.drawable->snapshot(state,*scene.batch);
        std::set<Pair> expected;
        for (const auto& pair : separate.pairs) if (mask & (1u<<std::get<2>(pair))) expected.insert(pair);
        CHECK(merged.pairs == expected);
        CHECK(merged.commands == (mask == 0 ? 0 : 2));
    }
    scene.parameters.activeViews = 0x3f;
    scene.begin(ChonkRenderPass::UNION,true);
    scene.draw();
    auto united = scene.drawable->snapshot(state,*scene.batch);
    CHECK(united.commands == 2);
    CHECK(united.instances == 6);
    for (const auto& pair : united.pairs) CHECK(std::get<2>(pair) == 0);
    // Application-owned ordinary shadow/depth cameras retain the original defines and primary-view uniform contract.
    scene.begin(ChonkRenderPass::PER_VIEW,true);
    scene.draw(0);
    auto independent = scene.pixels();
    auto cameraState = scene.renderCamera->getOrCreateStateSet();
    ChonkRenderPass::set(cameraState,nullptr);
    cameraState->setDefine("OE_IS_SHADOW_CAMERA");
    cameraState->setDefine("OE_IS_DEPTH_CAMERA");
    cameraState->addUniform(new osg::Uniform("oe_shadowToPrimaryMatrix",osg::Matrixf::identity()));
    cameraState->addUniform(new osg::Uniform("oe_primaryProjectionMatrix",osg::Matrixf(scene.parameters.lodProjection)));
    cameraState->addUniform(new osg::Uniform("oe_primaryViewport",scene.parameters.lodViewport));
    scene.frame();
    auto ordinary = scene.pixels();
    CHECK(std::equal(ordinary.begin(),ordinary.begin()+256*256*4,independent.begin()));
    CHECK(scene.errors == 0);
    REQUIRE(glGetError() == GL_NO_ERROR);
}
