/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/LineSymbol>
#include <osgEarth/Style>
#include <limits>

using namespace osgEarth;

OSGEARTH_REGISTER_SIMPLE_SYMBOL(line, LineSymbol);

LineSymbol::LineSymbol(const Config& conf) :
    Symbol(conf),
    _stroke(Stroke()),
    _tessellation(0),
    _creaseAngle(0.0f),
    _useGLLines(false),
    _useWireLines(false),
    _doubleSided(false)
{
    mergeConfig(conf);
}

LineSymbol::LineSymbol(const LineSymbol& rhs, const osg::CopyOp& copyop) :
    Symbol(rhs, copyop),
    _stroke(rhs._stroke),
    _material(rhs._material),
    _outlineMaterial(rhs._outlineMaterial),
    _materialPeriod(rhs._materialPeriod),
    _tessellation(rhs._tessellation),
    _creaseAngle(rhs._creaseAngle),
    _tessellationSize(rhs._tessellationSize),
    _imageURI(rhs._imageURI),
    _imageLength(rhs._imageLength),
    _useGLLines(rhs._useGLLines),
    _useWireLines(rhs._useWireLines),
    _doubleSided(rhs._doubleSided)
{
    //nop
}

Config 
LineSymbol::getConfig() const
{
    Config conf = Symbol::getConfig();
    conf.key() = "line";
    conf.set("stroke",       _stroke);
    conf.set("material", material());
    conf.set("outline_material", outlineMaterial());
    conf.set("material_period", materialPeriod());
    conf.set("tessellation", _tessellation);
    conf.set("crease_angle", _creaseAngle);
    conf.set("tessellation_size", _tessellationSize );
    conf.set("image", _imageURI);
    conf.set("image_length", imageLength());
    conf.set("use_gl_lines", _useGLLines);
    conf.set("use_wire_lines", _useWireLines);
    conf.set("double_sided", doubleSided());
    return conf;
}

void 
LineSymbol::mergeConfig( const Config& conf )
{
    conf.get("stroke",       _stroke);
    conf.get("material", material());
    conf.get("outline_material", outlineMaterial());
    conf.get("material_period", materialPeriod());
    conf.get("tessellation", _tessellation);
    conf.get("crease_angle", _creaseAngle);
    conf.get("tessellation_size", _tessellationSize);
    conf.get("image", _imageURI);
    conf.get("image_length", imageLength());
    conf.get("use_gl_lines", _useGLLines);
    conf.get("use_wire_lines", _useWireLines);
    conf.get("double_sided", _doubleSided);
}

