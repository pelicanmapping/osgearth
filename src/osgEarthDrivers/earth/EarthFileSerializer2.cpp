/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "EarthFileSerializer"
#include <osgEarth/FileUtils>
#include <osgEarth/Extension>
#include <osgEarth/StringUtils>
#include <osgEarth/FileUtils>
#include <osgEarth/URI>
#include <osgEarth/GLUtils>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>
#include <stdio.h>
#include <ctype.h>

using namespace osgEarth_osgearth;
using namespace osgEarth;

#undef  LC
#define LC "[EarthSerializer2] "

static const char * const PATH_SEPARATORS = "/\\";
static unsigned int PATH_SEPARATORS_LEN = 2;

namespace
{

	class PathIterator
    {
		public:
			PathIterator(const std::string & v);
			bool valid() const { return start!=end; }
			PathIterator & operator++();
			std::string operator*();

		protected:
			std::string::const_iterator end;     ///< End of path string
			std::string::const_iterator start;   ///< Points to the first char of an element, or ==end() if no more
			std::string::const_iterator stop;    ///< Points to the separator after 'start', or ==end()

			/// Iterate until 'it' points to something different from a separator
			std::string::const_iterator skipSeparators(std::string::const_iterator it);
			std::string::const_iterator next(std::string::const_iterator it);
    };

	PathIterator::PathIterator(const std::string & v) : end(v.end()), start(v.begin()), stop(v.begin()) { operator++(); }

	PathIterator & PathIterator::operator++()
	{
		if (!valid()) return *this;
			start = skipSeparators(stop);
		if (start != end) stop = next(start);
		return *this;
	}
	std::string PathIterator::operator*()
	{
		if (!valid()) return std::string();
		return std::string(start, stop);
	}

	std::string::const_iterator PathIterator::skipSeparators(std::string::const_iterator it)
	{
		for (; it!=end && std::find_first_of(it, it+1, PATH_SEPARATORS, PATH_SEPARATORS+PATH_SEPARATORS_LEN) != it+1; ++it) {}
		return it;
	}

	std::string::const_iterator PathIterator::next(std::string::const_iterator it)
	{
		return std::find_first_of(it, end, PATH_SEPARATORS, PATH_SEPARATORS+PATH_SEPARATORS_LEN);
	}

    // Config tags that we handle specially (versus just letting the plugin mechanism
    // take take of them)
    bool isReservedWord(const std::string& k)
    {
        return
            k == "options" ||
            //k == "image" ||
            //k == "elevation" ||
            //k == "heightfield" ||
            //k == "model" ||
            //k == "mask" ||
            k == "external" ||
            //k == "extensions" ||
            k == "libraries";
    }

    /**
     * Looks at each key in a Config and tries to match that key to a shared library name;
     * loads the shared library associated with the name. This will "pre-load" all the DLLs
     * associated with extensions in the earth file even if they weren't linked.
     *
     * Will also pre-load any expressly indicated shared libraries in the "libraries" element.
     */
    void preloadExtensionLibs(const Config& conf)
    {
        for(ConfigSet::const_iterator i = conf.children().begin(); i != conf.children().end(); ++i)
        {
            const std::string& name = i->key();

            // Namespaced extensions load their nodekits through Extension::create.
            if ( isReservedWord(name) || name.find(':') != std::string::npos )
                continue;

            if (!name.empty())
            {
                // Load the extension library if necessary.
                std::string libName = osgDB::Registry::instance()->createLibraryNameForExtension("osgearth_" + name);
                osgDB::Registry::LoadStatus status = osgDB::Registry::instance()->loadLibrary(libName);
                if (status == osgDB::Registry::NOT_LOADED)
                {
                    // If it failed to load, try loading an extension from an osgEarth library with the same name.
                    // Capitalize the name of the extension.
                    std::string capName = name;
                    capName[0] = ::toupper(capName[0]);
                    libName = osgDB::Registry::instance()->createLibraryNameForNodeKit("osgEarth" + capName);
                    status = osgDB::Registry::instance()->loadLibrary(libName);
                }
            }
        }

        // Preload any libraries
        Config libraries = conf.child("libraries");
        if (!libraries.value().empty())
        {
            auto libs = StringTokenizer()
                .delim(";")
                .tokenize(libraries.value());

            for (StringVector::iterator itr = libs.begin(); itr != libs.end(); ++itr)
            {
                std::string lib = *itr;
                trim2(lib);
                std::string libName = osgDB::Registry::instance()->createLibraryNameForNodeKit(lib);
                osgDB::Registry::LoadStatus status = osgDB::Registry::instance()->loadLibrary(libName);
                if (status != osgDB::Registry::NOT_LOADED)
                {
                    OE_DEBUG << LC << "Loaded nodekit library \"" << libName << "\" OK" << std::endl;
                }
                else
                {
                    OE_WARN << LC << "Failed to nodekit library \"" << libName << "\"" << std::endl;
                }
            }
        }        
    }

