/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/CachePolicy>
#include <algorithm>
#include <climits>

using namespace osgEarth;

//------------------------------------------------------------------------

//statics
CachePolicy CachePolicy::DEFAULT;
CachePolicy CachePolicy::NO_CACHE( CachePolicy::USAGE_NO_CACHE );
CachePolicy CachePolicy::CACHE_ONLY( CachePolicy::USAGE_CACHE_ONLY );

//------------------------------------------------------------------------

CachePolicy::CachePolicy() :
    _usage(USAGE_READ_WRITE),
    _maxAge(INT_MAX),
    _minTime(0)
{
    //nop
}

CachePolicy::CachePolicy(const Usage& usage) :
    _usage(usage),
    _maxAge(INT_MAX),
    _minTime(0)
{
    _usage = usage; // explicity set the optional<>
}

CachePolicy::CachePolicy(const Config& conf) :
    _usage(USAGE_READ_WRITE),
    _maxAge(INT_MAX),
    _minTime(0)
{
    fromConfig(conf);
}

CachePolicy::CachePolicy(const CachePolicy& rhs) :
    _usage(rhs._usage),
    _maxAge(rhs._maxAge),
    _minTime(rhs._minTime)
{
    //nop
}

void
CachePolicy::mergeAndOverride(const CachePolicy& rhs)
{
    if ( rhs.usage().isSet() )
        usage() = rhs.usage().get();

    if ( rhs.minTime().isSet() )
        minTime() = rhs.minTime().get();

    if ( rhs.maxAge().isSet() )
        maxAge() = rhs.maxAge().get();
}

void
CachePolicy::mergeAndOverride(const optional<CachePolicy>& rhs)
{
    if ( rhs.isSet() )
    {
        mergeAndOverride( rhs.get() );
    }
}

TimeStamp
CachePolicy::getMinAcceptTime() const
{
    TimeStamp minimum = _minTime.getOrUse(0);
    if (_maxAge.isSet())
    {
        const TimeStamp cutoff = ::time(nullptr) - _maxAge.value();
        minimum = std::max(minimum, cutoff);
    }
    return minimum;
}

bool
CachePolicy::isExpired(TimeStamp lastModified) const
{
    return lastModified < getMinAcceptTime();    
}

bool
CachePolicy::operator == (const CachePolicy& rhs) const
{
    return 
        (_usage.get() == rhs._usage.get()) &&
        (_maxAge.get() == rhs._maxAge.get()) &&
        (_minTime.get() == rhs._minTime.get());
}

CachePolicy&
CachePolicy::operator = ( const CachePolicy& rhs )
{
    _usage  = optional<Usage>(rhs._usage);
    _maxAge = optional<TimeSpan>(rhs._maxAge);
    _minTime = optional<TimeStamp>(rhs._minTime);

    return *this;
}

std::string
CachePolicy::usageString() const
{
    if ( _usage == USAGE_READ_WRITE ) return "read-write";
    if ( _usage == USAGE_READ_ONLY )  return "read-only";
    if ( _usage == USAGE_CACHE_ONLY)  return "cache-only";
    if ( _usage == USAGE_NO_CACHE)    return "no-cache";
    return "unknown";
}

bool
CachePolicy::empty() const
{
    bool isSet = _usage.isSet() || _maxAge.isSet() || _minTime.isSet();
    return !isSet;
}

void
CachePolicy::fromConfig( const Config& conf )
{
    conf.get( "usage", "read_write",   _usage, USAGE_READ_WRITE );
    conf.get( "usage", "read_only",    _usage, USAGE_READ_ONLY );
    conf.get( "usage", "cache_only",   _usage, USAGE_CACHE_ONLY );
    conf.get( "usage", "no_cache",     _usage, USAGE_NO_CACHE );
    conf.get( "usage", "none",         _usage, USAGE_NO_CACHE );
    conf.get( "max_age", _maxAge );
    conf.get( "min_time", _minTime );
}

Config
CachePolicy::getConfig() const
{
    Config conf( "cache_policy" );
    conf.set( "usage", "read_write",   _usage, USAGE_READ_WRITE );
    conf.set( "usage", "read_only",    _usage, USAGE_READ_ONLY );
    conf.set( "usage", "cache_only",   _usage, USAGE_CACHE_ONLY );
    conf.set( "usage", "no_cache",     _usage, USAGE_NO_CACHE );
    conf.set( "max_age", _maxAge );
    conf.set( "min_time", _minTime );
    return conf;
}
