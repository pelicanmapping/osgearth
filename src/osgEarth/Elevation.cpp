
/* osgEarth
* Copyright 2008-2016 Pelican Mapping
* MIT License
*/
#include <osgEarth/Elevation>
#include <osgEarth/Registry>
#include <osgEarth/Map>
#include <osgEarth/Progress>
#include <osgEarth/Math>
#include <unordered_map>
#include <array>

// for OSGEARTH_USE_16BIT_ELEVATION_TEXTURES
#include <osgEarth/BuildConfig>

using namespace osgEarth;

// NOTE: if you change this, don't forget to change the decoding in the SDK shaders as well!
#ifdef OSGEARTH_USE_16BIT_ELEVATION_TEXTURES
    //#define ELEV_PIXEL_FORMAT GL_RG
    //#define ELEV_INTERNAL_FORMAT GL_RG8
    //#define ELEV_DATA_TYPE GL_UNSIGNED_BYTE
    #define ELEV_PIXEL_FORMAT GL_RED
    #define ELEV_INTERNAL_FORMAT GL_R16
    #define ELEV_DATA_TYPE GL_UNSIGNED_SHORT
#else
    #define ELEV_PIXEL_FORMAT GL_RED
    #define ELEV_INTERNAL_FORMAT GL_R32F
    #define ELEV_DATA_TYPE GL_FLOAT
#endif

osg::Texture*
osgEarth::createEmptyElevationTile()
{
    osg::Image* image = new osg::Image();
    image->allocateImage(1, 1, 1, ELEV_PIXEL_FORMAT, ELEV_DATA_TYPE);
    image->setInternalTextureFormat(ELEV_INTERNAL_FORMAT);
    ImageUtils::PixelWriter write(image);
    write(osg::Vec4f(0.0f, 0.0f, 0.0f, 0.0f), 0, 0); // zero out
    osg::Texture2D* tex = new osg::Texture2D(image);
    tex->setInternalFormat(ELEV_INTERNAL_FORMAT);
    tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    tex->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());
    tex->setName("empty:elevation");
    return tex;
}

osg::Texture*
osgEarth::createEmptyNormalMapTexture()
{
    osg::Image* image = new osg::Image();
    image->allocateImage(1, 1, 1, GL_RG, GL_UNSIGNED_BYTE);
    image->setInternalTextureFormat(GL_RG8);
    ImageUtils::PixelWriter write(image);
    write(osg::Vec4(0.5f, 0.5f, 0.0f, 0.0f), 0, 0); // BC5 octahedral encoded Z-up
    osg::Texture2D* tex = new osg::Texture2D(image);
    tex->setInternalFormat(image->getInternalTextureFormat());
    tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    tex->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());
    tex->setName("empty:normalmap");
    return tex;
}

