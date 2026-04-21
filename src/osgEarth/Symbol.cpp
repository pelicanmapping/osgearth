/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/Symbol>
#include <osgEarth/Registry>
#include <mutex>

using namespace osgEarth;

SymbolRegistry*
SymbolRegistry::instance()
{
    static std::once_flag s_once;
    static osg::ref_ptr<SymbolRegistry> s_singleton;

    std::call_once(s_once, []() {
        s_singleton = new SymbolRegistry();
        Registry::instance()->registerSingleton(s_singleton.get());
    });

    return s_singleton;
}

void
SymbolRegistry::add( SymbolFactory* factory )
{
    _factories.push_back( factory );
}

Symbol*
SymbolRegistry::create( const Config& conf )
{
    for (SymbolFactoryList::iterator itr = _factories.begin(); itr != _factories.end(); itr++)
    {
        Symbol* symbol = itr->get()->create( conf );
        if (symbol) return symbol;
    }
    return 0;
} 

void SymbolRegistry::parseSLD(const Config& c, class Style& style) const
{
    for (SymbolFactoryList::const_iterator itr = _factories.begin(); itr != _factories.end(); itr++)
    {
        itr->get()->parseSLD( c, style );     
    }
}

//------------------------------------------------------------------------

Symbol::Symbol(const Config& conf)
{
    _uriContext = URIContext(conf.referrer());
    mergeConfig(conf);
}

Symbol::Symbol(const Symbol& rhs, const osg::CopyOp& copyop) :
    osg::Object(rhs, copyop)
{
    _library = rhs._library;
    _script = rhs._script;
    _uriContext = rhs._uriContext;
}

void
Symbol::mergeConfig(const Config& conf)
{
    conf.get("script", script());
    conf.get("library", library());
    if (conf.hasChild("__original"))
        _ctorConfig = conf.child("__original");
    else
        _ctorConfig = conf;
}

Config
Symbol::getConfig() const
{
    Config conf;
    conf.set("script", script());
    conf.set("library", library());
    if (!_ctorConfig.empty())
        conf.add("__original", _ctorConfig);
    return conf;
}

bool
Symbol::match(const std::string& s, const char* reservedWord)
{
    if ( s.compare(reservedWord) == 0 ) return true;
    std::string temp1 = toLower(s), temp2 = toLower(reservedWord);
    replaceIn(temp1, "_", "-");
    replaceIn(temp2, "_", "-");
    return temp1.compare(temp2) == 0;
}
