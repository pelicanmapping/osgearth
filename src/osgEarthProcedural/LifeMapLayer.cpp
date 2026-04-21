/* osgEarth
* Copyright 2008-2012 Pelican Mapping
* MIT License
*/
#include "LifeMapLayer"

#include <osgEarth/NoiseTextureFactory>
#include <osgEarth/Map>
#include <osgEarth/ElevationPool>
#include <osgEarth/Math>
#include <osgEarth/MetaTile>
#include <osgEarth/TerrainEngineNode>
#include <random>

#define LC "[" << className() << "] \"" << getName() << "\" "

using namespace osgEarth;
using namespace osgEarth::Procedural;

REGISTER_OSGEARTH_LAYER(lifemap, LifeMapLayer);

Config
LifeMapLayer::Options::getConfig() const
{
    Config conf = super::getConfig();
    biomeLayer().set(conf, "biomes_layer");
    maskLayer().set(conf, "mask_layer");
    waterLayer().set(conf, "water_layer");
    colorLayer().set(conf, "color_layer");
    landCoverLayer().set(conf, "coverage_layer");
    conf.set("coverage_weight", landCoverWeight());
    conf.set("coverage_blur", landCoverBlur());
    conf.set("terrain_weight", terrainWeight());
    conf.set("color_weight", colorWeight());
    conf.set("noise_weight", noiseWeight());
    conf.set("lush_factor", lushFactor());
    return conf;
}

void
LifeMapLayer::Options::fromConfig(const Config& conf)
{
    biomeLayer().get(conf, "biomes_layer");
    maskLayer().get(conf, "mask_layer");
    waterLayer().get(conf, "water_layer");
    colorLayer().get(conf, "color_layer");
    landCoverLayer().get(conf, "coverage_layer");
    conf.get("coverage_weight", landCoverWeight());
    conf.get("coverage_blur", landCoverBlur());
    conf.get("terrain_weight", terrainWeight());
    conf.get("color_weight", colorWeight());
    conf.get("noise_weight", noiseWeight());
    conf.get("lush_factor", lushFactor());
}

//........................................................................

namespace
{
    // noise channels
    constexpr unsigned SMOOTH = 0;
    constexpr unsigned RANDOM = 1;
    constexpr unsigned RANDOM2 = 2;
    constexpr unsigned CLUMPY = 3;


    class CoordScaler
    {
    public:
        CoordScaler(const Profile* profile, unsigned int lod, unsigned int refLOD) :
            _profile(profile),
            _lod(lod),
            _refLOD(refLOD)
        {
            _profile->getNumTiles(lod, _tilesX, _tilesY);

            _dL = (double)(lod - refLOD);
            _factor = exp2(_dL);
            _invFactor = 1.0f / _factor;
        }

        void scaleCoordsToRefLOD(osg::Vec2d& tc, const TileKey& key)
        {
            if (key.getLOD() <= _refLOD)
                return;

            double rx = tc.x() * _invFactor;
            double ry = tc.y() * _invFactor;

            double tx = (double)key.getTileX();
            double ty = (double)(_tilesY - key.getTileY() - 1);

            double ax = floor(tx * _invFactor);
            double ay = floor(ty * _invFactor);

            double bx = ax * _factor;
            double by = ay * _factor;

            double cx = bx + _factor;
            double cy = by + _factor;

            if (_factor >= 1.0f)
            {
                rx += (tx - bx) / (cx - bx);
                ry += (ty - by) / (cy - by);
            }

            tc.set(rx, ry);
        }

        unsigned int _lod;
        unsigned int _refLOD;
        osg::ref_ptr< const Profile > _profile;

        double _dL;
        double _factor;
        double _invFactor;
        unsigned int _tilesX;
        unsigned int _tilesY;
    };

