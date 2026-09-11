/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/Capabilities>
#include <osgEarth/MapNode>
#include <osgEarth/Sky>
#include <osgEarth/VirtualProgram>
#include <osg/GraphicsContext>
#include <osg/Texture3D>
#include <osgUtil/UpdateVisitor>
#include <thread>

using namespace osgEarth;

namespace
{
    struct SkyProbe
    {
        osg::ref_ptr<osg::GraphicsContext> context;
        osg::ref_ptr<osg::View> view = new osg::View;
        osg::ref_ptr<SkyNode> sky;
        osg::ref_ptr<osg::FrameStamp> frame = new osg::FrameStamp;
        osg::ref_ptr<osg::Texture3D> texture;
        osg::Vec3d sun;
        int unit = 0;

        SkyProbe()
        {
            Capabilities::get();
            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->readDISPLAY();
            traits->setUndefinedScreenDetailsToDefaultScreen();
            traits->width = traits->height = 1;
            traits->pbuffer = true;
            traits->doubleBuffer = false;
            context = osg::GraphicsContext::createGraphicsContext(traits);
            REQUIRE(context.valid());
            REQUIRE(context->realize());
            REQUIRE(context->makeCurrent());

            osg::ref_ptr<MapNode> map = new MapNode;
            REQUIRE(map->open());
            sky = SkyNode::create("simple");
            REQUIRE(sky.valid());
            sky->addChild(map);
            sky->attach(view,0);
            const auto& position = view->getLight()->getPosition();
            sun.set(position.x(),position.y(),position.z());
            sun.normalize();
            update(sun);
            auto ss = sky->getStateSet();
            REQUIRE(ss->getUniform("oe_sky_environmentTex") != nullptr);
            REQUIRE(ss->getUniform("oe_sky_environmentTex")->get(unit));
            texture = dynamic_cast<osg::Texture3D*>(ss->getTextureAttribute(unit,osg::StateAttribute::TEXTURE));
            REQUIRE(texture.valid());
            REQUIRE(texture->getImage() != nullptr);
            REQUIRE(ss->getDefinePair("OE_SKY_ENVIRONMENT") != nullptr);
        }

        void update(const osg::Vec3d& up)
        {
            view->getCamera()->setViewMatrix(osg::Matrixd::translate(up*(-7000000.0)));
            frame->setFrameNumber(frame->getFrameNumber()+1);
            osgUtil::UpdateVisitor visitor;
            visitor.setFrameStamp(frame);
            sky->accept(visitor);
        }

        osg::State& state() { return *context->getState(); }

        void upload()
        {
            state().setActiveTextureUnit(unit);
            texture->apply(state());
            REQUIRE(glGetError() == GL_NO_ERROR);
        }

        ~SkyProbe()
        {
            state().setLastAppliedProgramObject(nullptr);
            sky->releaseGLObjects(&state());
            context->releaseContext();
        }
    };

    osg::Vec3 pixel(const osg::Image* image, unsigned x, unsigned y, unsigned z)
    {
        auto p = reinterpret_cast<const float*>(image->data(x,y,z));
        return osg::Vec3(p[0],p[1],p[2]);
    }
}

TEST_CASE("SimpleSky volume uploads and refreshes without GL errors", "[sky][gl]")
{
    SkyProbe probe;
    auto image = probe.texture->getImage();
    REQUIRE(image->r() == 6);
    REQUIRE_FALSE(image->isMipmap());
    REQUIRE(probe.texture->getWrap(osg::Texture::WRAP_S) == osg::Texture::REPEAT);
    REQUIRE(probe.texture->getFilter(osg::Texture::MIN_FILTER) == osg::Texture::LINEAR);
    probe.upload();
    auto object = probe.texture->getTextureObject(probe.state().getContextID());
    REQUIRE(object->isAllocated());
    std::vector<float> gpu(image->s()*image->t()*image->r()*3);
    glGetTexImage(GL_TEXTURE_3D,0,GL_RGB,GL_FLOAT,gpu.data());
    REQUIRE(glGetError() == GL_NO_ERROR);
    REQUIRE(*std::max_element(gpu.begin(),gpu.end()) > 0.1f);

    for (int pass=0; pass<3; ++pass)
    {
        // SkyEnvironment throttles regeneration using wall-clock time.
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        probe.update(pass%2 == 0 ? -probe.sun : probe.sun);
        probe.upload();
        REQUIRE(probe.texture->getTextureObject(probe.state().getContextID())->isAllocated());
        glGetTexImage(GL_TEXTURE_3D,0,GL_RGB,GL_FLOAT,gpu.data());
        REQUIRE(glGetError() == GL_NO_ERROR);
        if (pass%2 == 0)
            REQUIRE(*std::max_element(gpu.begin(),gpu.end()) == 0.0f);
        else
            REQUIRE(*std::max_element(gpu.begin(),gpu.end()) > 0.1f);
    }
}

