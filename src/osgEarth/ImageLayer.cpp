/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ImageLayer>
#include <osgEarth/Progress>
#include <osgEarth/Metrics>
#include <osgEarth/NetworkMonitor>
#include <osgEarth/Random>
#include <osgEarth/Math>
#include <osgEarth/MetaTile>

#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
#include <Superluminal/PerformanceAPI.h>
#endif

using namespace osgEarth;

#define LC "[" << className() << "] \"" << getName() << "\" "

// TESTING
//#undef  OE_DEBUG
//#define OE_DEBUG OE_INFO

//------------------------------------------------------------------------

void
ImageLayer::Options::fromConfig(const Config& conf)
{
    conf.get( "nodata_image",   _noDataImageFilename );
    conf.get( "shared",         _shared );
    conf.get( "coverage",       _coverage );
    conf.get( "altitude",       _altitude );
    conf.get( "accept_draping", acceptDraping());
    conf.get( "edge_buffer_ratio", _edgeBufferRatio);
    conf.get( "reprojected_tilesize", _reprojectedTileSize);

    if ( conf.hasValue( "transparent_color" ) )
        _transparentColor = stringToColor( conf.value( "transparent_color" ), osg::Vec4ub(0,0,0,0));

    if ( conf.hasChild("color_filters") )
    {
        _colorFilters.mutable_value().clear();
        ColorFilterRegistry::instance()->readChain( conf.child("color_filters"), _colorFilters.mutable_value() );
    }

    conf.get("mag_filter","LINEAR",                _magFilter,osg::Texture::LINEAR);
    conf.get("mag_filter","LINEAR_MIPMAP_LINEAR",  _magFilter,osg::Texture::LINEAR_MIPMAP_LINEAR);
    conf.get("mag_filter","LINEAR_MIPMAP_NEAREST", _magFilter,osg::Texture::LINEAR_MIPMAP_NEAREST);
    conf.get("mag_filter","NEAREST",               _magFilter,osg::Texture::NEAREST);
    conf.get("mag_filter","NEAREST_MIPMAP_LINEAR", _magFilter,osg::Texture::NEAREST_MIPMAP_LINEAR);
    conf.get("mag_filter","NEAREST_MIPMAP_NEAREST",_magFilter,osg::Texture::NEAREST_MIPMAP_NEAREST);
    conf.get("min_filter","LINEAR",                _minFilter,osg::Texture::LINEAR);
    conf.get("min_filter","LINEAR_MIPMAP_LINEAR",  _minFilter,osg::Texture::LINEAR_MIPMAP_LINEAR);
    conf.get("min_filter","LINEAR_MIPMAP_NEAREST", _minFilter,osg::Texture::LINEAR_MIPMAP_NEAREST);
    conf.get("min_filter","NEAREST",               _minFilter,osg::Texture::NEAREST);
    conf.get("min_filter","NEAREST_MIPMAP_LINEAR", _minFilter,osg::Texture::NEAREST_MIPMAP_LINEAR);
    conf.get("min_filter","NEAREST_MIPMAP_NEAREST",_minFilter,osg::Texture::NEAREST_MIPMAP_NEAREST);

    conf.get("texture_compression", textureCompression());

    // uniform names
    conf.get("shared_sampler", _shareTexUniformName);
    conf.get("shared_matrix",  _shareTexMatUniformName);

    // automatically set shared=true if the uniform name is set.
    if (shareTexUniformName().isSet() && !shared().isSet())
        shared() = true;
    
    conf.get("async", async());
    conf.get("morph_imagery", morphImagery());
}

