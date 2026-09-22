/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "WindLayer"
#include "Math"
#include "NoiseTextureFactory"
#include "Shaders"
#include "StringUtils"
#include "Registry"
#include "Capabilities"
#include "GLUtils"
#include "TerrainEngineNode"
#include <osg/BindImageTexture>
#include <osg/Texture3D>
#include <osg/Program>
#include <osgUtil/CullVisitor>
#include <cmath>
#include <algorithm>

#ifdef OE_HAVE_BINDIMAGETEXTURE
#include <osg/BindImageTexture>
#endif

using namespace osgEarth;

#define LC "[WindLayer] "

REGISTER_OSGEARTH_LAYER(wind, WindLayer);

// texture size (3D) -- in view space. A larger Z dimension (which cooresponds
// to your camera look vector) seesm to help with aliasing.
#define WIND_DIM_X 8
#define WIND_DIM_Y 8
#define WIND_DIM_Z 16

//........................................................................

namespace 
{
    struct WindData // keep me 16-byte aligned
    {
        GLfloat position[4];
        GLfloat direction[3];
        GLfloat speed;
    };

    // GL data that must be stored separately per state since it will
    // differ by camera/view and changes every frame
    struct GLObjects : public PerStateGLObjects
    {
        GLBuffer::Ptr _buffer;
    };

    // Data stored per-camera
    struct CameraState
    {
        WindData* _windData;
        unsigned _numWindsAllocated;

        osg::ref_ptr<osg::StateSet> _computeStateSet;
        osg::ref_ptr<osg::Uniform> _viewToTexMatrix;

        osg::ref_ptr<osg::StateSet> _sharedStateSet;
        osg::ref_ptr<osg::Uniform> _texToViewMatrix;

        CameraState() :
            _windData(nullptr),
            _numWindsAllocated(0) { }

        ~CameraState() {
            if (_windData)
                delete[] _windData;
            releaseGLObjects(nullptr);
        }

        void releaseGLObjects(osg::State* state) const {
            if (_computeStateSet.valid())
                _computeStateSet->releaseGLObjects(state);
            if (_sharedStateSet.valid())
                _sharedStateSet->releaseGLObjects(state);
        }

        void resizeGLObjectBuffers(unsigned size) {
            if (_computeStateSet.valid())
                _computeStateSet->resizeGLObjectBuffers(size);
            if (_sharedStateSet.valid())
                _sharedStateSet->resizeGLObjectBuffers(size);
        }
    };

    // mutexed per-camera map of CameraState objects
    struct CameraStates : public Mutexed<std::unordered_map<const osg::Camera*, CameraState>>
    {
        CameraState& get(const osg::Camera* camera)
        {
            std::lock_guard<std::mutex> lock(mutex());
            return (*this)[camera];
        }

        void for_each(const std::function<void(CameraState&)>& func) {
            std::lock_guard<std::mutex> lock(mutex());
            for (auto& i : *this) {
                func(i.second);
            }
        }

        void for_each(const std::function<void(const CameraState&)>& func) const {
            std::lock_guard<std::mutex> lock(mutex());
            for (auto& i : *this) {
                func(i.second);
            }
        }
    };

    struct WindDrawable;

    // custom cull callback for the wind drawable
    struct WindStateCuller : public osg::Drawable::CullCallback
    {
        bool cull(osg::NodeVisitor* nv, osg::Drawable* drawable, osg::RenderInfo* renderInfo) const override;
    };

    //! Drawable that runs the compute shader to populate our wind texture.
    struct WindDrawable : public osg::Drawable
    {
    public:
        WindDrawable(const osgDB::Options* readOptions);

        void accept(osg::NodeVisitor& nv) override {
            osg::Drawable::accept(nv);
        }
        void setupPerCameraState(const osg::Camera* camera);
        void streamDataToGPU(osg::RenderInfo& ri, GLObjects& globjects) const;
        void updateBuffers(CameraState&, osgUtil::CullVisitor*, const SpatialReference* srs);

        void drawImplementation(osg::RenderInfo& ri) const override;
        void releaseGLObjects(osg::State* state) const override;
        void resizeGLObjectBuffers(unsigned maxSize) override;

        osg::ref_ptr<const osgDB::Options> _readOptions;
        osg::ref_ptr<osg::Program> _computeProgram;
        osg::ref_ptr<osg::Texture> _noiseTexture;
        std::vector<osg::ref_ptr<Wind> > _winds;
        mutable osg::buffered_object<GLObjects> _globjects;
        mutable CameraStates _cameraState;
        TextureImageUnitReservation _unitReservation;
    };

