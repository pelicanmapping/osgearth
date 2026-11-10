/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include <osgEarthPrestige/LabelLayer>
#include <osgEarth/Map>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <mutex>
#include <osgEarth/CssUtils>
#include <osgEarth/Color>
#ifdef OSGEARTH_HAVE_SLUGHORN
#include "LabelTilesInternal.h"
#include "LabelStyleInternal.h"
#include "LabelSceneInternal.h"
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osg/Camera>
#include <osg/View>
#include <osgGA/EventVisitor>
#include <osgGA/GUIActionAdapter>
#include <osgText/Font>
#include <osgUtil/CullVisitor>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>
#include <cstdlib>
#endif

namespace osgEarthPrestige
{
using namespace osgEarth;
REGISTER_OSGEARTH_LAYER_FACTORY("prestige:labels", LabelLayer);

#ifdef OSGEARTH_HAVE_SLUGHORN
using namespace Labeling;
namespace
{
    //! Resolves an explicit relative/local font, or a platform font; performs no glyph work or GL calls.
    std::string labelFont(const Config& conf, const osgDB::Options* readOptions)
    {
        const auto requested = conf.value("font");
        if (!requested.empty())
        {
            auto path = osgDB::findDataFile(URI(requested, URIContext(conf.referrer())).full(), readOptions);
            if (path.empty())
            {
                path = osgText::findFontFile(requested);
                // OSG's font locator can silently return its default font for a missing filename.
                if (!ci_equals(osgDB::getSimpleFileName(path), osgDB::getSimpleFileName(requested))) path.clear();
            }
            if (path.empty() || !osgDB::fileExists(path))
                throw std::invalid_argument("Cannot resolve Prestige label font: " + requested);
            return path;
        }
#ifdef _WIN32
        const char* windows = std::getenv("WINDIR");
        const auto systemFont = std::string(windows ? windows : "C:/Windows") + "/Fonts/segoeui.ttf";
        if (osgDB::fileExists(systemFont)) return systemFont;
#endif
        for (const char* candidate : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf"})
            if (osgDB::fileExists(candidate)) return candidate;
        for (const char* candidate : {"DejaVuSans.ttf", "LiberationSans-Regular.ttf", "Arial.ttf"})
        {
            const auto path = osgText::findFontFile(candidate);
            if (!path.empty() && osgDB::fileExists(path) &&
                ci_equals(osgDB::getSimpleFileName(path), candidate)) return path;
        }
        throw std::invalid_argument("Set <font> to a local TrueType/OpenType font for Prestige labels");
    }

    //! Routes only primary view cameras; shadow/pick/RTT traversals do not acquire independent placements.
    //! Registration/reclamation happens on construction/update, never in cull; each slot has its own ViewNode.
    class LabelCameras : public osg::Group
    {
        struct Slot
        {
            osg::observer_ptr<osg::Camera> camera;
            osg::ref_ptr<ViewNode> node;
            bool bound = false;
            std::atomic<bool> culling {false};
        };
        std::shared_ptr<Engine> _engine;
        std::shared_ptr<TileSource> _tiles;
        std::shared_ptr<SceneSource> _scene;
        std::array<Slot, 8> _slots;
        std::mutex _mutex;
        float _density;
        double _padding;
    public:
        //! Preallocates eight bounded camera registrations on the map/control thread; capacity failure throws.
        LabelCameras(std::shared_ptr<TileSource> tiles, std::shared_ptr<SceneSource> scene,
            float density, double padding) :
            _tiles(std::move(tiles)), _scene(std::move(scene)), _density(density), _padding(padding)
        {
            Limits limits; limits.inputBytes = 32 * 1024 * 1024;
            limits.declutter.padding = padding;
            _engine = std::make_shared<Engine>(limits, Solver(), prepareText());
            for (auto& slot : _slots)
                slot.node = new ViewNode(_engine, {}, _density, true, _tiles, {}, _scene);
            setCullingActive(false);
            setNumChildrenRequiringUpdateTraversal(1);
            setNumChildrenRequiringEventTraversal(1);
        }

        //! Posts padding edits to existing camera nodes without touching their cull-owned history.
        void setDeclutterPadding(double padding)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _padding = padding;
            for (auto& slot : _slots) if (slot.node) slot.node->setDeclutterPadding(padding);
        }

        //! Reclaims dead-camera histories on update; event traversal keeps on-demand viewers alive during work/fades.
        //! Cull uses nonblocking routing and rejects simultaneous reentry of one camera's placement state.
        void traverse(osg::NodeVisitor& visitor) override
        {
            if (visitor.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
            {
                std::lock_guard<std::mutex> lock(_mutex);
                for (auto& slot : _slots)
                {
                    if (slot.bound && !slot.camera.valid() && !slot.culling.load())
                    {
                        slot.node = nullptr; slot.bound = false;
                    }
                    if (!slot.node)
                    {
                        // Engine retires closed slots on its worker. Retry next update if it has not retired yet.
                        try
                        {
                            slot.node = new ViewNode(_engine, {}, _density, true, _tiles, {}, _scene);
                            slot.node->setDeclutterPadding(_padding);
                        }
                        catch (const std::runtime_error&) { }
                    }
                }
            }
            else if (visitor.getVisitorType() == osg::NodeVisitor::EVENT_VISITOR)
            {
                auto* event = dynamic_cast<osgGA::EventVisitor*>(&visitor);
                std::lock_guard<std::mutex> lock(_mutex);
                if (event && event->getActionAdapter())
                {
                    if (_scene->needsFrame()) event->getActionAdapter()->requestRedraw();
                    for (const auto& slot : _slots)
                        if (!slot.node || (slot.bound && slot.node->needsFrame()))
                        { event->getActionAdapter()->requestRedraw(); break; }
                }
            }
            else if (auto* cv = dynamic_cast<osgUtil::CullVisitor*>(&visitor))
            {
                auto* camera = cv->getCurrentCamera();
                if (!camera || !camera->getView() || camera->getView()->getCamera() != camera) return;
                Slot* selected = nullptr;
                osg::ref_ptr<ViewNode> node;
                {
                    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
                    if (!lock) return;
                    for (auto& slot : _slots)
                        if (slot.bound && slot.camera == camera) { selected = &slot; break; }
                    if (!selected)
                        for (auto& slot : _slots)
                            if (!slot.bound && slot.node)
                            { slot.camera = camera; slot.bound = true; selected = &slot; break; }
                    if (!selected || selected->culling.exchange(true)) return;
                    node = selected->node;
                }
                struct Retire
                {
                    std::atomic<bool>& flag;
                    //! Releases the camera's cull claim even if traversal unwinds through an exception.
                    ~Retire() { flag.store(false); }
                } retire {selected->culling};
                node->setDevicePixelRatio(_density * Registry::instance()->getDevicePixelRatio(camera->getGraphicsContext()));
                node->accept(visitor);
            }
        }
    protected:
        //! Retires views before joining their shared workers; destroy only after traversal has stopped.
        ~LabelCameras() override
        {
            const auto stats = _tiles ? _tiles->stats() : TileStats();
            OE_INFO << "[PrestigeLabels] tiles=" << stats.loads << " failures=" << stats.failures
                << " skipped=" << stats.skipped << " truncated=" << stats.truncated
                << " resident_bytes=" << stats.residentBytes << " error=" << stats.error << std::endl;
            for (auto& slot : _slots)
            {
                if (slot.bound && slot.node)
                    OE_INFO << "[PrestigeLabels] view=" << slot.node->viewKey().slot
                        << " accepted=" << slot.node->layoutStats().accepted
                        << " submissions=" << slot.node->submissions() << std::endl;
                slot.node = nullptr;
            }
        }
    };
}

#endif

struct LabelLayer::Impl
{
    osg::ref_ptr<osg::Group> root = new osg::Group;
    mutable std::mutex styleMutex;
#ifdef OSGEARTH_HAVE_SLUGHORN
    std::shared_ptr<TileSource> tiles;
    std::string font;
    float density = 1.0f;
    double declutterPadding = 2.0;
    TileOptions tileOptions;
    std::vector<TextStyle> styles;
#endif
};

Config LabelLayer::StyleValues::getConfig(const Config& base) const
{
    Config result = base;
    result.key() = "style";
    if (!trim(base.value()).empty() || base.value("type") == "text/css")
    {
        ConfigSet blocks;
        CssUtils::readConfig(base.value(), base.referrer(), blocks);
        if (blocks.empty()) blocks.emplace_back("default");
        auto& text = blocks.front();
        text.set("text-size", size);
        text.set("text-fill", Color(fill).toHTML());
        text.set("text-halo", Color(halo).toHTML());
        text.set("text-halo-width", haloWidth);
        text.set("text-halo-softness", haloSoftness);
        text.set("text-halo-backdrop-type", "outline");
        std::ostringstream css;
        for (const auto& block : blocks)
        {
            css << block.key() << " {\n";
            for (const auto& property : block.children())
                css << "    " << property.key() << ": " << property.value() << ";\n";
            css << "}\n";
        }
        result.setValue(css.str());
    }
    else
    {
        Config symbols = base.child("symbols"); symbols.key() = "symbols";
        Config text = symbols.child("text"); text.key() = "text";
        Config fillConfig = text.child("fill"), haloConfig = text.child("halo");
        fillConfig.key() = "fill"; haloConfig.key() = "halo";
        fillConfig.set("color", Color(fill).toHTML()); haloConfig.set("color", Color(halo).toHTML());
        text.set(fillConfig); text.set(haloConfig); text.set("size", size);
        text.set("halo_width", haloWidth); text.set("halo_softness", haloSoftness);
        text.set("halo_backdrop_type", "outline");
        symbols.set(text); result.set(symbols);
    }
    return result;
}

std::vector<LabelLayer::StyleRule> LabelLayer::getStyles() const
{
    std::lock_guard<std::mutex> lock(_impl->styleMutex);
    return options().styles();
}

Status LabelLayer::resolveStyle(const Config& style, StyleValues& output)
{
#ifndef OSGEARTH_HAVE_SLUGHORN
    return Status(Status::ServiceUnavailable, "LabelLayer requires OSGEARTH_ENABLE_SLUGHORN");
#else
    try
    {
        const auto parsed = readTextStyle(style);
        StyleValues result;
        result.size = parsed.pixels; result.fill = parsed.color; result.halo = parsed.halo;
        result.haloWidth = parsed.haloWidth; result.haloSoftness = parsed.haloSoftness;
        output = result;
        return Status::OK();
    }
    catch (const std::exception& e) { return Status(Status::ConfigurationError, e.what()); }
#endif
}

double LabelLayer::getDeclutterPadding() const
{
    std::lock_guard<std::mutex> lock(_impl->styleMutex);
    return options().declutterPadding().get();
}

Status LabelLayer::setDeclutterPadding(double padding)
{
    if (!std::isfinite(padding) || padding < 0)
        return Status(Status::ConfigurationError, "Prestige declutter_padding must be nonnegative and finite");
    std::lock_guard<std::mutex> lock(_impl->styleMutex);
#ifdef OSGEARTH_HAVE_SLUGHORN
    if (_impl->root->getNumChildren() > 0)
        if (auto* cameras = dynamic_cast<LabelCameras*>(_impl->root->getChild(0))) cameras->setDeclutterPadding(padding);
    _impl->declutterPadding = padding;
#endif
    options().declutterPadding() = padding;
    return Status::OK();
}

Status LabelLayer::setStyle(unsigned rule, const Config& style)
{
#ifndef OSGEARTH_HAVE_SLUGHORN
    return Status(Status::ServiceUnavailable, "LabelLayer requires OSGEARTH_ENABLE_SLUGHORN");
#else
    const auto rules = getStyles();
    if (rule >= rules.size()) return Status(Status::ConfigurationError, "Label style rule index is out of range");
    return setStyle(rule, style, rules[rule].minLevel, rules[rule].maxLevel);
#endif
}

Status LabelLayer::setStyle(unsigned rule, const Config& style, unsigned minLevel, unsigned maxLevel)
{
#ifndef OSGEARTH_HAVE_SLUGHORN
    return Status(Status::ServiceUnavailable, "LabelLayer requires OSGEARTH_ENABLE_SLUGHORN");
#else
    try
    {
        std::lock_guard<std::mutex> lock(_impl->styleMutex);
        if (rule >= options().styles().size())
            return Status(Status::ConfigurationError, "Label style rule index is out of range");
        if (minLevel > maxLevel || maxLevel > 16)
            return Status(Status::ConfigurationError, "Label rule levels must satisfy 0 <= min <= max <= 16");
        auto parsed = readTextStyle(style);
        if (options().styles()[rule].placement == "callout") parsed.calloutGap = 12.0f;
        if (_impl->tiles)
        {
            auto next = _impl->styles;
            next[rule] = parsed;
            auto rules = _impl->tileOptions.rules;
            rules[rule].minLevel = minLevel; rules[rule].maxLevel = maxLevel;
            if (!_impl->tiles->setStyles(next, rules))
                return Status(Status::ResourceUnavailable, "Label style worker is unavailable");
            _impl->styles = std::move(next);
            _impl->tileOptions.rules = std::move(rules);
        }
        else if (rule < _impl->styles.size()) _impl->styles[rule] = parsed;
        if (rule < _impl->tileOptions.rules.size())
        {
            _impl->tileOptions.rules[rule].minLevel = minLevel;
            _impl->tileOptions.rules[rule].maxLevel = maxLevel;
        }
        options().styles()[rule].style = style;
        options().styles()[rule].minLevel = minLevel; options().styles()[rule].maxLevel = maxLevel;
        return Status::OK();
    }
    catch (const std::exception& e) { return Status(Status::ConfigurationError, e.what()); }
#endif
}

LabelLayer::StyleRule::StyleRule(const Config& conf) : style(conf)
{
    auto values = conf.value("classes");
    std::replace(values.begin(), values.end(), ',', ' ');
    std::istringstream input(values);
    for (std::string value; input >> value;) classes.push_back(value);
    minLevel = conf.value<unsigned>("min_level", 0);
    maxLevel = conf.value<unsigned>("max_level", 16);
    priority = conf.value<double>("priority", 0);
    placement = conf.value<std::string>("placement", "point");
}

Config LabelLayer::StyleRule::getConfig() const
{
    Config conf = style;
    conf.key() = "style";
    std::string values;
    for (const auto& category : classes)
    {
        if (!values.empty()) values += ',';
        values += category;
    }
    conf.set("classes", values);
    conf.set("min_level", minLevel);
    conf.set("max_level", maxLevel);
    conf.set("priority", priority);
    conf.set("placement", placement);
    return conf;
}

Config LabelLayer::Options::getConfig() const
{
    Config conf = VisibleLayer::Options::getConfig();
    conf.remove("features");
    featureSource().set(conf, "features");
    conf.set("font", font());
    conf.set("pixel_scale", pixelScale());
    conf.set("declutter_padding", declutterPadding());
    conf.set("text_attribute", textAttribute());
    conf.set("fallback_text_attribute", fallbackTextAttribute());
    conf.set("rank_attribute", rankAttribute());
    conf.set("class_attribute", classAttribute());
    conf.set("max_level", maxLevel());
    conf.set("tile_pixels", tilePixels());
    conf.set("gpu_paths", gpuPaths());
    Config rules("styles");
    for (const auto& rule : styles()) rules.add(rule.getConfig());
    conf.set(rules);
    return conf;
}

void LabelLayer::Options::fromConfig(const Config& conf)
{
    featureSource().get(conf, "features");
    conf.get("font", font());
    conf.get("pixel_scale", pixelScale());
    conf.get("declutter_padding", declutterPadding());
    conf.get("text_attribute", textAttribute());
    conf.get("fallback_text_attribute", fallbackTextAttribute());
    conf.get("rank_attribute", rankAttribute());
    conf.get("class_attribute", classAttribute());
    conf.get("max_level", maxLevel());
    conf.get("tile_pixels", tilePixels());
    conf.get("gpu_paths", gpuPaths());
    if (conf.hasChild("styles"))
    {
        styles().clear();
        for (const auto& rule : conf.child("styles").children("style")) styles().emplace_back(rule);
    }
}

void LabelLayer::setFeatureSource(FeatureSource* source)
{
    options().featureSource() = LayerReference<FeatureSource>();
    options().featureSource().setLayer(source);
}

FeatureSource* LabelLayer::getFeatureSource() const { return options().featureSource().getLayer(); }
osg::Node* LabelLayer::getNode() const { return _impl->root.get(); }
LabelLayer::~LabelLayer() = default;

void LabelLayer::init()
{
    VisibleLayer::init();
    _impl = std::make_shared<Impl>();
#ifdef OSGEARTH_HAVE_SLUGHORN
    _scene = std::make_shared<SceneSource>();
#endif
    _impl->root->setCullingActive(false);
}

Status LabelLayer::openImplementation()
{
    OE_RETURN_STATUS_ON_ERROR(VisibleLayer::openImplementation());
#ifndef OSGEARTH_HAVE_SLUGHORN
    return Status(Status::ServiceUnavailable, "LabelLayer requires OSGEARTH_ENABLE_SLUGHORN");
#else
    if (!Registry::capabilities().supportsGLSL(330u))
        return Status(Status::ServiceUnavailable, "Prestige labels require GLSL 3.30");
    const auto& o = options();
    try
    {
        if (!std::isfinite(o.pixelScale().get()) || o.pixelScale().get() <= 0)
            throw std::invalid_argument("Prestige pixel_scale must be positive and finite");
        if (!std::isfinite(o.declutterPadding().get()) || o.declutterPadding().get() < 0)
            throw std::invalid_argument("Prestige declutter_padding must be nonnegative and finite");
        if (o.maxLevel().get() > 16 || !std::isfinite(o.tilePixels().get()) || o.tilePixels().get() <= 0)
            throw std::invalid_argument("Prestige max_level must be 0-16 and tile_pixels must be positive and finite");
        _impl->font = labelFont(o.getConfig(), getReadOptions());
        _impl->density = o.pixelScale().get();
        _impl->declutterPadding = o.declutterPadding().get();
        _impl->tileOptions = TileOptions();
        auto& tiles = _impl->tileOptions;
        tiles.sourceID = getUID();
        tiles.textAttribute = o.textAttribute().get();
        tiles.fallbackTextAttribute = o.fallbackTextAttribute().get();
        tiles.priorityAttribute = o.rankAttribute().get();
        tiles.classAttribute = o.classAttribute().get();
        tiles.maxLevel = o.maxLevel().get();
        tiles.tilePixels = o.tilePixels().get();
        tiles.requireFeatureID = true;
        // Resolve once on open; applications using older GL contexts can force worker fitting explicitly.
        tiles.gpuPaths = o.gpuPaths().get() && Registry::capabilities().supportsGLSL(430u);
        _impl->styles.clear();
        for (const auto& configured : o.styles())
        {
            TileStyleRule rule;
            rule.classes = configured.classes;
            rule.minLevel = configured.minLevel;
            rule.maxLevel = configured.maxLevel;
            rule.priority = configured.priority;
            rule.style = static_cast<unsigned>(_impl->styles.size());
            const auto& placement = configured.placement;
            if (placement != "point" && placement != "line" && placement != "callout")
                throw std::invalid_argument("Prestige placement must be point, line, or callout");
            rule.line = placement == "line";
            auto style = readTextStyle(configured.style);
            if (placement == "callout") style.calloutGap = 12.0f;
            if (rule.classes.empty() || rule.minLevel > rule.maxLevel || rule.maxLevel > 16 ||
                !std::isfinite(rule.priority)) throw std::invalid_argument("Invalid Prestige label style rule");
            for (const auto& category : rule.classes)
                if (category.empty() || category.find_first_of(", \t\r\n") != std::string::npos)
                    throw std::invalid_argument("Prestige label classes must be nonempty tokens");
            tiles.rules.push_back(rule);
            _impl->styles.push_back(style);
        }
        if (o.featureSource().isSet() && _impl->styles.empty())
            throw std::invalid_argument("Prestige labels require class-based <styles>");
    }
    catch (const std::exception& e) { return Status(Status::ConfigurationError, e.what()); }
    return o.featureSource().isSet() ? options().featureSource().open(getReadOptions()) : Status::OK();
#endif
}

void LabelLayer::addedToMap(const Map* map)
{
    VisibleLayer::addedToMap(map);
    if (getStatus().isError()) return;
    options().featureSource().addedToMap(map);
#ifdef OSGEARTH_HAVE_SLUGHORN
    auto* source = getFeatureSource();
    if (options().featureSource().isSet() && (!source || !source->isOpen()))
    { setStatus(Status(Status::ResourceUnavailable, "Prestige label source is missing or failed to open")); return; }
    if (!map->getSRS()->isGeographic())
    { setStatus(Status(Status::ConfigurationError, "Prestige labels currently require a geocentric map")); return; }
    try
    {
        std::shared_ptr<TileSource> tiles;
        if (source) tiles = std::make_shared<TileSource>(source, _impl->font, _impl->styles, _impl->tileOptions);
        _impl->root->removeChildren(0, _impl->root->getNumChildren());
        _scene->open(_impl->font, (std::uint64_t(1) << 63) | std::uint64_t(getUID()));
        _impl->root->addChild(new LabelCameras(tiles, _scene, _impl->density, _impl->declutterPadding));
        _impl->tiles = std::move(tiles);
        OE_INFO << "[PrestigeLabels] " << getName() << " | source=" << (source ? source->getName() : "LabelNodes")
            << " | font=" << _impl->font << " | gpu_paths=" << _impl->tileOptions.gpuPaths << std::endl;
    }
    catch (const std::exception& e) { setStatus(Status(Status::ConfigurationError, e.what())); }
#endif
}

void LabelLayer::removedFromMap(const Map* map)
{
    _impl->root->removeChildren(0, _impl->root->getNumChildren());
#ifdef OSGEARTH_HAVE_SLUGHORN
    _impl->tiles.reset();
    _scene->close();
#endif
    options().featureSource().removedFromMap(map);
    VisibleLayer::removedFromMap(map);
}

Status LabelLayer::closeImplementation()
{
    _impl->root->removeChildren(0, _impl->root->getNumChildren());
#ifdef OSGEARTH_HAVE_SLUGHORN
    _impl->tiles.reset();
    _scene->close();
#endif
    if (!options().featureSource().isSetByUser()) options().featureSource().close();
    return VisibleLayer::closeImplementation();
}
}
