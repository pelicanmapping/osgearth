/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ElevationRanges>

using namespace osgEarth;

// Generated using code at https://gist.github.com/jasonbeverage/9fa0388163b44e47d67a97cc62b68bc8
#include "elevation_ranges.cpp"

unsigned int ElevationRanges::getMaxLevel()
{
    return s_maxLevel;
}

osg::ref_ptr<const Profile> ElevationRanges::getProfile()
{
    // Cache the immutable GLOBAL_GEODETIC profile; construction runs Config::toJSON twice.
    static osg::ref_ptr<const Profile> s_profile = Profile::create(Profile::GLOBAL_GEODETIC);
    return s_profile;
}

bool ElevationRanges::getDefaultElevationRange(short& min, short& max)
{
    min = -2000;
    max = 9000;
    return true;
}

bool ElevationRanges::getElevationRange(unsigned int level, unsigned int x, unsigned int y, short& min, short& max)
{
    osg::ref_ptr< const Profile > profile = getProfile();

    unsigned int width;
    unsigned int height;
    profile->getNumTiles(level, width, height);

    if (level < 0 || level > s_maxLevel ||
        x < 0 || x >= width ||
        y < 0 || y >= height)
    {
        OE_WARN << "Requested tile coordinate " << level << " (" << x << ", " << y << ") are outside of valid range" << std::endl;
        return false;
    }

    unsigned int index = y * width + x;    
    min = s_minElevationsLOD[level][index];
    max = s_maxElevationsLOD[level][index];
    return true;
}