    bool WindStateCuller::cull(osg::NodeVisitor* nv, osg::Drawable* drawable, osg::RenderInfo* renderInfo) const
    {
        osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);
        const osg::Camera* camera = cv->getCurrentCamera();
        WindDrawable* wd = static_cast<WindDrawable*>(drawable);
        CameraState& cs = wd->_cameraState.get(camera); // should exist by now
        if (cs._computeStateSet.valid())
        {
            cv->pushStateSet(cs._computeStateSet.get());
            cv->addDrawableAndDepth(drawable, cv->getModelViewMatrix(), 0.0f);
            cv->popStateSet();
        }
        return true;
    }

    WindDrawable::WindDrawable(const osgDB::Options* readOptions)
    {
        OE_SOFT_ASSERT(sizeof(WindData) % 16 == 0, "struct WindData is not 16-byte aligned; expect chaos");

        // Always run the shader.
        setCullingActive(false);

        // So we can update the wind instances synchronously
        setDataVariance(osg::Object::DYNAMIC);

        // Prevent some variations of OSG from building an old-school display list
        // and then never calling drawImplementation again. Cry.
        setUseDisplayList(false);

        // Set up our compute shader
        Shaders shaders;
        std::string source = ShaderLoader::load(shaders.WindComputer, shaders, readOptions);
        osg::Shader* computeShader = new osg::Shader(osg::Shader::COMPUTE, source);
        _computeProgram = new osg::Program();
        _computeProgram->addShader(computeShader);

        setCullCallback(new WindStateCuller());

        // simplex noise texture for wind speed variation
        NoiseTextureFactory ntf;
        _noiseTexture = ntf.create(256, 4);
    }

    void WindDrawable::setupPerCameraState(const osg::Camera* camera)
    {
        // wind texture
        osg::Texture3D* tex = new osg::Texture3D();
        tex->setTextureSize(WIND_DIM_X, WIND_DIM_Y, WIND_DIM_Z);
        tex->setSourceFormat(GL_RGBA);
        tex->setInternalFormat(GL_RGBA8);
        tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        tex->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
        tex->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());

        // state set for the compute program that populates the wind texture
        CameraState& cs = _cameraState.get(camera);
        cs._computeStateSet = new osg::StateSet();
        cs._computeStateSet->setAttribute(_computeProgram.get(), 1);

        // Binding so the compute shader can write to the texture.
        // Note, setting "layered=GL_TRUE" is required to bind the entire 3D texture.
        cs._computeStateSet->addUniform(new osg::Uniform("oe_wind_tex", 0));
        cs._computeStateSet->setAttribute(new osg::BindImageTexture(0, tex, osg::BindImageTexture::WRITE_ONLY, GL_RGBA8, 0, GL_TRUE));

        cs._texToViewMatrix = new osg::Uniform("oe_wind_texToViewMatrix", osg::Matrixf::identity());
        cs._computeStateSet->addUniform(cs._texToViewMatrix.get());

        cs._computeStateSet->setRenderBinDetails(-90210, "RenderBin"); // necessary? probably not

        // expose the noise texture to the compute shader
        cs._computeStateSet->addUniform(new osg::Uniform("oe_noise_tex", 1));
        cs._computeStateSet->setTextureAttribute(1, _noiseTexture.get(), osg::StateAttribute::ON);


        // Shared stateset using during the cull traversal
        // that gives the rest of the Map access to the computed wind texture.
        cs._sharedStateSet = new osg::StateSet();

        cs._viewToTexMatrix = new osg::Uniform("oe_wind_matrix", osg::Matrixf::identity());
        cs._sharedStateSet->addUniform(cs._viewToTexMatrix.get());
        cs._sharedStateSet->setDefine("OE_WIND_TEX_MATRIX", "oe_wind_matrix");

        cs._sharedStateSet->addUniform(new osg::Uniform("oe_wind_tex", _unitReservation.unit()));
        cs._sharedStateSet->setTextureAttribute(_unitReservation.unit(), tex, osg::StateAttribute::ON);
        cs._sharedStateSet->setDefine("OE_WIND_TEX", "oe_wind_tex");
        cs._sharedStateSet->addUniform(new osg::Uniform("oe_wind_power",1.0f));
    }

    void WindDrawable::streamDataToGPU(osg::RenderInfo& ri, GLObjects& gl) const
    {
        osg::State* state = ri.getState();
        if (state)
        {
            CameraState& cs = _cameraState.get(ri.getCurrentCamera());

            if (gl._buffer == nullptr)
            {
                gl._buffer = GLBuffer::create(GL_SHADER_STORAGE_BUFFER, *state);
                gl._buffer->bind();
                gl._buffer->debugLabel("Wind", "SSBO");
                gl._buffer->unbind();
            }

            // upload to GPU
            gl._buffer->uploadData(
                sizeof(WindData) * (_winds.size() + 1), 
                cs._windData);
        }
    }

    void WindDrawable::updateBuffers(CameraState& cs, osgUtil::CullVisitor* cv, const SpatialReference* srs)
    {
        if (cs._numWindsAllocated < _winds.size()+1)
        {
            if (cs._windData != nullptr)
                delete [] cs._windData;

            // add one for the terminator.
            cs._windData = new WindData[_winds.size()+1];
            ::memset(cs._windData, 0, sizeof(WindData)*(_winds.size()+1));

            cs._numWindsAllocated = _winds.size()+1;
        }

        const osg::Matrix& vm = cv->getCurrentCamera()->getViewMatrix();
        const osg::Matrix& mvm = *cv->getModelViewMatrix();
        const osg::Matrix ivm = cv->getCurrentCamera()->getInverseViewMatrix();
        const SpatialReference* worldSRS = srs->isGeographic() ? srs->getGeocentricSRS() : srs;
        osg::Matrix local2world;
        auto eyeWorld = osg::Vec3d() * ivm;
        worldSRS->createLocalToWorld(eyeWorld, local2world);

        size_t i;
        for(i=0; i<_winds.size(); ++i)
        {
            const Wind* wind = _winds[i].get();

            if (wind->type() == Wind::TYPE_POINT)
            {
                // transform from world to camera-view space
                osg::Vec3d posView = wind->getPointWorld() * mvm;

                cs._windData[i].position[0] = posView.x();
                cs._windData[i].position[1] = posView.y();
                cs._windData[i].position[2] = posView.z();
                cs._windData[i].position[3] = 1.0;
            }
            else // TYPE_DIRECTIONAL
            {
                // direction is a local tangent space vector:
                osg::Vec3d dir;
                dir.x() = wind->direction()->x();
                dir.y() = wind->direction()->y();

                // transform it from local tangent space to view space:
                dir = osg::Matrixd::transform3x3(dir, local2world * mvm);
                
                dir.normalize();

                cs._windData[i].direction[0] = dir.x();
                cs._windData[i].direction[1] = dir.y();
                cs._windData[i].direction[2] = dir.z();
                cs._windData[i].position[3] = 0.0f;
            }

            double speed = wind->speed()->as(Units::METERS_PER_SECOND);
            cs._windData[i].speed = std::isfinite(speed) ? float(std::max(0.0,speed)) : 0.0f;
        }

        // terminator record: set power to negative.
        cs._windData[i].speed = -1.0f;
    }

    void WindDrawable::releaseGLObjects(osg::State* state) const
    {
        if (state)
        {
            auto& gl = GLObjects::get(_globjects, *state);
            gl._buffer = nullptr;
        }
        else
        {
            _globjects.clear();
        }

        _cameraState.for_each([&](const CameraState& cs)
            {
                cs.releaseGLObjects(state);
            });

        osg::Drawable::releaseGLObjects(state);
    }

    void WindDrawable::resizeGLObjectBuffers(unsigned maxSize)
    {
        if (maxSize > _globjects.size())
            _globjects.resize(maxSize);

        _cameraState.for_each([&](CameraState& cs)
            {
                cs.resizeGLObjectBuffers(maxSize);
            });

        osg::Drawable::resizeGLObjectBuffers(maxSize);
    }

    void WindDrawable::drawImplementation(osg::RenderInfo& ri) const
    {
        if (ri.getCurrentCamera() == nullptr)
            return;

        OE_GL_ZONE;

        auto& gl = GLObjects::get(_globjects, *ri.getState());

        // update buffer with wind data
        streamDataToGPU(ri, gl);

        // activate layout() binding point:
        gl._buffer->bindBufferBase(0);

        // run it
        gl._buffer->ext()->glDispatchCompute(WIND_DIM_X, WIND_DIM_Y, WIND_DIM_Z);

        // sync the output
        gl._buffer->ext()->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT|GL_TEXTURE_FETCH_BARRIER_BIT);
    }
}