    inline void getNoise(
        osg::Vec4& noise,
        ImageUtils::PixelReader& read,
        const osg::Vec2d& coords)
    {
        read(noise, coords.x(), coords.y());
        noise *= 2.0;
        noise.r() -= 1.0, noise.g() -= 1.0, noise.b() -= 1.0, noise.a() -= 1.0;
    }
}

//........................................................................

void
LifeMapLayer::init()
{
    ImageLayer::init();

    // LifeMap is invisible AND shared by default.
    options().visible().setDefault(false);
    options().shared().setDefault(true);

    NoiseTextureFactory nf;
    _noiseFunc = nf.createImage(1024u, 4u);
}

Status
LifeMapLayer::openImplementation()
{
    Status parent = super::openImplementation();
    if (parent.isError())
        return parent;

    options().biomeLayer().open(getReadOptions());
    options().maskLayer().open(getReadOptions());
    options().waterLayer().open(getReadOptions());
    options().colorLayer().open(getReadOptions());
    options().landCoverLayer().open(getReadOptions());

    setProfile(Profile::create(Profile::GLOBAL_GEODETIC));

    return Status::OK();
}

Status
LifeMapLayer::closeImplementation()
{
    return super::closeImplementation();
}

void
LifeMapLayer::prepareForRendering(TerrainEngine* terrain)
{
    super::prepareForRendering(terrain);
    _terrainOptions = terrain->getOptions();
}

void
LifeMapLayer::checkForLayerError(Layer* layer)
{
    if (layer && layer->getStatus().isError())
    {
        std::string name = layer->getName();
        if (name.empty())
            name = Stringify() << "Unnamed " << layer->className();

        std::string msg = Stringify()
            << "Error in dependent layer \"" << name << "\" : "
            << layer->getStatus().message();

        setStatus(Status::ResourceUnavailable, msg);
    }
}

void
LifeMapLayer::addedToMap(const Map* map)
{
    super::addedToMap(map);

    options().biomeLayer().addedToMap(map);
    options().maskLayer().addedToMap(map);
    options().waterLayer().addedToMap(map);
    options().colorLayer().addedToMap(map);
    options().landCoverLayer().addedToMap(map);

    // not specified; try to find it
    if (!getBiomeLayer())
        setBiomeLayer(map->getLayer<BiomeLayer>());

    Status biomeStatus = options().biomeLayer().open(getReadOptions());
    if (getBiomeLayer())
    {
        OE_INFO << LC << "Using biome layer \"" << getBiomeLayer()->getName() << "\"" << std::endl;
    }

    // validate that all dependent layers opened OK, since we do not
    // want to generate invalid lifemap data.
    checkForLayerError(getBiomeLayer());
    checkForLayerError(getMaskLayer());
    checkForLayerError(getWaterLayer());
    checkForLayerError(getColorLayer());
    checkForLayerError(getLandCoverLayer());

    // Initialize the landcover creator
    if (getLandCoverLayer() && getLandCoverLayer()->isOpen())
    {
        _landCoverFactory = LandCoverSample::Factory::create(getLandCoverLayer());
    }

    _map = map;
}

void
LifeMapLayer::removedFromMap(const Map* map)
{
    _map = nullptr;
    options().biomeLayer().removedFromMap(map);
    options().maskLayer().removedFromMap(map);
    options().waterLayer().removedFromMap(map);
    options().colorLayer().removedFromMap(map);
    options().landCoverLayer().removedFromMap(map);
    super::removedFromMap(map);
}

void
LifeMapLayer::setBiomeLayer(BiomeLayer* layer)
{
    options().biomeLayer().setLayer(layer);
}

BiomeLayer*
LifeMapLayer::getBiomeLayer() const
{
    return options().biomeLayer().getLayer();
}

void
LifeMapLayer::setMaskLayer(ImageLayer* layer)
{
    options().maskLayer().setLayer(layer);
}

ImageLayer*
LifeMapLayer::getMaskLayer() const
{
    return options().maskLayer().getLayer();
}