    /**
     * Visits a Config hierarchy and rewrites relative pathnames to be relative to a new referrer.
     */
    struct RewritePaths
    {
        bool        _rewriteAbsolutePaths;
        std::string _newReferrerAbsPath;
        std::string _newReferrerFolder;

        RewritePaths(const std::string& referrer)
        {
            _rewriteAbsolutePaths = false;
            _newReferrerAbsPath = osgDB::convertFileNameToUnixStyle( osgDB::getRealPath(referrer) );
            _newReferrerFolder  = osgDB::getFilePath( _newReferrerAbsPath );
        }

        /** Whether to make absolute paths into relative paths if possible */
        void setRewriteAbsolutePaths(bool value)
        {
            _rewriteAbsolutePaths = value;
        }

        /** Returns true if key contains "_fragment", or is identical to "fragment" */
        bool keyContainsFragment(const std::string& key, const std::string& fragment) const
        {
            return key == fragment || key.find("_" + fragment) != std::string::npos;
        }

        bool isLocation(const Config& input) const
        {
            if ( input.value().empty() )
                return false;

            if ( input.referrer().empty() )
                return false;

            return 
                keyContainsFragment(input.key(), "url")      ||
                keyContainsFragment(input.key(), "uri")      ||
                keyContainsFragment(input.key(), "href")     ||
                keyContainsFragment(input.key(), "filename") ||
                keyContainsFragment(input.key(), "file")     ||
                keyContainsFragment(input.key(), "pathname") ||
                keyContainsFragment(input.key(), "path");
        }

        void apply(Config& input)
        {
            if ( isLocation(input) )
            {
                // resolve the absolute path of the input:
                URI inputURI( input.value(), URIContext(input.referrer()) );

                std::string newValue = resolve(inputURI);
                if ( newValue != input.value() )
                {
                    input.setValue(newValue);
                    input.setReferrer( _newReferrerAbsPath );
                }

                if ( !input.externalRef().empty() )
                {
                    URI xrefURI( input.externalRef(), URIContext(input.referrer()) );
                    std::string newXRef = resolve(xrefURI);
                    if ( newXRef != input.externalRef() )
                    {
                        input.setExternalRef( newXRef );
                        input.setReferrer( _newReferrerAbsPath );
                    }
                }
            }

            for(ConfigSet::iterator i = input.children().begin(); i != input.children().end(); ++i)
            {
                apply( *i );
            }
        }

        std::string resolve(const URI& inputURI )
        {
            std::string inputAbsPath = osgDB::convertFileNameToUnixStyle( inputURI.full() );

            // if the abs paths have different roots, no resolution; use the input.
            if (osgDB::getPathRoot(inputAbsPath) != osgDB::getPathRoot(_newReferrerFolder))
            {
                return inputURI.full();
            }

            // see whether the file exists (this is how we verify that it's actually a path)
            //if ( osgDB::fileExists(inputAbsPath) )
            {
                if ( !osgDB::isAbsolutePath(inputURI.base()) || _rewriteAbsolutePaths )
                {
                    std::string inputNewRelPath = getPathRelative( _newReferrerFolder, inputAbsPath );
                    
                    //OE_DEBUG << LC << "\n"
                    //    "   Rewriting \"" << input.value() << "\" as \"" << inputNewRelPath << "\"\n"
                    //    "   Absolute = " << inputAbsPath << "\n"
                    //    "   ReferrerFolder = " << _newReferrerFolder << "\n";

                    return inputNewRelPath;
                }
            }

            return inputURI.base();
        }