//........................................................................

// default: gentle west-to-east breeze
Wind::Wind() :
    _type(TYPE_DIRECTIONAL),
    _direction(osg::Vec2f(1,0)),
    _speed(Speed(3.0, Units::KNOTS)),
    _pointWorld(osg::Vec3d(0,0,0))
{
    //nop
}

void
Wind::setPoint(const GeoPoint& point)
{
    _point = point;
    point.toWorld(_pointWorld);
}

Wind::Wind(const Config& conf) : Wind()
{
    conf.get("type", "point", type(), TYPE_POINT);
    conf.get("type", "directional", type(), TYPE_DIRECTIONAL);
    conf.get("point", point());
    conf.get("direction", direction());
    conf.get("speed", speed());
    if (getPoint().isValid()) getPoint().toWorld(_pointWorld);
}

Config
Wind::getConfig() const
{
    Config conf("wind");
    conf.set("type", "point", type(), TYPE_POINT);
    conf.set("type", "directional", type(), TYPE_DIRECTIONAL);
    conf.set("point", point());
    conf.set("direction", direction());
    conf.set("speed", speed());
    return conf;
}

//........................................................................

Config
WindLayer::Options::getConfig() const
{
    Config conf = Layer::Options::getConfig();
    conf.set("ortho", ortho());
    conf.set("radius", radius());
    conf.set("speed_factor", speedFactor());
    if (!winds().empty())
    {
        Config windsConf("winds");
        for(std::vector<osg::ref_ptr<Wind> >::const_iterator i = winds().begin();
            i != winds().end();
            ++i)
        {
            windsConf.add("wind", i->get()->getConfig());
        }
        conf.add(windsConf);
    }
    return conf;
}

