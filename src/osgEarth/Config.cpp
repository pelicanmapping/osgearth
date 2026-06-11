/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarth/Config>
#include <osgEarth/XmlUtils>
#include <osgEarth/JsonUtils>
#include <osgEarth/FileUtils>
#include <osgEarth/URI>
#include <osgDB/FileNameUtils>
#include <osgEarth/Notify>

using namespace osgEarth;

#define LC "[Config] "

Config&
Config::setReferrer( const std::string& referrer )
{
    if (referrer.empty())
        return *this;

    std::string absReferrer;
    if( !osgDB::containsServerAddress( referrer ) && !osgDB::isAbsolutePath( referrer ) ) {

        absReferrer = osgEarth::getAbsolutePath( referrer );

        if( osgEarth::isRelativePath( absReferrer ) )
        {
            OE_WARN << LC << "ILLEGAL: call to setReferrer with relative path:  "
                "key=" << key() << "; referrer=" << referrer << "\n";
            return *this;
        }
    }
    else {
        absReferrer = referrer;
    }

    // Don't overwrite an existing referrer:
    if ( _referrer.empty() )
    {
        _referrer = absReferrer;
    }

    for( ConfigSet::iterator i = _children.begin(); i != _children.end(); i++ )
    { 
        i->setReferrer( absReferrer );
    }

    return *this;
}

bool
Config::fromXML( std::istream& in )
{
    osg::ref_ptr<XmlDocument> xml = XmlDocument::load( in );
    if (xml.valid())
    {
        *this = std::move(xml->getConfig());
    }
    return xml.valid();
}

bool
Config::fromURI(const URI& uri)
{
    auto result = uri.readString();
    if (result.failed())
        return false;

    std::string data = result.getString();
    if (!data.empty())
    {
        data = trim(data);

        if (data[0] == '{')
        {
            fromJSON(data);
        }
        else if (data[0] == '<')
        {
            std::istringstream in(data);
            fromXML(in);
            if (key() == "Document" && children().size() > 0)
            {
                Config temp = std::move(children().front());
                *this = std::move(temp);
            }               
        }
        else
        {
            return false;
        }
    }

    setReferrer(uri.full());
    return true;
}

const Config&
Config::child(const std::string& childName) const
{
    for (auto& c : _children) {
        if (keys_equal(c.key(), childName))
            return c;
    }

    static Config s_emptyConf;
    return s_emptyConf;
}

const Config*
Config::child_ptr(const std::string& childName) const
{
    for (auto& c : _children)
    {
        if (keys_equal(c.key(), childName))
            return &c;
    }

    return nullptr;
}

Config*
Config::mutable_child( const std::string& childName )
{
    for (auto& c : _children)
    {
        if (keys_equal(c.key(), childName))
            return &c;
    }

    return nullptr;
}

void
Config::merge(const Config& rhs)
{
    // remove any matching keys first; this will allow the addition of multi-key values
    for (auto& c : rhs._children)
        remove(c.key());

    // add in the new values.
    for (auto& c : rhs._children)
        add(c);
}

const Config*
Config::find(const std::string& key, bool checkMe) const
{
    if (checkMe && keys_equal(key, this->key()))
        return this;

    for (auto& c : _children)
    {
        if (keys_equal(key, c.key()))
            return &c;
    }

    for (auto& c : _children)
    {
        const Config* r = c.find(key, false);
        if (r) return r;
    }

    return nullptr;
}

Config*
Config::find(const std::string& key, bool checkMe)
{
    if (checkMe && keys_equal(key, this->key()))
        return this;

    for (auto& c : _children)
        if (keys_equal(key, c.key()))
            return &c;

    for (auto& c : _children)
    {
        Config* r = c.find(key, false);
        if (r) return r;
    }

    return nullptr;
}