Config
ImageLayer::Options::getConfig() const
{
    Config conf = TileLayer::Options::getConfig();

    conf.set( "nodata_image",   _noDataImageFilename );
    conf.set( "shared",         _shared );
    conf.set( "coverage",       _coverage );
    conf.set( "altitude",       _altitude );
    conf.set( "accept_draping", acceptDraping());
    conf.set( "edge_buffer_ratio", _edgeBufferRatio);
    conf.set( "reprojected_tilesize", _reprojectedTileSize);

    if (_transparentColor.isSet())
        conf.set("transparent_color", colorToString( _transparentColor.value()));

    if ( _colorFilters->size() > 0 )
    {
        Config filtersConf("color_filters");
        if ( ColorFilterRegistry::instance()->writeChain( _colorFilters.get(), filtersConf ) )
        {
            conf.set( filtersConf );
        }
    }

    conf.set("mag_filter","LINEAR",                _magFilter,osg::Texture::LINEAR);
    conf.set("mag_filter","LINEAR_MIPMAP_LINEAR",  _magFilter,osg::Texture::LINEAR_MIPMAP_LINEAR);
    conf.set("mag_filter","LINEAR_MIPMAP_NEAREST", _magFilter,osg::Texture::LINEAR_MIPMAP_NEAREST);
    conf.set("mag_filter","NEAREST",               _magFilter,osg::Texture::NEAREST);
    conf.set("mag_filter","NEAREST_MIPMAP_LINEAR", _magFilter,osg::Texture::NEAREST_MIPMAP_LINEAR);
    conf.set("mag_filter","NEAREST_MIPMAP_NEAREST",_magFilter,osg::Texture::NEAREST_MIPMAP_NEAREST);
    conf.set("min_filter","LINEAR",                _minFilter,osg::Texture::LINEAR);
    conf.set("min_filter","LINEAR_MIPMAP_LINEAR",  _minFilter,osg::Texture::LINEAR_MIPMAP_LINEAR);
    conf.set("min_filter","LINEAR_MIPMAP_NEAREST", _minFilter,osg::Texture::LINEAR_MIPMAP_NEAREST);
    conf.set("min_filter","NEAREST",               _minFilter,osg::Texture::NEAREST);
    conf.set("min_filter","NEAREST_MIPMAP_LINEAR", _minFilter,osg::Texture::NEAREST_MIPMAP_LINEAR);
    conf.set("min_filter","NEAREST_MIPMAP_NEAREST",_minFilter,osg::Texture::NEAREST_MIPMAP_NEAREST);

    conf.set("texture_compression", textureCompression());

    // uniform names
    conf.set("shared_sampler", _shareTexUniformName);
    conf.set("shared_matrix",  _shareTexMatUniformName);

    conf.set("async", async());
    conf.set("morph_imagery", morphImagery());

    return conf;
}

//------------------------------------------------------------------------

void
ImageLayer::setShared(bool value)
{
    setOptionThatRequiresReopen(options().shared(), value);
}

bool
ImageLayer::getShared() const
{
    return options().shared().get();
}

void
ImageLayer::setCoverage(bool value)
{
    setOptionThatRequiresReopen(options().coverage(), value);
}

bool
ImageLayer::getCoverage() const
{
    return options().coverage().get();
}

void
ImageLayer::setSharedTextureUniformName(const std::string& value)
{
    if (options().shareTexUniformName() != value)
        options().shareTexUniformName() = value;
}

const std::string&
ImageLayer::getSharedTextureUniformName() const
{
    return options().shareTexUniformName().get();
}

void
ImageLayer::setSharedTextureMatrixUniformName(const std::string& value)
{
    if (options().shareTexMatUniformName() != value)
        options().shareTexMatUniformName() = value;
}

const std::string&
ImageLayer::getSharedTextureMatrixUniformName() const
{
    return options().shareTexMatUniformName().get();
}

void
ImageLayer::setAsyncLoading(bool value)
{
    options().async() = value;
}

bool
ImageLayer::getAsyncLoading() const
{
    return options().async().get();
}

ImageLayer*
ImageLayer::create(const ConfigOptions& options)
{
    osg::ref_ptr<Layer> layer = Layer::create(options);
    ImageLayer* result = dynamic_cast<ImageLayer*>(layer.get());
    if (result)
    {
        layer.release();
        return result;
    }
    return 0L;
}

