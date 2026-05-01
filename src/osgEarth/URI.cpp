/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/URI>
#include <osgEarth/HTTPClient>
#include <osgEarth/Cache>
#include <osgEarth/Registry>
#include <osgEarth/FileUtils>
#include <osgEarth/Progress>
#include <osgEarth/NetworkMonitor>
#include <osgDB/FileNameUtils>
#include <osgDB/ReadFile>
#include <osgDB/Archive>
#include <utility>

#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
#include <Superluminal/PerformanceAPI.h>
#endif

#define LC "[URI] "

#define OE_TEST OE_NULL
//#define OE_TEST OE_NOTICE

// Only uncomment this is you want to resize textures as they are loaded.
// Don't uncomment this, ever. So many bad things will happen. Some images
// like elevation, land cover, etc. are not meant to be resized.
//#define SUPPORT_MAX_TEXTURE_SIZE

using namespace osgEarth;
using namespace osgEarth::Threading;

namespace
{
    static Gate<std::string> uri_gate;    
}

//------------------------------------------------------------------------

URIStream::URIStream(const URI& uri, std::ios_base::openmode mode) :
_instream( 0L )
{
    if ( osgDB::containsServerAddress(uri.full()) )
    {
        HTTPResponse res = HTTPClient::get( uri.full() );
        if ( res.isOK() )
        {
            std::string buf = res.getPartAsString(0);
            _instream = new std::istringstream(buf);
        }
    }
    else
    {
        _instream = new std::ifstream(uri.full().c_str(), mode);
    }
}

URIStream::~URIStream()
{
    if (_instream)
        delete _instream;
}

URIStream::operator std::istream& ()
{
    static std::istringstream s_nullStream;

    if ( _instream )
        return *_instream;
    else
        return s_nullStream;
}

//------------------------------------------------------------------------

URIContext::URIContext()
{
}

URIContext::URIContext(const std::string& referrer) :
    _referrer(referrer)
{
}

URIContext::URIContext(std::string&& referrer) :
    _referrer(std::move(referrer))
{
}

URIContext::URIContext(const URIContext& rhs) :
    _referrer(rhs._referrer),
    _headers(rhs._headers)
{
}

std::string
URIContext::getOSGPath( const std::string& target ) const
{
    return osgEarth::getFullPath( _referrer, target );
}

void
URIContext::addHeader(const std::string& name, const std::string& value)
{
    _headers[name] = value;
}

void
URIContext::addHeader(std::string&& name, std::string&& value)
{
    _headers[std::move(name)] = std::move(value);
}

const Headers&
URIContext::getHeaders() const
{
    return _headers;
}

Headers&
URIContext::getHeaders()
{
    return _headers;
}


URIContext
URIContext::add( const URIContext& sub ) const
{
    return URIContext( osgEarth::getFullPath(_referrer, sub._referrer) );
}

URIContext
URIContext::add( const std::string& sub ) const
{
    return URIContext(osgDB::concatPaths( _referrer, sub ));
}

void
URIContext::store( osgDB::Options* options )
{
    if ( options )
    {
        if (_referrer.empty() == false)
        {
            options->setDatabasePath( _referrer );
            options->setPluginStringData( "osgEarth::URIContext::referrer", _referrer );
        }
    }
}

URIContext::URIContext( const osgDB::Options* options )
{
    if ( options )
    {
        _referrer = options->getPluginStringData( "osgEarth::URIContext::referrer" );

        if ( _referrer.empty() && options->getDatabasePathList().size() > 0 )
        {
            const std::string& front = options->getDatabasePathList().front();
            if ( osgEarth::isArchive(front) )
            {
                _referrer = front + "/";
            }
        }
    }
}

//------------------------------------------------------------------------

URI::URI()
{
    //nop
}

URI::URI(const URI& rhs) :
_baseURI(rhs._baseURI),
_fullURI(rhs._fullURI),
_context(rhs._context),
_cacheKey(rhs._cacheKey)
{
    //nop
}

URI::URI( const std::string& location )
{
    _baseURI = location;
    _fullURI = osgEarth::Util::stripRelativePaths(location);
    ctorCacheKey();
}

