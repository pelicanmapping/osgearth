/* osgEarth
* Copyright 2008-2012 Pelican Mapping
* MIT License
*/
#include "VegetationLayer"
#include "ProceduralShaders"

#include <osgEarth/NoiseTextureFactory>
#include <osgEarth/VirtualProgram>
#include <osgEarth/CameraUtils>
#include <osgEarth/LineDrawable>
#include <osgEarth/NodeUtils>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/Math>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/Metrics>
#include <osgEarth/GLUtils>
#include <osgEarth/Chonk>
#include <osgEarth/rtree.h>
#include <osgEarth/TerrainConstraintLayer>
#include <osgEarth/AnnotationUtils>
#include <osgEarth/Threading>

#include <osg/BlendFunc>
#include <osg/Multisample>
#include <osg/Texture2D>
#include <osg/Version>
#include <osg/ComputeBoundsVisitor>
#include <osg/MatrixTransform>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgDB/FileNameUtils>
#include <osgUtil/CullVisitor>
#include <osgUtil/Optimizer>
#include <osgUtil/SmoothingVisitor>

#include <cstdlib> // getenv
#include <random>

#define LC "[VegetationLayer] " << getName() << ": "

#define JOB_ARENA_VEGETATION "oe.vegetation"

#define OE_DEVEL OE_DEBUG

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

#ifndef GL_SAMPLE_ALPHA_TO_COVERAGE_ARB
#define GL_SAMPLE_ALPHA_TO_COVERAGE_ARB   0x809E
#endif

using namespace osgEarth;
using namespace osgEarth::Procedural;

REGISTER_OSGEARTH_LAYER(vegetation, VegetationLayer);

//........................................................................

namespace
{
    osg::ref_ptr<osg::Node> createDebugBound(osg::Drawable* d)
    {
        auto& bbox = d->getBoundingBox();

        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry();
        geom->setUseVertexBufferObjects(true);
        geom->setUseDisplayList(false);

        const osg::Vec3f verts[8] = {
            { bbox.xMin(), bbox.yMin(), bbox.zMin() },
            { bbox.xMax(), bbox.yMin(), bbox.zMin() },
            { bbox.xMax(), bbox.yMax(), bbox.zMin() },
            { bbox.xMin(), bbox.yMax(), bbox.zMin() },
            { bbox.xMin(), bbox.yMin(), bbox.zMax() },
            { bbox.xMax(), bbox.yMin(), bbox.zMax() },
            { bbox.xMax(), bbox.yMax(), bbox.zMax() },
            { bbox.xMin(), bbox.yMax(), bbox.zMax() }
        };
        geom->setVertexArray(new osg::Vec3Array(osg::Array::BIND_PER_VERTEX, 8, verts));

        const osg::Vec4f colors[1] = {
            {1,1,1,1}
        };
        geom->setColorArray(new osg::Vec4Array(osg::Array::BIND_OVERALL, 1, colors));

        const std::uint8_t indices[24] = {
            0,1, 1,2, 2,3, 3,0,
            4,5, 5,6, 6,7, 7,4,
            0,4, 1,5, 2,6, 3,7
        };
        geom->addPrimitiveSet(new osg::DrawElementsUByte(GL_LINES, 24, indices));

        return geom;
    }
}

//........................................................................

#define GROUP_TREES "trees"
#define GROUP_BUSHES "bushes"
#define GROUP_UNDERGROWTH "undergrowth"

Config
VegetationLayer::Options::getConfig() const
{
    Config conf = PatchLayer::Options::getConfig();
    biomeLayer().set(conf, "biomes_layer");

    conf.set("alpha_to_coverage", alphaToCoverage());
    conf.set("impostor_low_angle", impostorLowAngle());
    conf.set("impostor_high_angle", impostorHighAngle());
    conf.set("far_lod_scale", farLODScale());
    conf.set("near_lod_scale", nearLODScale());
    conf.set("lod_transition_padding", lodTransitionPadding());
    conf.set("use_impostor_normal_maps", useImpostorNormalMaps());
    conf.set("use_impostor_pbr_maps", useImpostorPBRMaps());
    conf.set("max_texture_size", maxTextureSize());
    conf.set("render_bin_number", renderBinNumber());
    conf.set("threads", threads());

    Config layers("layers");
    for (auto group_name : { GROUP_TREES, GROUP_BUSHES, GROUP_UNDERGROWTH })
    {
        auto group_conf = group(group_name).getConfig();
        if (!group_conf.empty())
        {
            layers.add(group_name, group_conf);
        }
    }
    if (!layers.empty())
        conf.add(layers);

    return conf;
}

namespace
{
    void fromGroupConf(
        const std::string& name,
        const Config& group_conf,
        VegetationLayer::Options& options)
    {
        if (group_conf.empty())
            return;
        VegetationLayer::Options::Group& group = options.groups()[name];
        group_conf.get("enabled", group.enabled());
        group_conf.get("max_range", group.maxRange());
        group_conf.get("instances_per_sqkm", group.instancesPerSqKm());
        group_conf.get("lod", group.lod());
        group_conf.get("cast_shadows", group.castShadows());
        group_conf.get("overlap", group.overlap());
        group_conf.get("far_lod_scale", group.farLODScale());
        group_conf.get("alpha_cutoff", group.alphaCutoff());
        group_conf.get("clamping_resolution", group.clampingResolution());
        if (group_conf.value("lod") == "auto") {
            group.lod() = 0;
        }
    }

    template<typename T>
    inline ChonkDrawable* asChonkDrawable(const osg::ref_ptr<T>& value)
    {
        return static_cast<ChonkDrawable*>(value.get());
    }

    bool inConstrainedRegion(double x, double y, const std::vector<MeshConstraint>& constraints)
    {
        for (auto& con : constraints)
        {
            for (auto& feature : con.features)
            {
                if (feature->getGeometry()->contains2D(x, y))
                {
                    return true;
                }
            }
        }
        return false;
    }
}

void
VegetationLayer::Options::fromConfig(const Config& conf)
{
    biomeLayer().get(conf, "biomes_layer");

    conf.get("alpha_to_coverage", alphaToCoverage());
    conf.get("impostor_low_angle", impostorLowAngle());
    conf.get("impostor_high_angle", impostorHighAngle());
    conf.get("far_lod_scale", farLODScale());
    conf.get("near_lod_scale", nearLODScale());
    conf.get("lod_transition_padding", lodTransitionPadding());
    conf.get("use_impostor_normal_maps", useImpostorNormalMaps());
    conf.get("use_impostor_pbr_maps", useImpostorPBRMaps());
    conf.get("max_texture_size", maxTextureSize());
    conf.get("render_bin_number", renderBinNumber());
    conf.get("threads", threads());

    // some nice default group settings
    groups()[GROUP_TREES].lod().setDefault(14);
    groups()[GROUP_TREES].enabled().setDefault(true);
    groups()[GROUP_TREES].castShadows().setDefault(true);
    groups()[GROUP_TREES].maxRange().setDefault(FLT_MAX);
    groups()[GROUP_TREES].instancesPerSqKm().setDefault(16384);
    groups()[GROUP_TREES].overlap().setDefault(0.0f);
    groups()[GROUP_TREES].farLODScale().setDefault(1.0f);
    groups()[GROUP_TREES].alphaCutoff().setDefault(0.75f);
    fromGroupConf(GROUP_TREES, conf.child("layers").child(GROUP_TREES), *this);
    fromGroupConf(GROUP_TREES, conf.child("groups").child(GROUP_TREES), *this);

    groups()[GROUP_BUSHES].lod().setDefault(18);
    groups()[GROUP_BUSHES].enabled().setDefault(true);
    groups()[GROUP_BUSHES].castShadows().setDefault(false);
    groups()[GROUP_BUSHES].maxRange().setDefault(FLT_MAX);
    groups()[GROUP_BUSHES].instancesPerSqKm().setDefault(4096);
    groups()[GROUP_BUSHES].overlap().setDefault(0.0f);
    groups()[GROUP_BUSHES].farLODScale().setDefault(1.0f);
    groups()[GROUP_BUSHES].alphaCutoff().setDefault(0.35f);
    fromGroupConf(GROUP_BUSHES, conf.child("layers").child(GROUP_BUSHES), *this);
    fromGroupConf(GROUP_BUSHES, conf.child("groups").child(GROUP_BUSHES), *this);

    groups()[GROUP_UNDERGROWTH].lod().setDefault(19);
    groups()[GROUP_UNDERGROWTH].enabled().setDefault(true);
    groups()[GROUP_UNDERGROWTH].castShadows().setDefault(false);
    groups()[GROUP_UNDERGROWTH].maxRange().setDefault(FLT_MAX);
    groups()[GROUP_UNDERGROWTH].instancesPerSqKm().setDefault(500000);
    groups()[GROUP_UNDERGROWTH].overlap().setDefault(1.0f);
    groups()[GROUP_UNDERGROWTH].farLODScale().setDefault(2.0f);
    groups()[GROUP_UNDERGROWTH].alphaCutoff().setDefault(0.25f);
    fromGroupConf(GROUP_UNDERGROWTH, conf.child("layers").child(GROUP_UNDERGROWTH), *this);
    fromGroupConf(GROUP_UNDERGROWTH, conf.child("groups").child(GROUP_UNDERGROWTH), *this);
}

