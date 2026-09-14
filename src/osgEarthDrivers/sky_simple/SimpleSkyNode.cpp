/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/

#include "SimpleSkyNode"
#include "SimpleSkyShaders"
#include "BrunetonImpl"

#include <osgEarth/StarData>
#include <osgEarth/Ephemeris>

#include <osgEarth/VirtualProgram>
#include <osgEarth/NodeUtils>
#include <osgEarth/Map>
#include <osgEarth/Utils>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/CullingUtils>
#include <osgEarth/ShaderFactory>
#include <osgEarth/ShaderGenerator>
#include <osgEarth/Shaders>
#include <osgEarth/GLUtils>
#include <osgEarth/Lighting>
#include <osgEarth/PointDrawable>
#include <osgEarth/TerrainEngineNode>

#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osg/PointSprite>
#include <osg/PolygonMode>
#include <osg/Texture2D>
#include <osg/BlendFunc>
#include <osg/FrontFace>
#include <osg/CullFace>
#include <osg/Program>
#include <osg/Camera>
#include <osg/Point>
#include <osg/Shape>
#include <osg/Depth>
#include <osg/Quat>

#include <sstream>
#include <time.h>

#define LC "[SimpleSkyNode] "

using namespace osgEarth;
using namespace osgEarth::Util;
using namespace osgEarth::SimpleSky;

//---------------------------------------------------------------------------

#define BIN_STARS       -100003
#define BIN_SUN         -100002
#define BIN_MOON        -100001
#define BIN_ATMOSPHERE  -100000

#define TWO_PI 6.283185307179586476925286766559

//---------------------------------------------------------------------------

namespace
{
    // constucts an ellipsoidal mesh that we will use to draw the atmosphere
    osg::Geometry* s_makeEllipsoidGeometry(
        const Ellipsoid& ellipsoid,
        double outerRadius,
        bool genTexCoords)
    {
        double hae = outerRadius - std::max(
            ellipsoid.getRadiusEquator(),
            ellipsoid.getRadiusPolar());

        osg::Geometry* geom = new osg::Geometry();
        geom->setName("SimpleSky");
        geom->setUseVertexBufferObjects(true);

        int latSegments = 100;
        int lonSegments = 2 * latSegments;

        double segmentSize = 180.0 / (double)latSegments; // degrees

        osg::Vec3Array* verts = new osg::Vec3Array();
        verts->reserve(latSegments * lonSegments);

        osg::Vec2Array* texCoords = 0;
        osg::Vec3Array* normals = 0;

        if (genTexCoords)
        {
            texCoords = new osg::Vec2Array();
            texCoords->reserve(latSegments * lonSegments);
            geom->setTexCoordArray(0, texCoords);

            normals = new osg::Vec3Array(osg::Array::BIND_PER_VERTEX);
            normals->reserve(latSegments * lonSegments);
            geom->setNormalArray(normals);
        }

        osg::DrawElementsUShort* el = new osg::DrawElementsUShort(GL_TRIANGLES);
        el->reserve(latSegments * lonSegments * 6);
        int row_size = lonSegments + 1;

        for (int y = 0; y <= latSegments; ++y)
        {
            double lat = clamp(-90.0 + segmentSize * (double)y, -90.0, 90.0);

            for (int x = 0; x <= lonSegments; ++x)
            {
                double lon = clamp(-180.0 + segmentSize * (double)x, -180.0, 180.0);
                osg::Vec3 g = ellipsoid.geodeticToGeocentric(osg::Vec3d(lon, lat, hae));
                verts->push_back(g);

                if (genTexCoords)
                {
                    float s = (lon + 180) / 360.0;
                    float t = (lat + 90.0) / 180.0;
                    texCoords->push_back(osg::Vec2(s, t));
                }

                if (normals)
                {
                    osg::Vec3d normal(g);
                    normal.normalize();
                    normals->push_back(osg::Vec3f(normal));
                }


                if (y < latSegments && x < lonSegments)
                {
                    int x_plus_1 = x + 1;
                    int y_plus_1 = y + 1;
                    el->push_back(y * row_size + x);
                    el->push_back(y * row_size + x_plus_1);
                    el->push_back(y_plus_1 * row_size + x);

                    el->push_back(y * row_size + x_plus_1);
                    el->push_back(y_plus_1 * row_size + x_plus_1);
                    el->push_back(y_plus_1 * row_size + x);
                }
            }
        }

        geom->setVertexArray(verts);
        geom->addPrimitiveSet(el);

        return geom;
    }

