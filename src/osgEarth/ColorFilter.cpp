/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ColorFilter>
#include <osgEarth/Registry>
#include <mutex>

using namespace osgEarth;

ColorFilterRegistry*
ColorFilterRegistry::instance()
{
    // OK to be in the local scope since this gets called at static init time
    // by the OSGEARTH_REGISTER_COLORFILTER macro
    static std::once_flag s_once;
    static osg::ref_ptr<ColorFilterRegistry> s_singleton;

    std::call_once(s_once, []() {
        s_singleton = new ColorFilterRegistry();
        Registry::instance()->registerSingleton(s_singleton.get()); 
    });

    return s_singleton;
}

bool
ColorFilterRegistry::readChain(const Config& conf, ColorFilterChain& out_chain)
{
    bool createdAtLeastOne = false;

    // first try to parse the top-level config:
    ColorFilter* top = createOne( conf );
    if ( top )
    {
        out_chain.push_back( top );
        createdAtLeastOne = true;
    }

    // failing that, treat it like a chain:
    else
    {
        for( ConfigSet::const_iterator i = conf.children().begin(); i != conf.children().end(); ++i )
        {
            ColorFilter* object = createOne( *i );
            if ( object )
            {
                out_chain.push_back( object );
                createdAtLeastOne = true;
            }
        }
    }

    return createdAtLeastOne;
}


bool
ColorFilterRegistry::writeChain(const ColorFilterChain& chain, Config& out_config)
{
    bool wroteAtLeastOne = false;

    for( ColorFilterChain::const_iterator i = chain.begin(); i != chain.end(); ++i )
    {
        Config conf = i->get()->getConfig();
        if ( !conf.empty() )
        {
            out_config.add( conf );
            wroteAtLeastOne = true;
        }
    }

    return wroteAtLeastOne;
}


void
ColorFilterRegistry::add( const std::string& key, class ColorFilterFactory* factory )
{
    if ( factory )
        _factories[key] = factory;
}

ColorFilter*
ColorFilterRegistry::createOne(const Config& conf) const
{
    FactoryMap::const_iterator f = _factories.find( conf.key() );
    if ( f != _factories.end() && f->second != 0L )
    {
        ColorFilter* object = f->second->create(conf);
        if ( object )
        {
            return object;
        }
    }
    return 0L;
}