VegetationLayer::Options::Group&
VegetationLayer::Options::group(const std::string& name)
{
    auto iter = groups().find(name);
    return iter != groups().end() ? iter->second : _emptyGroup;
}

const VegetationLayer::Options::Group&
VegetationLayer::Options::group(const std::string& name) const
{
    auto iter = groups().find(name);
    return iter != groups().end() ? iter->second : _emptyGroup;
}

Config
VegetationLayer::Options::Group::getConfig() const
{
    Config conf;
    conf.set("enabled", enabled());
    conf.set("max_range", maxRange());
    conf.set("instances_per_sqkm", instancesPerSqKm());
    conf.set("lod", lod());
    conf.set("cast_shadows", castShadows());
    conf.set("overlap", overlap());
    conf.set("far_lod_scale", farLODScale());
    conf.set("alpha_cutoff", alphaCutoff());
    return conf;
}

//........................................................................

bool
VegetationLayer::LayerAcceptor::acceptLayer(
    osg::NodeVisitor& nv,
    const osg::Camera* camera) const
{
    // if this is a shadow camera and the layer is configured to cast shadows, accept it.
    if (CameraUtils::isShadowCamera(camera))
    {
        return _layer->getCastShadows();
    }

    // if this is a depth-pass camera (and not a shadow cam), reject it.
    if (CameraUtils::isDepthCamera(camera))
    {
        return false;
    }

    // otherwise accept the layer.
    return true;
}

bool
VegetationLayer::LayerAcceptor::acceptKey(const TileKey& key) const
{
    return _layer->hasEnabledGroupAtLOD(key.getLOD());
}

//........................................................................

void
VegetationLayer::init()
{
    PatchLayer::init();

    _biomeRevision = 0;
    setAcceptCallback(new LayerAcceptor(this));

    // make a 4-channel noise texture to use
    NoiseTextureFactory noise;
    _noiseTex = Texture::create(noise.create(256u, 4u));

    // set a static bounding box buffer that can account for geometry in this layer.
    // 25 meters on the sides and 50m on the top will be a rough guess for now.
    _buffer.set(-25, -25, 0, 25, 25, 50);
}

VegetationLayer::~VegetationLayer()
{
    close();
}

Status
VegetationLayer::openImplementation()
{
    // GL version requirement
    if (Capabilities::get().getGLSLVersion() < 4.6f ||  Capabilities::get().supportsNVGL() == false)
    {
        _renderingSupported = false;
        OE_WARN << LC << "Rendering will be disabled - requires NVIDIA GL 4.6" << std::endl;
        //return Status(Status::ResourceUnavailable, "Requires NVIDIA GL 4.6");
    }

    // Clamp the layer's max visible range the maximum range of the farthest
    // asset group. This will minimize the number of tiles sent to the
    // renderer and improve performance. Doing this here (in open) so the
    // user can close a layer, adjust parameters, and re-open if desired
    std::set<int> lods_used;
    float max_range = 0.0f;
    for (auto& group_iter : options().groups())
    {
        Options::Group& group = group_iter.second;
        max_range = std::max(max_range, group.maxRange().get());
        if (lods_used.insert(group.lod().get()).second == false)
        {
            return Status(Status::ConfigurationError,
                "Illegal configuration: multiple layers at the same LOD");
        }
    }
    max_range = std::min(max_range, getMaxVisibleRange());
    setMaxVisibleRange(max_range);

    _lastVisit.setFrameNumber(~0);

    return PatchLayer::openImplementation();
}

Status
VegetationLayer::closeImplementation()
{
    releaseGLObjects(nullptr);
    reset();

    return PatchLayer::closeImplementation();
}

Layer::Stats
VegetationLayer::reportStats() const
{
    Layer::Stats report;
    report.push_back({ "Resident tiles", std::to_string(_tiles.size()) });
    return report;
}

void
VegetationLayer::modifyTileBoundingBox(const TileKey& key, osg::BoundingBox& box) const
{
    //box.zMax() += 25.0f;
}

void
VegetationLayer::update(osg::NodeVisitor& nv)
{
    // this code will release memory after the layer's not been
    // used for a while.
    if (isOpen())
    {
        int df = nv.getFrameStamp()->getFrameNumber() - _lastVisit.getFrameNumber();
        double dt = nv.getFrameStamp()->getReferenceTime() - _lastVisit.getReferenceTime();

        if (dt > 5.0 && df > 60)
        {
            reset();

            if (getBiomeLayer())
            {
                getBiomeLayer()->getBiomeManager().flush();
            }

            releaseGLObjects(nullptr);

            OE_DEBUG << LC << "timed out for inactivity." << std::endl;
        }

        checkForNewAssets();

        if (_newAssets.available())
        {
            std::lock_guard<std::mutex> lock(_assets.mutex());
            _assets = std::move(_newAssets.release());
        }

        // do we need to activate A2C?
        updateAlphaToCoverage();
    }
}

void
VegetationLayer::dirty()
{
    _tiles.scoped_lock([this]()
        {
            _tiles.clear();
            _placeholders.clear();
        });

    _cameraState.scoped_lock([this]()
        {
            _cameraState.clear();
        });
}

void
VegetationLayer::setUseImpostorNormalMaps(bool value)
{
    if (value != getUseImpostorNormalMaps())
        options().useImpostorNormalMaps() = value;

    auto ss = getOrCreateStateSet();
    if (value == false)
        ss->setDefine("OE_CHONK_MAX_LOD_FOR_NORMAL_MAPS", "0");
    else
        ss->setDefine("OE_CHONK_MAX_LOD_FOR_NORMAL_MAPS", "99");
}

bool
VegetationLayer::getUseImpostorNormalMaps() const
{
    return options().useImpostorNormalMaps().get();
}

void
VegetationLayer::setUseImpostorPBRMaps(bool value)
{
    if (value != getUseImpostorPBRMaps())
        options().useImpostorPBRMaps() = value;

    auto ss = getOrCreateStateSet();
    if (value == false)
        ss->setDefine("OE_CHONK_MAX_LOD_FOR_PBR_MAPS", "0");
    else
        ss->setDefine("OE_CHONK_MAX_LOD_FOR_PBR_MAPS", "99");
}

bool
VegetationLayer::getUseImpostorPBRMaps() const
{
    return options().useImpostorPBRMaps().get();
}

void
VegetationLayer::setImpostorLowAngle(const Angle& value)
{
    if (value != getImpostorLowAngle())
        options().impostorLowAngle() = value;

    getOrCreateStateSet()->getOrCreateUniform(
        "oe_veg_bbd0", osg::Uniform::FLOAT)->set(
            clamp(1.0f - cosf(value.as(Units::RADIANS)), 0.0f, 1.0f));
}

const Angle&
VegetationLayer::getImpostorLowAngle() const
{
    return options().impostorLowAngle().get();
}

void
VegetationLayer::setImpostorHighAngle(const Angle& value)
{
    if (value != getImpostorHighAngle())
        options().impostorHighAngle() = value;

    getOrCreateStateSet()->getOrCreateUniform(
        "oe_veg_bbd1", osg::Uniform::FLOAT)->set(
            clamp(1.0f - cosf(value.as(Units::RADIANS)), 0.0f, 1.0f));
}

const Angle&
VegetationLayer::getImpostorHighAngle() const
{
    return options().impostorHighAngle().get();
}

void
VegetationLayer::setFarLODScale(float value)
{
    if (getFarLODScale() != value)
        options().farLODScale() = value;

    osg::Vec4f vector(
        options().nearLODScale().get(),
        options().farLODScale().get(),
        1.0f, 1.0f);

    osg::Uniform* u = new osg::Uniform("oe_lod_scale", vector);
    getOrCreateStateSet()->addUniform(u, osg::StateAttribute::OVERRIDE | 0x01);
}

float
VegetationLayer::getFarLODScale() const
{
    return options().farLODScale().get();
}

void
VegetationLayer::setNearLODScale(float value)
{
    if (getNearLODScale() != value)
        options().nearLODScale() = value;

    osg::Vec4f vector(
        options().nearLODScale().get(),
        options().farLODScale().get(),
        1.0f, 1.0f);

    osg::Uniform* u = new osg::Uniform("oe_lod_scale", vector);
    getOrCreateStateSet()->addUniform(u, osg::StateAttribute::OVERRIDE | 0x01);
}