TEST_CASE("SimpleSky volume shader interpolates roughness and closes longitude and pole seams", "[sky][gl]")
{
    SkyProbe probe;
    auto image = probe.texture->getImage();
    for (int z=0; z<image->r(); ++z)
    for (int x=1; x<image->s(); ++x)
    {
        REQUIRE(pixel(image,x,0,z) == pixel(image,0,0,z));
        REQUIRE(pixel(image,x,image->t()-1,z) == pixel(image,0,image->t()-1,z));
    }
    probe.upload();

    auto shader = VirtualProgram::get(probe.sky->getStateSet())->getPolyShader("oe_sky_environment_init");
    REQUIRE(shader != nullptr);
    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(new osg::Shader(osg::Shader::VERTEX,
        "#version 330\nvoid main() { vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2); gl_Position=vec4(p*2.0-1.0,0,1); }"));
    program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
        shader->getShaderSource() +
        "\nuniform vec3 probeDirection; uniform float probeRoughness; out vec4 result;"
        "\nvoid main() { result=vec4(0.25*oe_sky_environmentRadiance(probeDirection,probeRoughness),1); }"));
    program->apply(probe.state());
    auto pcp = probe.state().getLastAppliedProgramObject();
    REQUIRE(pcp != nullptr);
    REQUIRE(pcp->isLinked());
    auto gl = probe.state().get<osg::GLExtensions>();
    GLuint vao;
    gl->glGenVertexArrays(1,&vao);
    gl->glBindVertexArray(vao);
    glViewport(0,0,1,1);
    glDrawBuffer(GL_FRONT);
    glReadBuffer(GL_FRONT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    gl->glUniform1i(gl->glGetUniformLocation(pcp->getHandle(),"oe_sky_environmentTex"),probe.unit);
    auto sample = [&](const osg::Vec3& direction, float roughness)
    {
        gl->glUniform3f(gl->glGetUniformLocation(pcp->getHandle(),"probeDirection"),direction.x(),direction.y(),direction.z());
        gl->glUniform1f(gl->glGetUniformLocation(pcp->getHandle(),"probeRoughness"),roughness);
        glDrawArrays(GL_TRIANGLES,0,3);
        float value[4];
        glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,value);
        REQUIRE(glGetError() == GL_NO_ERROR);
        return osg::Vec3(value[0],value[1],value[2])*4.0f;
    };
    auto near = [](const osg::Vec3& a, const osg::Vec3& b)
    {
        for (unsigned c=0; c<3; ++c) REQUIRE(std::abs(a[c]-b[c]) < 0.018f);
    };
    constexpr float pi = 3.14159265359f;
    // Directions at texel centers give an independent check of axis mapping.
    for (int x : {0,17,47,63})
    {
        float longitude=2*pi*(x+0.5f)/image->s();
        osg::Vec3 direction(std::cos(longitude),std::sin(longitude),0);
        for (int z=0; z<image->r(); ++z)
            near(sample(direction,float(z)/(image->r()-1)),pixel(image,x,image->t()/2,z));
        near(sample(direction,0.5f),
            (pixel(image,x,image->t()/2,2)+pixel(image,x,image->t()/2,3))*0.5f);
    }
    for (float roughness : {0.0f,0.3f,1.0f})
    {
        near(sample(osg::Vec3(1,1e-6f,0),roughness),sample(osg::Vec3(1,-1e-6f,0),roughness));
        near(sample(osg::Vec3(0,0,1),roughness),sample(osg::Vec3(1e-6f,0,1),roughness));
        near(sample(osg::Vec3(0,0,-1),roughness),sample(osg::Vec3(0,-1e-6f,-1),roughness));
    }
    near(sample(osg::Vec3(0,0,1),0),pixel(image,0,0,0));
    near(sample(osg::Vec3(0,0,-1),1),pixel(image,0,image->t()-1,image->r()-1));
    gl->glBindVertexArray(0);
    gl->glDeleteVertexArrays(1,&vao);
    program->releaseGLObjects(&probe.state());
}