URI::URI( std::string&& location )
{
    _baseURI = std::move(location);
    _fullURI = osgEarth::Util::stripRelativePaths(_baseURI);
    ctorCacheKey();
}

URI::URI( const std::string& location, const URIContext& context )
{
    _context = context;
    _baseURI = location;
    _fullURI = context.getOSGPath( _baseURI );
    ctorCacheKey();
}

URI::URI( std::string&& location, const URIContext& context )
{
    _context = context;
    _baseURI = std::move(location);
    _fullURI = context.getOSGPath( _baseURI );
    ctorCacheKey();
}

URI::URI( const char* location )
{
    _baseURI = std::string(location);
    _fullURI = osgEarth::Util::stripRelativePaths(_baseURI);
    ctorCacheKey();
}

URI
URI::append( const std::string& suffix ) const
{
    URI result;
    result._baseURI = _baseURI + suffix;
    result._fullURI = _fullURI + suffix;
    result._context = _context;
    result.ctorCacheKey();
    return result;
}

void
URI::ctorCacheKey()
{
    _cacheKey = Cache::makeCacheKey(_fullURI, "uri");
}

bool
URI::isRemote() const
{
    return osgDB::containsServerAddress( _fullURI );
}

Config
URI::getConfig() const
{
    Config conf("uri", base());
    conf.set("option_string", _optionString);
    conf.setReferrer(context().referrer());
    conf.setIsLocation(true);

    const Headers& headers = context().getHeaders();
    if (!headers.empty())
    {
        Config headersconf("headers");
        for(Headers::const_iterator i = headers.begin(); i != headers.end(); ++i)
        {
            if (!i->first.empty() && !i->second.empty())
            {
                headersconf.add(Config(i->first, i->second));
            }
        }
        conf.add(std::move(headersconf));
    }

    return conf;
}

void
URI::mergeConfig(const Config& conf)
{
    conf.get("option_string", _optionString);

    const ConfigSet headers = conf.child("headers").children();
    for (ConfigSet::const_iterator i = headers.begin(); i != headers.end(); ++i)
    {
        const Config& header = *i;
        if (!header.key().empty() && !header.value().empty())
        {
            _context.addHeader(header.key(), header.value());
        }
    }
}

namespace
{
    // convert an osgDB::ReaderWriter::ReadResult to an osgEarth::ReadResult
    ReadResult toReadResult( osgDB::ReaderWriter::ReadResult& rr )
    {
        if ( rr.validObject() )
            return ReadResult( rr.getObject() );
        else
            return ReadResult( ReadResult::RESULT_NOT_FOUND );
    }

    // Utility to redirect a local file read if it has an archive name in the URI
    ReadResult readStringFile( const std::string& uri, const osgDB::Options* opt )
    {
        osgDB::Registry::ArchiveExtensionList e = osgDB::Registry::instance()->getArchiveExtensions();
        for(osgDB::Registry::ArchiveExtensionList::iterator aitr = e.begin(); aitr != e.end(); ++aitr )
        {
            std::string archiveExtension = "." + (*aitr);
            std::string::size_type positionArchive = uri.find(archiveExtension+'/');
            if (positionArchive == std::string::npos) positionArchive = uri.find(archiveExtension+'\\');
            if (positionArchive != std::string::npos)
            {
                std::string::size_type endArchive = positionArchive + archiveExtension.length();
                std::string archiveName( uri.substr(0, endArchive));
                std::string fileName( uri.substr(endArchive+1, std::string::npos) );

                osgDB::ReaderWriter::ReadResult result = osgDB::Registry::instance()->openArchiveImplementation(
                    archiveName, osgDB::ReaderWriter::READ, 4096, opt );

                if (!result.validArchive())
                    return ReadResult(); // error

                osgDB::Archive* archive = result.getArchive();

                osg::ref_ptr<osgDB::ReaderWriter::Options> options = opt ? opt->cloneOptions() :
                    Registry::instance()->cloneOrCreateOptions();

                options->setDatabasePath(archiveName);

                osgDB::ReaderWriter::ReadResult rr = archive->readObject(fileName, options.get());
                if ( rr.success() )
                {
                    ReadResult result( rr.takeObject() );
                    result.setLastModifiedTime( osgEarth::getLastModifiedTime(uri) );
                    return result;
                }
                else
                {
                    return ReadResult();
                }
            }
        }

        // no archive; just read it normally
        std::ifstream input( uri.c_str(), std::ios::binary );
        if ( input.is_open() )
        {
            input >> std::noskipws;
            std::stringstream buf;
            buf << input.rdbuf();
            std::string bufStr;
            bufStr = buf.str();
            ReadResult result( new StringObject(bufStr) );
            result.setLastModifiedTime( osgEarth::getLastModifiedTime(uri) );
            return result;
        }

        // no good
        return ReadResult();
    }