void
WindLayer::Options::fromConfig(const Config& conf)
{
    ortho().setDefault(true);
    radius().setDefault(Distance(75.0, Units::METERS));
    speedFactor().setDefault(1.0f);
    winds().clear();

    conf.get("ortho", ortho());
    conf.get("radius", radius());
    conf.get("speed_factor", speedFactor());
    const ConfigSet windsConf = conf.child("winds").children();
    for(ConfigSet::const_iterator i = windsConf.begin(); i != windsConf.end(); ++i)
    {
        Wind* wind = new Wind(*i);
        winds().push_back(wind);
    }
}

//........................................................................

void
WindLayer::addWind(Wind* wind)
{
    if (!wind) return;
    options().winds().push_back(wind);
    WindDrawable* wd = dynamic_cast<WindDrawable*>(_drawable.get()); // also checks for nullptr
    if (wd)
    {
        wd->_winds.push_back(wind);
    }
}

void
WindLayer::removeWind(Wind* wind)
{
    auto& sources = options().winds();
    auto source = std::find(sources.begin(),sources.end(),wind);
    if (source != sources.end()) sources.erase(source);
    WindDrawable* wd = dynamic_cast<WindDrawable*>(_drawable.get()); // also checks for nullptr
    if (wd)
    {
        for (std::vector<osg::ref_ptr<Wind>>::iterator i = wd->_winds.begin();
            i != wd->_winds.end();
            ++i)
        {
            if (i->get() == wind)
            {
                wd->_winds.erase(i);
                break;
            }
        }
    }
}

void WindLayer::setSpeedFactor(float value)
{
    options().speedFactor() = std::isfinite(value) ? std::max(0.0f,value) : 1.0f;
}

float WindLayer::getSpeedFactor() const
{
    float value = options().speedFactor().get();
    return std::isfinite(value) ? std::max(0.0f,value) : 1.0f;
}

osg::Vec2d WindLayer::getDirectionalVelocity() const
{
    osg::Vec2d velocity;
    for (const auto& wind : options().winds())
    {
        if (!wind || wind->getType() != Wind::TYPE_DIRECTIONAL) continue;
        osg::Vec2d direction(wind->getDirection().x(),wind->getDirection().y());
        double speed = wind->getSpeed().as(Units::METERS_PER_SECOND);
        if (!std::isfinite(direction.x()) || !std::isfinite(direction.y()) || !std::isfinite(speed)) continue;
        direction.normalize();
        velocity += direction*std::max(0.0,speed);
    }
    return velocity*getSpeedFactor();
}

osg::Vec2d WindLayer::getDirectionalDisplacement(double time) const
{
    std::lock_guard<std::mutex> lock(_motionMutex);
    if (!std::isfinite(time)) return _displacement;
    auto velocity = isOpen() ? getDirectionalVelocity() : osg::Vec2d();
    if (!_motionStarted || time < _motionTime)
        _displacement = velocity*time;
    else
        _displacement += _previousVelocity*(time-_motionTime);
    _previousVelocity = velocity;
    _motionTime = time;
    _motionStarted = true;
    return _displacement;
}