    // makes a disc geometry that we'll use to render the sun/moon
    osg::Geometry* s_makeDiscGeometry(double radius)
    {
        int segments = 48;
        float deltaAngle = 360.0 / (float)segments;

        osg::Geometry* geom = new osg::Geometry();
        geom->setName("SimpleSky");
        geom->setUseVertexBufferObjects(true);

        osg::Vec3Array* verts = new osg::Vec3Array();
        verts->reserve(1 + segments);
        geom->setVertexArray(verts);

        osg::DrawElementsUShort* el = new osg::DrawElementsUShort(GL_TRIANGLES);
        el->reserve(1 + 2 * segments);
        geom->addPrimitiveSet(el);

        verts->push_back(osg::Vec3(0, 0, 0)); // center point

        for (int i = 0; i < segments; ++i)
        {
            double angle = osg::DegreesToRadians(deltaAngle * (float)i);
            double x = radius * cos(angle);
            double y = radius * sin(angle);
            verts->push_back(osg::Vec3(x, y, 0.0));

            int i_plus_1 = i < segments - 1 ? i + 1 : 0;
            el->push_back(0);
            el->push_back(1 + i);
            el->push_back(1 + i_plus_1);
        }

        return geom;
    }
}

//---------------------------------------------------------------------------

SimpleSkyNode::SimpleSkyNode(const SimpleSkyOptions& options) :
    SkyNode(options),
    _options(options),
    _eb_initialized(false)
{
    construct();

    // temp -- for the EB stateset
    ADJUST_UPDATE_TRAV_COUNT(this, +1);
}

void
SimpleSkyNode::construct()
{
    // protect us from the ShaderGenerator.
    ShaderGenerator::setIgnoreHint(this, true);

    // containers for sky elements. DO NOT add children directly to 
    // the skynode; uswe the cullContainer instead!
    _cullContainer = new osg::Group();

    osg::Vec3f lightPos(0.0f, 1.0f, 0.0f);

    _light = new LightGL3(0);
    _light->setPosition(osg::Vec4f(0.0f, 0.0f, 1.0f, 0.0f));
    float ambient = osg::clampBetween(_options.ambient().get(), 0.0f, 1.0f);
    _light->setAmbient(osg::Vec4f(ambient, ambient, ambient, 1.0f));
    _light->setDiffuse(osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));
    _light->setSpecular(osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f)); // does nothing in PBR mode

    // install the Sun as a lightsource.
    osg::LightSource* lightSource = new osg::LightSource();
    lightSource->setLight(_light.get());
    lightSource->setCullingActive(false);
    _cullContainer->addChild(lightSource);
    lightSource->addCullCallback(new LightSourceGL3UniformGenerator());

    // only supports geocentric for now.
    if (getReferencePoint().isValid())
    {
        OE_WARN << LC << "Found an ephemeris reference point, but SimpleSky does not support projected maps" << std::endl;
        return;
    }

    // set up the astronomical parameters:
    osg::ref_ptr<const SpatialReference> wgs84 = SpatialReference::get("wgs84");
    _ellipsoid = wgs84->getEllipsoid();
    _innerRadius = std::min(
        _ellipsoid.getRadiusPolar(),
        _ellipsoid.getRadiusEquator());

    _outerRadius = _innerRadius * 1.025; // +60000.0;

    CelestialBody sun = getEphemeris()->getSunPosition(DateTime());
    _sunDistance = sun.altitude.as(Units::METERS);

    // parse the quality setting:
    _usePhong = false;
    _useONeil = false;
    _usePBR = false;
    _useBruneton = false;

    switch (_options.quality().get())
    {
    case SkyOptions::QUALITY_HIGH:
    case SkyOptions::QUALITY_BEST:
        _useBruneton = true;
        _usePBR = true;
        break;
    case SkyOptions::QUALITY_MEDIUM:
    case SkyOptions::QUALITY_DEFAULT:
        _useONeil = true;
        _usePBR = true;
        break;
    case SkyOptions::QUALITY_LOW:
    default:
        _usePhong = true;
        break;
    }

    if (_useBruneton && Registry::capabilities().getGLSLVersion() < 4.30f)
    {
        OE_WARN << LC << "Bruneton lighting requires GL 4.3+, falling back on O'Neil" << std::endl;
        _useBruneton = false;
        _useONeil = true;
    }

    if (Registry::capabilities().supportsGLSL())
    {
        osg::StateSet* stateset = this->getOrCreateStateSet();

        _lightPosUniform = new osg::Uniform(osg::Uniform::FLOAT_VEC3, "atmos_v3LightDir");
        _lightPosUniform->set(lightPos / lightPos.length());
        stateset->addUniform(_lightPosUniform.get());

        stateset->setDefine(OE_LIGHTING_DEFINE, osg::StateAttribute::ON);

        // make the uniforms and the terrain lighting shaders.
        makeSceneLighting();

        // make the sky elements (don't change the order here)
        makeAtmosphere(_ellipsoid);

        makeSun();

        makeMoon();

        makeStars();

        // Decorations are shown by default, so hide them as needed
        if (_options.sunVisible() == false) setSunVisible(false);
        if (_options.moonVisible() == false) setMoonVisible(false);
        if (_options.starsVisible() == false) setStarsVisible(false);
        if (_options.atmosphereVisible() == false) setAtmosphereVisible(false);
    }

    // Update everything based on the date/time.
    onSetDateTime();
}