Status
ImageLayer::openImplementation()
{
    Status parent = TileLayer::openImplementation();
    if (parent.isError())
        return parent;

    if (!_emptyImage.valid())
        _emptyImage = ImageUtils::createEmptyImage();

    if (!options().shareTexUniformName().isSet())
        options().shareTexUniformName().init( Stringify() << "layer_" << getUID() << "_tex" );

    if (!options().shareTexMatUniformName().isSet() )
        options().shareTexMatUniformName().init(Stringify() << options().shareTexUniformName().get() << "_matrix");

    if (options().noDataImageFilename().isSet())
    {
        _nodataImage = options().noDataImageFilename()->getImage();
        if (!_nodataImage.valid())
        {
            OE_WARN << LC << "Failed to load the 'no-data' image from \""
                << options().noDataImageFilename()->full()
                << "\""
                << std::endl;
        }
    }

    for (auto layer : _postLayers)
    {
        Status s = layer->open(getReadOptions());
        if (s.isError())
            return s;
    }

    return Status::NoError;
}

void
ImageLayer::init()
{
    TileLayer::init();

    _useCreateTexture = false;

    // image layers render as a terrain texture.
    setRenderType(RENDERTYPE_TERRAIN_SURFACE);

    if (options().altitude().isSet())
    {
        setAltitude(options().altitude().get());
    }

    if (options().acceptDraping().isSet())
    {
        setAcceptDraping(options().acceptDraping().get());
    }

    setMorphImagery(options().morphImagery().get());
}

void
ImageLayer::setMorphImagery(bool value)
{
    options().morphImagery() = value;
    auto* stateSet = getOrCreateStateSet();
    stateSet->getOrCreateUniform("oe_layer_morphImagery", osg::Uniform::BOOL)->set(value);
}

bool
ImageLayer::getMorphImagery() const
{
    return options().morphImagery().get();
}