float
VegetationLayer::getNearLODScale() const
{
    return options().nearLODScale().get();
}

void
VegetationLayer::setLODTransitionPadding(float value)
{
    value = clamp(value, 0.0f, 1.0f);

    if (getLODTransitionPadding() != value)
        options().lodTransitionPadding() = value;

    getOrCreateStateSet()->getOrCreateUniform(
        "oe_chonk_lod_transition_factor", osg::Uniform::FLOAT)->set(
            value);
}

float
VegetationLayer::getLODTransitionPadding() const
{
    return options().lodTransitionPadding().get();
}

void
VegetationLayer::setOverlapPercentage(
    const std::string& groupName,
    float value)
{
    options().group(groupName).overlap() = clamp(value, 0.0f, 1.0f);
}

float
VegetationLayer::getOverlapPercentage(
    const std::string& groupName) const
{
    return options().group(groupName).overlap().get();
}

void
VegetationLayer::setAlphaCutoff(
    const std::string& groupName,
    float value)
{
    options().group(groupName).alphaCutoff() = clamp(value, 0.0f, 1.0f);
}

float
VegetationLayer::getAlphaCutoff(
    const std::string& groupName) const
{
    return options().group(groupName).alphaCutoff().get();
}

void
VegetationLayer::setBiomeLayer(BiomeLayer* layer)
{
    _biomeLayer.setLayer(layer);
}

BiomeLayer*
VegetationLayer::getBiomeLayer() const
{
    return _biomeLayer.getLayer();
}

void
VegetationLayer::setLifeMapLayer(LifeMapLayer* layer)
{
    _lifeMapLayer.setLayer(layer);
}

LifeMapLayer*
VegetationLayer::getLifeMapLayer() const
{
    return _lifeMapLayer.getLayer();
}

void
VegetationLayer::setUseAlphaToCoverage(bool value)
{
    options().alphaToCoverage() = value;
}

bool
VegetationLayer::getUseAlphaToCoverage() const
{
    return options().alphaToCoverage().get();
}

void
VegetationLayer::updateAlphaToCoverage()
{
    if (getUseAlphaToCoverage() && _alphaToCoverageSupported && !_alphaToCoverageInstalled)
    {
        auto ss = getOrCreateStateSet();
        ss->setMode(GL_MULTISAMPLE, 1);
        ss->setDefine("OE_USE_ALPHA_TO_COVERAGE");
        ss->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB, 1);
        ss->setAttributeAndModes(new osg::BlendFunc(), 0 | osg::StateAttribute::OVERRIDE);
        _alphaToCoverageInstalled = true;
    }
    else if (!getUseAlphaToCoverage() && _alphaToCoverageInstalled)
    {
        auto ss = getOrCreateStateSet();
        ss->removeMode(GL_MULTISAMPLE);
        ss->removeDefine("OE_USE_ALPHA_TO_COVERAGE");
        ss->removeMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB);
        ss->removeAttribute(osg::StateAttribute::BLENDFUNC);
        ss->removeMode(GL_BLEND);
        _alphaToCoverageInstalled = false;
    }
}

bool
VegetationLayer::getCastShadows() const
{
    for (auto& iter : options().groups())
    {
        auto& group = iter.second;
        if (group.castShadows() == true)
            return true;
    }
    return false;
}

bool
VegetationLayer::hasEnabledGroupAtLOD(unsigned lod) const
{
    for (auto& iter : options().groups())
    {
        auto& group = iter.second;
        if (group.lod() == lod && group.enabled() == true)
            return true;
    }
    return false;
}

std::string
VegetationLayer::getGroupAtLOD(unsigned lod) const
{
    for (auto& iter : options().groups())
    {
        auto& group = iter.second;
        if (group.lod() == lod)
            return iter.first;
    }
    return std::string();
}

unsigned
VegetationLayer::getGroupLOD(const std::string& name) const
{
    return options().group(name).lod().get();
}

void
VegetationLayer::setMaxRange(const std::string& name, float value)
{
    options().groups()[name].maxRange() = value;
}

float
VegetationLayer::getMaxRange(const std::string& name) const
{
    return options().group(name).maxRange().get();
}

void
VegetationLayer::setEnabled(const std::string& name, bool value)
{
    options().groups()[name].enabled() = value;
}

bool
VegetationLayer::getEnabled(const std::string& name) const
{
    return options().group(name).enabled().get();
}

void
VegetationLayer::setMaxTextureSize(unsigned value)
{
    if (value != options().maxTextureSize().value())
    {
        if (getBiomeLayer())
        {
            options().maxTextureSize() = clamp(value, 1u, 63356u);
            auto arena = getBiomeLayer()->getBiomeManager().getTextures();
            arena->setMaxTextureSize(options().maxTextureSize().value());
        }
    }
}

void
VegetationLayer::setShowTileBoundingBoxes(bool value)
{
    _showTileBoundingBoxes = value;
}

bool
VegetationLayer::getShowTileBoundingBoxes() const
{
    return _showTileBoundingBoxes;
}

unsigned
VegetationLayer::getMaxTextureSize() const
{
    return options().maxTextureSize().get();
}

void
VegetationLayer::addedToMap(const Map* map)
{
    PatchLayer::addedToMap(map);

    if (!getLifeMapLayer())
        setLifeMapLayer(map->getLayer<LifeMapLayer>());

    if (!getBiomeLayer())
        setBiomeLayer(map->getLayer<BiomeLayer>());

    _map = map;

    if (getBiomeLayer() == nullptr)
    {
        setStatus(Status::ResourceUnavailable, "No Biomes layer available in the Map");
        return;
    }

    if (getLifeMapLayer() == nullptr)
    {
        setStatus(Status::ResourceUnavailable, "No LifeMap available in the Map");
        return;
    }
}

void
VegetationLayer::removedFromMap(const Map* map)
{
    PatchLayer::removedFromMap(map);
}

void
VegetationLayer::prepareForRendering(TerrainEngine* engine)
{
    if (!getBiomeLayer())
    {
        setStatus(Status::ResourceUnavailable, "Biome layer not available");
        return;
    }

    if (!getLifeMapLayer())
    {
        setStatus(Status::ResourceUnavailable, "LifeMap layer not available");
        return;
    }

    PatchLayer::prepareForRendering(engine);

    TerrainResources* res = engine->getResources();
    if (res)
    {
        // Compute LOD for each asset group if necessary.
        for (auto iter : options().groups())
        {
            Options::Group& group = iter.second;
            if (group.lod() == 0)
            {
                unsigned bestLOD = 0;
                for (unsigned lod = 1; lod <= 99; ++lod)
                {
                    bestLOD = lod - 1;
                    float lodRange = res->getVisibilityRangeHint(lod);
                    if (group.maxRange() > lodRange || lodRange == FLT_MAX)
                    {
                        break;
                    }
                }
                group.lod() = bestLOD;

                OE_DEBUG << LC
                    << "Rendering asset group" << iter.first
                    << " at terrain level " << bestLOD << std::endl;
            }
        }
    }

    // Create impostor geometries for each group
    configureImpostor(GROUP_TREES);
    configureImpostor(GROUP_BUSHES);
    configureImpostor(GROUP_UNDERGROWTH);

    // NEXT assemble the asset group statesets.
    osg::StateSet* ss = getOrCreateStateSet();
    ss->setName(typeid(*this).name());

    // Backface culling should be off.
    ss->setMode(GL_CULL_FACE, 0x0 | osg::StateAttribute::PROTECTED);

    // Install the texture arena:
    TextureArena* textures = getBiomeLayer()->getBiomeManager().getTextures();
    ss->setAttribute(textures);

    // Apply a maximum GPU texture size
    textures->setMaxTextureSize(std::min(
        (int)options().maxTextureSize().get(),
        Registry::instance()->getMaxTextureSize()));

    // Custom shaders:
    VirtualProgram* vp = VirtualProgram::getOrCreate(ss);
    ProceduralShaders shaders;
    shaders.load(vp, shaders.Vegetation);

    // noise sampler
    int index = textures->add(_noiseTex, getReadOptions());
    ss->setDefine("OE_NOISE_TEX_INDEX", std::to_string(index));

    // If multisampling is on, use alpha to coverage.
    _alphaToCoverageSupported = false;
    if (osg::DisplaySettings::instance()->getNumMultiSamples() > 1)
    {
        _alphaToCoverageSupported = true;
    }

    // apply the various uniform-based options
    setNearLODScale(options().nearLODScale().get());
    setFarLODScale(options().farLODScale().get());
    setImpostorLowAngle(options().impostorLowAngle().get());
    setImpostorHighAngle(options().impostorHighAngle().get());
    setLODTransitionPadding(options().lodTransitionPadding().get());
    setUseImpostorNormalMaps(options().useImpostorNormalMaps().get());

    // configure the thread pool
    jobs::get_pool(JOB_ARENA_VEGETATION)->set_concurrency(options().threads().get());
}