osg::BoundingSphere
SimpleSkyNode::computeBound() const
{
    return osg::BoundingSphere();
}

void
SimpleSkyNode::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == nv.CULL_VISITOR && _cullContainer.valid())
    {
        // Generate LUTs on the first pass
        if (_useBruneton && !_eb_drawable.valid())
        {
            std::lock_guard<std::mutex> lock(_eb_mutex);
            if (!_eb_drawable.valid())
            {
                _eb_drawable = new Bruneton::ComputeDrawable(
                    _innerRadius,
                    _outerRadius,
                    _options.quality() == SkyOptions::QUALITY_BEST);

                _eb_drawable->accept(nv);
            }
        }

        osgUtil::CullVisitor* cv = Culling::asCullVisitor(nv);

        bool needToRestoreInheritanceMask =
            (cv->getInheritanceMask() & osg::CullSettings::CLAMP_PROJECTION_MATRIX_CALLBACK) > 0;

        // If there's a custom projection matrix clamper installed, remove it temporarily. 
        // We dont' want it mucking with our sky elements. 
        osg::ref_ptr<osg::CullSettings::ClampProjectionMatrixCallback> cb =
            cv->getClampProjectionMatrixCallback();

        cv->setClampProjectionMatrixCallback(nullptr);

        _cullContainer->accept(nv);

        // restore a custom clamper. 
        if (cb.valid())
        {
            cv->setClampProjectionMatrixCallback(cb.get());
        }

        if (needToRestoreInheritanceMask)
        {
            cv->setInheritanceMask(
                cv->getInheritanceMask() | osg::CullSettings::CLAMP_PROJECTION_MATRIX_CALLBACK);
        }
    }

    else if (nv.getVisitorType() == nv.UPDATE_VISITOR)
    {
        if (_useONeil && _options.atmosphericLighting() == true &&
            _options.environmentLighting() == true && _environmentView.valid())
        {
            if (!_environment.valid())
            {
                auto terrain = osgEarth::findTopMostNodeOfType<TerrainEngineNode>(this);
                if (terrain)
                {
                    _environment = new SkyEnvironment;
                    if (!_environment->attach(getOrCreateStateSet(), terrain->getResources()))
                    {
                        OE_WARN << LC << "No texture units available for sky environment lighting" << std::endl;
                        _options.environmentLighting() = false;
                        _environment = nullptr;
                    }
                }
            }
            if (_environment.valid())
            {
                const auto& p = _light->getPosition();
                _environment->update(_environmentView.get(), osg::Vec3d(p.x(),p.y(),p.z()));
                if (_environment->isReady())
                    getOrCreateStateSet()->setDefine("OE_SKY_ENVIRONMENT");
            }
        }

        // If the Bruneton compute shaders are finished, set up the rendering statesets.
        if (_useBruneton &&
            !_eb_initialized &&
            _eb_drawable.valid())
        {
            auto eb = static_cast<Bruneton::ComputeDrawable*>(_eb_drawable.get());
            if (eb->isReady())
            {
                TerrainEngineNode* terrain = osgEarth::findTopMostNodeOfType<TerrainEngineNode>(this);
                if (terrain)
                {
                    osg::StateSet* groundStateSet =
                        _options.atmosphericLighting() == true ?
                        getOrCreateStateSet() : nullptr;

                    bool ok = eb->populateRenderingStateSets(
                        groundStateSet,
                        _atmosphere->getOrCreateStateSet(),
                        terrain->getResources());

                    _eb_initialized = true;

                    if (ok && groundStateSet && _options.environmentLighting() == true)
                        groundStateSet->setDefine("OE_SKY_ENVIRONMENT");

                    if (!ok)
                    {
                        OE_WARN << LC << "Bruneton lighting failed to initialize" << std::endl;
                        _eb_drawable = nullptr;
                    }
                }

                ADJUST_UPDATE_TRAV_COUNT(this, -1);
            }
        }
    }

    SkyNode::traverse(nv);
}

void
SimpleSkyNode::releaseGLObjects(osg::State* state) const
{
    SkyNode::releaseGLObjects(state);
    if (_cullContainer.valid())
        _cullContainer->releaseGLObjects(state);
}

void
SimpleSkyNode::resizeGLObjectBuffers(unsigned maxSize)
{
    SkyNode::resizeGLObjectBuffers(maxSize);
    if (_cullContainer.valid())
        _cullContainer->resizeGLObjectBuffers(maxSize);
}

void
SimpleSkyNode::onSetEphemeris()
{
    // trigger the date/time update.
    onSetDateTime();
}