    //--------------------------------------------------------------------
    // Read functors (used by the doRead method)

#ifdef SUPPORT_MAX_TEXTURE_SIZE
    osg::Image* resize(osg::Image* image, int maxdim)
    {
        unsigned new_s, new_t;
        float ar = (float)image->s() / (float)image->t();

        if (image->s() >= image->t()) {
            new_s = maxdim;
            new_t = (int)((float)new_s * ar);
        }
        else {
            new_t = maxdim;
            new_s = (int)((float)new_t / ar);
        }

        osg::ref_ptr<osg::Image> new_image;
        if (ImageUtils::resizeImage(image, new_s, new_t, new_image))
            return new_image.release();
        else
            return image;
    }

    struct ShrinkTexturesVisitor : public TextureAndImageVisitor {
        const osgDB::Options* options;
        void apply(osg::Texture& texture) {
            for (int i = 0; i < texture.getNumImages(); ++i) {
                osg::Image* image = texture.getImage(i);
                if (image) {
                    optional<int> maxdim = ImageUtils::getMaxTextureSize(image, options);
                    if (maxdim.isSet()) {
                        image = resize(image, maxdim.value());
                        if (image)
                            texture.setImage(i, image);
                    }
                }
            }
        }
    };
#endif

    struct ReadObject
    {
        bool callbackRequestsCaching( URIReadCallback* cb ) const { return !cb || ((cb->cachingSupport() & URIReadCallback::CACHE_OBJECTS) != 0); }
        ReadResult fromCallback( URIReadCallback* cb, const std::string& uri, const osgDB::Options* opt ) { return cb->readObject(uri, opt); }
        ReadResult fromHTTP( const URI& uri, const osgDB::Options* opt, ProgressCallback* p, TimeStamp lastModified )
        {
            HTTPRequest req(uri.full());
            req.getHeaders() = uri.context().getHeaders();
            if (lastModified > 0)
            {
                req.setLastModified(lastModified);
            }
            return HTTPClient::readObject(req, opt, p);
        }
        ReadResult fromFile( const std::string& uri, const osgDB::Options* opt ) {
            osgDB::ReaderWriter::ReadResult osgRR = osgDB::Registry::instance()->readObject(uri, opt);
            if (osgRR.validObject()) return ReadResult(osgRR.takeObject());
            else return ReadResult(osgRR.message());
        }
    };

    struct ReadNode
    {
        bool callbackRequestsCaching( URIReadCallback* cb ) const { return !cb || ((cb->cachingSupport() & URIReadCallback::CACHE_NODES) != 0); }
        ReadResult fromCallback( URIReadCallback* cb, const std::string& uri, const osgDB::Options* opt ) { return cb->readNode(uri, opt); }
        ReadResult fromHTTP(const URI& uri, const osgDB::Options* opt, ProgressCallback* p, TimeStamp lastModified )
        {
            HTTPRequest req(uri.full());
            req.getHeaders() = uri.context().getHeaders();
            if (lastModified > 0)
            {
                req.setLastModified(lastModified);
            }
            return HTTPClient::readNode(req, opt, p);
        }
        ReadResult fromFile( const std::string& uri, const osgDB::Options* opt ) {
            osgDB::ReaderWriter::ReadResult osgRR = osgDB::Registry::instance()->readNode(uri, opt);
            if (osgRR.validNode()) return ReadResult(osgRR.takeNode());
            else return ReadResult(osgRR.message());
        }
        ReadResult postProcess(ReadResult& r, const osgDB::Options* opt) {
#ifdef SUPPORT_MAX_TEXTURE_SIZE
            if (r.getNode() == nullptr)
                return r;
            if (ImageUtils::getMaxTextureSize(nullptr, opt).isSet()) {
                ShrinkTexturesVisitor v;
                v.options = opt;
                r.getNode()->accept(v);
            }
#endif
            return r;
        }
    };

