/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/IOTypes>
#include <osgEarth/URI>
#include <osgDB/Registry>
#include <osgDB/FileNameUtils>
#include <utility>

using namespace osgEarth;

//------------------------------------------------------------------------

const std::string IOMetadata::CONTENT_TYPE = "Content-Type";

//------------------------------------------------------------------------

StringObject::StringObject() :
osg::Object()
{
    //nop
}

StringObject::~StringObject()
{
}

const std::string& StringObject::getString() const
{
    return _str;
}

void StringObject::setString( const std::string& value )
{
    _str = value;
}

void StringObject::setString( std::string&& value )
{
    _str = std::move(value);
}

//----------------------------------------------------------------------------

ProxySettings::ProxySettings( const Config& conf )
{
    mergeConfig( conf );
}

ProxySettings::ProxySettings( const std::string& host, int port ) :
    _hostName(host),
    _port(port)
{
    //nop
}

void
ProxySettings::mergeConfig( const Config& conf )
{
    _hostName = conf.value<std::string>( "host", "" );
    _port = conf.value<int>( "port", 8080 );
    _userName = conf.value<std::string>( "username", "" );
    _password = conf.value<std::string>( "password", "" );
}

Config
ProxySettings::getConfig() const
{
    Config conf( "proxy" );
    conf.add( "host", _hostName );
    conf.add( "port", toString(_port) );
    conf.add( "username", _userName);
    conf.add( "password", _password);

    return conf;
}

bool
ProxySettings::fromOptions( const osgDB::Options* dbOptions, optional<ProxySettings>& out )
{
    if ( dbOptions )
    {
        std::string jsonString = dbOptions->getPluginStringData( "osgEarth::ProxySettings" );
        if ( !jsonString.empty() )
        {
            Config conf;
            conf.fromJSON( jsonString );
            out = ProxySettings( conf );
            return true;
        }
    }
    return false;
}

void
ProxySettings::apply( osgDB::Options* dbOptions ) const
{
    if ( dbOptions )
    {
        Config conf = getConfig();
        dbOptions->setPluginStringData( "osgEarth::ProxySettings", conf.toJSON() );
    }
}

//------------------------------------------------------------------------

URIReadCallback::URIReadCallback()
{
    //nop
}

URIReadCallback::~URIReadCallback()
{
}

//------------------------------------------------------------------------

/**
 * Registerd "StringObject" with OSG's serialization framework. Basically
 * that means that StringObject instances can be read/written to an .osgb
 * file. We use this for caching string data (XML, JSON files for example).
 */
namespace osgEarth { namespace Serializers { namespace StringObject
{
    REGISTER_OBJECT_WRAPPER(StringObject,
                            new osgEarth::StringObject,
                            osgEarth::StringObject,
                            "osgEarth::StringObject")
    {
        ADD_STRING_SERIALIZER( String, "" );  // _str
    }
} } }

//------------------------------------------------------------------------

/**
 * This is an OSG reader/writer template. We're using this to register 
 * readers for "XML" and "JSON" format -- these are just text files, but we
 * have to do this to enable OSG to read them from inside an archive (like
 * a ZIP file). For example, this lets OSG read the tilemap.xml file stores
 * with a TMS repository in a ZIP.
 */

#define STRING_READER_WRITER_SHIM(SUFFIX, EXTENSION, DEF) \
struct osgEarthStringReaderWriter##SUFFIX : public osgDB::ReaderWriter \
{ \
    osgEarthStringReaderWriter##SUFFIX () { \
        supportsExtension( EXTENSION, DEF ); \
    } \
    osgDB::ReaderWriter::ReadResult readObject(const std::string& uri, const osgDB::Options* dbOptions) const { \
        std::string ext = osgDB::getLowerCaseFileExtension( uri ); \
        if ( !acceptsExtension(ext) ) return osgDB::ReaderWriter::ReadResult::FILE_NOT_HANDLED; \
        osgEarth::ReadResult r = URI( uri ).readString( dbOptions ); \
        if ( r.succeeded() ) return r.release<StringObject>(); \
        return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE; \
    } \
    osgDB::ReaderWriter::ReadResult readObject(std::istream& in, const osgDB::Options* dbOptions ) const { \
        URIContext uriContext( dbOptions ); \
        return new StringObject( Stringify() << in.rdbuf() ); \
    } \
    osgDB::ReaderWriter::WriteResult writeObject( const osg::Object& obj, const std::string& location, const osgDB::Options* dbOptions ) const { \
        std::string ext = osgDB::getLowerCaseFileExtension(location); \
        if ( !acceptsExtension(ext) ) return osgDB::ReaderWriter::WriteResult::FILE_NOT_HANDLED; \
        const StringObject* so = dynamic_cast<const StringObject*>(&obj); \
        if ( !so ) return osgDB::ReaderWriter::WriteResult::FILE_NOT_HANDLED; \
        std::ofstream out(location.c_str()); \
        if ( out.is_open() ) { out << so->getString(); out.close(); return osgDB::ReaderWriter::WriteResult::FILE_SAVED; } \
        return osgDB::ReaderWriter::WriteResult::ERROR_IN_WRITING_FILE; \
    } \
}

STRING_READER_WRITER_SHIM( XML, "xml", "osgEarth XML shim" );
REGISTER_OSGPLUGIN( xml, osgEarthStringReaderWriterXML );

STRING_READER_WRITER_SHIM( JSON, "json", "osgEarth JSON shim" );
REGISTER_OSGPLUGIN( json, osgEarthStringReaderWriterJSON );
