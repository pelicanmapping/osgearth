#include <osgEarthPrestige/Canopy>
#include <algorithm>

using namespace osgEarth;
using namespace osgEarthPrestige;

double osgEarthPrestige::canopyReferenceError(const TileKey& key)
{
    const auto& e = key.getExtent();
    // Global-geodetic cells are angular squares; their north/south span bounds horizontal extent at any latitude.
    const double span = e.getSRS()->isProjected() ?
        Units::convert(e.getSRS()->getUnits(),Units::METERS,std::max(e.width(),e.height())) : e.height(Units::METERS);
    return span/48.0;
}