void
LifeMapLayer::setWaterLayer(ImageLayer* layer)
{
    options().waterLayer().setLayer(layer);
}

ImageLayer*
LifeMapLayer::getWaterLayer() const
{
    return options().waterLayer().getLayer();
}

void
LifeMapLayer::setLandCoverLayer(CoverageLayer* layer)
{
    options().landCoverLayer().setLayer(layer);
}

CoverageLayer*
LifeMapLayer::getLandCoverLayer() const
{
    return options().landCoverLayer().getLayer();
}

void
LifeMapLayer::setColorLayer(ImageLayer* layer)
{
    options().colorLayer().setLayer(layer);
}

ImageLayer*
LifeMapLayer::getColorLayer() const
{
    return options().colorLayer().getLayer();
}

void
LifeMapLayer::setLandCoverWeight(float value)
{
    options().landCoverWeight() = value;
}

float
LifeMapLayer::getLandCoverWeight() const
{
    return options().landCoverWeight().get();
}

bool
LifeMapLayer::getUseLandCover() const
{
    return getLandCoverWeight() > 0;
}

void
LifeMapLayer::setTerrainWeight(float value)
{
    options().terrainWeight() = value;
}

float
LifeMapLayer::getTerrainWeight() const
{
    return options().terrainWeight().get();
}

bool
LifeMapLayer::getUseTerrain() const
{
    return getTerrainWeight() > 0.0f;
}

void
LifeMapLayer::setColorWeight(float value)
{
    options().colorWeight() = value;
}

float
LifeMapLayer::getColorWeight() const
{
    return options().colorWeight().get();
}

bool
LifeMapLayer::getUseColor() const
{
    return getColorWeight() > 0.0f;
}

void
LifeMapLayer::setNoiseWeight(float value)
{
    options().noiseWeight() = value;
}

float
LifeMapLayer::getNoiseWeight() const
{
    return options().noiseWeight().get();
}

bool
LifeMapLayer::getUseNoise() const
{
    return getNoiseWeight() > 0.0f;
}

#define NUM_INPUTS 4

#define NOISE 0
#define TERRAIN 1
#define LANDCOVER 2
#define COLOR 3

#define NOISE_LEVELS 2

