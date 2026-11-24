/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/DepthPyramid>
#include <osgEarth/GLUtils>
#include <osgEarth/Notify>
#include <osgEarth/ShaderLoader>
#include <osgEarth/Shaders>

#include <osg/FrameBufferObject>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#define LC "[DepthPyramid] "

using namespace osgEarth;
using namespace osgEarth::Util;

// GL enums used by the occlusion pyramid that OSG's headers may not define.
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE 0x8211
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE
#define GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE 0x8216
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE
#define GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE 0x8217
#endif
#ifndef GL_DEPTH32F_STENCIL8
#define GL_DEPTH32F_STENCIL8 0x8CAD
#endif
#ifndef GL_READ_WRITE
#define GL_READ_WRITE 0x88BA
#endif

namespace
{
    //! GL 4.5 direct-state-access and bindless entry points for the occlusion pyramid. None of them
    //! binds a texture unit, image unit or framebuffer, so OSG's state tracking stays valid.
    //! Load with the owning context current.
    struct HiZFunctions
    {
        void (GL_APIENTRY* createTextures)(GLenum, GLsizei, GLuint*) = nullptr;
        void (GL_APIENTRY* textureStorage2D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei) = nullptr;
        void (GL_APIENTRY* textureParameteri)(GLuint, GLenum, GLint) = nullptr;
        void (GL_APIENTRY* createFramebuffers)(GLsizei, GLuint*) = nullptr;
        void (GL_APIENTRY* namedFramebufferTexture)(GLuint, GLenum, GLuint, GLint) = nullptr;
        void (GL_APIENTRY* namedFramebufferDrawBuffer)(GLuint, GLenum) = nullptr;
        void (GL_APIENTRY* namedFramebufferReadBuffer)(GLuint, GLenum) = nullptr;
        GLenum (GL_APIENTRY* checkNamedFramebufferStatus)(GLuint, GLenum) = nullptr;
        void (GL_APIENTRY* getNamedFramebufferAttachmentParameteriv)(GLuint, GLenum, GLenum, GLint*) = nullptr;
        void (GL_APIENTRY* blitNamedFramebuffer)(GLuint, GLuint, GLint, GLint, GLint, GLint,
            GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
        GLuint64 (GL_APIENTRY* getTextureHandle)(GLuint) = nullptr;
        void (GL_APIENTRY* makeTextureHandleResident)(GLuint64) = nullptr;
        GLuint64 (GL_APIENTRY* getImageHandle)(GLuint, GLint, GLboolean, GLint, GLenum) = nullptr;
        // Loaded here because OSG declares glMakeImageHandleResident without its access argument.
        void (GL_APIENTRY* makeImageHandleResident)(GLuint64, GLenum) = nullptr;
        void (GL_APIENTRY* uniformHandleui64)(GLint, GLuint64) = nullptr;
        bool valid = false;

        HiZFunctions()
        {
            osg::setGLExtensionFuncPtr(createTextures, "glCreateTextures");
            osg::setGLExtensionFuncPtr(textureStorage2D, "glTextureStorage2D");
            osg::setGLExtensionFuncPtr(textureParameteri, "glTextureParameteri");
            osg::setGLExtensionFuncPtr(createFramebuffers, "glCreateFramebuffers");
            osg::setGLExtensionFuncPtr(namedFramebufferTexture, "glNamedFramebufferTexture");
            osg::setGLExtensionFuncPtr(namedFramebufferDrawBuffer, "glNamedFramebufferDrawBuffer");
            osg::setGLExtensionFuncPtr(namedFramebufferReadBuffer, "glNamedFramebufferReadBuffer");
            osg::setGLExtensionFuncPtr(checkNamedFramebufferStatus, "glCheckNamedFramebufferStatus");
            osg::setGLExtensionFuncPtr(getNamedFramebufferAttachmentParameteriv,
                "glGetNamedFramebufferAttachmentParameteriv");
            osg::setGLExtensionFuncPtr(blitNamedFramebuffer, "glBlitNamedFramebuffer");
            osg::setGLExtensionFuncPtr(getTextureHandle, "glGetTextureHandleARB");
            osg::setGLExtensionFuncPtr(makeTextureHandleResident, "glMakeTextureHandleResidentARB");
            osg::setGLExtensionFuncPtr(getImageHandle, "glGetImageHandleARB");
            osg::setGLExtensionFuncPtr(makeImageHandleResident, "glMakeImageHandleResidentARB");
            osg::setGLExtensionFuncPtr(uniformHandleui64, "glUniformHandleui64ARB");
            valid = createTextures && textureStorage2D && textureParameteri && createFramebuffers &&
                namedFramebufferTexture && namedFramebufferDrawBuffer && namedFramebufferReadBuffer &&
                checkNamedFramebufferStatus && getNamedFramebufferAttachmentParameteriv &&
                blitNamedFramebuffer && getTextureHandle && makeTextureHandleResident && getImageHandle &&
                makeImageHandleResident && uniformHandleui64;
        }
    };