    struct ReadImage
    {
        bool callbackRequestsCaching( URIReadCallback* cb ) const {
            return !cb || ((cb->cachingSupport() & URIReadCallback::CACHE_IMAGES) != 0);
        }
        ReadResult fromCallback( URIReadCallback* cb, const std::string& uri, const osgDB::Options* opt ) {
            ReadResult r = cb->readImage(uri, opt);
            if ( r.getImage() ) r.getImage()->setFileName(uri);
            return postProcess(r, opt);
        }
        ReadResult fromHTTP(const URI& uri, const osgDB::Options* opt, ProgressCallback* p, TimeStamp lastModified ) {
            HTTPRequest req(uri.full());
            req.getHeaders() = uri.context().getHeaders();

            if (lastModified > 0)
            {
                req.setLastModified(lastModified);
            }
            ReadResult r = HTTPClient::readImage(req, opt, p);
            if ( r.getImage() ) r.getImage()->setFileName( uri.full() );
            return postProcess(r, opt);
        }
        ReadResult fromFile( const std::string& uri, const osgDB::Options* opt ) {
            // Call readImageImplementation instead of readImage to bypass any readfile callbacks installed in the registry.
            osgDB::ReaderWriter::ReadResult osgRR = osgDB::Registry::instance()->readImageImplementation(uri, opt);
            if (osgRR.validImage()) {
                osgRR.getImage()->setFileName(uri);
                ReadResult rr(osgRR.takeImage());
                return postProcess(rr, opt);
            }
            else return ReadResult(osgRR.message());
        }
        ReadResult postProcess(ReadResult& r, const osgDB::Options* opt) const
        {
#ifdef SUPPORT_MAX_TEXTURE_SIZE
            if (r.getImage() == nullptr)
                return r;

            auto maxdim = ImageUtils::getMaxTextureSize(r.getImage(), opt);
            if (!maxdim.isSet())
                return r;

            if (r.getImage()->getPixelFormat() != GL_RGB && r.getImage()->getPixelFormat() != GL_RGBA)
                return r;

            auto new_image = resize(r.getImage(), maxdim.value());
            if (new_image)
                return ReadResult(new_image, r.metadata());
            else
                return r;
#else
            return r;
#endif
        }
    };

    struct ReadString
    {
        bool callbackRequestsCaching( URIReadCallback* cb ) const {
            return !cb || ((cb->cachingSupport() & URIReadCallback::CACHE_STRINGS) != 0);
        }
        ReadResult fromCallback( URIReadCallback* cb, const std::string& uri, const osgDB::Options* opt ) {
            return cb->readString(uri, opt);
        }
        ReadResult fromHTTP(const URI& uri, const osgDB::Options* opt, ProgressCallback* p, TimeStamp lastModified )
        {
            HTTPRequest req(uri.full());
            req.getHeaders() = uri.context().getHeaders();
            if (lastModified > 0)
            {
                req.setLastModified(lastModified);
            }
            return HTTPClient::readString(req, opt, p);
        }
        ReadResult fromFile( const std::string& uri, const osgDB::Options* opt ) {
            return readStringFile(uri, opt);
        }
    };

    //--------------------------------------------------------------------
    // MASTER read template function. I templatized this so we wouldn't
    // have 4 95%-identical code paths to maintain...

