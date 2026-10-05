#include <osgEarthProcedural2/Canopy>
#include <algorithm>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

double osgEarth::Procedural2::canopyReferenceError(const TileKey& key)
{
    const auto& e = key.getExtent();
    // Global-geodetic cells are angular squares; their north/south span bounds horizontal extent at any latitude.
    const double span = e.getSRS()->isProjected() ?
        Units::convert(e.getSRS()->getUnits(),Units::METERS,std::max(e.width(),e.height())) : e.height(Units::METERS);
    return span/48.0;
}