void
SimpleSkyNode::onSetDateTime()
{
    osg::View* view = 0L;
    const DateTime& dt = getDateTime();

    bool useECI = _options.coordinateSystem() == SkyOptions::COORDSYS_ECI;

    CelestialBody sun = getEphemeris()->getSunPosition(dt);
    setSunPosition(useECI ? sun.eci : sun.geocentric);

    CelestialBody moon = getEphemeris()->getMoonPosition(dt);
    setMoonPosition(useECI ? moon.eci : moon.geocentric);

    // position the stars:
    double time_r = dt.hours() / 24.0; // 0..1
    double rot_z = -osg::PI + TWO_PI * time_r;

    if (_starsXform.valid())
        _starsXform->setMatrix(osg::Matrixd::rotate(-rot_z, 0, 0, 1));
}

void
SimpleSkyNode::attach(osg::View* view, int lightNum)
{
    if (!view || !_light.valid())
        return;

    _light->setLightNum(lightNum);
    _environmentView = view;

    // black background
    view->getCamera()->setClearColor(osg::Vec4(0, 0, 0, 1));

    // install the light in the view (so other modules can access it, like shadowing)
    view->setLight(_light.get());

    // Tell the view not to automatically include a light.
    view->setLightingMode(osg::View::NO_LIGHT);


    onSetDateTime();
}

void
SimpleSkyNode::setSunPosition(const osg::Vec3d& pos)
{
    osg::Vec3d npos = pos;
    _light->setPosition(osg::Vec4(npos, 0.0f)); // directional light
    npos.normalize();

    //OE_NOTICE << pos.x() << ", " << pos.y() << ", " << pos.z() << std::endl;

    if (_lightPosUniform.valid())
    {
        _lightPosUniform->set(osg::Vec3(npos));
    }

    if (_sunXform.valid())
    {
        _sunXform->setMatrix(osg::Matrix::translate(pos));

        if (_moonXform.valid())
        {
            osg::Vec3d moonToSun = _sunXform->getMatrix().getTrans() - _moonXform->getMatrix().getTrans();
            moonToSun.normalize();
            _moonXform->getOrCreateStateSet()->getOrCreateUniform("moonToSun", osg::Uniform::FLOAT_VEC3)->set(osg::Vec3f(moonToSun));
        }
    }
}

void
SimpleSkyNode::setMoonPosition(const osg::Vec3d& pos)
{
    if (_moonXform.valid())
    {
        _moonXform->setMatrix(osg::Matrixd::translate(pos));

        if (_sunXform.valid())
        {
            osg::Vec3d moonToSun = _sunXform->getMatrix().getTrans() - _moonXform->getMatrix().getTrans();
            moonToSun.normalize();
            _moonXform->getOrCreateStateSet()->getOrCreateUniform("moonToSun", osg::Uniform::FLOAT_VEC3)->set(osg::Vec3f(moonToSun));
        }
    }
}

void
SimpleSkyNode::onSetStarsVisible()
{
    if (_starsXform.valid())
        _starsXform->setNodeMask(getStarsVisible() ? ~0 : 0);
}

void
SimpleSkyNode::onSetMoonVisible()
{
    if (_moonXform.valid())
        _moonXform->setNodeMask(getMoonVisible() ? ~0 : 0);
}

void
SimpleSkyNode::onSetSunVisible()
{
    if (_sunXform.valid())
        _sunXform->setNodeMask(getSunVisible() ? ~0 : 0);
}

void
SimpleSkyNode::onSetAtmosphereVisible()
{
    if (_atmosphere.valid())
        _atmosphere->setNodeMask(getAtmosphereVisible() ? ~0 : 0);
}