namespace
{
    // adds the defines for a shared image layer
    void bind(
        ImageLayer* layer,
        const std::string& sampler,
        const std::string& matrix,
        osg::StateSet* stateset)
    {
        if (layer) {
            stateset->setDefine(sampler, layer->getSharedTextureUniformName());
            stateset->setDefine(matrix, layer->getSharedTextureMatrixUniformName());
        }
        else {
            stateset->removeDefine(sampler);
            stateset->removeDefine(matrix);
        }
    }
}

void
VegetationLayer::configureImpostor(const std::string& groupName)
{
    Options::Group& group = options().groups()[groupName];
    bool isUndergrowth = (groupName == GROUP_UNDERGROWTH);

    // functor for generating cross hatch geometry for trees:
    group._createImpostor = [&group, isUndergrowth](
        const osg::BoundingBox& b,
        float top_billboard_z,
        std::vector<osg::Texture*>& textures)
    {
        osg::Group* node = new osg::Group();

        // Checked externally using URIPostReadCallback. This allows a user
        // to know if they are processing the imposter or a model.
        node->setName("Impostor");

        // one part if we only have side textures;
        // two parts if we also have top textures
        int parts = textures.size() > 3 ? 2 : 1;

        float xmin = std::min(b.xMin(), b.yMin());
        float ymin = xmin;
        float xmax = std::max(b.xMax(), b.yMax());
        float ymax = xmax;

        const osg::Vec4f colors[1] = {
            {1,1,1,1}
        };

        for (int i = 0; i < parts; ++i)
        {
            if (i == 0 && textures[0] != nullptr)
            {
                osg::Geometry* geom = new osg::Geometry();
                geom->setName("Vegetation Impostor");
                geom->setUseVertexBufferObjects(true);
                geom->setUseDisplayList(false);
                osg::StateSet* ss = geom->getOrCreateStateSet();

                static const GLushort indices[12] = {
                    0,1,2,  2,3,0,
                    4,5,6,  6,7,4 };

                const osg::Vec3f verts[8] = {
                    { xmin, 0, b.zMin() },
                    { xmax, 0, b.zMin() },
                    { xmax, 0, b.zMax() },
                    { xmin, 0, b.zMax() },
                    { 0, ymin, b.zMin() },
                    { 0, ymax, b.zMin() },
                    { 0, ymax, b.zMax() },
                    { 0, ymin, b.zMax() }
                };

                osg::Vec3f normals[8] = {
                    {0,1,0}, {0,1,0}, {0,1,0}, {0,1,0},
                    {1,0,0}, {1,0,0}, {1,0,0}, {1,0,0}
                };

                const osg::Vec2f uvs[8] = {
                    {0,0},{1,0},{1,1},{0,1},
                    {0,0},{1,0},{1,1},{0,1}
                };

                const osg::Vec3f flexors[8] = {
                    {0,0,0}, {0,0,0}, {0,0,1}, {0,0,1},
                    {0,0,0}, {0,0,0}, {0,0,1}, {0,0,1}
                };

                geom->addPrimitiveSet(new osg::DrawElementsUShort(GL_TRIANGLES, 12, &indices[0]));
                geom->setVertexArray(new osg::Vec3Array(osg::Array::BIND_PER_VERTEX, 8, verts));
                geom->setNormalArray(new osg::Vec3Array(osg::Array::BIND_PER_VERTEX, 8, normals));
                geom->setColorArray(new osg::Vec4Array(osg::Array::BIND_OVERALL, 1, colors));
                geom->setTexCoordArray(0, new osg::Vec2Array(osg::Array::BIND_PER_VERTEX, 8, uvs));
                geom->setTexCoordArray(3, new osg::Vec3Array(osg::Array::BIND_PER_VERTEX, 8, flexors));

                auto normal_techniques = new osg::UByteArray(1);
                normal_techniques->setBinding(osg::Array::BIND_OVERALL);
                geom->setVertexAttribArray(6, normal_techniques);

                if (isUndergrowth)
                    (*normal_techniques)[0] = Chonk::NORMAL_TECHNIQUE_ZAXIS;
                else
                    (*normal_techniques)[0] = Chonk::NORMAL_TECHNIQUE_HEMISPHERE;

                if (textures.size() > 0)
                    ss->setTextureAttribute(0, textures[0], 1); // side albedo

                if (textures.size() > 1)
                    ss->setTextureAttribute(1, textures[1], 1); // side normal

                if (textures.size() > 2)
                    ss->setTextureAttribute(2, textures[2], 1); // side meta/smooth/ao

                node->addChild(geom);
            }
            else if (i == 1 && textures[3] != nullptr)
            {
                osg::Geometry* geom = new osg::Geometry();
                geom->setName("Tree impostor");
                geom->setUseVertexBufferObjects(true);
                geom->setUseDisplayList(false);
                osg::StateSet* ss = geom->getOrCreateStateSet();

                static const GLushort indices[6] = {
                    0,1,2,  2,3,0
                };
                const osg::Vec3f verts[4] = {
                    {xmin, ymin, top_billboard_z},
                    {xmax, ymin, top_billboard_z},
                    {xmax, ymax, top_billboard_z},
                    {xmin, ymax, top_billboard_z}
                };
                osg::Vec3f normals[4] = {
                    {-1,-1,2}, {1,-1,2}, {1,1,2}, {-1,1,2}
                };
                for (auto& n : normals) n.normalize();

                const osg::Vec2f uvs[4] = {
                    {0,0}, {1,0}, {1,1}, {0,1}
                };

                geom->addPrimitiveSet(new osg::DrawElementsUShort(GL_TRIANGLES, 6, &indices[0]));
                geom->setVertexArray(new osg::Vec3Array(osg::Array::BIND_PER_VERTEX, 4, verts));
                geom->setNormalArray(new osg::Vec3Array(osg::Array::BIND_PER_VERTEX, 4, normals));
                geom->setColorArray(new osg::Vec4Array(osg::Array::BIND_OVERALL, 1, colors));
                geom->setTexCoordArray(0, new osg::Vec2Array(osg::Array::BIND_PER_VERTEX, 4, uvs));
                geom->setTexCoordArray(3, new osg::Vec3Array(osg::Array::BIND_PER_VERTEX, 4)); // flexors

                auto normal_techniques = new osg::UByteArray(1);
                normal_techniques->setBinding(osg::Array::BIND_OVERALL);
                geom->setVertexAttribArray(6, normal_techniques);
                if (isUndergrowth)
                    (*normal_techniques)[0] = Chonk::NORMAL_TECHNIQUE_ZAXIS;
                else
                    (*normal_techniques)[0] = Chonk::NORMAL_TECHNIQUE_HEMISPHERE;

                if (textures.size() > 3)
                    ss->setTextureAttribute(0, textures[3], 1); // top albedo

                if (textures.size() > 4)
                    ss->setTextureAttribute(1, textures[4], 1); // top normal

                if (textures.size() > 5)
                    ss->setTextureAttribute(2, textures[5], 1); // top MSA

                node->addChild(geom);
            }
        }

        BiomeManager::Impostor result;
        result._node = node;
        result._farLODScale = group.farLODScale().get();
        return result;
    };

    getBiomeLayer()->getBiomeManager().setCreateImpostorFunction(
        groupName,
        group._createImpostor);
}