GeoImage
LifeMapLayer::createImageImplementation(
    const TileKey& key,
    ProgressCallback* progress) const
{
    OE_PROFILING_ZONE;

    osg::ref_ptr<const Map> map;
    if (!_map.lock(map))
        return GeoImage::INVALID;

    // collect the elevation data:
    osg::ref_ptr<ElevationTile> elevTile;
    ElevationPool* ep = map->getElevationPool();
    ep->getTile(key, true, elevTile, &_workingSet, progress);

    // ensure we have a normal map for slopes and curvatures:
    if (elevTile.valid() && getTerrainWeight() > 0.0f && _terrainOptions.getUseNormalMaps())
    {
        elevTile->generateNormalMap(map.get(), _terrainOptions.getNormalMapTileSize(),
            &_workingSet, progress);
    }

    GeoExtent extent = key.getExtent();

    // set up the land cover data metatiler:
    MetaTile<GeoCoverage<LandCoverSample>> landcover;
    if (_landCoverFactory)
    {
        auto creator = [&](const TileKey& key, ProgressCallback* p) {
            return _landCoverFactory->createCoverage(key, p);
            };
        landcover.setCreateTileFunction(creator);
        landcover.setCenterTileKey(key, progress);
    }

    GeoImage densityMask;
    ImageUtils::PixelReader readDensityMask;
    osg::Matrixf dm_matrix;

    // the mask layer zero's out density(etc)
    if (getMaskLayer())
    {
        TileKey dm_key(key);

        while (dm_key.valid() && !densityMask.valid())
        {
            densityMask = getMaskLayer()->createImage(dm_key, progress);
            if (!densityMask.valid())
                dm_key.makeParent();
        }

        if (densityMask.valid())
        {
            readDensityMask.setImage(densityMask.getImage());
            readDensityMask.setBilinear(true);
            readDensityMask.setSampleAsTexture(true);
            extent.createScaleBias(dm_key.getExtent(), dm_matrix);
        }
    }

    GeoImage waterMask;
    ImageUtils::PixelReader readWaterMask;
    osg::Matrixf wm_matrix;

    if (getWaterLayer())
    {
        TileKey wm_key(key);

        while (wm_key.valid() && !waterMask.valid())
        {
            waterMask = getWaterLayer()->createImage(wm_key, progress);
            if (!waterMask.valid())
                wm_key.makeParent();
        }

        if (waterMask.valid())
        {
            readWaterMask.setImage(waterMask.getImage());
            readWaterMask.setBilinear(true);
            readWaterMask.setSampleAsTexture(true);
            extent.createScaleBias(wm_key.getExtent(), wm_matrix);
        }
    }

    // the color layer alters lifemap values based on colors.
    GeoImage color;
    ImageUtils::PixelReader readColor;
    osg::Matrixf color_matrix;

    if (getColorLayer() && getColorWeight() > 0.0f)
    {
        TileKey color_key(key);

        while (color_key.valid() && !color.valid())
        {
            color = getColorLayer()->createImage(color_key, progress);
            if (!color.valid())
                color_key.makeParent();
        }

        if (color.valid())
        {
            readColor.setImage(color.getImage());
            readColor.setBilinear(true);
            readColor.setSampleAsTexture(true);
            extent.createScaleBias(color_key.getExtent(), color_matrix);
        }
    }

    // assemble the image:
    osg::ref_ptr<osg::Image> image = new osg::Image();
    image->allocateImage(
        getTileSize(),
        getTileSize(),
        1,
        GL_RGBA,
        GL_UNSIGNED_BYTE);

    ImageUtils::PixelWriter write(image.get());

    float elevation;
    osg::Vec3 normal;
    float slope;
    const osg::Vec3 up(0, 0, 1);

    std::string lu_id;
    osg::Vec4f hsl;

    osg::Vec2d noiseCoords[NOISE_LEVELS];
    osg::Vec4 noise[NOISE_LEVELS];
    const unsigned noiseLOD[NOISE_LEVELS] = { 10u, 14u };
    //    12u, 13u, 14u, 15u //, 16u // 0u, 9u, 13u, 16u
    //};
    const unsigned noisePattern[NOISE_LEVELS] = { RANDOM, CLUMPY };
    //RANDOM, SMOOTH, CLUMPY, RANDOM2 };

    CoordScaler coordScalers[NOISE_LEVELS] = {
        CoordScaler(key.getProfile(), key.getLOD(), noiseLOD[0]),
        CoordScaler(key.getProfile(), key.getLOD(), noiseLOD[1]) //,
        //CoordScaler(key.getProfile(), key.getLOD(), noiseLOD[2]),
        //CoordScaler(key.getProfile(), key.getLOD(), noiseLOD[3])
    };

    ImageUtils::PixelReader noiseSampler(_noiseFunc.get());
    noiseSampler.setBilinear(true);
    noiseSampler.setSampleAsRepeatingTexture(true);

    // size of the tile in meters:
    GeoExtent ext = key.getExtent();
    double width_m = ext.width(Units::METERS);
    double height_m = ext.height(Units::METERS);

    // land cover blurring values
    double lc_blur_m = std::max(0.0, options().landCoverBlur()->as(Units::METERS));
    double lc_blur_u = lc_blur_m / width_m;
    double lc_blur_v = lc_blur_m / height_m;

    double mpp_x = width_m / (double)getTileSize();
    double mpp_y = height_m / (double)getTileSize();

    // landcover material index lookup table:
    std::unordered_map<std::string, unsigned> materialLUT;
    if (getBiomeLayer() && getLandCoverLayer() && getUseLandCover())
    {
        unsigned ptr = 0;
        for (auto& material : getBiomeLayer()->getBiomeCatalog()->getAssets().getMaterials())
        {
            materialLUT[material.name().get()] = ptr++;
        }
    }

    std::minstd_rand gen(key.hash());

    GeoImage result(image.get(), extent);

    {
        OE_PROFILING_ZONE_NAMED("RasterizeLifeMap");

        double bu = 0.5 / (double)image->s();
        double bv = 0.5 / (double)image->t();

        for (unsigned int t = 0; t < result.t(); ++t)
        {
            double v = bv + ((double)t * 2.0 * bv);
            double y = result.getExtent().yMin() + result.getExtent().height() * v;

            for (unsigned int s = 0; s < result.s(); ++s)
            {
                double u = bu + ((double)s * 2.0 * bu);
                double x = result.getExtent().xMin() + result.getExtent().width() * u;

                osg::Vec4f pixel[NUM_INPUTS];
                float weight[NUM_INPUTS] = { 0,0,0,0 };
                osg::Vec4f temp;

                // in case the land cover specifies a custom material.
                unsigned customMaterialIndex = 0u;

                // NOISE contribution
                if (getUseNoise())
                {
                    for (int n = 0; n < NOISE_LEVELS; ++n)
                    {
                        if (key.getLOD() >= coordScalers[n]._refLOD)
                        {
                            noiseCoords[n].set(u, v);
                            coordScalers[n].scaleCoordsToRefLOD(noiseCoords[n], key);
                            getNoise(noise[n], noiseSampler, noiseCoords[n]);

                            //double L = 1.0; // 1.0 / pow(2.0, double(NOISE_LEVELS - 1 - n));
                            int p = noisePattern[n];
                            double L = 1.0; //  n == 0 ? 0.25 : 1.0;

                            pixel[NOISE][LIFEMAP_DENSE] += noise[n][p] * L; // 3
                            pixel[NOISE][LIFEMAP_LUSH] = 0.0; // += noise[n][p] * L; // = 0.0;

                            noiseCoords[n].set(v, u);
                            getNoise(noise[n], noiseSampler, noiseCoords[n]);
                            pixel[NOISE][LIFEMAP_RUGGED] += noise[n][p] * L; // 2
                        }
                    }

                    //pixel[NOISE][LIFEMAP_DENSE] = clamp(pixel[NOISE][LIFEMAP_DENSE], 0.0f, 1.0f);
                    //pixel[NOISE][LIFEMAP_RUGGED] = clamp(pixel[NOISE][LIFEMAP_RUGGED], 0.0f, 1.0f);

                    weight[NOISE] = getNoiseWeight();
                }

                // LAND COVER CONTRIBUTION
                if (getLandCoverLayer() && landcover.valid())
                {
                    const LandCoverSample* temp;
                    LandCoverSample sample;
                    int dense_samples = 0;
                    int lush_samples = 0;
                    int rugged_samples = 0;

                    if (equivalent(lc_blur_m, 0.0))
                    {
                        //temp = landcover.read((int)s, (int)t);
                        temp = landcover.read(u, v);
                        if (temp)
                        {
#if 0
                            // RNG with normal distribution between approx +1/-1
                            const float coverage_stddev = 1.0f / 12.0f; // 6.0f;
                            {
                                std::normal_distribution<float> normal_dist(temp->dense().get(), coverage_stddev);
                                pixel[LANDCOVER][LIFEMAP_DENSE] = clamp(normal_dist(gen), 0.0f, 1.0f);
                            }
                            {
                                std::normal_distribution<float> normal_dist(temp->lush().get(), coverage_stddev);
                                pixel[LANDCOVER][LIFEMAP_LUSH] = clamp(normal_dist(gen), 0.0f, 1.0f);
                            }
                            {
                                std::normal_distribution<float> normal_dist(temp->rugged().get(), coverage_stddev);
                                pixel[LANDCOVER][LIFEMAP_RUGGED] = clamp(normal_dist(gen), 0.0f, 1.0f);
                            }
#else
                            pixel[LANDCOVER][LIFEMAP_DENSE] = temp->dense().get();
                            pixel[LANDCOVER][LIFEMAP_LUSH] = temp->lush().get();
                            pixel[LANDCOVER][LIFEMAP_RUGGED] = temp->rugged().get();
#endif
                            weight[LANDCOVER] = getLandCoverWeight();

                            if (temp->material().isSet() && getBiomeLayer())
                            {
                                // land cover asked for a custom material. Find its index.
                                auto i = materialLUT.find(temp->material().get());
                                if (i != materialLUT.end())
                                    customMaterialIndex = i->second + 1;
                            }
                        }
                    }
                    else
                    {
                        // read the landcover with a blurring filter.
                        for (int a = -1; a <= 1; ++a)
                        {
                            for (int b = -1; b <= 1; ++b)
                            {
                                int ss = a * (int)(lc_blur_m / mpp_x);
                                int tt = b * (int)(lc_blur_m / mpp_y);

                                temp = landcover.read((int)s + ss, (int)t + tt);

                                if (temp)
                                {
                                    if (temp->dense().isSet())
                                    {
                                        sample.dense() = sample.dense().get() + temp->dense().get();
                                        ++dense_samples;
                                    }

                                    if (temp->lush().isSet())
                                    {
                                        sample.lush() = sample.lush().get() + temp->lush().get();
                                        ++lush_samples;
                                    }

                                    if (temp->rugged().isSet())
                                    {
                                        sample.rugged() = sample.rugged().get() + temp->rugged().get();
                                        ++rugged_samples;
                                    }

                                    if (temp->material().isSet() && getBiomeLayer())
                                    {
                                        // land cover asked for a custom material. Find its index.
                                        auto i = materialLUT.find(temp->material().get());
                                        if (i != materialLUT.end())
                                            customMaterialIndex = i->second + 1;
                                    }
                                }
                            }
                        }

                        weight[LANDCOVER] = 0.0f;

                        if (dense_samples > 0)
                        {
                            pixel[LANDCOVER][LIFEMAP_DENSE] = sample.dense().get() / (float)dense_samples;
                            weight[LANDCOVER] = getLandCoverWeight();
                        }
                        if (lush_samples > 0)
                        {
                            pixel[LANDCOVER][LIFEMAP_LUSH] = sample.lush().get() / (float)lush_samples;
                            weight[LANDCOVER] = getLandCoverWeight();
                        }
                        if (rugged_samples > 0)
                        {
                            pixel[LANDCOVER][LIFEMAP_RUGGED] = sample.rugged().get() / (float)rugged_samples;
                            weight[LANDCOVER] = getLandCoverWeight();
                        }
                    }
                }

                // COLOR CONTRIBUTION:
                if (color.valid())
                {
                    double uu = u * color_matrix(0, 0) + color_matrix(3, 0);
                    double vv = v * color_matrix(1, 1) + color_matrix(3, 1);
                    readColor(temp, uu, vv);

                    // convert to HSL:
                    Color c(temp.r(), temp.g(), temp.b(), 0.0f);
                    hsl = c.asHSL();

                    constexpr float red = 0.0f;
                    constexpr float green = 0.3333333f;
                    constexpr float blue = 0.6666667f;

                    // amplification factors for greenness and redness,
                    // obtained empirically
                    constexpr float green_amp = 2.0f;
                    constexpr float red_amp = 5.0f;

                    // Set lower limits for saturation and lightness, because
                    // when these levels get too low, the HUE channel starts to
                    // introduce math errors that can result in bad color values
                    // that we do not want. (We determined these empirically
                    // using an interactive shader.)
                    constexpr float saturation_threshold = 0.2f;
                    constexpr float lightness_threshold = 0.03f;

                    // "Greenness" implies vegetation
                    float dist_to_green = fabs(green - hsl[0]);
                    if (dist_to_green > 0.5f)
                        dist_to_green = 1.0f - dist_to_green;
                    float greenness = 1.0f - 2.0f * dist_to_green;

                    // "redness" implies ruggedness/rock
                    float dist_to_red = fabs(red - hsl[0]);
                    if (dist_to_red > 0.5f)
                        dist_to_red = 1.0f - dist_to_red;
                    float redness = 1.0f - 2.0f * dist_to_red;

                    if (hsl[1] < saturation_threshold)
                    {
                        greenness *= hsl[1] / saturation_threshold;
                        redness *= hsl[1] / saturation_threshold;
                    }
                    if (hsl[2] < lightness_threshold)
                    {
                        greenness *= hsl[2] / lightness_threshold;
                        redness *= hsl[2] / lightness_threshold;
                    }

                    greenness = pow(greenness, green_amp);
                    redness = pow(redness, red_amp);

                    float w = options().colorWeight().get();

                    pixel[COLOR][LIFEMAP_DENSE] = greenness;
                    pixel[COLOR][LIFEMAP_LUSH] = greenness * (1.0 - hsl.z()); // lighter green is less lush.
                    pixel[COLOR][LIFEMAP_RUGGED] = redness;

                    // if the lightness value is too high, it's white, which is usually
                    // snow or clouds, and we can't use it for anything meaningful
                    if (pow(hsl[2], 5.0f) > 0.5f)
                        weight[COLOR] = 0.0f;
                    else
                        weight[COLOR] = getColorWeight(); // * max(greeness, redness) ...???
                }

                // TERRAIN CONTRIBUTION:
                if (getUseTerrain() && elevTile.valid())
                {
                    // Establish elevation at this pixel:
                    elevation = elevTile->getElevation(x, y).elevation().as(Units::METERS);

                    // Normal map at this pixel:
                    normal = elevTile->getNormal(x, y);

                    // exaggerate the slope value
                    slope = 1.0 - (normal * up);
                    float r = decel(slope * options().slopeIntensity().get());
                    pixel[TERRAIN][LIFEMAP_RUGGED] = r;
                    pixel[TERRAIN][LIFEMAP_DENSE] = -r;
                    pixel[TERRAIN][LIFEMAP_LUSH] = -r;

                    weight[TERRAIN] = getTerrainWeight();
                }

                // CONBINE WITH WEIGHTS:
                osg::Vec4f combined_pixel;

                // first, combine landcover and color by relative weight.
                float w2 = weight[LANDCOVER] + weight[COLOR];
                if (w2 > 0.0f)
                {
                    combined_pixel =
                        pixel[LANDCOVER] * weight[LANDCOVER] / w2 +
                        pixel[COLOR] * weight[COLOR] / w2;
                }

                // apply terrain additively:
                combined_pixel += pixel[TERRAIN] * weight[TERRAIN];

                // apply the noise additively:
                combined_pixel += pixel[NOISE] * weight[NOISE];

                // apply the lushness static factor
                //combined_pixel[LIFEMAP_LUSH] *= options().lushFactor().get();

                // MASK CONTRIBUTION (applied to final combined pixel data)
                if (densityMask.valid())
                {
                    double uu = clamp(u * dm_matrix(0, 0) + dm_matrix(3, 0), 0.0, 1.0);
                    double vv = clamp(v * dm_matrix(1, 1) + dm_matrix(3, 1), 0.0, 1.0);
                    readDensityMask(temp, uu, vv);

                    // multiply all 3 so that roads can have a barren look
                    combined_pixel[LIFEMAP_DENSE] *= temp.r();
                    combined_pixel[LIFEMAP_LUSH] *= temp.r();
                    combined_pixel[LIFEMAP_RUGGED] *= temp.r();
                }

                // WATER MASK
                if (waterMask.valid())
                {
                    double uu = clamp(u * wm_matrix(0, 0) + wm_matrix(3, 0), 0.0, 1.0);
                    double vv = clamp(v * wm_matrix(1, 1) + wm_matrix(3, 1), 0.0, 1.0);
                    readWaterMask(temp, uu, vv);

                    combined_pixel[LIFEMAP_DENSE] *= temp.r();
                    combined_pixel[LIFEMAP_LUSH] *= temp.r();
                    combined_pixel[LIFEMAP_RUGGED] *= temp.r();
                    combined_pixel[3] = 1.0f - temp.r();
                }
                else combined_pixel[3] = 0.0f;

                if (customMaterialIndex > 0)
                {
                    combined_pixel[3] = (float)customMaterialIndex / 255.0f;
                }

                // Clamp everything to [0..1] and write it out.
                for (int i = 0; i < 4; ++i)
                {
                    combined_pixel[i] = clamp(combined_pixel[i], 0.0f, 1.0f);
                }

                write(combined_pixel, s, t);
            }
        }
    }

    return std::move(result);
}