        std::string getPathRelative(const std::string& from, const std::string& to)
        {
            // This implementation is not 100% robust, and should be replaced with C++0x "std::path" as soon as possible.

            // Definition: an "element" is a part between slashes. Ex: "/a/b" has two elements ("a" and "b").
            // Algorithm:
            // 1. If paths are neither both absolute nor both relative, then we cannot do anything (we need to make them absolute, but need additional info on how to make it). Return.
            // 2. If both paths are absolute and root isn't the same (for Windows only, as roots are of the type "C:", "D:"), then the operation is impossible. Return.
            // 3. Iterate over two paths elements until elements are equal
            // 4. For each remaining element in "from", add ".." to result
            // 5. For each remaining element in "to", add this element to result

            // 1 & 2
            const std::string root = osgDB::getPathRoot(from);
            if (root != osgDB::getPathRoot(to)) {
                OSG_INFO << "Cannot relativise paths. From=" << from << ", To=" << to << ". Returning 'to' unchanged." << std::endl;
                //return to;
                return osgDB::getSimpleFileName(to);
            }

            // 3
            PathIterator itFrom(from), itTo(to);
            // Iterators may point to Windows roots. As we tested they are equal, there is no need to ++itFrom and ++itTo.
            // However, if we got an Unix root, we must add it to the result.
            // std::string res(root == "/" ? "/" : "");
            // Since result is a relative path, even in unix, no need to add / to the result first.
            std::string res = "";
            for (; itFrom.valid() && itTo.valid() && *itFrom == *itTo; ++itFrom, ++itTo) {}

            // 4
            for (; itFrom.valid(); ++itFrom) res += "../";

            // 5
            for (; itTo.valid(); ++itTo) res += *itTo + "/";

            // Remove trailing slash before returning
            if (!res.empty() && std::find_first_of(res.rbegin(), res.rbegin() + 1, PATH_SEPARATORS, PATH_SEPARATORS + PATH_SEPARATORS_LEN) != res.rbegin() + 1)
            {
                return res.substr(0, res.length() - 1);
            }
            return res;
        }
    };
}

//............................................................................

namespace
{
    // support for "special" extension names (convenience and backwards compat)
    Extension* createSpecialExtension(const Config& conf)
    {
        // special support for the default sky extension:
        if (conf.key() == "sky" && !conf.hasValue("driver"))
            return Extension::create("sky_simple", conf);

        if (conf.key() == "ocean" && !conf.hasValue("driver"))
            return Extension::create("ocean_simple", conf);

        return 0L;
    }

    bool addLayer(const Config& conf, LayerVector& layers)
    {
        auto layer = Layer::create(conf);
        if (layer.valid())
        {
            layers.push_back(layer);

            // Temp: force NVGL OFF to support the legacy splat library
            const std::string splatLibrary("osgEarthSplat");
            if (std::string(layer->libraryName()) == splatLibrary)
            {
                GLUtils::useNVGL(false);
                OE_WARN << LC << "DISABLED NVGL after detecting layer of type \"" << layer->libraryName() << "::" << layer->className() << "\"" << std::endl;
            }
        }
        return layer != 0L;
    }

    Extension* loadExtension(const Config& conf)
    {
        std::string name = conf.key();
        Extension* extension = Extension::create( conf.key(), conf );
        if ( !extension )
        {
            name = conf.key() + "_" + conf.value("driver");
            extension = Extension::create(name, conf);

            if (!extension)
                extension = createSpecialExtension(conf);
        }

        if (!extension)
        {
            OE_INFO << LC << "Failed to find an extension for \"" << name << "\"\n";
        }

        return extension;
    }

    void reportErrors(const Map* map)
    {
        for (unsigned i = 0; i < map->getNumLayers(); ++i)
        {
            const Layer* layer = map->getLayerAt(i);

            if (layer->getStatus().isError() &&
                layer->getOpenAutomatically() == true)
            {
                OE_WARN << LC << layer->getTypeName() << " \"" << layer->getName() << "\" : " << layer->getStatus().toString() << std::endl;
            }
        }
    }

    void replaceV2withV3(Config& c, const std::string& newkey)
    {
        OE_WARN << LC << "Replacing key \"" << c.key() << "\" with \"" << newkey << "\"; please update your earth file" << std::endl;
        c.key() = newkey;
    }