void
SimpleSkyNode::makeSceneLighting()
{
    // installs the main uniforms and the shaders that will light the subgraph (terrain).
    osg::StateSet* stateset = this->getOrCreateStateSet();

    VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
    vp->setName("SimpleSky Scene Lighting");

    stateset->getOrCreateUniform("atmos_fInnerRadius", osg::Uniform::FLOAT)->set(_innerRadius);
    stateset->getOrCreateUniform("atmos_fOuterRadius", osg::Uniform::FLOAT)->set(_outerRadius);

    if (_options.atmosphericLighting() == true)
    {
        Shaders pkg;

        if (_useBruneton)
        {
            if (_options.quality() == SkyOptions::QUALITY_HIGH)
                OE_INFO << LC << "Using Bruneton per-vertex lighting" << std::endl;
            else
                OE_INFO << LC << "Using Bruneton per-fragment lighting" << std::endl;
            // nothing to do here since we have to run precomputation first

            //TODO: api
            stateset->getOrCreateUniform("atmos_haze_cutoff", osg::Uniform::FLOAT)->set(0.0f);
            stateset->getOrCreateUniform("atmos_haze_strength", osg::Uniform::FLOAT)->set(1.0f);
            if (_options.environmentLighting() == true)
                stateset->getOrCreateUniform("oe_sky_groundReflectance", osg::Uniform::FLOAT)->set(_options.groundReflectance().get());
        }
        else if (_useONeil)
        {
            pkg.load(vp, pkg.Ground_ONeil_Vert);
            OE_INFO << LC << "Using O'Neil lighting" << std::endl;
            pkg.load(vp, pkg.Ground_ONeil_Frag);
            stateset->getOrCreateUniform("oe_sky_ambientBoostFactor", osg::Uniform::FLOAT)->set(_options.daytimeAmbientBoost().get());
            stateset->getOrCreateUniform("oe_sky_maxAmbientIntensity", osg::Uniform::FLOAT)->set(_options.maxAmbientIntensity().get());
        }
        else if (_usePhong)
        {
            _phong = new PhongLightingEffect();
            _phong->attach(stateset);
            OE_INFO << LC << "Using Phong lighting" << std::endl;
        }
    }

    else
    {
        _phong = new PhongLightingEffect();
        _phong->attach(stateset);
        OE_INFO << LC << "Using Phong lighting" << std::endl;
    }

    stateset->getOrCreateUniform("oe_sky_exposure", osg::Uniform::FLOAT)->set(
        _options.exposure().value());
    if ((_useONeil || _useBruneton) && _options.atmosphericLighting() == true && _options.environmentLighting() == true)
        stateset->getOrCreateUniform("oe_sky_iblStrength", osg::Uniform::FLOAT)->set(1.0f);
}

void
SimpleSkyNode::makeAtmosphere(const Ellipsoid& em)
{
    // create some skeleton geometry to shade:
#if 1
    osg::Geometry* drawable = s_makeEllipsoidGeometry(em, _outerRadius, false);
    drawable->setName("Atmosphere Drawable");
#else
    // TEST!
    osg::Geometry* drawable = new osg::Geometry();
    drawable->setName("SimpleSky");
    drawable->setCullingActive(false);
    drawable->setName("Atmosphere Drawable");
    drawable->setUseVertexBufferObjects(true);
    osg::Vec3Array* verts = new osg::Vec3Array(osg::Array::BIND_PER_VERTEX, 4);
    drawable->setVertexArray(verts);
    const unsigned short elements[6] = { 0,2,1,2,0,3 };
    drawable->addPrimitiveSet(new osg::DrawElementsUShort(GL_TRIANGLES, 6, elements));
#endif

    // disable wireframe/point rendering on the atmosphere, since it is distracting.
    if (_options.allowWireframe() == false)
    {
        drawable->getOrCreateStateSet()->setAttributeAndModes(
            new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::FILL),
            osg::StateAttribute::PROTECTED);
    }

    // configure the state set:
    osg::StateSet* atmosSet = drawable->getOrCreateStateSet();
    GLUtils::setLighting(atmosSet, osg::StateAttribute::OFF);
    atmosSet->setAttributeAndModes(new osg::CullFace(osg::CullFace::FRONT), osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
    atmosSet->setAttributeAndModes(new osg::Depth(osg::Depth::LESS, 0, 1, false)); // no depth write
    atmosSet->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS, 0, 1, false)); // no zbuffer
    atmosSet->setAttributeAndModes(new osg::BlendFunc(GL_ONE, GL_ONE), osg::StateAttribute::ON);

    // first install the atmosphere rendering shaders.
    VirtualProgram* vp = VirtualProgram::getOrCreate(atmosSet);
    vp->setName("SimpleSky Atmosphere");
    vp->setInheritShaders(false);

    if (!_useBruneton)
    {
        Shaders pkg;
        pkg.load(vp, pkg.Atmosphere_Vert);
        pkg.load(vp, pkg.Atmosphere_Frag);
    }

    // A nested camera isolates the projection matrix calculations so the node won't 
    // affect the clip planes in the rest of the scene.
    osg::Camera* cam = new osg::Camera();
    cam->setName("Atmosphere Cam");
    cam->getOrCreateStateSet()->setRenderBinDetails(BIN_ATMOSPHERE, "RenderBin");
    cam->setRenderOrder(osg::Camera::NESTED_RENDER);
    cam->setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES);
    cam->addChild(drawable);

    _cullContainer->addChild(cam);

    _atmosphere = drawable;
}