void
ImageLayer::setAltitude(const Distance& value)
{
    options().altitude() = value;

    if (value.getValue() != 0.0)
    {
        osg::StateSet* stateSet = getOrCreateStateSet();

        stateSet->addUniform(
            new osg::Uniform("oe_terrain_altitude", (float)options().altitude()->as(Units::METERS)),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

        stateSet->setMode(GL_CULL_FACE, 0);
    }
    else
    {
        osg::StateSet* stateSet = getOrCreateStateSet();
        getOrCreateStateSet()->removeUniform("oe_terrain_altitude");
        stateSet->removeMode(GL_CULL_FACE);
    }
}

const Distance&
ImageLayer::getAltitude() const
{
    return options().altitude().get();
}

void
ImageLayer::setAcceptDraping(bool value)
{
    options().acceptDraping() = value;

    if (value == true && getStateSet() != nullptr)
        getStateSet()->removeDefine("OE_DISABLE_DRAPING");
    else
        getOrCreateStateSet()->setDefine("OE_DISABLE_DRAPING");
}

bool
ImageLayer::getAcceptDraping() const
{
    return options().acceptDraping().get();
}

void
ImageLayer::setUseCreateTexture()
{
    _useCreateTexture = true;
}

void
ImageLayer::addColorFilter( ColorFilter* filter )
{
    options().colorFilters().mutable_value().push_back( filter );
}

void
ImageLayer::removeColorFilter( ColorFilter* filter )
{
    ColorFilterChain& filters = options().colorFilters().mutable_value();
    ColorFilterChain::iterator i = std::find(filters.begin(), filters.end(), filter);
    if ( i != filters.end() )
    {
        filters.erase( i );
    }
}

const ColorFilterChain&
ImageLayer::getColorFilters() const
{
    return options().colorFilters().get();
}

GeoImage
ImageLayer::createImage(const TileKey& key)
{
    return createImage(key, nullptr);
}

GeoImage
ImageLayer::createImage(const TileKey& key, ProgressCallback* progress)
{
    OE_PROFILING_ZONE;
    OE_PROFILING_ZONE_TEXT(getName() + " " + key.str());

#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
    PERFORMANCEAPI_INSTRUMENT_FUNCTION();
    PERFORMANCEAPI_INSTRUMENT_DATA("key", key.str().c_str());
    PERFORMANCEAPI_INSTRUMENT_DATA("layer", getName().c_str());
#endif

    if (!isOpen())
    {
        return GeoImage::INVALID;
    }

    NetworkMonitor::ScopedRequestLayer layerRequest(getName());

    GeoImage result = createImageInKeyProfile(key, progress);

    // Post-cache operations:

    if (!_postLayers.empty())
    {
        for (auto& post : _postLayers)
        {
            // if we didn't get a result from the actual key, try to fall back
            // so we can apply the post to "something".
            if (!result.valid())
            {
                TileKey bestKey = getBestAvailableTileKey(key);
                result = createImageInKeyProfile(bestKey, progress);
            }

            result = applyPostLayer(result, key, post.get(), progress);
        }
    }

    if (result.valid())
    {
        postCreateImageImplementation(result, key, progress);
    }

    return result;
}

GeoImage
ImageLayer::applyPostLayer(const GeoImage& canvas, const TileKey& key, Layer* post, ProgressCallback* progress) const
{
    auto post_imageLayer = dynamic_cast<ImageLayer*>(post);
    if (post_imageLayer)
    {
        return post_imageLayer->createImage(canvas, key, progress);
    }
    else return canvas;
}

GeoImage
ImageLayer::createImage(const GeoImage& canvas, const TileKey& key, ProgressCallback* progress)
{
    Threading::ScopedReadLock lock(inUseMutex());
    return createImageImplementation(canvas, key, progress);
}

GeoImage
ImageLayer::createImageInKeyProfile(const TileKey& key, ProgressCallback* progress)
{
    OE_SOFT_ASSERT_AND_RETURN(getCacheSettings(), {});

    // If the layer is disabled, bail out.
    if ( !isOpen() )
    {
        return GeoImage::INVALID;
    }

    // Make sure the request is in range.
    // TODO: perhaps this should be a call to mayHaveData(key) instead.
    if ( !isKeyInLegalRange(key) )
    {
        return GeoImage::INVALID;
    }

    // Tile gate prevents two threads from requesting the same key
    // at the same time, which would be unnecessary work. Only lock
    // the gate if there is an L2 cache active
    //
    // Note: be careful. We had to remove the gate from ElevationLayer
    // because it was causing deadlocks.
    ScopedGate<TileKey> scopedGate(_sentry, key, _memCache.valid());

    GeoImage result;

    // the cache key combines the Key and the horizontal profile.
    std::string cacheKey = Cache::makeCacheKey(
        Stringify() << key.str() << "-" << std::hex << key.getProfile()->getHorizSignature(),
        "image");

    // The L2 cache key includes the layer revision of course!
    std::string memCacheKey;

    const CachePolicy& policy = getCacheSettings()->cachePolicy().get();

    // Check the layer L2 cache first
    if ( _memCache.valid() )
    {
        memCacheKey = Stringify() << std::to_string(getRevision()) << "/" << key.str() << "/" << key.getProfile()->getHorizSignature();
        CacheBin* bin = _memCache->getOrCreateDefaultBin();
        ReadResult result = bin->readObject(memCacheKey, nullptr);
        if (result.succeeded())
        {
            return GeoImage(static_cast<osg::Image*>(result.releaseObject()), key.getExtent());
        }
    }

    // locate the cache bin for the target profile for this layer:
    CacheBin* cacheBin = getCacheBin( key.getProfile() );

    // validate the existance of a valid layer profile (unless we're in cache-only mode, in which
    // case there is no layer profile)
    if ( !policy.isCacheOnly() && !getProfile() )
    {
        disable("Could not establish a valid profile");
        return GeoImage::INVALID;
    }

    osg::ref_ptr< osg::Image > cachedImage;

    // First, attempt to read from the cache. Since the cached data is stored in the
    // map profile, we can try this first.
    if ( cacheBin && policy.isCacheReadable() )
    {
        ReadResult r = cacheBin->readImage(cacheKey, 0L);
        if ( r.succeeded() )
        {
            cachedImage = r.releaseImage();
            bool expired = policy.isExpired(r.lastModifiedTime());
            if (!expired)
            {
                return GeoImage(cachedImage.get(), key.getExtent());
            }
        }
    }

    // The data was not in the cache. If we are cache-only, fail sliently
    if ( policy.isCacheOnly() )
    {
        // If it's cache only and we have an expired but cached image, just return it.
        if (cachedImage.valid())
        {
            return GeoImage( cachedImage.get(), key.getExtent() );
        }
        else
        {
            return GeoImage::INVALID;
        }
    }

    if (key.getProfile()->isHorizEquivalentTo(getProfile()))
    {
        bool createUpsampledImage = false;

        if (getUpsample() == true && getMaxDataLevel() > key.getLOD())
        {
            TileKey best = getBestAvailableTileKey(key, false);
            if (best.valid())
            {
                TileKey best_upsampled = getBestAvailableTileKey(key, true);
                if (best_upsampled.valid() && best.getLOD() < best_upsampled.getLOD())
                {
                    createUpsampledImage = true;
                }
            }
        }

        if (createUpsampledImage == true)
        {
            result = createFractalUpsampledImage(key, progress);
        }
        else
        {
            Threading::ScopedReadLock lock(inUseMutex());
            result = createImageImplementation(key, progress);
        }
    }
    else
    {
        // If the profiles are different, use a compositing method to assemble the tile.
        result = assembleImage( key, progress );
    }

    // Check for cancelation before writing to a cache:
    if (progress && progress->isCanceled())
    {
        return GeoImage::INVALID;
    }

    // Check against a no-data image.
    if (result.valid() && _nodataImage.valid())
    {
        if (ImageUtils::areEquivalent(result.getImage(), _nodataImage.get()))
        {
            return GeoImage::INVALID;
        }
    }

    if (result.valid())
    {        
        // invoke user callbacks
        onCreate.fire(key, result);

#if HOOKS
        if (hooks)
        {
            auto status = hooks->posthook_image(this, key, result, progress, result);
            if (status.isError())
                return GeoImage(status);
        }
#endif

#if 1
        if (_memCache.valid())
        {
            CacheBin* bin = _memCache->getOrCreateDefaultBin();
            bin->write(memCacheKey, result.getImage(), 0L);
        }

        // If we got a result, the cache is valid and we are caching in the map profile,
        // write to the map cache.
        if (cacheBin        &&
            policy.isCacheWriteable())
        {
            if ( key.getExtent() != result.getExtent() )
            {
                OE_INFO << LC << "WARNING! mismatched extents." << std::endl;
            }

            cacheBin->write(cacheKey, result.getImage(), 0L);
        }
    }

    else // result.valid() == false
    {
        // We couldn't get an image from the source.  So see if we have an expired cached image
        if (cachedImage.valid())
        {
            //OE_DEBUG << LC << "Using cached but expired image for " << key.str() << std::endl;
            result = GeoImage( cachedImage.get(), key.getExtent());
        }
#endif
    }

    return result;
}

GeoImage
ImageLayer::assembleImage(const TileKey& key, ProgressCallback* progress)
{
    // If we got here, asset that there's a non-null layer profile.
    if (!getProfile())
    {
        setStatus(Status::Error(Status::AssertionFailure, "assembleImage with undefined profile"));
        return GeoImage::INVALID;
    }

    GeoImage output;
    unsigned targetLOD = getProfile()->getEquivalentLOD(key.getProfile(), key.getLOD());

    std::vector<TileKey> intersectingKeys;
    getProfile()->getIntersectingTiles(key, intersectingKeys);

    using KeyedImage = std::pair<TileKey, GeoImage>;
    std::vector<KeyedImage> sources;

    if (intersectingKeys.size() > 0)
    {
        bool hasAtLeastOneSourceAtTargetLOD = false;

        for (auto& intersectingKey : intersectingKeys)
        {
            TileKey subKey = intersectingKey;
            GeoImage subTile;
            while (subKey.valid() && !subTile.valid())
            {
                subTile = createImageInKeyProfile(subKey, progress);
                if (!subTile.valid())
                    subKey.makeParent();

                if (progress && progress->isCanceled())
                    return {};
            }

            if (subTile.valid())
            {
                if (subKey.getLOD() == targetLOD)
                {
                    hasAtLeastOneSourceAtTargetLOD = true;
                }

                // got a valid image, so add it to our sources collection:
                sources.emplace_back(subKey, subTile);
            }
        }

        // If we actually got at least one piece of usable data,
        // move ahead and build a mosaic of all sources.
        if (hasAtLeastOneSourceAtTargetLOD)
        {
            auto* proto = sources.front().second.getImage();

            unsigned cols = getTileSize();
            unsigned rows = getTileSize();
            unsigned layers = proto->r();

            // sort the sources by LOD (highest first).
            std::sort(
                sources.begin(), sources.end(),
                [](const KeyedImage& lhs, const KeyedImage& rhs) {
                    return lhs.first.getLOD() > rhs.first.getLOD();
                });

            // assume all tiles to mosaic are in the same SRS.
            auto* key_srs = key.getExtent().getSRS();
            auto* source_srs = sources[0].second.getSRS();

            // new output:
            auto mosaic = new osg::Image();
            mosaic->allocateImage(cols, rows, layers, proto->getPixelFormat(), proto->getDataType());
            mosaic->setInternalTextureFormat(proto->getInternalTextureFormat());

            // Working set of points. it's much faster to xform an entire vector all at once.
            std::vector<osg::Vec3d> points;
            points.resize(cols * rows);

            double minx, miny, maxx, maxy;
            key.getExtent().getBounds(minx, miny, maxx, maxy);
            double dx = (maxx - minx) / (double)(cols);
            double dy = (maxy - miny) / (double)(rows);

            Bounds sourceBounds;
            sources[0].second.getSRS()->getBounds(sourceBounds);

            // build a grid of sample points:
            for (unsigned t = 0; t < rows; ++t)
            {
                double y = miny + (0.5 * dy) + (dy * (double)t);
                for (unsigned s = 0; s < cols; ++s)
                {
                    double x = minx + (0.5 * dx) + (dx * (double)s);
                    points[t * cols + s] = { x, y, 0.0 };
                }
            }

            // transform the sample points to the SRS of our source data tiles:
            if (source_srs && key_srs)
            {
                key_srs->transform(points, source_srs);

                if (sourceBounds.valid())
                {
                    for (auto& point : points)
                    {
                        point.x() = clamp(point.x(), sourceBounds.xMin(), sourceBounds.xMax());
                        point.y() = clamp(point.y(), sourceBounds.yMin(), sourceBounds.yMax());
                    }
                }
            }

            // Mosaic our sources into a single output image.
            std::vector<GeoImagePixelReader> readers;
            for (unsigned i = 0; i < sources.size(); ++i)
            {
                readers.emplace_back(sources[i].second);
                readers.back().setBilinear(!isCoverage());
            }

            ImageUtils::PixelWriter write_mosaic(mosaic);
            osg::Vec4f pixel;

            write_mosaic.forEachPixel([&](auto& iter)
                {
                    unsigned i = iter.t() * cols + iter.s();
                    pixel.set(0, 0, 0, 0);

                    // check each source (high to low LOD) until we get a valid pixel.
                    for (unsigned k = 0; k < sources.size() && pixel.a() == 0.0f; ++k)
                    {
                        readers[k].readCoordWithoutClamping(pixel, points[i].x(), points[i].y(), iter.r());
                    }

                    write_mosaic(pixel, iter);
                }
            );

            return GeoImage(mosaic, key.getExtent());
        }            
    }

    return output;
}

Status
ImageLayer::writeImage(const TileKey& key, const osg::Image* image, ProgressCallback* progress)
{
    if (getStatus().isError())
        return getStatus();

    Threading::ScopedReadLock lock(inUseMutex());
    return writeImageImplementation(key, image, progress);
}

Status
ImageLayer::writeImageImplementation(const TileKey& key, const osg::Image* image, ProgressCallback* progress) const
{
    return Status(Status::ServiceUnavailable);
}

Result<osg::ref_ptr<osg::Image>>
ImageLayer::encodeImage(const TileKey& key, const osg::Image* image, ProgressCallback* progress)
{
    if (getStatus().isError())
        return getStatus();

    Threading::ScopedReadLock lock(inUseMutex());
    return encodeImageImplementation(key, image, progress);
}

Result<osg::ref_ptr<osg::Image>>
ImageLayer::encodeImageImplementation(const TileKey& key, const osg::Image* image, ProgressCallback* progress) const
{
    return Status(Status::ServiceUnavailable);
}

const std::string
ImageLayer::getCompressionMethod() const
{
    if (isCoverage())
        return "none";

    return options().textureCompression().get();
}

void
ImageLayer::modifyTileBoundingBox(const TileKey& key, osg::BoundingBox& box) const
{
    if (options().altitude().isSet())
    {
        if (options().altitude()->as(Units::METERS) > box.zMax())
        {
            box.zMax() = options().altitude()->as(Units::METERS);
        }
    }
    TileLayer::modifyTileBoundingBox(key, box);
}

void
ImageLayer::addPostLayer(Layer* layer)
{
    std::lock_guard<std::mutex> lock(_postLayers.mutex());
    _postLayers.push_back(layer);
}

//...................................................................

#define ARENA_ASYNC_LAYER "oe.rex.loadtile"
//#define FUTURE_IMAGE_COLOR_PLACEHOLDER

FutureTexture2D::FutureTexture2D(ImageLayer* layer, const TileKey& key) :
    osg::Texture2D(),
    FutureTexture(),
    _layer(layer),
    _key(key)
{
    // since we'll be updating it mid stream
    setDataVariance(osg::Object::DYNAMIC);

    setName(_key.str() + ":" + _layer->getName());

    // start loading the image
    dispatch();
}

void
FutureTexture2D::dispatch() const
{
    osg::observer_ptr<ImageLayer> layer_ptr(_layer);
    TileKey key(_key);

    auto task = [layer_ptr, key](Cancelable& progress) mutable
        {
            GeoImage result;
            osg::ref_ptr<ImageLayer> safe(layer_ptr);
            if (safe.valid())
            {
                osg::ref_ptr<ProgressCallback> p = new ProgressCallback(&progress);
                result = safe->createImage(key, p.get());
            }
            return result;
        };

    jobs::context context{
        Stringify() << key.str() << " " << _layer->getName(),
        jobs::get_pool(ARENA_ASYNC_LAYER), // pool
        [key]() { return key.getLOD(); }
    };

    _result = jobs::dispatch(task, context);
}

void
FutureTexture2D::update()
{
    if (_resolved)
    {
        return;
    }

    else if (_result.canceled())
    {
        dispatch();
        return;
    }

    else if (_result.available())
    {
        // fetch the result
        GeoImage geoImage = _result.value();

        if (geoImage.getStatus().isError())
        {
            _failed = true;
        }
        else
        {
            osg::ref_ptr<osg::Image> image = geoImage.takeImage();

            if (image.valid())
            {
                this->setImage(image);
                this->dirtyTextureObject();
            }

            else
            {
                _failed = true;
                this->dirtyTextureObject();
            }
        }

        // reset the future so update won't be called again
        _result.abandon();

        _resolved = true;
    }
}

GeoImage
ImageLayer::createFractalUpsampledImage(
    const TileKey& key,
    ProgressCallback* progress)
{
    OE_PROFILING_ZONE;

    // Input metatile grid. Always use the immediate parent for
    // fractal enhancement.
    MetaTile<GeoImage> input;
    input.setCreateTileFunction(
        [&](const TileKey& key, ProgressCallback* p) -> GeoImage
        {
            if (this->isKeyInLegalRange(key))
                return this->createImage(key, p);
            else
                return GeoImage::INVALID;
        }
    );
    TileKey parentKey = key.createParentKey();
    osg::Matrix scale_bias;
    key.getExtent().createScaleBias(parentKey.getExtent(), scale_bias);
    input.setCenterTileKey(parentKey, scale_bias);

    // validate that we have a good metatile.
    if (input.valid() == false)
        return GeoImage::INVALID;

    // set up a workspace for creating the new image.
    int ws_width = getTileSize() + 3;
    int ws_height = getTileSize() + 3;

    osg::ref_ptr<osg::Image> workspace = new osg::Image();
    workspace->allocateImage(
        ws_width, ws_height, 1,
        input.getCenterTile().getImage()->getPixelFormat(),
        input.getCenterTile().getImage()->getDataType(),
        input.getCenterTile().getImage()->getPacking());

    ImageUtils::PixelWriter writeToWorkspace(workspace.get());
    ImageUtils::PixelReader readFromWorkspace(workspace.get());

    // output image:
    osg::ref_ptr<osg::Image> output = new osg::Image();
    output->allocateImage(
        getTileSize(), getTileSize(), 1,
        input.getCenterTile().getImage()->getPixelFormat(),
        input.getCenterTile().getImage()->getDataType(),
        input.getCenterTile().getImage()->getPacking());

    // Random number generator for fractal algorithm:
    Util::Random prng(key.hash());

    // temporaries:
    GeoImage::pixel_type pixel, p0, p1, p2, p3;
    float k0, k1, k2, k3;
    unsigned r;
    int s, t;

    // First pass: loop over the grid and populate even-numbered
    // pixels with values from the ancestors.
    for (t = 0; t < ws_height; t += 2)
    {
        for (s = 0; s < ws_width; s += 2)
        {
            input.read(pixel, s - 2, t - 2);
            writeToWorkspace(pixel, s, t);

            if (progress && progress->isCanceled())
                return GeoImage::INVALID;
        }

        if (progress && progress->isCanceled())
            return GeoImage::INVALID;
    }

    // Second pass: diamond
    for (t = 1; t < workspace->t() - 1; t += 2)
    {
        for (s = 1; s < workspace->s() - 1; s += 2)
        {
            r = prng.next(4u);

            // Diamond: pick one of the four diagonals to copy into the
            // center pixel, attempting to preserve curves. When there is
            // no clear choice, go random.
            readFromWorkspace(p0, s - 1, t - 1); k0 = p0.r();
            readFromWorkspace(p1, s + 1, t - 1); k1 = p1.r();
            readFromWorkspace(p2, s + 1, t + 1); k2 = p2.r();
            readFromWorkspace(p3, s - 1, t + 1); k3 = p3.r();

            // three the same
            if (k0 == k1 && k1 == k2 && k2 != k3) pixel = p0;
            else if (k1 == k2 && k2 == k3 && k3 != k0) pixel = p1;
            else if (k2 == k3 && k3 == k0 && k0 != k1) pixel = p2;
            else if (k3 == k0 && k0 == k1 && k1 != k2) pixel = p3;

            // continuations
            else if (k0 == k2 && k0 != k1 && k0 != k3) pixel = p0;
            else if (k1 == k3 && k1 != k2 && k1 != k0) pixel = p1;

            // all else, random.
            else pixel = (r == 0) ? p0 : (r == 1) ? p1 : (r == 2) ? p2 : p3;

            writeToWorkspace(pixel, s, t);
        }
    }

    // Third pass: square
    for (t = 2; t < workspace->t() - 1; ++t)
    {
        for (s = 2; s < workspace->s() - 1; ++s)
        {
            if (((s & 1) == 1 && (t & 1) == 0) || ((s & 1) == 0 && (t & 1) == 1))
            {
                r = prng.next(4u);

                // Square: pick one of the four adjacents to copy into the
                // center pixel, attempting to preserve curves. When there is
                // no clear choice, go random.
                readFromWorkspace(p0, s - 1, t); k0 = p0.r();
                readFromWorkspace(p1, s, t - 1); k1 = p1.r();
                readFromWorkspace(p2, s + 1, t); k2 = p2.r();
                readFromWorkspace(p3, s, t + 1); k3 = p3.r();

                // three the same
                if (k0 == k1 && k1 == k2 && k2 != k3) pixel = p0;
                else if (k1 == k2 && k2 == k3 && k3 != k0) pixel = p1;
                else if (k2 == k3 && k3 == k0 && k0 != k1) pixel = p2;
                else if (k3 == k0 && k0 == k1 && k1 != k2) pixel = p3;

                // continuations
                else if (k0 == k2 && k0 != k1 && k0 != k3) pixel = p0;
                else if (k1 == k3 && k1 != k2 && k1 != k0) pixel = p1;

                // all else, random.
                else pixel = (r == 0) ? p0 : (r == 1) ? p1 : (r == 2) ? p2 : p3;

                writeToWorkspace(pixel, s, t);
            }
        }
    }

    // finally blit from workspace interior to the output image.
    ImageUtils::PixelWriter writeToOutput(output.get());
    for (t = 0; t < output->t(); ++t)
    {
        for (s = 0; s < output->s(); ++s)
        {
            readFromWorkspace(pixel, s + 2, t + 2);
            writeToOutput(pixel, s, t);
        }
    }

    if (progress && progress->isCanceled())
    {
        return GeoImage::INVALID;
    }

    return GeoImage(output.get(), key.getExtent());
}