bool
VegetationLayer::checkForNewAssets() const
{
    OE_SOFT_ASSERT_AND_RETURN(getBiomeLayer() != nullptr, false);

    BiomeManager& biomeMan = getBiomeLayer()->getBiomeManager();

    // if the revision has not changed, bail out.
    if (_biomeRevision.exchange(biomeMan.getRevision()) == biomeMan.getRevision())
        return false;

    // revision changed -- start loading new assets.

    osg::observer_ptr<const VegetationLayer> layer_weakptr(this);

    auto loadNewAssets = [layer_weakptr](Cancelable& c) -> AssetsByGroup
    {
        OE_PROFILING_ZONE_NAMED("VegetationLayer::loadNewAssets(job)");

        AssetsByGroup result;

        osg::ref_ptr<const VegetationLayer> layer;
        if (layer_weakptr.lock(layer))
        {
            BiomeManager::ResidentBiomesById biomes = layer->getBiomeLayer()->getBiomeManager().getResidentBiomes(
                layer->getReadOptions());

            // re-organize the data into a form we can readily use.
            for (auto iter : biomes)
            {
                if (c.canceled())
                    break;

                const Biome* biome = iter.second.biome;
                auto& instances = iter.second.instances;

                // first, sort the instances by group:
                vector_map<
                    std::string,
                    std::vector<ResidentModelAssetInstance>> instances_by_group;

                for (auto& instance : instances)
                {
                    instances_by_group[instance.residentAsset()->assetDef()->group()]
                        .push_back(instance);
                }

                // next, foreach group, calculate the relative weighting by
                // inserting heavy instances more than once.
                for (auto iter : instances_by_group)
                {
                    auto& group = iter.first;
                    auto& instances = iter.second;

                    float total_weight = 0.0f;
                    float smallest_weight = FLT_MAX;

                    for (auto& instance : instances)
                    {
                        total_weight += instance.weight();
                        smallest_weight = std::min(smallest_weight, instance.weight());
                    }

                    // calculate the weight multiplier
                    float weight_scale = 1.0f / smallest_weight;
                    total_weight *= weight_scale;

                    for (auto& instance : instances)
                    {
                        unsigned num = std::max(1u, (unsigned)(instance.weight() * weight_scale));

                        auto& biome_assets = result[group][biome->id()];
                        biome_assets.biome = biome;

                        biome_assets.instances.reserve(num);
                        for (unsigned i = 0; i < num; ++i)
                        {
                            biome_assets.instances.push_back(instance);
                        }
                    }
                }
            }
        }
        return result;
    };

    jobs::context context{
        "VegetationLayer asset loader",
        jobs::get_pool(JOB_ARENA_VEGETATION)
    };

    _newAssets = jobs::dispatch(loadNewAssets, context);

    return true;
}

//........................................................................

void
VegetationLayer::reset()
{
    _lastVisit.setReferenceTime(DBL_MAX);
    _lastVisit.setFrameNumber(~0U);

    _biomeRevision = 0;

    _assets.scoped_lock([this]()
        {
            _assets.clear();
        });

    _tiles.scoped_lock([this]()
        {
            _tiles.clear();
            _placeholders.clear();
        });

    _cameraState.scoped_lock([this]()
        {
            _cameraState.clear();
        });
}

// random-texture channels
#define N_SMOOTH   0
#define N_RANDOM   1
#define N_RANDOM_2 2
#define N_CLUMPY   3

Future<osg::ref_ptr<osg::Drawable>>
VegetationLayer::createDrawableAsync(
    const TileKey& key_,
    const std::string& group_,
    const osg::BoundingBox& tile_bbox_,
    const osg::FrameStamp* framestamp_,
    double backup_birthday,
    float range) const
{
    osg::ref_ptr<const VegetationLayer> layer = this;
    TileKey key = key_;
    const std::string group = group_;
    osg::BoundingBox tile_bbox = tile_bbox_;
    osg::ref_ptr<const osg::FrameStamp> framestamp = framestamp_;

    auto function = [layer, key, group, tile_bbox, framestamp, backup_birthday](Cancelable& c)
    {
        osg::ref_ptr<ProgressCallback> p = new ProgressCallback(&c);
        auto result = layer->createDrawable(key, group, tile_bbox, p.get());
        if (result.valid())
            asChonkDrawable(result)->setBirthday(
                framestamp ? framestamp->getReferenceTime() : backup_birthday);
        return result;
    };

    jobs::context context;
    context.name = "Vegetation create drawable";
    context.pool = jobs::get_pool(JOB_ARENA_VEGETATION);
    context.priority = [range]() { return -range; }; // closer is sooner

    return jobs::dispatch(function, context);
}

#undef RAND
#define RAND() prng.next()