namespace
{
    // Converts a Config to JSON. The "nicer" flag formats the data in a more 
    // readable way than nicer=false. Nicer=true attempts to create JSON "objects",
    // whereas nicer=false makes "$key" and "$children" members.
    Json::Value conf2json(const Config& conf, bool nicer, int depth)
    {
        Json::Value value( Json::objectValue );

        if ( conf.isSimple() )
        {
            value[ conf.key() ] = conf.value();
        }
        else
        {
            if ( !nicer )
            {
                if ( !conf.key().empty() )
                {
                    value["$key"] = conf.key();
                }
            }

            if ( !conf.value().empty() )
            {
                value["$value"] = conf.value();
            }

            if ( conf.children().size() > 0 )
            {
                if ( nicer )
                {
                    std::map< std::string, std::vector<Config> > sets; // sorted

                    // sort into bins by name:
                    for(auto& child : conf.children())
                    {
                        sets[child.key()].push_back(child);
                    }

                    if (sets.size() == 1 && sets.begin()->second.size() > 1)
                    {
                        // if there's only one set, and it has more than one member, it's a JSON array.
                        auto& only_set = *sets.begin();
                        auto& members = only_set.second;
                        Json::Value array_value(Json::arrayValue);
                        for (auto& member : members)
                        {
                            array_value.append(conf2json(member, nicer, depth + 1));
                        }
                        value = array_value;
                    }

                    else
                    {
                        for (auto& i : sets)
                        {
                            if (i.second.size() == 1)
                            {
                                auto& c = i.second[0];
                                if (c.isSimple())
                                {
                                    if (c.isNumber())
                                        value[i.first] = c.valueAs<double>(0.0);
                                    else
                                        value[i.first] = c.value();
                                }
                                else
                                {
                                    value[i.first] = conf2json(c, nicer, depth + 1);
                                }
                            }
                            else
                            {
                                std::string array_key = Stringify() << i.first << "__array__";
                                Json::Value array_value(Json::arrayValue);
                                for (std::vector<Config>::iterator j = i.second.begin(); j != i.second.end(); ++j)
                                {
                                    array_value.append(conf2json(*j, nicer, depth + 1));
                                }
                                value[array_key] = array_value;
                                //value = array_value;
                            }
                        }
                    }
                }
                else
                {
                    Json::Value children( Json::arrayValue );
                    unsigned i = 0;

                    for( ConfigSet::const_iterator c = conf.children().begin(); c != conf.children().end(); ++c )
                    {
                        if ( c->isSimple() )
                            value[c->key()] = c->value();
                        else
                            children[i++] = conf2json(*c, nicer, depth+1);
                    }

                    if ( !children.empty() )
                    {
                        value["$children"] = children;
                    }
                }
            }

            // At the root, embed the Config in a single JSON object.
            if ( depth == 0 )
            {
                Json::Value root;
                root[conf.key()] = value;
                value = root;
            }
        }

        return value;
    }

    void json2conf(const Json::Value& json, Config& conf, int depth)
    {
        if ( json.type() == Json::objectValue )
        {
            Json::Value::Members members = json.getMemberNames();

            for( Json::Value::Members::const_iterator i = members.begin(); i != members.end(); ++i )
            {
                const Json::Value& value = json[*i];

                if ( value.isObject() )
                {
                    if (depth == 0 && members.size() == 1)
                    {
                        conf.key() = *i;
                        json2conf(value, conf, depth+1);
                    }
                    else
                    {
                        Config element( *i );
                        json2conf( value, element, depth+1 );
                        conf.add( std::move(element) );
                    }
                }
                else if ( value.isArray() )
                {
                    if ( endsWith(*i, "__array__") )
                    {
                        std::string key = i->substr(0, i->length()-9);
                        for( Json::Value::const_iterator j = value.begin(); j != value.end(); ++j )
                        {
                            Config child;
                            json2conf( *j, child, depth+1 );
                            conf.add( key, std::move(child) );
                        }
                    }
                    else if ( endsWith(*i, "_$set") ) // backwards compatibility
                    {
                        std::string key = i->substr(0, i->length()-5);
                        for( Json::Value::const_iterator j = value.begin(); j != value.end(); ++j )
                        {
                            Config child;
                            json2conf( *j, child, depth+1 );
                            conf.add( key, std::move(child) );
                        }
                    }
                    else
                    {
                        Config element( *i );
                        json2conf( value, element, depth+1 );
                        conf.add( std::move(element) );
                    }
                }
                else if ( (*i) == "$key" )
                {
                    conf.key() = value.asString();
                }
                else if ( (*i) == "$value" )
                {
                    conf.setValue(value.asString());
                }
                else if ( (*i) == "$children" && value.isArray() )
                {
                    json2conf( value, conf, depth+1 );
                }
                else
                {
                    if( value.isBool())
                        conf.add(*i, value.asBool());
                    else if( value.isDouble())
                        conf.add(*i, value.asDouble());
                    else if (value.isInt())
                        conf.add(*i, value.asInt());
                    else if (value.isUInt())
                        conf.add(*i, value.asUInt());
                    else
                        conf.add(*i, value.asString());
                }
            }
        }
        else if ( json.type() == Json::arrayValue )
        {          
            for( Json::Value::const_iterator j = json.begin(); j != json.end(); ++j )
            {
                Config child;
                json2conf( *j, child, depth+1 );
                if ( !child.empty() )
                    conf.add( std::move(child) );
            }
        }
        else if ( json.type() != Json::nullValue )
        {
            conf.setValue(json.asString());
        }
    }
}

std::string
Config::toJSON( bool pretty ) const
{
    Json::Value root = conf2json( *this, true, 0 );
    if ( pretty )
        return Json::StyledWriter().write( root );
    else
        return Json::FastWriter().write( root );
}

bool
Config::fromJSON( const std::string& input )
{
    Json::Reader reader;
    Json::Value root( Json::objectValue );
    if ( reader.parse( input, root ) )
    {
        json2conf( root, *this, 0 );
        return true;
    }
    else
    {
        OE_WARN 
            << "JSON decoding error: "
            << reader.getFormatedErrorMessages() 
            << std::endl;
    }
    return false;
}

Config
Config::readJSON(const std::string& json)
{
    Config conf;
    conf.fromJSON(json);
    return conf;
}

Config
Config::operator - ( const Config& rhs ) const
{
    Config result( *this );

    for( ConfigSet::const_iterator i = rhs.children().begin(); i != rhs.children().end(); ++i )
    {
        result.remove( i->key() );
    }

    return result;
}