ElevationTile::ElevationTile(const TileKey& key, const GeoHeightField& in_hf, std::vector<float>&& resolutions) :
    _tilekey(key),
    _extent(in_hf.getExtent()),
    _resolutions(std::move(resolutions))
{
    if (in_hf.valid())
    {
        _heightField = in_hf.getHeightField();

        _maxima = { in_hf.getMinHeight(), in_hf.getMaxHeight() };

        osg::Vec4 value;

        // Encode elevation as a 2-channel texture, RG, where R = high byte and G = low byte of a 16-bit integer.
        // The 16-bit integer is a normalized value between 0 and 65535, where 0 corresponds to the minimum height
        // and 65535 corresponds to the maximum height as encoded in the tile metadata.
        osg::Image* heights = new osg::Image();
        heights->allocateImage(_heightField->getNumColumns(), _heightField->getNumRows(), 1, ELEV_PIXEL_FORMAT, ELEV_DATA_TYPE);
        heights->setInternalTextureFormat(ELEV_INTERNAL_FORMAT);

        _allHeightsAtNativeResolution = true;
        float nativeRes = _heightField->getYInterval(); // assume square pixels

        if (heights->getPixelFormat() == GL_RED && heights->getDataType() == GL_UNSIGNED_SHORT)
        {
            // GL_RED/SHORT = 16-bit encoded relative height values (0..1 from min to max)
            auto* data = reinterpret_cast<GLshort*>(heights->data());
            unsigned rp = 0;
            for (auto h : _heightField->getHeightList())
            {
                float t = (h - _maxima.first) / (_maxima.second - _maxima.first);
                *data++ = (GLushort)(65535.0f * t);

                // in the meantime determine whether this tile is native resolution.
                if (_allHeightsAtNativeResolution && !_resolutions.empty() && !osgEarth::equivalent(_resolutions[rp++], nativeRes))
                {
                    _allHeightsAtNativeResolution = false;
                }
            }

            _encoding = Encoding::R16;
        }
        else if (heights->getPixelFormat() == GL_RG)
        {
            // GL_RG = 16-bit encoded relative height values (0..1 from min to max)
            auto* data = heights->data();
            unsigned rp = 0;
            for (auto h : _heightField->getHeightList())
            {
                float t = (h - _maxima.first) / (_maxima.second - _maxima.first);
                int ti = (int)(65535.0f * t);
                *data++ = (std::int8_t)((ti >> 8) & 0xFF); // high byte
                *data++ = (std::int8_t)(ti & 0xFF); // low byte

                // in the meantime determine whether this tile is native resolution.
                if (_allHeightsAtNativeResolution && !_resolutions.empty() && !osgEarth::equivalent(_resolutions[rp++], nativeRes))
                {
                    _allHeightsAtNativeResolution = false;
                }
            }

            _encoding = Encoding::RG8;
        }
        else // GL_R32F
        {
            memcpy(heights->data(), _heightField->getHeightList().data(), sizeof(float) * _heightField->getNumRows() * _heightField->getNumColumns());

            // native res detection
            for (unsigned rp = 0; rp < _resolutions.size(); ++rp)
            {
                if (!osgEarth::equivalent(_resolutions[rp], nativeRes))
                {
                    _allHeightsAtNativeResolution = false;
                    break;
                }
            }

            _encoding = Encoding::R32F;
        }

        // Can't compress the elevation because it will no longer match up 
        // at the tile seams due to the lossy compression of BC5 or BC7.
        //ImageUtils::compressImageInPlace(heights);

        _elevationTex = new osg::Texture2D(heights);
        _elevationTex->setName(key.str() + ":elevation");

        _elevationTex->setDataVariance(osg::Object::STATIC);
        _elevationTex->setInternalFormat(heights->getInternalTextureFormat());
        _elevationTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        _elevationTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        _elevationTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        _elevationTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        _elevationTex->setResizeNonPowerOfTwoHint(false);
        _elevationTex->setMaxAnisotropy(1.0f);

        // Pooled, so never expire them.
        _elevationTex->setUnRefImageDataAfterApply(false);

        _read.setTexture(_elevationTex.get());
        _read.setSampleAsTexture(false);

        _resolution = Distance(
            getExtent().height() / ((double)(heights->t() - 1)),
            getExtent().getSRS()->getUnits());
    }
}

ElevationSample
ElevationTile::getElevation(double x, double y) const
{
    double u = (x - getExtent().xMin()) / getExtent().width();
    double v = (y - getExtent().yMin()) / getExtent().height();

    return getElevationUV(u, v);
}

osg::Vec3
ElevationTile::getNormal(double x, double y) const
{
    osg::Vec3 normal(0,0,1);

    if (_readNormal.valid())
    {
        double u = (x - getExtent().xMin()) / getExtent().width();
        double v = (y - getExtent().yMin()) / getExtent().height();
        normal = NormalMapGenerator::unpack(_readNormal(u, v));
    }
    else if (getHeightField())
    {
        auto& e = getExtent();
        auto res = getHeightField()->getYInterval(); // assume square pixels

        auto west = getElevation(std::max(x - res, e.xMin()), y);
        auto east = getElevation(std::min(x + res, e.xMax()), y);
        auto south = getElevation(x, std::max(y - res, e.yMin()));
        auto north = getElevation(x, std::min(y + res, e.yMax()));

        if (west.hasData() && east.hasData() && south.hasData() && north.hasData())
        {
            auto res_m = getExtent().getSRS()->transformDistance(Distance(res, e.getSRS()->getUnits()), Units::METERS, (e.yMin() + e.yMax()) / 2.0);
            osg::Vec3 a[4];
            a[0].set(-res, 0, west.elevation().as(Units::METERS));
            a[1].set(res, 0, east.elevation().as(Units::METERS));
            a[2].set(0, -res, south.elevation().as(Units::METERS));
            a[3].set(0, res, north.elevation().as(Units::METERS));
            normal = (a[1] - a[0]) ^ (a[3] - a[2]);
            normal.normalize();
        }
    }
    return normal;
}