bool
VegetationLayer::getAssetPlacements(
    const TileKey& key,
    const std::string& group,
    bool loadBiomesOnDemand,
    std::vector<VegetationLayer::Placement>& output,
    ProgressCallback* progress) const
{
    OE_PROFILING_ZONE;

    // bail out if the Map has disappeared
    osg::ref_ptr<const Map> map;
    if (!_map.lock(map))
    {
        return false;
    }

    // bail out if missing or disabled group
    const Options::Group& groupOptions = options().group(group);
    if (groupOptions.enabled() == false)
    {
        return false;
    }

    std::vector<Placement> result;

    // Safely copy the instance list. The object is immutable
    // once we get to this point, since all assets are materialized
    // by the biome manager.
    AssetsByBiomeId groupAssets;

    if (loadBiomesOnDemand == false)
    {
        std::lock_guard<std::mutex> lock(_assets.mutex());

        auto iter = _assets.find(group);
        if (iter == _assets.end())
        {
            //OE_INFO << LC << "(1) GAP returning false because _assets doesn't exist for group " << group << std::endl;
            return !_newAssets.working();
            //return false; // data is unavailable.
        }
        else
        {
            groupAssets = iter->second; //shallow copy
        }

        // if it's empty, bail out (and probably return later)
        if (groupAssets.empty())
        {
            //OE_INFO << LC << "GAP returning false because the groupAssets is empty" << std::endl;
            return false;
        }
    }

    // Load a lifemap raster:
    GeoImage lifemap;
    osg::Matrix lifemap_sb;
    if (getLifeMapLayer())
    {
        // Cannot use getBestAvailableKey here because lifemap might use
        // a post-layer for dynamic terrain, and post-layers do not yet
        // publish dataextents to their hosts
        for (TileKey bestKey = key;
            bestKey.valid() && !lifemap.valid();
            bestKey.makeParent())
        {
            lifemap = getLifeMapLayer()->createImage(bestKey, progress);
            if (lifemap.valid())
            {
                key.getExtent().createScaleBias(lifemap.getExtent(), lifemap_sb);
            }
        }
    }

    // Load a biome map raster:
    GeoImage biomemap;
    osg::Matrix biomemap_sb;
    if (getBiomeLayer())
    {
        TileKey bestKey = getBiomeLayer()->getBestAvailableTileKey(key);
        biomemap = getBiomeLayer()->createImage(bestKey, progress);
        key.getExtent().createScaleBias(biomemap.getExtent(), biomemap_sb);
        biomemap.getReader().setBilinear(false);
    }

    // Prepare to deal with holes in the terrain, where we do not want
    // to place vegetation
    TerrainConstraintQuery query;
    map->getLayers<TerrainConstraintLayer>(query.layers, [](const auto* layer)
        {
            auto clayer = static_cast<const TerrainConstraintLayer*>(layer);
            return clayer->getRemoveInterior() == true;
        });

    MeshConstraints constraints;
    query.getConstraints(key, constraints, progress);

    // If the biome residency is not up to date, do that now
    // after loading the biome map.
    if (loadBiomesOnDemand)
    {
        std::lock_guard<std::mutex> lock(_assets.mutex());

        if (checkForNewAssets() == true)
        {
            _newAssets.join(progress);
            AssetsByGroup newAssets = _newAssets.release();
            if (!newAssets.empty())
            {
                _assets = std::move(newAssets);
            }
        }

        // make a shallow copy of assets list safely
        {
            auto iter = _assets.find(group);
            if (iter == _assets.end())
            {
                //OE_INFO << LC << "(2) GAP returning false because _assets doesn't exist for group " << group << std::endl;
                return false;
            }
            else
            {
                groupAssets = iter->second; // shallow copy
            }
        }

        // if it's empty, bail out (and probably return later)
        if (groupAssets.empty())
        {
            output = std::move(result);
            return true;
        }
    }

    const Biome* default_biome = groupAssets.begin()->second.biome;

    osg::Vec4f noise;
    ImageUtils::PixelReader readNoise(_noiseTex->osgTexture()->getImage(0));
    readNoise.setSampleAsRepeatingTexture(true);

    using Index = RTree<int, double, 2>;
    Index index;

    Random prng(0);

    // normal distribution generator.
    const auto normal_dist_01 = [&prng](double input) -> double
        {
            // map input=[0..1] to [input-1..input+1] with normal distribution
            // and a standard deviation of 1/6 using Box-Muller transform:

            // Box-Muller: generate a standard normal value
            double u1 = std::max(1e-6, prng.next());
            double u2 = prng.next();
            double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);

            // Scale to desired stddev and mean
            const double stddev = 1.0 / 6.0;
            double mean = input;
            double result = mean + z * stddev;

            // Clamp to [mean-1, mean+1]
            //result = std::max(mean - 1.0, std::min(mean + 1.0, result));
            return result;
        };


    // approximate area of the tile in km
    GeoCircle c = key.getExtent().computeBoundingGeoCircle();
    double x = 0.001 * c.getRadius() * 2.8284271247;
    double area_sqkm = x * x;

    unsigned max_instances = groupOptions.instancesPerSqKm().get() * area_sqkm;

    float overlap = clamp(groupOptions.overlap().get(), 0.0f, 1.0f);

    // reserve some memory, maybe more than we need
    result.reserve(max_instances);

    // store these separately so we can clamp them all in one go
    std::vector<osg::Vec3d> map_points;
    map_points.reserve(max_instances);

    const GeoExtent& e = key.getExtent();

    osg::Vec4f lifemap_value;
    osg::Vec4f biomemap_value;
    std::vector<double> hits;

    auto catalog = getBiomeLayer()->getBiomeCatalog();

    bool scaleWithDensity = (group == GROUP_UNDERGROWTH);

    // determine a local tile bbox size for collisions and uv generation
    // note. This doesn't take elevation data into account. Does that matter?
    auto& ex = key.getExtent();
    GeoPoint centroid = ex.getCentroid();
    osg::ref_ptr<const SpatialReference> ltp = ex.getSRS()->createTangentPlaneSRS(centroid.vec3d());
    double x0, y0, x1, y1;
    ex.getSRS()->transform2D(ex.xMin(), ex.yMin(), ltp.get(), x0, y0);
    ex.getSRS()->transform2D(ex.xMax(), ex.yMax(), ltp.get(), x1, y1);
    osg::BoundingBox local_bbox(x0, y0, 0, x1, y1, 0);
    double local_width = x1 - x0;
    double local_height = y1 - y0;

    // keep track of biomes with no assets (for a possible error condition?)
    std::set<const Biome*> empty_biomes;

    // indicies of assets selected based on their lushness
    std::vector<unsigned> assetIndices;

    // cumulative density function based on asset weights
    std::vector<float> assetCDF;


    //TEMP - DEBUGGING DETERMINISTIC BEHAVIOR.
    bool debug = false; // key.is(14, 17117, 4120);
    if (debug) {
        OE_INFO << LC << "---" << std::endl;
        OE_INFO << LC << "Attempting to place " << max_instances << std::endl;
    }

    // Generate random instances within the tile:
    for (unsigned i = 0; i < max_instances; ++i)
    {
        // perform all random number generations first to preserve determinism
        // in the even of an early loop break.

        // random tile-normalized position:
        float u = RAND();
        float v = RAND();

        float asset_index_rand = RAND();
        float rotation_rand = RAND();
        float normal_rand = RAND();
        float lush_offset = normal_dist_01(0.0); // normal_dist(gen);


        // resolve the biome at this position:
        const Biome* biome = nullptr;
        if (biomemap.valid())
        {
            float uu = u * biomemap_sb(0, 0) + biomemap_sb(3, 0);
            float vv = v * biomemap_sb(1, 1) + biomemap_sb(3, 1);
            biomemap.getReader()(biomemap_value, uu, vv);
            int index = (int)biomemap_value.r();
            biome = catalog->getBiomeByIndex(index);
            if (!biome)
            {
                if (debug) OE_INFO << LC << "Instance " << i << " has invalid biome index " << index << std::endl;
                continue;
            }
        }

        if (biome == nullptr)
        {
            // not sure this is even possible
            biome = default_biome;
        }

        // fetch the collection of assets belonging to the selected biome:
        auto iter = groupAssets.find(biome->id());
        if (iter == groupAssets.end())
        {
            empty_biomes.insert(biome);
            if (debug) OE_INFO << LC << "Instance " << i << " has no assets for biome " << biome->id() << std::endl;
            continue;
        }
        ResidentBiomeModelAssetInstances& biome_assets = iter->second;

        // sample the noise texture at this (u,v)
        readNoise(noise, u, v);

        // read the life map at this point:
        float density = 1.0f;
        float lush = 1.0f;
        if (lifemap.valid())
        {
            float uu = u * lifemap_sb(0, 0) + lifemap_sb(3, 0);
            float vv = v * lifemap_sb(1, 1) + lifemap_sb(3, 1);
            lifemap.getReader()(lifemap_value, uu, vv);
            density = lifemap_value[LIFEMAP_DENSE];
            lush = lifemap_value[LIFEMAP_LUSH];
        }

        auto& assetInstances = biome_assets.instances;

        // RNG with normal distribution between approx lush-1..lush+1
        // Note: moved this earlier in the loop to make it deterministic
        //std::normal_distribution<float> normal_dist(lush, 1.0f / 6.0f);
        lush = clamp(lush + lush_offset, 0.0f, 1.0f);

        assetIndices.clear();
        assetCDF.clear();
        float cumulativeWeight = 0.0f;
        for (unsigned i = 0; i < assetInstances.size(); ++i)
        {
            float min_lush = assetInstances[i].residentAsset()->assetDef()->minLush().get();
            float max_lush = assetInstances[i].residentAsset()->assetDef()->maxLush().get();

            if (lush >= min_lush && lush <= max_lush)
            {
                assetIndices.push_back(i);
                cumulativeWeight += assetInstances[i].weight();
                assetCDF.push_back(cumulativeWeight);
            }
        }

        // if there are no assets that match the lushness criteria, move on.
        if (assetIndices.empty())
        {
            if (debug) OE_INFO << LC << "Instance " << i << " has no assets for lushness " << lush << std::endl;
            continue;
        }

        int assetIndex = 0;
        if (assetIndices.size() > 1)
        {
            float k = asset_index_rand * cumulativeWeight;
            for (assetIndex = 0;
                assetIndex < assetCDF.size() - 1 && k > assetCDF[assetIndex];
                ++assetIndex);
        }
        auto& instance = assetInstances[assetIndices[assetIndex]];
        auto& asset = instance.residentAsset();

        // if there's no geometry... bye
        if (asset->chonk() == nullptr)
        {
            if (debug) OE_INFO << LC << "Instance " << i << " has no geometry" << std::endl;
            continue;
        }

        osg::Vec3d scale(1, 1, 1);

        // Apply a size variation with some randomness
        if (asset->assetDef()->sizeVariation().isSet())
        {
            scale *= 1.0 + (asset->assetDef()->sizeVariation().get() *
                (noise[N_CLUMPY] * 2.0f - 1.0f));
        }

        // apply instance-specific density adjustment:
        density *= instance.coverage();

#if 0
        // Removed, because this is causing the placement to go non-deterministic
        // for some reason that I have not yet identified.
        const float edge_threshold = 0.10f;
        if (scaleWithDensity && density < edge_threshold)
        {
            float edginess = (density / edge_threshold);
            scale *= edginess;
        }
#endif

        // tile-local coordinates of the position:
        osg::Vec2d local(
            local_bbox.xMin() + u * local_width,
            local_bbox.yMin() + v * local_height);

        bool pass = true;

        if (overlap < 1.0f)
        {
            // To prevent overlap, write positions and radii to an r-tree. 
            // TODO: consider using a Blend2d raster to update the 
            // density/lifemap raster as we place objects..?
            pass = false;

            // scale the asset bounding box in preparation for collision:
            const auto& aabb = asset->boundingBox();

            double so = (1.0 - overlap);
            double a_min[2] = { local.x() + aabb.xMin() * scale.x() * so, local.y() + aabb.yMin() * scale.y() * so };
            double a_max[2] = { local.x() + aabb.xMax() * scale.x() * so, local.y() + aabb.yMax() * scale.y() * so };
            
            if (index.Search(a_min, a_max) == 0)
            {
                index.Insert(a_min, a_max, 0);
                pass = true;
            }
        }

        if (pass)
        {
            osg::Vec3d map_point(e.xMin() + u * e.width(), e.yMin() + v * e.height(), 0);

            if (!inConstrainedRegion(map_point.x(), map_point.y(), constraints))
            {
                map_points.emplace_back(map_point);

                Placement p;
                p.localPoint() = local;
                p.uv().set(u, v);
                p.scale() = scale;
                p.rotation() = rotation_rand * 3.1415927 * 2.0;
                p.asset() = asset;
                p.density() = density;
                p.biome = biome;

                result.emplace_back(std::move(p));
            }
            else
            {
                if (debug) OE_INFO << LC << "Instance " << i << " is in a constrained region" << std::endl;
            }
        }
    }

    // Next, go through and remove assets based on the density 
    // threshold. We have to do this after the fact so that
    // lifemap changes don't change existing assets (due to the
    // collision rtree).
    if (debug) OE_INFO << LC << (max_instances - result.size()) << " instances removed due to overlap" << std::endl;

    std::vector<Placement> result_culled;
    result_culled.reserve(result.size());

    std::vector<osg::Vec3d> map_points_culled;
    map_points_culled.reserve(result.size());

    // Use a NEW PRNG for the density thresholding, since we can't know
    // the state of the old one and we need this to be completely deterministic
    Random density_prng(key.getTileX() + key.getTileY());

    for (int i = 0; i < result.size(); ++i)
    {
        Placement& p = result[i];

        if (density_prng.next() <= p.density())
        {
            result_culled.emplace_back(std::move(p));
            map_points_culled.emplace_back(std::move(map_points[i]));
        }
    }

    if (debug) OE_INFO << LC << (result.size()-result_culled.size()) << " instances removed due to density" << std::endl;

    std::swap(result, result_culled);
    std::swap(map_points, map_points_culled);

    if (debug) OE_INFO << LC << "Final instance count = " << result.size() << std::endl;

    // clamp everything to the terrain
    map->getElevationPool()->sampleMapCoords(
        map_points.begin(), map_points.end(),
        groupOptions.clampingResolution().value(),
        nullptr,
        nullptr,
        0.0f); // store zero upon failure?

    // copy the clamped map points back over
    for (std::size_t i = 0; i < map_points.size(); ++i)
    {
        result[i].mapPoint() = std::move(map_points[i]);
    }