    void updateVersion2ToVersion3(Config& c)
    {
        std::string key0 = c.key();

        if (c.key() == "image" && c.hasValue("driver"))
        {
            const std::string& driver = c.value("driver");
            if (driver == "gdal") c.key() = "GDALImage";
            else if (driver == "mbtiles") replaceV2withV3(c, "MBTilesImage");
            else if (driver == "arcgisonline") replaceV2withV3(c, "ArcGISServerImage");
            else if (driver == "arcgis") replaceV2withV3(c, "ArcGISServerImage");
            else if (driver == "tilepackage") replaceV2withV3(c, "ArcGISTilePackageImage");
            else if (driver == "bing") replaceV2withV3(c, "BingImage");
            else if (driver == "cesiumion") replaceV2withV3(c, "CesiumIonImage");
            else if (driver == "landcover") replaceV2withV3(c, "LandCover");
            else if (driver == "tilecache") replaceV2withV3(c, "TileCacheImage");
            else if (driver == "tms") replaceV2withV3(c, "TMSImage");
            else if (driver == "video") replaceV2withV3(c, "VideoImage");
            else if (driver == "wms") replaceV2withV3(c, "WMSImage");
            else if (driver == "xyz") replaceV2withV3(c, "XYZImage");
            else if (driver == "agglite") replaceV2withV3(c, "FeatureImage");
            else if (driver == "debug") replaceV2withV3(c, "DebugImage");
            else if (driver == "road_surface") replaceV2withV3(c, "RoadSurface");
            else if (driver == "db") replaceV2withV3(c, "DBImage");
            else if (driver == "composite" && !c.children().empty())
            {
                ConfigSet children = c.children("image");
                c.remove("image");
                Config layers("layers");
                layers.add(children);
                c.add(layers);
                replaceV2withV3(c, "CompositeImage");
            }
        }
        else if (c.key() == "elevation" && c.hasValue("driver"))
        {
            const std::string& driver = c.value("driver");
            if (driver == "gdal") replaceV2withV3(c, "GDALElevation");
            else if (driver == "mbtiles") replaceV2withV3(c, "MBTilesElevation");
            else if (driver == "bing") replaceV2withV3(c, "BingElevation");
            else if (driver == "tms") replaceV2withV3(c, "TMSElevation");
            else if (driver == "xyz") replaceV2withV3(c, "XYZElevation");
            else if (driver == "tilecache") replaceV2withV3(c, "TileCacheElevation");
            else if (driver == "flatten_elevation") replaceV2withV3(c, "FlattenElevation");
            else if (driver == "fractal_elevation") replaceV2withV3(c, "FractalElevation");
            else if (driver == "db") replaceV2withV3(c, "DBElevation");
            else if (driver == "composite" && !c.children().empty())
            {
                ConfigSet children = c.children("elevation");
                c.remove("elevation");
                Config layers("layers");
                layers.add(children);
                c.add(layers);
                replaceV2withV3(c, "CompositeElevation");
            }
        }
        else if (c.key() == "model" && c.hasValue("driver"))
        {
            const std::string& driver = c.value("driver");
            if (driver == "simple") replaceV2withV3(c, "Model");
            else if (driver == "feature_geom") replaceV2withV3(c, "FeatureModel");
        }
        else if (c.key() == "mask" && c.hasValue("driver"))
        {
            const std::string& driver = c.value("driver");
            if (driver == "feature") c.key() = "FeatureMask";
        }
        else if (c.key() == "feature_source" || c.key() == "features")
        {
            const std::string& driver = c.value("driver");
            if (driver == "ogr") replaceV2withV3(c, "OGRFeatures");
            else if (driver == "wfs") replaceV2withV3(c, "WFSFeatures");
            else if (driver == "tfs") replaceV2withV3(c, "TFSFeatures");
            else if (driver == "mapnikvectortiles") replaceV2withV3(c, "MVTFeatures");
            else if (driver == "xyz") replaceV2withV3(c, "XYZFeatures");
            else if (driver == "image_to_feature") replaceV2withV3(c, "ImageToFeature");
        }
        else if (c.key() == "splat_imagery") replaceV2withV3(c, "SplatImage");
        else if (c.key() == "splat_groundcover") replaceV2withV3(c, "GroundCover");
        else if (c.key() == "land_cover") replaceV2withV3(c, "LandCover");

        if (key0 != c.key())
        {
            c.remove("driver");
        }

        for (ConfigSet::iterator j = c.children().begin(); j != c.children().end(); ++j)
        {
            updateVersion2ToVersion3(*j);
        }
    }

    // Look for the first layer with basemap=true set, and adopt this layer's
    // profile as the map profile.
    void checkForProfileLayer(Map* map)
    {
        const std::string& profileLayer = const_cast<const Map*>(map)->options().profileLayer().get();
        if (profileLayer.empty())
            return;

        TileLayerVector layers;
        map->getLayers(layers);
        for(TileLayerVector::const_iterator i = layers.begin();
            i != layers.end();
            ++i)
        {
            const TileLayer* layer = i->get();
            if (profileLayer == layer->getName())
            {
                const Profile* profile = layer->getProfile();
                if (profile)
                {
                    map->setProfile(profile);
                    break;
                }
            }
        }
    }
}

EarthFileSerializer2::EarthFileSerializer2() :
_rewritePaths        ( true ),
_rewriteAbsolutePaths( false )
{
    // nop
}


