/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/Capabilities>
#include <osgEarthImGui/ImGuiEventHandler>
#include <osgEarthImGui/imgui_impl_opengl3.h>
#include <osg/ArgumentParser>
#include <osg/GraphicsContext>
#include <osg/FrameStamp>

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
#ifndef GL_SAMPLES
#define GL_SAMPLES 0x80A9
#endif

namespace
{
    struct TestGui : osgEarth::ImGuiEventHandler
    {
        void draw(osg::RenderInfo&) override
        {
            CHECK(glIsEnabled(GL_MULTISAMPLE) == GL_FALSE);
            ImGui::Begin("Multisampling test");
            ImGui::TextUnformatted("Panel rendering keeps multisampling disabled.");
            ImGui::End();
        }
    };

    struct GuiContext
    {
        osg::ref_ptr<osg::GraphicsContext> gc;
        osg::ref_ptr<osg::View> view = new osg::View;
        osg::ref_ptr<TestGui> gui = new TestGui;

        GuiContext(osg::DisplaySettings* settings)
        {
            osgEarth::Capabilities::get();
            osg::ref_ptr<osg::GraphicsContext::Traits> traits =
                new osg::GraphicsContext::Traits(settings);
            traits->readDISPLAY();
            traits->setUndefinedScreenDetailsToDefaultScreen();
            traits->width = traits->height = 256;
            traits->pbuffer = true;
            traits->doubleBuffer = false;
            gc = osg::GraphicsContext::createGraphicsContext(traits);
            REQUIRE(gc.valid());
            REQUIRE(gc->realize());
            REQUIRE(gc->makeCurrent());

            view->setFrameStamp(new osg::FrameStamp);
            view->getCamera()->setGraphicsContext(gc);
            view->getCamera()->setViewport(0, 0, 256, 256);
            gui->setAutoAdjustProjectionMatrix(false);
            gui->onStartup = []() { ImGui::GetIO().IniFilename = nullptr; };
        }

        ~GuiContext()
        {
            if (ImGui::GetCurrentContext())
            {
                ImGui_ImplOpenGL3_Shutdown();
                ImNodes::DestroyContext();
                ImGui::DestroyContext();
            }
            gc->releaseContext();
        }
    };
}

TEST_CASE("ImGui preserves scene multisampling across frames", "[imgui][.gl]")
{
    const unsigned sampleCount = GENERATE(Catch::values(0u, 4u));
    const bool enabled = GENERATE(Catch::values(false, true));
    CAPTURE(sampleCount);
    CAPTURE(enabled);

    // Follow the --samples parsing and context-traits path used by the app.
    char name[] = "osgearth_imgui";
    char option[] = "--samples";
    char value[] = "0";
    value[0] = static_cast<char>('0' + sampleCount);
    char* argv[] = { name, option, value, nullptr };
    int argc = 3;
    osg::ArgumentParser args(&argc, argv);
    osg::ref_ptr<osg::DisplaySettings> settings = new osg::DisplaySettings;
    settings->readCommandLine(args);
    REQUIRE(settings->getNumMultiSamples() == sampleCount);
    REQUIRE(argc == 1);

    GuiContext context(settings);
    GLint actualSamples = 0;
    glGetIntegerv(GL_SAMPLES, &actualSamples);
    CAPTURE(actualSamples);
    REQUIRE(actualSamples >= static_cast<GLint>(sampleCount));

    auto state = context.gc->getState();
    state->haveAppliedMode(GL_MULTISAMPLE, glIsEnabled(GL_MULTISAMPLE));
    state->applyMode(GL_MULTISAMPLE, enabled);
    REQUIRE((glIsEnabled(GL_MULTISAMPLE) == GL_TRUE) == enabled);

    osg::RenderInfo ri(state, context.view);
    ri.pushCamera(context.view->getCamera());
    for (unsigned frame = 0; frame < 3; ++frame)
    {
        CAPTURE(frame);
        context.view->getFrameStamp()->setSimulationTime((frame + 1) / 60.0);
        context.gui->newFrame(ri);
        context.gui->render(ri);
        CHECK((glIsEnabled(GL_MULTISAMPLE) == GL_TRUE) == enabled);
        CHECK(state->getLastAppliedMode(GL_MULTISAMPLE) == enabled);

        // OSG may skip this GL call when its cached state already matches.
        state->applyMode(GL_MULTISAMPLE, enabled);
        CHECK((glIsEnabled(GL_MULTISAMPLE) == GL_TRUE) == enabled);
    }
}