    template<typename READ_FUNCTOR>
    ReadResult doRead(
        const URI&            inputURI,
        const osgDB::Options* dbOptions,
        ProgressCallback*     progress)
    {
#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
        PERFORMANCEAPI_INSTRUMENT_FUNCTION();
        PERFORMANCEAPI_INSTRUMENT_DATA("url", inputURI.full().c_str());
#endif
        ScopedGate<std::string> gatelock(uri_gate, inputURI.full());

        //osg::Timer_t startTime = osg::Timer::instance()->tick();

        unsigned long handle = NetworkMonitor::begin(inputURI.full(), "Pending", inputURI.isRemote() ? "Network" : "File");
        ReadResult result;

        if (osgEarth::Registry::instance()->isBlacklisted(inputURI.full()))
        {
            NetworkMonitor::end(handle, "Blacklisted");
            return result;
        }

        if ( !inputURI.empty() )
        {
            // establish our IO options:
            osg::ref_ptr<osgDB::Options> localOptions = dbOptions ? Registry::cloneOrCreateOptions(dbOptions) : Registry::cloneOrCreateOptions(Registry::instance()->getDefaultOptions());

            // if we have an option string, incorporate it.
            if ( inputURI.optionString().isSet() )
            {
                localOptions->setOptionString(
                    inputURI.optionString().get() + " " + localOptions->getOptionString());
            }

            // Store a new URI context within the local options so that subloaders know the location they are loading from
            // This is necessary for things like gltf files that can store external binary files that are relative to the gltf file.
            URIContext(inputURI.full()).store(localOptions.get());

            READ_FUNCTOR reader;

            URI uri = inputURI;

            bool gotResultFromCallback = false;

            // check if there's an alias map, and if so, attempt to resolve the alias:
            URIAliasMap* aliasMap = URIAliasMap::from( localOptions.get() );
            if ( aliasMap )
            {
                uri = aliasMap->resolve(inputURI.full(), inputURI.context());
            }

            // check if there's a URI cache in the options.
            URIResultCache* memCache = URIResultCache::from( localOptions.get() );
            if ( memCache )
            {
                auto cached = memCache->get(uri);
                if (cached.has_value())
                {
                    result = cached.value();
                }
            }

            if ( result.empty() )
            {
                // see if there's a read callback installed.
                URIReadCallback* cb = Registry::instance()->getURIReadCallback();

                // for a local URI, bypass all the caching logic
                if ( !uri.isRemote() )
                {
                    // try to use the callback if it's set. Callback ignores the caching policy.
                    if ( cb )
                    {
                        // if this returns "not implemented" we fill fall back
                        result = reader.fromCallback( cb, uri.full(), localOptions.get() );

                        if ( result.code() != ReadResult::RESULT_NOT_IMPLEMENTED )
                        {
                            // "not implemented" is the only excuse to fall back.
                            gotResultFromCallback = true;
                        }
                    }

                    if ( !gotResultFromCallback )
                    {
                        // no callback, just read from a local file.
                        result = reader.fromFile( uri.full(), localOptions.get() );
                    }
                }

                // remote URI
                else
                {
                    // If it's not cached, or it is cached but is expired then try to hit the server.
                    if ( result.empty() )
                    {
                        // Need to do this to support nested PLODs and Proxynodes.
                        osg::ref_ptr<osgDB::Options> remoteOptions =
                            Registry::instance()->cloneOrCreateOptions( localOptions.get() );
                        remoteOptions->getDatabasePathList().push_front( osgDB::getFilePath(uri.full()) );

                        // Store the existing object from the cache if there is one.
                        osg::ref_ptr< osg::Object > object = result.getObject();

                        // try to use the callback if it's set. Callback ignores the caching policy.
                        if ( cb )
                        {
                            result = reader.fromCallback( cb, uri.full(), remoteOptions.get() );

                            if ( result.code() != ReadResult::RESULT_NOT_IMPLEMENTED )
                            {
                                // "not implemented" is the only excuse for falling back
                                gotResultFromCallback = true;
                            }
                        }

                        if ( !gotResultFromCallback )
                        {
                            // still no data, go to the source:
                            if (result.empty())
                            {
                                result = reader.fromHTTP(uri, remoteOptions.get(), progress, result.lastModifiedTime());
                            }

                            // Check for cancellation before a cache write
                            if (progress && progress->isCanceled())
                            {
                                NetworkMonitor::end(handle, "Canceled");
                                return 0L;
                            }
                        }
                    }
                }

                // Check for cancelation before a potential cache write
                if (progress && progress->isCanceled())
                {
                    NetworkMonitor::end(handle, "Canceled");
                    return 0L;
                }

                if (result.getObject() && !gotResultFromCallback)
                {
                    result.getObject()->setName( uri.base() );

                    if ( memCache )
                    {
                        memCache->insert( uri, result );
                    }
                }

                // If the request failed with an unrecoverable error,
                // blacklist so we don't waste time on it again
                if (result.failed() && result.code() == ReadResult::RESULT_NOT_FOUND)
                {
                    osgEarth::Registry::instance()->blacklist(inputURI.full());
                }
            }

            OE_TEST << LC
                << uri.base() << ": "
                << (result.succeeded() ? "OK" : "FAILED")
                //<< "; policy=" << cp->usageString()
                << (result.isFromCache() && result.succeeded() ? "; (from cache)" : "")
                << std::endl;
        }

        // post-process if there's a post-URI callback.
        URIPostReadCallback* post = URIPostReadCallback::from(dbOptions);
        if ( post )
        {
            (*post)(result);
        }

        auto msg = result.getResultCodeString();

        if (result.isFromCache() && result.succeeded())
        {
            msg = "Cache";
        }

        std::string details;

        if (!result.metadata().empty())
            details += result.metadata().toJSON(true);

        if (!result.errorDetail().empty())
            details += result.errorDetail();

        NetworkMonitor::end(handle, msg, details);

        return result;
    }
}