#if 0
    // print warnings about empty biomes
    if (!empty_biomes.empty())
    {
        for (auto biome : empty_biomes)
        {
            OE_WARN << LC << "No assets defined in group " << AssetGroup::name(group) << " for biome " << biome->id() << std::endl;
        }
    }
#endif

    output = std::move(result);
    return true;
}


std::string
VegetationLayer::simulateAssetPlacement(const GeoPoint& point, const std::string& group) const
{
    const bool loadBiomesOnDemand = true;
    ProgressCallback* progress = nullptr;

    std::stringstream log;

    // bail out if the Map has disappeared
    osg::ref_ptr<const Map> map;
    if (!_map.lock(map))
    {
        log << "Map pointer is null" << std::endl;
        return log.str();
    }

    // bail out if missing or disabled group
    const Options::Group& groupOptions = options().group(group);
    if (groupOptions.enabled() == false)
    {
        log << "Group " << group << " is disabled - abort" << std::endl;
        return log.str();
    }


    TileKey key = map->getProfile()->createTileKey(point, groupOptions.lod().get());
    log << "Resolved to tile key " << key.str() << std::endl;

    std::vector<Placement> result;

    // Safely copy the instance list. The object is immutable
    // once we get to this point, since all assets are materialized
    // by the biome manager.
    AssetsByBiomeId groupAssets;

    if (loadBiomesOnDemand == false)
    {
        std::lock_guard<std::mutex> lock(_assets.mutex());

        auto iter = _assets.find(group);
        if (iter == _assets.end())
        {
            log << "No assets" << std::endl;
            return log.str();
        }
        else
            groupAssets = iter->second; //shallow copy

        // if it's empty, bail out (and probably return later)
        if (groupAssets.empty())
        {
            log << "Asset list is empty for group " << group << " - abort" << std::endl;
            return log.str();
        }
    }

    // Load a lifemap raster:
    GeoImage lifemap;
    osg::Matrix lifemap_sb;
    if (getLifeMapLayer())
    {
        // Cannot use getBestAvailableKey here because lifemap might use
        // a post-layer for dynamic terrain, and post-layers do not yet
        // publish dataextents to their hosts
        for (TileKey bestKey = key;
            bestKey.valid() && !lifemap.valid();
            bestKey.makeParent())
        {
            lifemap = getLifeMapLayer()->createImage(bestKey, progress);
            if (lifemap.valid())
            {
                key.getExtent().createScaleBias(lifemap.getExtent(), lifemap_sb);
                log << "Sampled lifemap at LOD " << bestKey.getLOD() << std::endl;
            }
        }
    }

    // Load a biome map raster:
    GeoImage biomemap;
    osg::Matrix biomemap_sb;
    if (getBiomeLayer())
    {
        TileKey maxKey = key;
        TileKey bestKey = getBiomeLayer()->getBestAvailableTileKey(maxKey);
        biomemap = getBiomeLayer()->createImage(bestKey, progress);
        key.getExtent().createScaleBias(biomemap.getExtent(), biomemap_sb);
        biomemap.getReader().setBilinear(false);
        log << "Sampled biomemap at LOD " << bestKey.getLOD() << std::endl;
    }

    // Prepare to deal with holes in the terrain, where we do not want
    // to place vegetation
    TerrainConstraintQuery query;
    map->getLayers<TerrainConstraintLayer>(query.layers, [](const TerrainConstraintLayer* layer)
        {
            auto clayer = static_cast<const TerrainConstraintLayer*>(layer);
            return clayer->getRemoveInterior() == true;
        });
    MeshConstraints constraints;
    query.getConstraints(key, constraints, progress);

    // If the biome residency is not up to date, do that now
    // after loading the biome map.
    if (loadBiomesOnDemand)
    {
        if (checkForNewAssets() == true)
        {
            _newAssets.join(progress);
            AssetsByGroup newAssets = _newAssets.release();
            if (!newAssets.empty())
            {
                std::lock_guard<std::mutex> lock(_assets.mutex());
                _assets = std::move(newAssets);
            }
        }

        // make a shallow copy of assets list safely
        {
            std::lock_guard<std::mutex> lock(_assets.mutex());
            auto iter = _assets.find(group);
            if (iter == _assets.end())
            {
                log << "No assets" << std::endl;
                return log.str();
            }
            else
                groupAssets = iter->second; // shallow copy
        }

        // if it's empty, bail out (and probably return later)
        if (groupAssets.empty())
        {
            log << "Asset group " << group << " is empty when loading biomes on demand - abort" << std::endl;
            return log.str();
        }
    }

    const Biome* default_biome = groupAssets.begin()->second.biome;

    osg::Vec4f noise;
    ImageUtils::PixelReader readNoise(_noiseTex->osgTexture()->getImage(0));
    readNoise.setSampleAsRepeatingTexture(true);

    // indicies of assets selected based on their lushness
    std::vector<unsigned> assetIndices;

    // cumulative density function based on asset weights
    std::vector<float> assetCDF;


    auto catalog = getBiomeLayer()->getBiomeCatalog();
    auto& ex = key.getExtent();
    osg::Vec4f lifemap_value;
    osg::Vec4f biomemap_value;

    // random tile-normalized position:
    float u = (point.x() - ex.xMin()) / ex.width();
    float v = (point.y() - ex.yMin()) / ex.height();

    log << "Tile uv = " << u << ", " << v << std::endl;

    // resolve the biome at this position:
    const Biome* biome = nullptr;
    if (biomemap.valid())
    {
        float uu = u * biomemap_sb(0, 0) + biomemap_sb(3, 0);
        float vv = v * biomemap_sb(1, 1) + biomemap_sb(3, 1);
        biomemap.getReader()(biomemap_value, uu, vv);
        int index = (int)biomemap_value.r();
        biome = catalog->getBiomeByIndex(index);
        if (!biome)
        {
            log << "No biome at those coordinates" << std::endl;
            return log.str();
        }
    }

    log << "Biome: " << biome->id() << " - " << biome->name().get() << std::endl;

    // fetch the collection of assets belonging to the selected biome:
    auto iter = groupAssets.find(biome->id());
    if (iter == groupAssets.end())
    {
        log << "Biome contains no " << group << std::endl;
        return log.str();
    }

    // sample the noise texture at this (u,v)
    readNoise(noise, u, v);

    // read the life map at this point:
    float density = 1.0f;
    float lush = 1.0f;
    if (lifemap.valid())
    {
        float uu = u * lifemap_sb(0, 0) + lifemap_sb(3, 0);
        float vv = v * lifemap_sb(1, 1) + lifemap_sb(3, 1);
        lifemap.getReader()(lifemap_value, uu, vv);
        density = lifemap_value[LIFEMAP_DENSE];
        lush = lifemap_value[LIFEMAP_LUSH];
    }

    log << "Density=" << density << "  Lush=" << lush << std::endl;

    ResidentBiomeModelAssetInstances& biome_assets = iter->second;

    log << "Candidate assets: " << std::endl;
    for (auto& ai : biome_assets.instances)
    {
        log << "> " << ai.residentAsset()->assetDef()->name()
            << " weight=" << ai.weight()
            << " fill=" << ai.coverage()
            << " lush=[" << ai.residentAsset()->assetDef()->minLush().get() << ", "
            << ai.residentAsset()->assetDef()->maxLush().get() << "]"
            << std::endl;
    }

    if (density == 0.0f)
        log << "NOTE: density is zero, no asset placed." << std::endl;

    return log.str();
}