void
SimpleSkyNode::makeSun()
{
    osg::Billboard* sun = new osg::Billboard();
    sun->setName("Sun billboard");
    sun->setMode(osg::Billboard::POINT_ROT_EYE);
    sun->setNormal(osg::Vec3(0, 0, 1));

    const double zoomFactor = 80.0; // to account for the solare glare
    const double sunRadius = 695508000.0; // radius of the run in meters
    sun->addDrawable(s_makeDiscGeometry(sunRadius * zoomFactor));

    osg::StateSet* set = sun->getOrCreateStateSet();
    set->setMode(GL_BLEND, 1);

    // configure the stateset
    set->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS, 0, 1, false), osg::StateAttribute::ON);

    // create shaders
    Shaders pkg;
    osg::Program* program = new osg::Program();

    osg::Shader* vs = new osg::Shader(
        osg::Shader::VERTEX,
        ShaderLoader::load(pkg.Sun_Vert, pkg));
    program->addShader(vs);

    osg::Shader* fs = new osg::Shader(
        osg::Shader::FRAGMENT,
        ShaderLoader::load(pkg.Sun_Frag, pkg));
    program->addShader(fs);

    set->setAttributeAndModes(program, osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);

    // A nested camera isolates the projection matrix calculations so the node won't 
    // affect the clip planes in the rest of the scene.
    osg::Camera* cam = new osg::Camera();
    cam->setName("Sun cam");
    cam->getOrCreateStateSet()->setRenderBinDetails(BIN_SUN, "RenderBin");
    cam->setRenderOrder(osg::Camera::NESTED_RENDER);
    cam->setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES);
    cam->addChild(sun);

    _sun = cam;

    // make the sun's transform:
    _sunXform = new osg::MatrixTransform();
    _sunXform->setName("Sun xform");
    _sunXform->setMatrix(osg::Matrix::translate(
        _sunDistance * _light->getPosition().x(),
        _sunDistance * _light->getPosition().y(),
        _sunDistance * _light->getPosition().z()));
    _sunXform->addChild(_sun.get());

    _cullContainer->addChild(_sunXform.get());
}

void
SimpleSkyNode::makeMoon()
{
    Ellipsoid em(1738140.0, 1735970.0);

    osg::Geometry* moonDrawable = s_makeEllipsoidGeometry(em, em.getRadiusEquator() * _options.moonScale().get(), true);
    moonDrawable->setName("Moon drawable");
    osg::StateSet* stateSet = moonDrawable->getOrCreateStateSet();

    osg::ref_ptr<osg::Image> image = _options.moonImageURI()->getImage();
    if (!image.valid())
    {
        OE_WARN << LC << "Failed to load moon texture from " << _options.moonImageURI()->full() << std::endl;
    }

    if (image.valid())
    {
        osg::Texture2D* texture = new osg::Texture2D(image.get());
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        texture->setResizeNonPowerOfTwoHint(false);
        texture->setUnRefImageDataAfterApply(false);
        stateSet->setTextureAttribute(0, texture, osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
        stateSet->addUniform(new osg::Uniform("moonTex", 0));

#ifdef OSG_GL3_AVAILABLE
        // Adjust for loss of GL_LUMINANCE in glTexture2D's format parameter.  OSG handles the texture's internal format,
        // but the format parameter comes from the image's pixel format field.
        if (image.valid() && image->getPixelFormat() == GL_LUMINANCE)
        {
            image->setPixelFormat(GL_RED);
            // Swizzle the RGB all to RED in order to match previous GL_LUMINANCE behavior
            texture->setSwizzle(osg::Vec4i(GL_RED, GL_RED, GL_RED, GL_ONE));
        }
#endif
    }

    osg::Vec4Array* colors = new osg::Vec4Array(osg::Array::BIND_OVERALL, 1);
    moonDrawable->setColorArray(colors);
    (*colors)[0] = osg::Vec4(1, 1, 1, 1);

    stateSet->setAttributeAndModes(new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON);
    stateSet->setRenderBinDetails(BIN_MOON, "RenderBin");
    stateSet->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS, 0, 1, false), osg::StateAttribute::ON);
    stateSet->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON);


    // create shaders
    Shaders pkg;
    osg::Program* program = new osg::Program();
    osg::Shader* vs = new osg::Shader(
        osg::Shader::VERTEX,
        ShaderLoader::load(pkg.Moon_Vert, pkg));
    program->addShader(vs);
    osg::Shader* fs = new osg::Shader(
        osg::Shader::FRAGMENT,
        ShaderLoader::load(pkg.Moon_Frag, pkg));
    program->addShader(fs);
    stateSet->setAttributeAndModes(program, osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);

    // A nested camera isolates the projection matrix calculations so the node won't 
    // affect the clip planes in the rest of the scene.
    osg::Camera* cam = new osg::Camera();
    cam->setName("Moon cam");
    cam->getOrCreateStateSet()->setRenderBinDetails(BIN_MOON, "RenderBin");
    cam->setRenderOrder(osg::Camera::NESTED_RENDER);
    cam->setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES);
    cam->addChild(moonDrawable);

    _moon = cam;

    // make the moon's transform:
    CelestialBody moon = getEphemeris()->getMoonPosition(getDateTime());

    _moonXform = new osg::MatrixTransform();
    _moonXform->setName("Moon xform");
    _moonXform->setMatrix(osg::Matrix::translate(moon.geocentric));
    _moonXform->addChild(_moon.get());

    //_moonXform->getOrCreateStateSet()->addUniform(new osg::Uniform("moonToSun", osg::Vec3f(0,0,1)));

    _cullContainer->addChild(_moonXform.get());

    //If we couldn't load the moon texture, turn the moon off
    if (!image)
    {
        OE_INFO << LC << "Couldn't load moon texture, add osgEarth's data directory your OSG_FILE_PATH" << std::endl;
        //_moonXform->setNodeMask( 0 );
        setMoonVisible(false);
    }
}