osg::ref_ptr<osg::Node>
EarthFileSerializer2::deserialize( 
    const Config& const_conf, 
    const std::string& referrer,
    const osgDB::Options* readOptions) const
{
    Config conf = const_conf;

    // First, pre-load any extension DLLs.
    preloadExtensionLibs(conf);
    preloadExtensionLibs(conf.child("extensions"));
    preloadExtensionLibs(conf.child("external"));

    Map::Options mapOptions(conf.child("options"));

    // Check for name/type in top-level attrs:
    if ( conf.hasValue( "name" ) || conf.hasValue( "type" ) )
    {
        Config temp;
        if ( conf.hasValue("name") ) temp.set( "name", conf.value("name") );
        if ( conf.hasValue("type") ) temp.set( "type", conf.value("type") );
        mapOptions.merge(ConfigOptions(temp));
    }

    // Check for NVGL override
    if (conf.hasValue("nvgl"))
    {
        GLUtils::useNVGL(conf.value<bool>("nvgl", true));
        Config temp;
        temp.add(conf.child("nvgl"));
        mapOptions.merge(ConfigOptions(temp));
    }

    // Check for profile layer setting
    if (conf.hasValue("profile_layer"))
    {
        std::string profileLayer = conf.value("profile_layer");
        if (!profileLayer.empty())
            mapOptions.profileLayer() = profileLayer;
    }

    osg::ref_ptr<Map> map = new Map(mapOptions, readOptions);

    if (map->getProfile() == nullptr)
        return nullptr;

    // First go through and update to version 3.
    updateVersion2ToVersion3(conf);

    // Start a batch update of the map:
    map->beginUpdate();

    LayerVector layers;
    Config externalConfig;
    std::vector<osg::ref_ptr<Extension> > extensions;
    bool forceDisableNVGL = false;
    
    for(ConfigSet::const_iterator i = conf.children().begin(); i != conf.children().end(); ++i)
    {
        if (i->key() == "options" || i->key() == "name" || i->key() == "type" || i->key() == "version")
        {
            // nop - handled earlier
        }

        else if ( i->key() == "external" )
        {
            externalConfig = *i;
            
            //for(ConfigSet::const_iterator e = i->children().begin(); e != i->children().end(); ++e)
            //{
            //    Extension* extension = loadExtension(*e);
            //    if (extension)
            //        extensions.push_back(extension);
            //}
        }

        else if ( !isReservedWord(i->key()) )
        {
            // try to add as a Layer first:
            bool addedLayer = addLayer(*i, layers); 

            // failing that, try to load as an Extension:
            if ( !addedLayer )
            {
                Extension* extension = loadExtension(*i);
                if (extension)
                    extensions.push_back(extension);
            }
        }
    }

    // Add our layers as a batch
    map->addLayers(layers);

    // Complete the batch update of the map
    map->endUpdate();
    
    // Check for a "basemap" layer that will set the map's profile.
    checkForProfileLayer(map.get());

    // If any errors occurred, report them now.
    reportErrors(map.get());

    // Yes, Map::Options and MapNode::Options share the same Config node. Weird but true.
    MapNode::Options mapNodeOptions( conf.child("options") );

    if (readOptions && (
        readOptions->getOptionString().find("OSGEARTH_USE_NVGL") != std::string::npos ||
        readOptions->getOptionString().find("OSGEARTH_USE_GL4") != std::string::npos))
    {
        GLUtils::useNVGL(true);
    }

    // Create a map node.
    osg::ref_ptr<MapNode> mapNode = new MapNode( map.get(), mapNodeOptions );

    // Apply the external conf if there is one.
    if (!externalConfig.empty())
    {
        mapNode->externalConfig() = externalConfig;
    }

    // Install the extensions
    for (unsigned i = 0; i < extensions.size(); ++i)
    {
        mapNode->addExtension(extensions[i].get());
    }

    // return the topmost parent of the mapnode. It's possible that
    // an extension added parents!
    osg::ref_ptr<osg::Node> top = mapNode;

    while (top->getNumParents() > 0)
    {
        top = top->getParent(0);
    }

    return top;
}


Config
EarthFileSerializer2::serialize(const MapNode* input, const std::string& referrer) const
{
    Config mapConf("map");
    
    if (input && input->getMap())
    {
        mapConf = input->getConfig();

        // Re-write pathnames in the Config so they are relative to the new referrer.
        if ( _rewritePaths && !referrer.empty() )
        {
            RewritePaths rewritePaths( referrer );
            rewritePaths.setRewriteAbsolutePaths( _rewriteAbsolutePaths );
            rewritePaths.apply( mapConf );
        }
    }

    return mapConf;
}