ReadResult
URI::readObject(const osgDB::Options* dbOptions,
                ProgressCallback*     progress ) const
{
    return doRead<ReadObject>( *this, dbOptions, progress );
}

ReadResult
URI::readNode(const osgDB::Options* dbOptions,
              ProgressCallback*     progress ) const
{
    return doRead<ReadNode>( *this, dbOptions, progress );
}

ReadResult
URI::readImage(const osgDB::Options* dbOptions,
               ProgressCallback*     progress ) const
{
    return doRead<ReadImage>( *this, dbOptions, progress );
}

ReadResult
URI::readString(const osgDB::Options* dbOptions,
                ProgressCallback*     progress ) const
{
    return doRead<ReadString>( *this, dbOptions, progress );
}

//------------------------------------------------------------------------

void
URIAliasMap::insert(const std::string& key, const std::string& value)
{
    _map.insert( std::make_pair(key, value) );
}

std::string
URIAliasMap::resolve(const std::string& input, const URIContext& context) const
{
    for(std::map<std::string,std::string>::const_iterator i = _map.begin(); i != _map.end(); ++i )
    {
        std::string source = context.getOSGPath(i->first);
        std::string pattern = context.getOSGPath(input);
        if ( ciEquals(source, pattern) )
            return context.getOSGPath(i->second);
    }
    return input;
}


//------------------------------------------------------------------------


URIAliasMapReadCallback::URIAliasMapReadCallback(const URIAliasMap& aliasMap,
                                                 const URIContext&  context ) :
_aliasMap( aliasMap ),
_context ( context )
{
    //nop
}

osgDB::ReaderWriter::ReadResult
URIAliasMapReadCallback::openArchive(const std::string& filename, osgDB::ReaderWriter::ArchiveStatus status, unsigned int indexBlockSizeHint, const osgDB::Options* useObjectCache)
{
    if (osgDB::Registry::instance()->getReadFileCallback()) return osgDB::Registry::instance()->getReadFileCallback()->openArchive(_aliasMap.resolve(filename,_context), status, indexBlockSizeHint, useObjectCache);
    else return osgDB::Registry::instance()->openArchive(_aliasMap.resolve(filename,_context), status, indexBlockSizeHint, useObjectCache);
}

osgDB::ReaderWriter::ReadResult
URIAliasMapReadCallback::readObject(const std::string& filename, const osgDB::Options* options)
{
    if (osgDB::Registry::instance()->getReadFileCallback()) return osgDB::Registry::instance()->getReadFileCallback()->readObject(_aliasMap.resolve(filename,_context),options);
    else return osgDB::Registry::instance()->readObjectImplementation(_aliasMap.resolve(filename,_context),options);
}

osgDB::ReaderWriter::ReadResult
URIAliasMapReadCallback::readImage(const std::string& filename, const osgDB::Options* options)
{
    //OE_INFO << LC << "Map: " << filename << " to " << _aliasMap.resolve(filename,_context) << std::endl;
    if (osgDB::Registry::instance()->getReadFileCallback()) return osgDB::Registry::instance()->getReadFileCallback()->readImage(_aliasMap.resolve(filename,_context),options);
    else return osgDB::Registry::instance()->readImageImplementation(_aliasMap.resolve(filename,_context),options);
}