    //! A texture or framebuffer created with direct state access and handed to osgEarth's object
    //! pool, which deletes it on its own context once nothing else references it. Dropping the
    //! last reference is therefore safe on any thread.
    class HiZObject : public GLObject
    {
    public:
        using Ptr = std::shared_ptr<HiZObject>;

        //! Creates a 2D texture with immutable storage; call with the context current.
        static Ptr texture(osg::State& state, const HiZFunctions& gl,
            GLsizei levels, GLenum format, GLsizei width, GLsizei height, std::size_t bytes)
        {
            Ptr object(new HiZObject(GL_TEXTURE, state, bytes));
            gl.createTextures(GL_TEXTURE_2D, 1, &object->_name);
            gl.textureStorage2D(object->_name, levels, format, width, height);
            GLObjectPool::get(state)->watch(object);
            return object;
        }

        //! Creates a framebuffer object; call with the context current.
        static Ptr framebuffer(osg::State& state, const HiZFunctions& gl)
        {
            Ptr object(new HiZObject(GL_FRAMEBUFFER_EXT, state, 0));
            gl.createFramebuffers(1, &object->_name);
            GLObjectPool::get(state)->watch(object);
            return object;
        }

        //! Called by the pool with the context current. Deleting a texture also deletes its
        //! bindless handles.
        void release() override
        {
            if (_name == 0) return;
            if (ns() == GL_FRAMEBUFFER_EXT) ext()->glDeleteFramebuffers(1, &_name);
            else glDeleteTextures(1, &_name);
            _name = 0;
        }

        GLsizei size() const override { return _bytes; }

    private:
        HiZObject(GLenum ns, osg::State& state, std::size_t bytes) :
            GLObject(ns, state),
            _bytes(GLsizei(std::min<std::size_t>(bytes, std::size_t(std::numeric_limits<GLsizei>::max())))) { }
        GLsizei _bytes;
    };

    //! One camera's single-sample depth copy and farthest-depth pyramid in one graphics context.
    //! Only that context's draw thread uses it.
    struct HiZTarget
    {
        unsigned contextID = 0;
        const osg::Camera* key = nullptr; // identity only
        const void* owner = nullptr; // identity only; separates pyramids of one camera
        osg::observer_ptr<const osg::Camera> camera;
        unsigned lastUsed = 0;
        GLint source = -1; // framebuffer last copied from
        GLenum sourceFormat = 0; // its depth format, or zero when it cannot be copied
        GLint width = 0, height = 0, levels = 0;
        GLenum format = 0; // format of the depth copy
        HiZObject::Ptr depth, fbo, pyramid;
        GLuint64 depthHandle = 0, pyramidHandle = 0;
        std::vector<GLuint64> images; // one image handle per pyramid level
        bool validated = false; // the first blit into this allocation raised no GL error
        bool failed = false; // the camera's framebuffer cannot be copied: it keeps drawing everything

        //! Drops the GL objects for the pool to delete; safe on any thread.
        void release()
        {
            depth = fbo = pyramid = nullptr;
            depthHandle = pyramidHandle = 0;
            images.clear();
            width = height = levels = 0;
            format = 0;
            validated = false;
        }
    };

    //! Occlusion pyramids for every camera and context. The mutex guards the containers; entry
    //! points stay loaded for the life of the process, and targets are shared so a concurrent
    //! release cannot pull one out from under a draw.
    struct HiZRegistry
    {
        std::mutex mutex;
        std::unordered_map<unsigned, std::unique_ptr<HiZFunctions>> functions;
        std::vector<std::shared_ptr<HiZTarget>> targets;
        std::atomic<std::uint64_t> generation{0}; // bumped whenever a target is discarded
    };