SimpleSkyNode::StarData::StarData(std::stringstream& ss) :
    right_ascension(0.0),
    declination(0.0),
    magnitude(0.0)
{
    std::getline(ss, name, ',');
    std::string buff;
    std::getline(ss, buff, ',');
    std::stringstream(buff) >> right_ascension;
    std::getline(ss, buff, ',');
    std::stringstream(buff) >> declination;
    std::getline(ss, buff, '\n');
    std::stringstream(buff) >> magnitude;
}

void
SimpleSkyNode::makeStars()
{
    // make the stars' transform:
    _starsXform = new osg::MatrixTransform();
    _starsXform->setName("Stars xform");

    _starRadius = 20000.0 * (_sunDistance > 0.0 ? _sunDistance : _outerRadius);

    URI starImageURI;

    if (_options.starImageURI().isSet())
    {
        starImageURI = _options.starImageURI().get();
    }
    else
    {
        const char* temp = ::getenv("OSGEARTH_STAR_IMAGE_URI");
        if (temp)
        {
            starImageURI = std::string(temp);
        }
    }

    if (!starImageURI.empty())
    {
        auto image = starImageURI.getImage();
        if (image)
        {
            Ellipsoid stars_em(_starRadius, _starRadius);
            _stars = buildStarImageGeometry(stars_em, image);
        }
        else
        {
            OE_WARN << LC << "Failed to load starfield image from " << starImageURI.full() << std::endl;
        }
    }

    if (!_stars.valid())
    {
        const char* magEnv = ::getenv("OSGEARTH_MIN_STAR_MAGNITUDE");
        if (magEnv)
            _minStarMagnitude = as<float>(std::string(magEnv), -1.0f);
        else
            _minStarMagnitude = -1.0f;

        std::vector<StarData> stars;

        if (_options.starFile().isSet())
        {
            if (parseStarFile(*_options.starFile(), stars) == false)
            {
                OE_WARN << LC
                    << "Unable to use star field defined in \"" << *_options.starFile()
                    << "\", using default star data instead." << std::endl;
            }
        }

        if (stars.empty())
        {
            getDefaultStars(stars);
        }

        _stars = buildStarGeometry(stars);
    }

    _stars->setName("Stars drawable");
    _starsXform->addChild(_stars.get());

    _cullContainer->addChild(_starsXform.get());
}

osg::Node*
SimpleSkyNode::buildStarGeometry(const std::vector<StarData>& stars)
{
    double minMag = DBL_MAX, maxMag = DBL_MIN;

    PointDrawable* drawable = new PointDrawable();
    drawable->setPointSize(_options.starSize().get());
    drawable->allocate(stars.size());

    for (unsigned p = 0; p < stars.size(); ++p)
    {
        const StarData& star = stars[p];

        osg::Vec3d v = getEphemeris()->getECEFfromRADecl(
            star.right_ascension,
            star.declination,
            _starRadius);

        drawable->setVertex(p, v);

        if (star.magnitude < minMag) minMag = star.magnitude;
        if (star.magnitude > maxMag) maxMag = star.magnitude;
    }

    for (unsigned p = 0; p < stars.size(); ++p)
    {
        const StarData& star = stars[p];
        float c = (star.magnitude - minMag) / (maxMag - minMag);
        drawable->setColor(p, osg::Vec4(c, c, c, 1.0f));
    }

    drawable->finish();

    osg::StateSet* sset = drawable->getOrCreateStateSet();

    const osgEarth::Capabilities& caps = osgEarth::Registry::capabilities();

    VirtualProgram* vp = VirtualProgram::getOrCreate(drawable->getOrCreateStateSet());
    vp->setName("SimpleSky Stars");
    Shaders shaders;
    shaders.load(vp, shaders.Stars_Vert);
    shaders.load(vp, shaders.Stars_Frag);
    vp->setInheritShaders(false);

    sset->setRenderBinDetails(BIN_STARS, "RenderBin");
    sset->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS, 0, 1, false), osg::StateAttribute::ON);
    sset->setMode(GL_BLEND, 1);

    // A separate camera isolates the projection matrix calculations.
    osg::Camera* cam = new osg::Camera();
    cam->setName("Stars cam");
    cam->getOrCreateStateSet()->setRenderBinDetails(BIN_STARS, "RenderBin");
    cam->setRenderOrder(osg::Camera::NESTED_RENDER);

    cam->addChild(drawable);
    return cam;
}