osgDB::ReaderWriter::ReadResult
URIAliasMapReadCallback::readHeightField(const std::string& filename, const osgDB::Options* options)
{
    if (osgDB::Registry::instance()->getReadFileCallback()) return osgDB::Registry::instance()->getReadFileCallback()->readHeightField(_aliasMap.resolve(filename,_context),options);
    else return osgDB::Registry::instance()->readHeightFieldImplementation(_aliasMap.resolve(filename,_context),options);
}

osgDB::ReaderWriter::ReadResult
URIAliasMapReadCallback::readNode(const std::string& filename, const osgDB::Options* options)
{
    if (osgDB::Registry::instance()->getReadFileCallback()) return osgDB::Registry::instance()->getReadFileCallback()->readNode(_aliasMap.resolve(filename,_context),options);
    else return osgDB::Registry::instance()->readNodeImplementation(_aliasMap.resolve(filename,_context),options);
}

osgDB::ReaderWriter::ReadResult
URIAliasMapReadCallback::readShader(const std::string& filename, const osgDB::Options* options)
{
    if (osgDB::Registry::instance()->getReadFileCallback()) return osgDB::Registry::instance()->getReadFileCallback()->readShader(_aliasMap.resolve(filename,_context),options);
    else return osgDB::Registry::instance()->readShaderImplementation(_aliasMap.resolve(filename,_context),options);
}

/**
* osg plugin that will use the URI class to make network requests instead of the curl plugin.
* The underlying HTTPClient will cache URI's and the CURL plugin will not and it will also update the network monitor in osgEarth.
* This is implemented as a plugin in addition to just being able to use the URI classes directly b/c
* if you load a node from a server and it references an external image (probably with a relative path on the same server)
* then osg's read file logic will end up trying to load the file via osgDB::readNodeFile using one of the plugins.
* We want to use the URI plugin to ensure that the images are cached as well as the node.
*/
class ReaderWriterURI : public osgDB::ReaderWriter
{
public:
    ReaderWriterURI()
    {        
    }

    virtual const char* className() const { return "osgEarth URI Reader"; }    

    std::string makeServerFilename(const std::string& fileName, const osgDB::ReaderWriter::Options* options) const
    {
        if (!osgDB::containsServerAddress(fileName))
        {
            if (options && !options->getDatabasePathList().empty())
            {
                if (osgDB::containsServerAddress(options->getDatabasePathList().front()))
                {
                    std::string newFileName = options->getDatabasePathList().front() + "/" + fileName;
                    return newFileName;
                }
            }
        }
        return fileName;
    }

    virtual ReadResult readImage(const std::string& fileName, const osgDB::ReaderWriter::Options* options) const
    {
        std::string serverFilename = makeServerFilename(fileName, options);
        if (!osgDB::containsServerAddress(serverFilename)) return ReadResult::FILE_NOT_HANDLED;

        URI uri(serverFilename);
        osgEarth::ReadResult result = uri.readImage(options);
        return result.releaseImage();
    }

    virtual ReadResult readObject(const std::string& fileName, const osgDB::ReaderWriter::Options* options = NULL) const
    {
        std::string serverFilename = makeServerFilename(fileName, options);
        if (!osgDB::containsServerAddress(serverFilename)) return ReadResult::FILE_NOT_HANDLED;

        URI uri(serverFilename);
        osgEarth::ReadResult result = uri.readObject(options);
        return result.releaseObject();
    }

    virtual ReadResult readNode(const std::string& fileName, const osgDB::ReaderWriter::Options* options = NULL) const
    {
        std::string serverFilename = makeServerFilename(fileName, options);
        if (!osgDB::containsServerAddress(serverFilename)) return ReadResult::FILE_NOT_HANDLED;

        URI uri(serverFilename);
        osgEarth::ReadResult result = uri.readNode(options);
        return result.releaseNode();
    }
};

REGISTER_OSGPLUGIN(uri, ReaderWriterURI)