osg::ref_ptr<osg::Drawable>
VegetationLayer::createDrawable(
    const TileKey& key,
    const std::string& group,
    const osg::BoundingBox& tile_bbox,
    ProgressCallback* progress) const
{
    std::vector<VegetationLayer::Placement> placements;

    bool placementsOK = getAssetPlacements(
        key,
        group,
        false,
        placements,
        progress);

    if (!placementsOK)
    {
        // data not available...bail out and return later.
        progress->cancel();
        return nullptr;
    }

    const osg::Vec3f ZAXIS(0, 0, 1);

    // finally, assemble the drawable.
    osg::ref_ptr<ChonkDrawable> result;

    if (options().renderBinNumber().isSet())
        result = new ChonkDrawable(options().renderBinNumber().value());
    else
        result = new ChonkDrawable();

    result->setName(key.str() + " Vegetation");

    for (auto& p : placements)
    {
        osg::Matrixd xform;

        if (p.mapPoint().z() < FLT_MAX)
        {
            xform.makeTranslate(p.localPoint().x(), p.localPoint().y(), p.mapPoint().z());
            xform.preMultRotate(osg::Quat(p.rotation(), ZAXIS));
            xform.preMultScale(p.scale());

            result->add(p.asset()->chonk(), xform, p.uv());
        }
    }

    return result;
}

void
VegetationLayer::cull(const TileBatch& batch, osg::NodeVisitor& nv) const
{
    // bail out if we cannot render due to insufficient GL version
    if (!_renderingSupported)
        return;

    // bail out if there are no assets to render
    if (_assets.empty())
        return;

    osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(&nv);

    // update the framestamp for auto-timeout support
    _lastVisit = *cv->getFrameStamp();

    // exclusive lock on the camera state (created a new one as necessary)
    _cameraState.lock();
    CameraState::Ptr& cs = _cameraState[cv->getCurrentCamera()];
    _cameraState.unlock();

    // check for multisampling
    if (!_alphaToCoverageSupported)
    {
        if (cv->getState()->getLastAppliedModeValue(GL_MULTISAMPLE) ||
            osg::DisplaySettings::instance()->getMultiSamples() == true ||
            osg::DisplaySettings::instance()->getNumMultiSamples() > 1)
        {
            // the next update() pass will pick this up and apply it if possible.
            _alphaToCoverageSupported = true;
        }
    }

    if (cs == nullptr)
        cs = std::make_shared<CameraState>();

    CameraState::TileViews active_views;

    for (auto& entry : batch.tiles())
    {
        const std::string& groupName = getGroupAtLOD(entry->getKey().getLOD());
        const Options::Group& group = options().group(groupName);

        if (group.enabled() == false)
        {
            continue;
        }

        if (CameraUtils::isShadowCamera(cv->getCurrentCamera()) &&
            group.castShadows() == false)
        {
            continue;
        }

        // combine the key and revision to make a Unique ID.
        TileKeyAndRevision rev_tile_key(
            { entry->getKey(), entry->getRevision() }
        );

        // find this camera's view on the tile, creating a new 
        // empty one if necessary:
        TileView& view = cs->_views[rev_tile_key];

        // If this camera doesn't have a view on the tile, establish one:
        if (view._tile == nullptr)
        {
            // We don't want more than one camera creating the
            // same drawable, so this _tiles table tracks tiles globally.
            std::lock_guard<std::mutex> lock(_tiles.mutex());

            // First, find a placeholder based on the same tile key,
            // ignoring the revision. (Even if we find an existing tile,
            // that doesn't mean it is ready to render)
            view._placeholder = _placeholders[entry->getKey()];

            Tile::WeakPtr& tile_weak = _tiles[rev_tile_key];
            Tile::Ptr tile = tile_weak.lock();
            if (tile == nullptr)
            {
                // If the placeholder exists, extact its birthday so we
                // can pass it along to the new tile we're creating.
                // This will prevent the new tile from fading in.
                double birthday = -1.0;

                if (view._placeholder)
                {
                    auto phcd = asChonkDrawable(view._placeholder->_drawable.value());
                    if (phcd)
                        birthday = phcd->getBirthday();
                }

                // new tile; create and fire off the loading job.
                tile = std::make_shared<Tile>();

                osg::Vec3d center = entry->getBBox().center() * entry->getLocalToWorld();
                float range = nv.getDistanceToViewPoint(center, true);

                tile->_drawable = createDrawableAsync(
                    entry->getKey(),
                    groupName,
                    entry->getBBox(),
                    birthday < 0.0 ? cv->getState()->getFrameStamp() : nullptr,
                    birthday,
                    range);

                tile_weak = tile;
            }

            view._tile = tile;
            view._matrix = new osg::RefMatrix();
        }

        // if the data is ready, cull it:
        if (view._tile->_drawable.available())
        {
            auto drawable = view._tile->_drawable.value();

            if (drawable.valid())
            {
                if (!view._loaded)
                {
                    auto cd = asChonkDrawable(drawable);

                    // install the fade range
                    cd->setFadeNearFar(
                        entry->getMorphStartRange(),
                        entry->getMorphEndRange());

                    cd->setAlphaCutoff(
                        group.alphaCutoff().get());

                    // release the reference to any placeholder.
                    view._placeholder = nullptr;

                    // update the placeholder for this tilekey, in preparation for
                    // the next time the tile revision changes.
                    _tiles.scoped_lock([&]()
                        {
                            _placeholders[entry->getKey()] = view._tile;
                        });

                    // create the debug bound geometry.
                    if (!view._tile->_debugBound.valid())
                    {
                        view._tile->_debugBound = createDebugBound(drawable);
                    }

                    view._loaded = true;
                }

                // Push the matrix and accept the drawable.
                view._matrix->set(entry->getModelViewMatrix());
                cv->pushModelViewMatrix(view._matrix.get(), osg::Transform::ABSOLUTE_RF);
                if (!cv->isCulled(drawable->getBoundingBox()))
                {
                    drawable->accept(nv);

                    if (_showTileBoundingBoxes && view._tile->_debugBound.valid())
                    {
                        view._tile->_debugBound->accept(nv);
                    }
                }
                cv->popModelViewMatrix();
            }
            else
            {
                // creation failed; reset for another try.
                //OE_DEBUG << LC << "Failed for " << entry->getKey().str() << "; will retry..." << std::endl;
                view._tile = nullptr;
            }
        }

        // If the job exists but was canceled for some reason,
        // Reset this view so it will try again later.
        else if (view._tile->_drawable.empty())
        {
            view._tile = nullptr;
        }

        // Still loading, so draw a placeholder if we have one.
        // The placeholder is just an older version of the tile.
        else if (view._placeholder != nullptr)
        {
            // push the matrix and accept the placeholder.
            view._matrix->set(entry->getModelViewMatrix());
            cv->pushModelViewMatrix(view._matrix.get(), osg::Transform::ABSOLUTE_RF);
            view._placeholder->_drawable.value()->accept(nv);
            cv->popModelViewMatrix();
        }

        if (view._tile)
        {
            active_views[rev_tile_key] = view;
        }
    }

    // Purge old tiles.
    cs->_views.swap(active_views);


    // purge unused tiles & placeholders
    _tiles.scoped_lock([this, &cs]()
        {
            bool report = false;

            for (auto it = _placeholders.begin(); it != _placeholders.end();)
            {
                if (it->second.use_count() == 1)
                    it = _placeholders.erase(it), report = true;
                else
                    ++it;
            }

            for (auto it = _tiles.begin(); it != _tiles.end(); )
            {
                if (it->second.expired())
                    it = _tiles.erase(it), report = true;
                else
                    ++it;
            }

            if (report)
            {
                OE_DEBUG << LC << "_tiles=" << _tiles.size() << " _placeholders=" << _placeholders.size()
                    << " _views=" << cs->_views.size() << std::endl;
            }
        });
}

void
VegetationLayer::resizeGLObjectBuffers(unsigned maxSize)
{
    PatchLayer::resizeGLObjectBuffers(maxSize);

    std::lock_guard<std::mutex> lock(_tiles.mutex());

    for (auto iter : _tiles)
    {
        Tile::Ptr tile = iter.second.lock();
        if (tile)
        {
            auto drawable = tile->_drawable.value();
            if (drawable.valid())
                drawable->resizeGLObjectBuffers(maxSize);
        }
    }
}

void
VegetationLayer::releaseGLObjects(osg::State* state) const
{
    PatchLayer::releaseGLObjects(state);

    //const_cast<VegetationLayer*>(this)->dirty();

    {
        std::lock_guard<std::mutex> lock(_tiles.mutex());

        for (auto iter : _tiles)
        {
            Tile::Ptr tile = iter.second.lock();
            if (tile)
            {
                auto drawable = tile->_drawable.value();
                if (drawable.valid())
                    drawable->releaseGLObjects(state);
            }
        }
    }

    if (state != nullptr)
    {
        std::lock_guard<std::mutex> lock(_cameraState.mutex());
        for (auto cs : _cameraState)
        {
            if (cs.first->getGraphicsContext() == state->getGraphicsContext())
            {
                _cameraState.erase(cs.first);
                break;
            }
        }
    }
}