    HiZRegistry& hizRegistry()
    {
        static HiZRegistry registry;
        return registry;
    }

    //! Internal format matching a framebuffer's depth buffer, which a depth blit requires; zero when
    //! there is no depth buffer or its format is unusual.
    GLenum matchingDepthFormat(const HiZFunctions& gl, GLint fbo)
    {
        const GLenum depthAttachment = fbo ? GL_DEPTH_ATTACHMENT_EXT : GL_DEPTH;
        const GLenum stencilAttachment = fbo ? GL_STENCIL_ATTACHMENT_EXT : GL_STENCIL;
        GLint type = GL_NONE, bits = 0, component = GL_NONE, stencilType = GL_NONE, stencil = 0;
        gl.getNamedFramebufferAttachmentParameteriv(fbo, depthAttachment,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &type);
        if (type == GL_NONE)
            return 0;
        gl.getNamedFramebufferAttachmentParameteriv(fbo, depthAttachment, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
        gl.getNamedFramebufferAttachmentParameteriv(fbo, depthAttachment,
            GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component);
        gl.getNamedFramebufferAttachmentParameteriv(fbo, stencilAttachment,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &stencilType);
        if (stencilType != GL_NONE)
            gl.getNamedFramebufferAttachmentParameteriv(fbo, stencilAttachment,
                GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
        if (component == GL_FLOAT && bits == 32) return stencil ? GL_DEPTH32F_STENCIL8 : GL_DEPTH_COMPONENT32F;
        if (bits == 24) return stencil ? GL_DEPTH24_STENCIL8_EXT : GL_DEPTH_COMPONENT24;
        if (stencil == 0 && bits == 16) return GL_DEPTH_COMPONENT16;
        if (stencil == 0 && bits == 32) return GL_DEPTH_COMPONENT32;
        return 0;
    }

    //! (Re)allocates a target's depth copy and floor-sized R32F pyramid, with resident handles.
    //! Call with the context current; returns false when any piece is unavailable.
    bool allocateHiZ(HiZTarget& t, osg::State& state, const HiZFunctions& gl, GLint width, GLint height,
        GLenum format)
    {
        t.release();
        t.width = width;
        t.height = height;
        t.format = format;

        const std::size_t texels = std::size_t(width) * std::size_t(height);
        const std::size_t texelBytes = format == GL_DEPTH32F_STENCIL8 ? 8u : format == GL_DEPTH_COMPONENT16 ? 2u : 4u;
        t.depth = HiZObject::texture(state, gl, 1, format, width, height, texels * texelBytes);
        t.depth->debugLabel("Chonk", "occlusion depth copy");
        gl.textureParameteri(t.depth->name(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.textureParameteri(t.depth->name(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        t.fbo = HiZObject::framebuffer(state, gl);
        const bool stencil = format == GL_DEPTH24_STENCIL8_EXT || format == GL_DEPTH32F_STENCIL8;
        gl.namedFramebufferTexture(t.fbo->name(),
            stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT_EXT, t.depth->name(), 0);
        gl.namedFramebufferDrawBuffer(t.fbo->name(), GL_NONE);
        gl.namedFramebufferReadBuffer(t.fbo->name(), GL_NONE);
        if (gl.checkNamedFramebufferStatus(t.fbo->name(), GL_DRAW_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
            return false;

        // Level 0 halves the viewport; like GL mipmaps, sizes round down, to 1x1 at the top.
        const GLint w0 = std::max(1, width/2), h0 = std::max(1, height/2);
        t.levels = 1;
        while ((std::max(w0, h0) >> t.levels) > 0) ++t.levels;
        t.pyramid = HiZObject::texture(state, gl, t.levels, GL_R32F, w0, h0,
            std::size_t(w0) * std::size_t(h0) * 16u / 3u);
        t.pyramid->debugLabel("Chonk", "occlusion pyramid");
        gl.textureParameteri(t.pyramid->name(), GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        gl.textureParameteri(t.pyramid->name(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.textureParameteri(t.pyramid->name(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.textureParameteri(t.pyramid->name(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Handles freeze the sampler state set above.
        t.depthHandle = gl.getTextureHandle(t.depth->name());
        t.pyramidHandle = gl.getTextureHandle(t.pyramid->name());
        if (t.depthHandle == 0 || t.pyramidHandle == 0)
            return false;
        gl.makeTextureHandleResident(t.depthHandle);
        gl.makeTextureHandleResident(t.pyramidHandle);
        for (GLint level = 0; level < t.levels; ++level)
        {
            const GLuint64 image = gl.getImageHandle(t.pyramid->name(), level, GL_FALSE, 0, GL_R32F);
            if (image == 0)
                return false;
            gl.makeImageHandleResident(image, GL_READ_WRITE);
            t.images.push_back(image);
        }
        return true;
    }
}

osg::Program*
DepthPyramid::createProgram()
{
    Shaders pkg;
    osg::Program* program = new osg::Program();
    program->setName("Occlusion depth pyramid");
    program->addShader(new osg::Shader(osg::Shader::COMPUTE, ShaderLoader::load(pkg.ChonkHiZ, pkg)));
    return program;
}

bool
DepthPyramid::build(osg::State& state, const osg::Program::PerContextProgram* program, const osg::Camera* camera,
    const void* owner, unsigned frameNumber, Result& result)
{
    const osg::Viewport* viewport = state.getCurrentViewport();
    if (!camera || !viewport || !program || !program->isLinked())
        return false;

    // A missing uniform means some other program is active (a broken build or an override).
    auto location = [&](const char* name) { return program->getUniformLocation(osg::Uniform::getNameID(name)); };
    const GLint depthLoc = location("oe_hiz_depth");
    const GLint sourceLoc = location("oe_hiz_source");
    const GLint targetLoc = location("oe_hiz_target");
    const GLint sourceSizeLoc = location("oe_hiz_source_size");
    const GLint targetSizeLoc = location("oe_hiz_target_size");
    const GLint fromDepthLoc = location("oe_hiz_from_depth");
    const GLint reverseLoc = location("oe_hiz_reverse_z");
    if (depthLoc < 0 || sourceLoc < 0 || targetLoc < 0 || sourceSizeLoc < 0 || targetSizeLoc < 0 ||
        fromDepthLoc < 0 || reverseLoc < 0)
        return false;

    // Read applied state, not camera settings: inherited overrides can supply the depth convention.
    // GL 4.5 clip control is available wherever this pyramid's DSA entry points are supported.
    GLint depthFunc = GL_LESS, clipDepth = 0, clipOrigin = 0;
    GLdouble depthRange[2] = {0.0, 1.0};
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &clipDepth);
    glGetIntegerv(GL_CLIP_ORIGIN, &clipOrigin);
    glGetDoublev(GL_DEPTH_RANGE, depthRange);
    const bool reverse = depthFunc == GL_GREATER || depthFunc == GL_GEQUAL;
    // The screen bounds currently assume a lower-left origin. Unsupported tests or a collapsed
    // depth range cannot prove occlusion; let the late pass retain every instance instead.
    if (!glIsEnabled(GL_DEPTH_TEST) || clipOrigin != GL_LOWER_LEFT ||
        (!reverse && depthFunc != GL_LESS && depthFunc != GL_LEQUAL) || depthRange[0] == depthRange[1])
        return false;
    const double span = depthRange[1] - depthRange[0];
    const double scale = clipDepth == GL_ZERO_TO_ONE ? span : 0.5 * span;
    const double bias = clipDepth == GL_ZERO_TO_ONE ? depthRange[0] : depthRange[0] + 0.5 * span;

    const GLint x = GLint(viewport->x()), y = GLint(viewport->y());
    const GLint width = GLint(viewport->width()), height = GLint(viewport->height());
    if (width < 2 || height < 2)
        return false;

    auto& registry = hizRegistry();
    const unsigned contextID = state.getContextID();
    HiZFunctions* gl = nullptr;
    std::shared_ptr<HiZTarget> target;
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        auto& functions = registry.functions[contextID];
        if (!functions) functions.reset(new HiZFunctions());
        gl = functions.get();
        if (!gl->valid)
            return false;

        // Retire this context's targets for dead cameras (even one whose address was reused)
        // and for cameras or owners that stopped building.
        auto& targets = registry.targets;
        for (auto i = targets.begin(); i != targets.end(); )
        {
            const HiZTarget& t = **i;
            if (t.contextID == contextID && (!t.camera.valid() ||
                ((t.key != camera || t.owner != owner) && frameNumber - t.lastUsed > 600u)))
            {
                i = targets.erase(i);
                ++registry.generation;
            }
            else
                ++i;
        }
        for (auto& t : targets)
            if (t->contextID == contextID && t->key == camera && t->owner == owner) target = t;
        if (!target)
        {
            target = std::make_shared<HiZTarget>();
            target->contextID = contextID;
            target->key = camera;
            target->owner = owner;
            target->camera = camera;
            targets.push_back(target);
        }
    }

    HiZTarget& t = *target;
    t.lastUsed = frameNumber;
    if (t.failed)
        return false;

    GLint source = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &source);
    if (source != t.source)
    {
        t.source = source;
        t.sourceFormat = matchingDepthFormat(*gl, source);
    }
    if (!t.depth || width != t.width || height != t.height || t.sourceFormat != t.format)
    {
        // A target without a depth buffer (or with an unusual one) simply keeps drawing everything.
        if (t.sourceFormat == 0)
        {
            t.release();
            t.failed = true;
            OE_INFO << LC << "Occlusion culling off for camera \"" << camera->getName()
                << "\": no copyable depth buffer" << std::endl;
            return false;
        }
        if (!allocateHiZ(t, state, *gl, width, height, t.sourceFormat))
        {
            t.release();
            t.failed = true;
            OE_WARN << LC << "Occlusion culling disabled for camera \"" << camera->getName()
                << "\": depth pyramid allocation failed" << std::endl;
            return false;
        }
    }

    // Errors raised earlier by other code are not the blit's; drain them before checking it.
    if (!t.validated)
        for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) { }

    // A depth blit keeps one sample of each multisampled pixel. The scissor test is the only
    // fragment operation that applies to blits, so lift it for the copy.
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    gl->blitNamedFramebuffer(source, t.fbo->name(), x, y, x + width, y + height, 0, 0, width, height,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (scissor) glEnable(GL_SCISSOR_TEST);

    if (!t.validated)
    {
        if (glGetError() != GL_NO_ERROR)
        {
            t.release();
            t.failed = true;
            OE_WARN << LC << "Occlusion culling disabled for camera \"" << camera->getName()
                << "\": its depth buffer cannot be copied" << std::endl;
            return false;
        }
        t.validated = true;
    }

    // Each dispatch halves the previous level, keeping the farthest depth.
    auto ext = state.get<osg::GLExtensions>();
    gl->uniformHandleui64(depthLoc, t.depthHandle);
    ext->glUniform1i(reverseLoc, reverse ? 1 : 0);
    GLint sw = width, sh = height;
    GLint tw = std::max(1, sw/2), th = std::max(1, sh/2);
    for (GLint level = 0; level < t.levels; ++level)
    {
        ext->glUniform1i(fromDepthLoc, level == 0 ? 1 : 0);
        gl->uniformHandleui64(sourceLoc, t.images[level > 0 ? level - 1 : 0]);
        gl->uniformHandleui64(targetLoc, t.images[level]);
        ext->glUniform2i(sourceSizeLoc, sw, sh);
        ext->glUniform2i(targetSizeLoc, tw, th);
        ext->glDispatchCompute(GLuint((tw + 7)/8), GLuint((th + 7)/8), 1u);
        ext->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        sw = tw;
        sh = th;
        tw = std::max(1, tw/2);
        th = std::max(1, th/2);
    }
    ext->glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

    result.handle = t.pyramidHandle;
    result.params.set(float(width), float(height), float(t.levels), reverse ? -1.0f : 1.0f);
    result.depthMapping.set(float(scale), float(bias));
    return true;
}

void
DepthPyramid::release(osg::State* state)
{
    auto& registry = hizRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto& targets = registry.targets;
    for (auto i = targets.begin(); i != targets.end(); )
    {
        if (state == nullptr || (*i)->contextID == state->getContextID())
        {
            i = targets.erase(i);
            ++registry.generation;
        }
        else
            ++i;
    }
}

void
DepthPyramid::releaseOwner(const void* owner)
{
    if (owner == nullptr)
        return;
    auto& registry = hizRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto& targets = registry.targets;
    for (auto i = targets.begin(); i != targets.end(); )
    {
        if ((*i)->owner == owner)
        {
            i = targets.erase(i);
            ++registry.generation;
        }
        else
            ++i;
    }
}

std::uint64_t
DepthPyramid::generation()
{
    return hizRegistry().generation.load();
}