GeoImage
LifeMapLayer::applyPostLayer(const GeoImage& canvas, const TileKey& key, Layer* postLayer, ProgressCallback* progress) const
{
    auto coverageLayer = dynamic_cast<CoverageLayer*>(postLayer);
    if (coverageLayer && canvas.valid())
    {
        // Is there any way to actually type-check this?
        auto factory = LandCoverSample::Factory::create(coverageLayer);

        auto coverage = factory->createCoverage(key, progress);

        if (coverage.valid() && !coverage.empty())
        {
            // landcover material index lookup table:
            // TODO: read this ONCE and store as a member - createImageImpl uses it too.
            std::unordered_map<std::string, unsigned> materialLUT;
            if (getBiomeLayer())
            {
                unsigned ptr = 0;
                for (auto& material : getBiomeLayer()->getBiomeCatalog()->getAssets().getMaterials())
                {
                    materialLUT[material.name().get()] = ptr++;
                }
            }

            auto result_image = osg::clone(canvas.getImage(), osg::CopyOp::DEEP_COPY_ALL);

            ImageUtils::PixelReader read_canvas(canvas.getImage());
            ImageUtils::PixelWriter write_result(result_image);

            const auto& extent = canvas.getExtent();

            for (unsigned t = 0; t < read_canvas.t(); ++t)
            {
                double v = (double)t / (double)read_canvas.t();
                for (unsigned s = 0; s < read_canvas.s(); ++s)
                {
                    double u = (double)s / (double)read_canvas.s();

                    // do we need to offset this be half a 'pixel'?
                    double x = extent.xMin() + u * extent.width();
                    double y = extent.yMin() + v * extent.height();

                    auto* sample = coverage.readAtCoords(x, y);
                    if (sample)
                    {
                        // populate the pixel with the original values:
                        osg::Vec4 value;
                        read_canvas(value, s, t);

                        // overwrite with the coverage data:
                        if (sample->rugged().isSet())
                            value[LIFEMAP_RUGGED] = sample->rugged().value();
                        if (sample->dense().isSet())
                            value[LIFEMAP_DENSE] = sample->dense().value();
                        if (sample->lush().isSet())
                            value[LIFEMAP_LUSH] = sample->lush().value();

                        if (sample->material().isSet())
                        {
                            // land cover asked for a custom material. Find its index.
                            auto i = materialLUT.find(sample->material().value());
                            if (i != materialLUT.end())
                                value[LIFEMAP_SPECIAL] = (i->second + 1);
                        }

                        write_result(value, s, t);
                    }
                }
            }
             
            return GeoImage(result_image, canvas.getExtent());
        }
    }

    return super::applyPostLayer(canvas, key, postLayer, progress);
}