void
WindLayer::init()
{
    Layer::init();

    // Never cache
    layerHints().cachePolicy() = CachePolicy::NO_CACHE;
}

Status
WindLayer::openImplementation()
{
    // GL version requirement
    if (Registry::capabilities().getGLSLVersion() < 4.3f)
    {
        return Status(Status::ResourceUnavailable, "WindLayer requires GL 4.3+");
    }

    _drawable = new WindDrawable(getReadOptions());
    static_cast<WindDrawable*>(_drawable.get())->_winds = options().winds();

    return Layer::openImplementation();
}

Status
WindLayer::closeImplementation()
{
    return Layer::closeImplementation();
}

osg::Node*
WindLayer::getNode() const
{
    return _drawable.get();
}

void
WindLayer::prepareForRendering(TerrainEngine* engine)
{
    Layer::prepareForRendering(engine);

    // Create the wind drawable that will provide a wind texture
    //WindDrawable* wd = new WindDrawable(getReadOptions());
    //_drawable = wd;
    WindDrawable* wd = dynamic_cast<WindDrawable*>(_drawable.get());
    OE_SOFT_ASSERT_AND_RETURN(wd, void());

    // texture image unit for the shared wind LUT
    engine->getResources()->reserveTextureImageUnit(wd->_unitReservation, "WindLayer");

#if 0 // TESTING
    Wind* wind = new Wind();
    wind->type() = Wind::TYPE_DIRECTIONAL;
    wind->direction()->set(-1.0f, 0.0f);
    wind->speed()->set(10, Units::KNOTS);
    addWind(wind);

    Wind* wind2 = new Wind();
    wind2->type() = Wind::TYPE_DIRECTIONAL;
    wind2->direction()->set(0.0f, -1.0f);
    wind2->speed()->set(10, Units::KNOTS);
    addWind(wind2);
#endif

    // Sources are copied when opening; rendering preparation may run more than once for a shared layer.
}

void
WindLayer::addedToMap(const Map* map)
{
    _srs = map->getSRS();
}

osg::StateSet*
WindLayer::getSharedStateSet(osg::NodeVisitor* nv) const
{
    if (!isOpen())
        return nullptr;

    OE_SOFT_ASSERT_AND_RETURN(_drawable.valid(), nullptr);

    osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);

    WindDrawable* windDrawable = static_cast<WindDrawable*>(_drawable.get());

    const osg::Camera* camera = cv->getCurrentCamera();
    CameraState& cs = windDrawable->_cameraState.get(camera);

    if (!cs._computeStateSet.valid())
    {
        windDrawable->setupPerCameraState(camera);
    }

    // this xforms from clip [-1..1] to texture [0..1] space
    static osg::Matrix clipToTexture = 
        osg::Matrix::translate(1.0,1.0,1.0) * 
        osg::Matrix::scale(0.5,0.5,0.5);

    osg::Matrix rttProjection;



    double R = options().radius()->as(Units::METERS);

    if (options().ortho() == true)
    {
        rttProjection = osg::Matrix::ortho(-R, R, -R, R, 0, R);
    }
    else
    { 
        double y,a,n,f;
        ProjectionMatrix::getPerspective(camera->getProjectionMatrix(), y, a, n, f);

        // pushing the NEAR out reduces jitter a lot
        ProjectionMatrix::setPerspective(rttProjection, y, a, 5.0, R);
        //rttProjection.makePerspective(y, a, 5.0, R);
    }

    // view to texture:
    osg::Matrix camViewToTexture = rttProjection * clipToTexture;
    cs._viewToTexMatrix->set(camViewToTexture);

    // texture to view (for the compute shader):
    osg::Matrix textureToCamView;
    textureToCamView.invert(camViewToTexture);
    cs._texToViewMatrix->set(textureToCamView);

    windDrawable->updateBuffers(cs, cv, _srs.get());
    cs._sharedStateSet->getUniform("oe_wind_power")->set(getSpeedFactor());

    return cs._sharedStateSet.get();
}

void
WindLayer::releaseGLObjects(osg::State* state) const
{
    if (_drawable.valid())
        _drawable->releaseGLObjects(state);
}

void
WindLayer::resizeGLObjectBuffers(unsigned maxSize)
{
    if (_drawable.valid())
        _drawable->resizeGLObjectBuffers(maxSize);
}