void
ElevationTile::generateNormalMap(const Map* map, unsigned tileSize, void* workingSet, ProgressCallback* progress)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_normalTex.valid())
    {
        NormalMapGenerator gen;

        _normalTex = gen.createNormalMap(getTileKey(), map, tileSize, allHeightsAtNativeResolution(), workingSet, progress);

        if (_normalTex.valid())
        {
            // these are pooled, so do not expire them.
            _normalTex->setUnRefImageDataAfterApply(false);

            if (_normalTex->getImage())
            {
                _readNormal.setImage(_normalTex->getImage());
                _readNormal.setBilinear(true);
            }
        }
    }
}


#undef LC
#define LC "[NormalMapGenerator] "

osg::Texture2D*
NormalMapGenerator::createNormalMap(const TileKey& key, const Map* map, unsigned tileSize,
    bool nativeResolutionFastPath, void* ws, ProgressCallback* progress)
{
    if (!map)
        return NULL;

    const bool compress = true;

    // the sampling goes a LOT faster if the normal map size is the same as the elevation tile size,
    // since there are fewer unique samples to take.
    // It is actually (must) faster to sample 1/4 the points and then resize the image to 256x256 later
    // during the compression stage.

    if (tileSize == 0)
        tileSize = compress ? 256 : ELEVATION_TILE_SIZE;

    ElevationPool::WorkingSet* workingSet = static_cast<ElevationPool::WorkingSet*>(ws);

    osg::ref_ptr<osg::Image> image = new osg::Image();
    image->allocateImage(tileSize, tileSize, 1, GL_RG, GL_UNSIGNED_BYTE);
    image->setInternalTextureFormat(GL_RG8);
    ImageUtils::PixelWriter write(image.get());

    ElevationPool* pool = map->getElevationPool();

    const GeoExtent& ex = key.getExtent();

    // fetch the base tile in order to get resolutions data.
    osg::ref_ptr<ElevationTile> heights;
    pool->getTile(key, false, heights, workingSet, progress); // no fallback!

    if (!heights.valid())
        return nullptr;

    auto* srs = key.getProfile()->getSRS();
    Distance res(heights->getHeightField()->getXInterval(), srs->getUnits());
    double dy = srs->transformDistance(res, Units::METERS);

    // if we are assuming that all data in the tile is native resolution, we can use the tile
    // directly to sample points and create normals (except along the edges). This is way
    // faster than sending all the points to the elevation pool for sampling.
    if (nativeResolutionFastPath)
    {
        double x0 = ex.xMin();
        double y0 = ex.yMin();
        double x1 = ex.xMax();
        double y1 = ex.yMax();
        osg::Vec3 normal;
        std::array<osg::Vec3, 4> a;
        double z_west = 0.0, z_east = 0.0, z_north = 0.0, z_south = 0.0;
        double du = 1.0 / (double)(tileSize - 1);
        double dv = 1.0 / (double)(tileSize - 1);

        // Process interior pixels only (excluding edges that need external sampling)
        for (int t = 1; t < (int)tileSize - 1; ++t)
        {
            double v = (double)t / (double)(tileSize - 1);
            double y_or_lat = ex.yMin() + v * ex.height();

            for (int s = 1; s < (int)tileSize - 1; ++s)
            {
                double u = (double)s / (double)(tileSize - 1);
                double x_or_lon = ex.xMin() + u * ex.width();

                // For interior pixels, all neighbors are within the tile
                z_west = heights->getRawElevationUV(u - du, v);
                z_east = heights->getRawElevationUV(u + du, v);
                z_north = heights->getRawElevationUV(u, v + dv);
                z_south = heights->getRawElevationUV(u, v - dv);

                double dx = srs->transformDistance(res, Units::METERS, y_or_lat);

                // only attempt to create a normal vector if all the data is valid:
                // four valid corner points.
                if (z_west != NO_DATA_VALUE && z_east != NO_DATA_VALUE && z_north != NO_DATA_VALUE && z_south != NO_DATA_VALUE)
                {
                    a[0].set(-dx, 0, z_west);
                    a[1].set(dx, 0, z_east);
                    a[2].set(0, -dy, z_south);
                    a[3].set(0, dy, z_north);
                    normal = (a[1] - a[0]) ^ (a[3] - a[2]);
                    normal.normalize();
                }
                else
                {
                    normal.set(0, 0, 1);
                }

                write(pack(normal), s, t);
            }
        }

        // now do the edges by using the elevation pool.
        struct Workspace
        {
            std::vector<osg::Vec4d> vectorToSample; // actual points we'll send to the elevation pool
        };
        static thread_local Workspace w;

        w.vectorToSample.clear();
        w.vectorToSample.reserve((tileSize+2) * 4);

        double r = res.getValue();

        // bottom row
        for (int s = 0; s < (int)tileSize; ++s)
        {
            double u = (double)s / (double)(tileSize - 1);
            double x = ex.xMin() + u * ex.width();
            w.vectorToSample.emplace_back(x, ex.yMin() - r, 0, r);
        }
        // top row
        for (int s = 0; s < (int)tileSize; ++s)
        {
            double u = (double)s / (double)(tileSize - 1);
            double x = ex.xMin() + u * ex.width();
            w.vectorToSample.emplace_back(x, ex.yMax() + r, 0, r);
        }
        // left column
        for (int t = 0; t < (int)tileSize; ++t)
        {
            double v = (double)t / (double)(tileSize - 1);
            double y = ex.yMin() + v * ex.height();
            w.vectorToSample.emplace_back(ex.xMin() - r, y, 0, r);
        }
        // right column
        for (int t = 0; t < (int)tileSize; ++t)
        {
            double v = (double)t / (double)(tileSize - 1);
            double y = ex.yMin() + v * ex.height();
            w.vectorToSample.emplace_back(ex.xMax() + r, y, 0, r);
        }

        int sampleOK = map->getElevationPool()->sampleMapCoords(
            w.vectorToSample.begin(), w.vectorToSample.end(),
            workingSet,
            progress);

        if (progress && progress->isCanceled())
        {
            // canceled. Bail.
            return nullptr;
        }

        if (sampleOK < 0)
        {
            OE_WARN << LC << "Internal error - contact support" << std::endl;
            return nullptr;
        }

        // Now compute normals for edge pixels using the sampled external data
        // Data organization in w.vectorToSample:
        // [0 to tileSize-1]: bottom row (y = yMin - dy) - tileSize points
        // [tileSize to 2*tileSize-1]: top row (y = yMax + dy) - tileSize points
        // [2*tileSize to 3*tileSize-1]: left column (x = xMin - dx) - tileSize points
        // [3*tileSize to 4*tileSize-1]: right column (x = xMax + dx) - tileSize points

        int bottomRowStart = 0;
        int topRowStart = tileSize;
        int leftColStart = 2 * tileSize;
        int rightColStart = 3 * tileSize;

        double dy = srs->transformDistance(res, Units::METERS, 0.0);

        // Process all edge pixels
        for (int t = 0; t < (int)tileSize; ++t)
        {
            for (int s = 0; s < (int)tileSize; ++s)
            {
                // Skip interior pixels - they were already processed
                if (t > 0 && t < (int)tileSize - 1 && s > 0 && s < (int)tileSize - 1)
                    continue;

                double u = (double)s / (double)(tileSize - 1);
                double v = (double)t / (double)(tileSize - 1);
                double y_or_lat = ex.yMin() + v * ex.height();

                double z_west, z_east, z_north, z_south;

                // Get west elevation (one pixel to the left)
                if (s == 0) {
                    // Left edge: use sampled left column data (external point)
                    int leftIdx = leftColStart + t;
                    z_west = w.vectorToSample[leftIdx].z();
                } else {
                    // Interior: use tile data (internal point)
                    z_west = heights->getRawElevationUV(u - du, v);
                }

                // Get east elevation (one pixel to the right)
                if (s == tileSize - 1) {
                    // Right edge: use sampled right column data (external point)
                    int rightIdx = rightColStart + t;
                    z_east = w.vectorToSample[rightIdx].z();
                } else {
                    // Interior: use tile data (internal point)
                    z_east = heights->getRawElevationUV(u + du, v);
                }

                // Get south elevation (one pixel down)
                if (t == 0) {
                    // Bottom edge: use sampled bottom row data (external point)
                    int bottomIdx = bottomRowStart + s;
                    z_south = w.vectorToSample[bottomIdx].z();
                } else {
                    // Interior: use tile data (internal point)
                    z_south = heights->getRawElevationUV(u, v - dv);
                }

                // Get north elevation (one pixel up)
                if (t == tileSize - 1) {
                    // Top edge: use sampled top row data (external point)
                    int topIdx = topRowStart + s;
                    z_north = w.vectorToSample[topIdx].z();
                } else {
                    // Interior: use tile data (internal point)
                    z_north = heights->getRawElevationUV(u, v + dv);
                }

                // Use the same fixed dx that was used for sampling external points
                // (In a perfect implementation, this should vary with latitude, but for
                // consistency with the sampling, we use the fixed value from line 331)

                double dx = srs->transformDistance(res, Units::METERS, y_or_lat);

                // Compute normal if all data is valid
                if (z_west != NO_DATA_VALUE && z_east != NO_DATA_VALUE && z_north != NO_DATA_VALUE && z_south != NO_DATA_VALUE)
                {
                    a[0].set(-dx, 0, z_west);
                    a[1].set(+dx, 0, z_east);
                    a[2].set(0, -dy, z_south);
                    a[3].set(0, +dy, z_north);
                    normal = (a[1] - a[0]) ^ (a[3] - a[2]);
                    normal.normalize();
                }
                else
                {
                    normal.set(0, 0, 1);
                }

                write(pack(normal), s, t);
            }
        }
    }

    else
    {
        struct Point {
            double x, y;
        };

        struct PointHash {
            inline std::size_t operator()(const Point& k) const {
                auto h1 = std::hash<double>()(k.x);
                auto h2 = std::hash<double>()(k.y);
                return h1 ^ (h2 << 1);
            }
        };
        struct PointEquals {
            inline bool operator()(const Point& lhs, const Point& rhs) const {
                return lhs.x == rhs.x && lhs.y == rhs.y;
            }
        };

        struct Workspace {
            std::unordered_map<Point, int, PointHash, PointEquals> uniquePoints; // map a point to its index in vectorToSample
            std::vector<osg::Vec4d> vectorToSample; // actual points we'll send to the elevation pool
            std::vector<int> rasterIndex; // maps the raster offset to an entry in vectorToSample.
        };
        static thread_local Workspace w;

        w.uniquePoints.clear();
        w.vectorToSample.clear();
        w.vectorToSample.reserve(write.s() * write.t() * 4);
        w.rasterIndex.clear();
        w.rasterIndex.reserve(write.s() * write.t() * 4);

        for (int t = 0; t < write.t(); ++t)
        {
            double v = (double)t / (double)(write.t() - 1);
            double y = ex.yMin() + v * ex.height();

            for (int s = 0; s < write.s(); ++s)
            {
                double u = (double)s / (double)(write.s() - 1);
                double x = ex.xMin() + u * ex.width();
                double r = heights->getResolution(s, t);

                {
                    auto a = w.uniquePoints.emplace(Point{ x - r, y }, (int)w.vectorToSample.size()); auto& iter = a.first; bool isNew = a.second;
                    //auto [iter, isNew] = w.uniquePoints.emplace(Point{ x - r, y }, (int)w.vectorToSample.size()); // C++17
                    if (isNew) w.vectorToSample.emplace_back(x - r, y, 0, r);
                    w.rasterIndex.emplace_back(iter->second);
                }
                {
                    auto a = w.uniquePoints.emplace(Point{ x + r, y }, (int)w.vectorToSample.size()); auto& iter = a.first; bool isNew = a.second;
                    //auto [iter, isNew] = w.uniquePoints.emplace(Point{ x + r, y }, (int)w.vectorToSample.size()); // C++17
                    if (isNew) w.vectorToSample.emplace_back(x + r, y, 0, r);
                    w.rasterIndex.emplace_back(iter->second);
                }
                {
                    auto a = w.uniquePoints.emplace(Point{ x, y - r }, (int)w.vectorToSample.size()); auto& iter = a.first; bool isNew = a.second;
                    //auto [iter, isNew] = w.uniquePoints.emplace(Point{ x, y - r }, (int)w.vectorToSample.size()); // C++17
                    if (isNew) w.vectorToSample.emplace_back(x, y - r, 0, r);
                    w.rasterIndex.emplace_back(iter->second);
                }
                {
                    auto a = w.uniquePoints.emplace(Point{ x, y + r }, (int)w.vectorToSample.size()); auto& iter = a.first; bool isNew = a.second;
                    //auto [iter, isNew] = w.uniquePoints.emplace(Point{ x, y + r }, (int)w.vectorToSample.size()); // C++17
                    if (isNew) w.vectorToSample.emplace_back(x, y + r, 0, r);
                    w.rasterIndex.emplace_back(iter->second);
                }
            }
        }

        int sampleOK = map->getElevationPool()->sampleMapCoords(
            w.vectorToSample.begin(), w.vectorToSample.end(),
            workingSet,
            progress);

        if (progress && progress->isCanceled())
        {
            // canceled. Bail.
            return NULL;
        }

        if (sampleOK < 0)
        {
            OE_WARN << LC << "Internal error - contact support" << std::endl;
            return NULL;
        }

        osg::Vec3 normal;
        std::array<osg::Vec3, 4> a;
        auto* srs = key.getProfile()->getSRS();
        Distance res(0.0, srs->getUnits());
        double dx, dy;
        double z_west, z_east, z_south, z_north;

        unsigned p = 0;

        for (int t = 0; t < write.t(); ++t)
        {
            double v = (double)t / (double)(write.t() - 1);
            double y_or_lat = ex.yMin() + v * ex.height();

            for (int s = 0; s < write.s(); ++s)
            {
                auto sampleRes = w.vectorToSample[w.rasterIndex[p]].w();
                z_west = w.vectorToSample[w.rasterIndex[p++]].z();
                z_east = w.vectorToSample[w.rasterIndex[p++]].z();
                z_south = w.vectorToSample[w.rasterIndex[p++]].z();
                z_north = w.vectorToSample[w.rasterIndex[p++]].z();

                res.set(sampleRes, res.getUnits());
                dx = srs->transformDistance(res, Units::METERS, y_or_lat);
                dy = srs->transformDistance(res, Units::METERS, 0.0);

                // only attempt to create a normal vector if all the data is valid:
                // a valid resolution value and four valid corner points.
                if (res.getValue() != FLT_MAX &&
                    z_west != NO_DATA_VALUE && z_east != NO_DATA_VALUE && z_south != NO_DATA_VALUE && z_north != NO_DATA_VALUE)
                {
                    a[0].set(-dx, 0, z_west);
                    a[1].set(+dx, 0, z_east);
                    a[2].set(0, -dy, z_south);
                    a[3].set(0, +dy, z_north);

                    normal = (a[1] - a[0]) ^ (a[3] - a[2]);
                    normal.normalize();
                }
                else
                {
                    normal.set(0, 0, 1);
                }

                write(pack(normal), s, t);
            }
        }
    }

    if (compress)
    {
        // compress the image (using BC5/RGTC2) and generate mipmaps.
        ImageUtils::compressImageInPlace(image.get());
    }

    osg::Texture2D* normalTex = new osg::Texture2D(image.get());

    normalTex->setDataVariance(osg::Object::STATIC);
    normalTex->setInternalFormat(image->getInternalTextureFormat());
    normalTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    normalTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
    normalTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    normalTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    normalTex->setResizeNonPowerOfTwoHint(false);
    normalTex->setMaxAnisotropy(1.0f);
    normalTex->setUnRefImageDataAfterApply(Registry::instance()->unRefImageDataAfterApply().get());

    return normalTex;
}

