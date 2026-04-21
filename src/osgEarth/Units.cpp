/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include "Units"
#include "Registry"
#include "StringUtils"
#include <string>

using namespace osgEarth;

//------------------------------------------------------------------------

namespace
{
    template<typename T>
    bool parseValueAndUnits(const std::string& input, T& out_value, UnitsType& out_units, const UnitsType& defaultUnits)
    {
        // parse the numeric part into "value", and point "ptr" at the first
        // non-numeric character in the string.
        auto value_and_index = Strings::parseDoubleAndIndex(input);

        if (std::isnan(value_and_index.first))
            return false;

        out_value = value_and_index.first;

        std::string unitsStr = trim(input.substr(value_and_index.second));

        if (unitsStr.empty())
        {
            out_units = defaultUnits;
            //return false;
        }
        else
        {
            UnitsType units;
            if (Units::parse(unitsStr, units))
                out_units = units;
            else if (unitsStr.back() != 's' && Units::parse(unitsStr + 's', units))
                out_units = units;
            else {
                //error!
                return false;
            }
        }

        return true;
    }
}

//------------------------------------------------------------------------

bool
Units::parse(const std::string& name, UnitsType& output)
{
    const UnitsType u = osgEarth::Registry::instance()->getUnits(name);

    if (u.valid())
    {
        output = u;
        return true;
    }
    return false;
}

bool
Units::parse(const std::string& input, float& out_value, UnitsType& out_units, const UnitsType& defaultUnits)
{
    return parseValueAndUnits(input, out_value, out_units, defaultUnits);
}

bool
Units::parse(const std::string& input, double& out_value, UnitsType& out_units, const UnitsType& defaultUnits)
{
    return parseValueAndUnits(input, out_value, out_units, defaultUnits);
}

void
Units::registerAll(Registry* r)
{
    r->registerUnits(Units::CENTIMETERS);
    r->registerUnits(Units::FEET);
    r->registerUnits(Units::FEET_US_SURVEY);
    r->registerUnits(Units::KILOMETERS);
    r->registerUnits(Units::METERS);
    r->registerUnits(Units::MILES);
    r->registerUnits(Units::MILLIMETERS);
    r->registerUnits(Units::YARDS);
    r->registerUnits(Units::NAUTICAL_MILES);
    r->registerUnits(Units::DATA_MILES);
    r->registerUnits(Units::INCHES);
    r->registerUnits(Units::FATHOMS);
    r->registerUnits(Units::KILOFEET);
    r->registerUnits(Units::KILOYARDS);

    r->registerUnits(Units::DEGREES);
    r->registerUnits(Units::RADIANS);
    r->registerUnits(Units::BAM);
    r->registerUnits(Units::NATO_MILS);
    r->registerUnits(Units::DECIMAL_HOURS);

    r->registerUnits(Units::DAYS);
    r->registerUnits(Units::HOURS);
    r->registerUnits(Units::MICROSECONDS);
    r->registerUnits(Units::MILLISECONDS);
    r->registerUnits(Units::MINUTES);
    r->registerUnits(Units::SECONDS);
    r->registerUnits(Units::WEEKS);

    r->registerUnits(Units::FEET_PER_SECOND);
    r->registerUnits(Units::YARDS_PER_SECOND);
    r->registerUnits(Units::METERS_PER_SECOND);
    r->registerUnits(Units::KILOMETERS_PER_SECOND);
    r->registerUnits(Units::KILOMETERS_PER_HOUR);
    r->registerUnits(Units::MILES_PER_HOUR);
    r->registerUnits(Units::DATA_MILES_PER_HOUR);
    r->registerUnits(Units::KNOTS);

    r->registerUnits(Units::PIXELS);
}

int
Units::unitTest()
{
    double value;
    UnitsType units;

    // test parsing scientific notation
    {
        Units::parse( "123e-003m", value, units, Units::MILES);
        if ( value != 123e-003 || units != Units::METERS )
            return 101;

        Units::parse( "123e+003m", value, units, Units::MILES );
        if ( value != 123e+003 || units != Units::METERS )
            return 102;

        Units::parse( "123E-003m", value, units, Units::MILES );
        if ( value != 123E-003 || units != Units::METERS )
            return 103;

        Units::parse( "123E+003m", value, units, Units::MILES );
        if ( value != 123E+003 || units != Units::METERS )
            return 104;
    }

    // normal parsing
    {
        Units::parse( "123m", value, units, Units::MILES );
        if ( value != 123 || units != Units::METERS )
            return 201;
        
        Units::parse( "123km", value, units, Units::MILES );
        if ( value != 123 || units != Units::KILOMETERS )
            return 202;
        
        Units::parse( "1.2rad", value, units, Units::DEGREES );
        if ( value != 1.2 || units != Units::RADIANS )
            return 203;
    }

    // add tests as needed

    return 0;
}