void
LineSymbol::parseSLD(const Config& c, Style& style)
{
    if (match(c.key(), "library")) {
        if (!c.value().empty())
            style.getOrCreate<SkinSymbol>()->library() = Strings::unquote(c.value());
    }
    else
    if ( match(c.key(), "stroke") ) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().color() = Color(c.value());
    }
    else if ( match(c.key(), "stroke-opacity") ) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().color().a() = as<float>( c.value(), 1.0f );
    }
    else if (match(c.key(), "stroke-width")) {
        style.getOrCreate<LineSymbol>()->stroke()->width() = c.value();
        style.getOrCreate<LineSymbol>()->stroke()->width()->setDefaultUnits(Units::PIXELS);
    }
    else if (match(c.key(), "stroke-width-units")) {
        UnitsType units;
        if (Units::parse(c.value(), units))
            style.getOrCreate<LineSymbol>()->stroke()->width()->setDefaultUnits(units);
    }
    else if ( match(c.key(), "stroke-linecap") ) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().lineCap() =
            c.value() == "flat"   ?   Stroke::LINECAP_FLAT   :
            c.value() == "square" ?   Stroke::LINECAP_SQUARE :
            /*value == "round"   ?*/  Stroke::LINECAP_ROUND;
    }
    else if (match(c.key(), "stroke-linejoin") ) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().lineJoin() =
            c.value() == "mitre" ?      Stroke::LINEJOIN_MITRE :
            c.value() == "miter" ?      Stroke::LINEJOIN_MITRE : // alternate spelling
            /*c.value() == "round"  ?*/ Stroke::LINEJOIN_ROUND;
    }
    else if ( match(c.key(), "stroke-rounding-ratio") ) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().roundingRatio() = as<float>(c.value(), 0.4f);
    }
    else if ( match(c.key(), "stroke-tessellation-segments") ) {
        style.getOrCreate<LineSymbol>()->tessellation() = as<unsigned>( c.value(), 0 );
    }
    else if ( match(c.key(), "stroke-tessellation-size") ) {
        float value;
        UnitsType units;
        if ( Units::parse(c.value(), value, units, Units::METERS) ) {
            style.getOrCreate<LineSymbol>()->tessellationSize() = Distance(value, units);
        }
    }        
    else if ( match(c.key(), "stroke-min-pixels") ) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().minPixels() = as<float>(c.value(), 0.0f);
    }
    else if ( match(c.key(), "stroke-stipple-factor") ) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().stippleFactor() = as<std::uint32_t>(c.value(), 1);
    }
    else if ( match(c.key(), "stroke-stipple-pattern") ||
              match(c.key(), "stroke-stipple") ) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().stipplePattern() = as<std::uint16_t>(c.value(), 0xFFFF);
    }
    else if ( match(c.key(), "stroke-crease-angle") ) {
        style.getOrCreate<LineSymbol>()->creaseAngle() = as<float>(c.value(), 0.0);
    }
    else if ( match(c.key(), "stroke-script") ) {
        style.getOrCreate<LineSymbol>()->script() = c.value();
    }
    else if (match(c.key(), "stroke-image")) {
        style.getOrCreate<LineSymbol>()->imageURI() = URI(Strings::unquote(c.value()), c.referrer());
    }
    else if (match(c.key(), "stroke-image-length")) {
        style.getOrCreate<LineSymbol>()->imageLength() = as<float>(c.value(), 0.0f);
    }
    else if (match(c.key(), "stroke-smooth")) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().smooth() = as<bool>(c.value(), false);
    }
    else if (match(c.key(), "stroke-gl-lines")) {
        style.getOrCreate<LineSymbol>()->useGLLines() = as<bool>(c.value(), false);
    }
    else if (match(c.key(), "stroke-wire-lines")) {
        style.getOrCreate<LineSymbol>()->useWireLines() = as<bool>(c.value(), false);
    }
    else if (match(c.key(), "stroke-outline")) {
        style.getOrCreate<LineSymbol>()->stroke().mutable_value().outlineColor() = Color(c.value());
    }
    else if (match(c.key(), "stroke-outline-width")) {
        style.getOrCreate<LineSymbol>()->stroke()->outlineWidth() = c.value();
        style.getOrCreate<LineSymbol>()->stroke()->outlineWidth()->setDefaultUnits(Units::PIXELS);
    }
    else if (match(c.key(), "stroke-material")) {
        style.getOrCreate<LineSymbol>()->material() = Strings::unquote(c.value());
    }
    else if (match(c.key(), "stroke-outline-material")) {
        style.getOrCreate<LineSymbol>()->outlineMaterial() = Strings::unquote(c.value());
    }
    else if (match(c.key(), "stroke-material-period")) {
        float value; UnitsType units;
        if (Units::parse(c.value(), value, units, Units::METERS) && units.canConvert(Units::METERS))
            style.getOrCreate<LineSymbol>()->materialPeriod() = float(units.convertTo(Units::METERS, value));
        else
            style.getOrCreate<LineSymbol>()->materialPeriod() = std::numeric_limits<float>::quiet_NaN();
    }
    else if (match(c.key(), "stroke-double-sided")) {
        style.getOrCreate<LineSymbol>()->doubleSided() = as<bool>(c.value(), false);
    }
}