osg::Node*
SimpleSkyNode::buildStarImageGeometry(const Ellipsoid& em, osg::Image* image)
{
    // create some skeleton geometry to shade:
    osg::Geometry* drawable = s_makeEllipsoidGeometry(em, _starRadius, true);
    drawable->setName("Starfield Drawable");

    // disable wireframe/point rendering on the stars
    if (_options.allowWireframe() == false)
    {
        drawable->getOrCreateStateSet()->setAttributeAndModes(
            new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::FILL),
            osg::StateAttribute::PROTECTED);
    }

    // configure the state set:
    osg::StateSet* ss = drawable->getOrCreateStateSet();
    GLUtils::setLighting(ss, osg::StateAttribute::OFF);
    ss->setAttributeAndModes(new osg::CullFace(osg::CullFace::FRONT), osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
    ss->setAttributeAndModes(new osg::Depth(osg::Depth::ALWAYS, 0, 1, false)); // no zbuffer
    ss->setAttributeAndModes(new osg::BlendFunc(GL_ONE, GL_ONE), osg::StateAttribute::ON);

    auto texture = new osg::Texture2D(image);
    texture->setWrap(texture->WRAP_S, texture->REPEAT);
    texture->setWrap(texture->WRAP_T, texture->REPEAT);
    ss->setAttribute(texture, 1);
    ss->addUniform(new osg::Uniform("starTex", 0));

    const char* vs = R"(
        out vec3 star_data;
        uniform mat4 osg_ViewMatrixInverse;
        uniform vec3 atmos_v3LightDir;
        float stars_remap(float V,float L,float H,float A,float B) {
            float vr = (clamp(V, L, H)-L)/(H-L); 
            return A + vr * (B-A); 
        }
        void stars_vert(inout vec4 vert) {
            vert.z = vert.w;
            star_data.st = gl_MultiTexCoord0.st;
            vec3 eye = osg_ViewMatrixInverse[3].xyz;
            float hae = length(eye) - 6378137.0; 
            float highness = stars_remap(hae, 25000.0, 150000.0, 0.0, 1.0);
            eye = normalize(eye); 
            // darkness: visibility increase as the sun goes around the other side of the earth
            float cosa = dot(eye, atmos_v3LightDir);
            float darkness = 1.0-stars_remap(dot(eye, atmos_v3LightDir), -0.25, 0.0, 0.0, 1.0); 
            star_data.z = clamp(highness + darkness, 0.0, 1.0);
        }
    )";
    const char* fs = R"(
        in vec3 star_data;
        uniform sampler2D starTex;
        void stars_frag(inout vec4 color) { 
            color = texture(starTex, star_data.st);
            color *= star_data.z;
        }
    )";

    auto vp = VirtualProgram::getOrCreate(ss);
    vp->setName("SimpleSky Stars");
    vp->setInheritShaders(false);
    vp->setFunction("stars_vert", vs, vp->LOCATION_VERTEX_CLIP);
    vp->setFunction("stars_frag", fs, vp->LOCATION_FRAGMENT_COLORING);

    //auto program = new osg::Program();
    //program->addShader(new osg::Shader(osg::Shader::VERTEX, vs));
    //program->addShader(new osg::Shader(osg::Shader::FRAGMENT, fs));
    //ss->setAttribute(program, 1);

    // A separate camera isolates the projection matrix calculations.
    auto cam = new osg::Camera();
    cam->setName("Stars cam");
    cam->getOrCreateStateSet()->setRenderBinDetails(BIN_STARS, "RenderBin");
    cam->setRenderOrder(osg::Camera::NESTED_RENDER);
    cam->addChild(drawable);
    return cam;
}

void
SimpleSkyNode::getDefaultStars(std::vector<StarData>& out_stars)
{
    out_stars.clear();

    for (const char** sptr = Util::s_defaultStarData; *sptr; sptr++)
    {
        std::stringstream ss(*sptr);
        out_stars.push_back(StarData(ss));

        if (out_stars[out_stars.size() - 1].magnitude < _minStarMagnitude)
            out_stars.pop_back();
    }
}

bool
SimpleSkyNode::parseStarFile(const std::string& starFile, std::vector<StarData>& out_stars)
{
    out_stars.clear();

    std::fstream in(starFile.c_str());
    if (!in)
    {
        OE_WARN << "Warning: Unable to open file star file \"" << starFile << "\"" << std::endl;
        return false;
    }

    while (!in.eof())
    {
        std::string line;

        std::getline(in, line);
        if (in.eof())
            break;

        if (line.empty() || line[0] == '#')
            continue;

        std::stringstream ss(line);
        out_stars.push_back(StarData(ss));

        if (out_stars[out_stars.size() - 1].magnitude < _minStarMagnitude)
            out_stars.pop_back();
    }

    in.close();

    return true;
}
