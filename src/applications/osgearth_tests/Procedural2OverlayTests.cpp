/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarthProcedural2/FeatureOverlay>
#include <osgEarthProcedural2/Canopy>
#include <osgEarthProcedural2/VegetationLayer2>
#include <osgEarthProcedural2/OverlayPager.h>
#include <osgEarth/NodeUtils>
#include <osgEarth/FileUtils>
#include <osgEarth/JsonUtils>
#include <osgDB/FileUtils>
#include <fstream>
#include <thread>
#include <future>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

namespace
{
    //! Local metric fixture makes buffer widths and unchanged placement positions measurable without GL/network.
    osg::ref_ptr<const Profile> overlayProfile()
    { return Profile::create("epsg:32618",500000,4500000,500400,4500400,"",1,1); }

    //! Constructs a local rectangular authoring outline.
    osg::ref_ptr<Polygon> outline(double x, double y, double size)
    {
        auto geometry = new Polygon();
        geometry->push_back({x,y,0}); geometry->push_back({x+size,y,0});
        geometry->push_back({x+size,y+size,0}); geometry->push_back({x,y+size,0});
        return geometry;
    }

    //! Defines authoring vocabulary independently of coverage policy.
    std::vector<OverlayType> overlayTypes()
    {
        OverlayType clearing; clearing.name = "clearing"; clearing.attributes["vegetation:exclude"] = "yes";
        OverlayType path; path.name = "path"; path.geometry = "line"; path.attributes["highway"] = "path";
        return {clearing,path};
    }

    //! Supplies shared base/overlay rules, including a population-specific local clearing policy.
    std::vector<CoverageRule> overlayRules()
    {
        CoverageRule forest; forest.key = "natural"; forest.value = "wood";
        CoverageRule clear; clear.key = "vegetation:exclude"; clear.value = "yes"; clear.action = "exclude";
        clear.source = "edits"; clear.group = "trees";
        CoverageRule path; path.key = "highway"; path.value = "*"; path.action = "exclude"; path.buffer = 4.0;
        return {forest,clear,path};
    }

    //! Base source supplies forest and a road that removing a local edit must never erase.
    struct BaseOverlayFixture : PlacementFeatureProvider
    {
        //! Returns stable read-only fixture records for the exact metric test domain.
        Status query(const TileKey&, double, std::vector<PlacementFeature>& output, ProgressCallback*) const override
        {
            auto srs = overlayProfile()->getSRS();
            osg::ref_ptr<Feature> forest = new Feature(outline(500000,4500000,400),srs);
            forest->set("natural","wood");
            auto line = new LineString(); line->push_back({500050,4500000,0}); line->push_back({500050,4500400,0});
            osg::ref_ptr<Feature> road = new Feature(line,srs,Style(),2); road->set("highway","primary");
            output = {{"osm",forest,{}},{"osm",road,{}}}; return Status::NoError;
        }
    };

    //! Loads a CPU-only paging request and merges it through the real PagingManager.
    bool loadPage(Util::PagedNode2* node, Util::PagingManager* manager)
    {
        node->load();
        for (unsigned i = 0; i < 2000u && !node->isLoadComplete(); ++i)
        { manager->update(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        return node->isLoadComplete() && node->getNumChildren() > 0;
    }
}

//! Edits own geometry, stable IDs, snapshots and history; validation failures cannot publish partial changes.
TEST_CASE("Procedural2 overlay document is transactional and owns immutable snapshots", "[procedural2][overlay]")
{
    FeatureOverlay document(overlayTypes(),"edits"); ScatterGroup trees; trees.name = "trees";
    REQUIRE(document.validateCatalog(overlayRules(),{trees}).isOK());
    auto geometry = outline(500100,4500100,80); FeatureID id = 0;
    REQUIRE(document.put("clearing",geometry,overlayProfile()->getSRS(),id).isOK());
    REQUIRE(id == 1);
    auto first = document.snapshot(); const auto point = first->features.front()->getGeometry()->front();
    geometry->front().x() += 100;
    CHECK(first->features.front()->getGeometry()->front() == point);
    REQUIRE(document.put("clearing",outline(500200,4500200,30),overlayProfile()->getSRS(),id).isOK());
    CHECK(document.snapshot()->features.front()->getFID() == id);
    CHECK(first->features.front()->getGeometry()->front() == point);
    REQUIRE(document.undo().isOK()); CHECK(document.snapshot()->features.front() == first->features.front());
    REQUIRE(document.redo().isOK()); CHECK(document.snapshot()->revision == 4);
    auto current = document.snapshot();
    CHECK_FALSE(document.put("missing",geometry,overlayProfile()->getSRS(),id).isOK());
    CHECK_FALSE(document.erase(999).isOK()); CHECK(document.snapshot() == current);
    auto crossing = outline(500100,4500100,80); std::swap((*crossing)[1],(*crossing)[2]);
    CHECK_FALSE(document.put("clearing",crossing,overlayProfile()->getSRS(),id).isOK());
    CHECK(document.snapshot() == current);
    REQUIRE(document.erase(id).isOK()); CHECK(document.snapshot()->features.empty());
    REQUIRE(document.undo().isOK()); CHECK(document.snapshot()->features.size() == 1);
    FeatureID another = 0;
    REQUIRE(document.put("clearing",outline(500100,4500100,20),overlayProfile()->getSRS(),another).isOK());
    CHECK(another != id); CHECK_FALSE(document.redo().isOK());
    auto bad = *document.snapshot(); bad.features.push_back(bad.features.front());
    current = document.snapshot(); CHECK_FALSE(document.replace(bad).isOK()); CHECK(document.snapshot() == current);
}

//! Base and local features share exact coverage rules, preserve population filters and stable remaining placements.
TEST_CASE("Procedural2 overlays compose exclusions without changing surviving placements", "[procedural2][overlay]")
{
    auto profile = overlayProfile(); TileKey key(0,0,0,profile);
    FeatureOverlay document(overlayTypes(),"edits");
    auto base = std::make_shared<BaseOverlayFixture>();
    ScatterGroup trees; trees.name = "trees"; trees.cellLevel = trees.renderCellLevel = 1; trees.density = 100000;
    auto before = std::make_shared<OverlayFeatureProvider>(base,document.snapshot(),"edits");
    FeatureScatterSource original(before,overlayRules());
    std::shared_ptr<const PlacementField> initial;
    REQUIRE(original.queryField(key,trees,17,initial).isOK());
    CHECK(initial->sample({500150,4500150,0}).density == 1.0f);
    CHECK(initial->sample({500050,4500150,0}).excluded);
    FeatureID id = 0;
    REQUIRE(document.put("clearing",outline(500100,4500100,100),profile->getSRS(),id).isOK());
    auto line = new LineString(); line->push_back({500300,4500000,0}); line->push_back({500300,4500400,0});
    FeatureID pathID = 0; REQUIRE(document.put("path",line,profile->getSRS(),pathID).isOK());
    auto provider = std::make_shared<OverlayFeatureProvider>(base,document.snapshot(),"edits");
    FeatureScatterSource edited(provider,overlayRules()); std::shared_ptr<const PlacementField> field;
    REQUIRE(edited.queryField(key,trees,17,field).isOK());
    CHECK(field->sample({500150,4500150,0}).excluded);
    CHECK(field->sample({500303,4500250,0}).excluded);
    CHECK_FALSE(field->sample({500306,4500250,0}).excluded);
    CHECK(field->sample({500050,4500150,0}).excluded);
    auto grass = trees; grass.name = "grass";
    std::shared_ptr<const PlacementField> grassField;
    REQUIRE(edited.queryField(key,grass,17,grassField).isOK());
    CHECK_FALSE(grassField->sample({500150,4500150,0}).excluded);
    CHECK(grassField->sample({500303,4500250,0}).excluded);
    std::vector<ScatterPlacement> a,b;
    REQUIRE(original.generate(key.createChildKey(0),trees,17,a).isOK());
    REQUIRE(edited.generate(key.createChildKey(0),trees,17,b).isOK());
    REQUIRE(!b.empty());
    for (const auto& placement : b)
    {
        auto found = std::find_if(a.begin(),a.end(),[&](const ScatterPlacement& p) { return p.id == placement.id; });
        REQUIRE(found != a.end()); CHECK(found->point == placement.point);
    }
    REQUIRE(document.erase(id).isOK());
    FeatureScatterSource restored(std::make_shared<OverlayFeatureProvider>(base,document.snapshot(),"edits"),overlayRules());
    REQUIRE(restored.queryField(key,trees,17,field).isOK());
    CHECK_FALSE(field->sample({500150,4500150,0}).excluded);
    CHECK(field->sample({500050,4500150,0}).excluded);
    // A worker retaining an older snapshot still sees a complete older document, never partly applied edits.
    REQUIRE(edited.queryField(key,trees,17,field).isOK()); CHECK(field->sample({500150,4500150,0}).excluded);
    // Both aggregate tiers query that same composed coverage without enumerating detailed placements.
    auto canopy = trees; canopy.canopy = true; canopy.asset = "trees"; canopy.renderCellLevel = canopy.cellLevel = 3;
    const auto far = key.createChildKey(2), medium = far.createChildKey(1);
    const CanopyElevation flat = [](std::vector<osg::Vec3d>& points)
        { for (auto& p : points) p.z() = 0.0; return Status::NoError; };
    for (const auto& page : {far,medium})
    {
        REQUIRE(edited.queryField(page,canopy,17,field).isOK());
        std::vector<CanopyPatch> patches;
        REQUIRE(buildCanopy(page,canopy,*field,profile->getSRS(),flat,patches).isOK());
        for (const auto& patch : patches)
        for (unsigned crown = 0; crown < 9; ++crown) if (patch.mask & (1u<<crown))
        {
            const auto footprint = canopyPatchCrownFootprint(patch,crown);
            if (!footprint.isValid() || footprint.width() <= 0.0 || footprint.height() <= 0.0) continue;
            CoverageSample value;
            REQUIRE(field->uniform(footprint,value)); CHECK_FALSE(value.excluded);
        }
    }
}

//! Storage supports empty documents, replacement saves and full-width IDs without altering the live document on bad input.
TEST_CASE("Procedural2 overlay GeoJSON round trips and validates imports", "[procedural2][overlay]")
{
    const auto path = osgEarth::getTempName("osgearth-overlay-",".geojson");
    GeoJSONOverlayStorage storage; FeatureOverlay document(overlayTypes(),"edits"); OverlaySnapshot decoded;
    REQUIRE(storage.write(path,*document.snapshot()).isOK()); REQUIRE(storage.read(path,decoded).isOK());
    CHECK(decoded.features.empty());
    FeatureID id = 0; REQUIRE(document.put("clearing",outline(500100,4500100,80),overlayProfile()->getSRS(),id).isOK());
    osg::ref_ptr<Feature> wide = new Feature(*document.snapshot()->features.front());
    wide->setFID(9007199254740993LL); OverlaySnapshot large; large.features.push_back(wide);
    REQUIRE(document.replace(large).isOK());
    REQUIRE(storage.write(path,*document.snapshot()).isOK()); REQUIRE(storage.read(path,decoded).isOK());
    // Check the wire format independently of osgEarth's permissive geometry reader.
    Util::Json::Value json; Util::Json::Reader reader; std::ifstream saved(path);
    REQUIRE(reader.parse(saved,json)); saved.close();
    const auto& ring = json["features"][0u]["geometry"]["coordinates"][0u];
    REQUIRE(ring.size() == 5u); CHECK((ring[0u] == ring[4u]));
    CHECK(document.snapshot()->features.front()->getGeometry()->size() == 4u);
    REQUIRE(decoded.features.size() == 1); CHECK(decoded.features.front()->getFID() == wide->getFID());
    REQUIRE(document.replace(decoded).isOK());
    auto original = document.snapshot();
    osg::ref_ptr<Feature> wrong = new Feature(*decoded.features.front()); wrong->set("highway","primary");
    decoded.features[0] = wrong;
    CHECK_FALSE(document.replace(decoded).isOK()); CHECK(document.snapshot() == original);
    { std::ofstream bad(path); bad << "{not geojson}"; }
    CHECK_FALSE(storage.read(path,decoded).isOK()); CHECK(document.snapshot() == original);
    CHECK(decoded.features.front() == wrong);
    { std::ofstream bad(path); bad << "{\"type\":[],\"features\":false}"; }
    CHECK_FALSE(storage.read(path,decoded).isOK());
    std::remove(path.c_str());
}

//! A real worker held across invalidation cannot publish stale coverage when the same branch is re-requested.
TEST_CASE("Procedural2 overlay edits reject in-flight stale pages", "[procedural2][overlay]")
{
    auto profile = overlayProfile(); osg::ref_ptr<Map> map = new Map();
    osg::ref_ptr<OverlayPager> pager = new OverlayPager(map,profile);
    pager->setMinLevel(1); pager->setMaxLevel(2);
    std::promise<void> started, release, finished;
    auto began = started.get_future(); auto gate = release.get_future().share(); auto ended = finished.get_future();
    std::atomic<unsigned> calls{0};
    pager->setCreateNodeFunction([&](const TileKey&,ProgressCallback*) -> osg::ref_ptr<osg::Node>
    {
        const bool stale = calls.fetch_add(1) == 0;
        if (stale) { started.set_value(); gate.wait(); }
        auto node = new osg::Group(); node->setUserValue("stale",stale);
        if (stale) finished.set_value();
        return node;
    });
    pager->setConfigurePagedNodeFunction([](const TileKey&,Util::PagedNode2* node) { node->setPreCompileGLObjects(false); });
    pager->build();
    osg::ref_ptr<Util::PagingManager> manager = new Util::PagingManager("overlay-stale-tests"); manager->addChild(pager);
    osg::NodeVisitor initialize(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN); manager->accept(initialize);
    auto root = dynamic_cast<Util::PagedNode2*>(pager->getChild(0)->asGroup()->getChild(0)); REQUIRE(root);
    root->load();
    const bool beganInTime = began.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!beganInTime) release.set_value();
    REQUIRE(beganInTime);
    CHECK(pager->invalidate({GeoExtent(profile->getSRS(),500010,4500310,500020,4500320)}) == 1u);
    root->load(); release.set_value();
    REQUIRE(ended.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(loadPage(root,manager));
    unsigned stale = 0;
    forEachNodeOfType<osg::Group>(root,[&](osg::Group* group)
    { bool value = false; if (group->getUserValue("stale",value) && value) ++stale; });
    CHECK(stale == 0);
    pager->setDone();
}

//! Real paging merges prove a local edit evicts all aggregate/detail descendants but retains another loaded region.
TEST_CASE("Procedural2 overlay invalidation retains unrelated SimplePager branches", "[procedural2][overlay]")
{
    auto profile = overlayProfile(); osg::ref_ptr<Map> map = new Map();
    osg::ref_ptr<OverlayPager> pager = new OverlayPager(map,profile);
    pager->setMinLevel(2); pager->setMaxLevel(4);
    pager->setCreateNodeFunction([](const TileKey&,ProgressCallback*) -> osg::ref_ptr<osg::Node>
        { return new osg::Group(); });
    pager->setConfigurePagedNodeFunction([](const TileKey&,Util::PagedNode2* node) { node->setPreCompileGLObjects(false); });
    pager->build();
    osg::ref_ptr<Util::PagingManager> manager = new Util::PagingManager("overlay-tests"); manager->addChild(pager);
    osg::NodeVisitor initialize(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN); manager->accept(initialize);
    auto root = dynamic_cast<Util::PagedNode2*>(pager->getChild(0)->asGroup()->getChild(0)); REQUIRE(root);
    REQUIRE(loadPage(root,manager)); manager->accept(initialize);
    std::vector<Util::PagedNode2*> children;
    forEachNodeOfType<Util::PagedNode2>(root,[&](Util::PagedNode2* node) { if (node != root) children.push_back(node); });
    REQUIRE(children.size() == 4);
    for (auto child : children) REQUIRE(loadPage(child,manager));
    manager->accept(initialize);
    auto untouched = children[3]->getChild(0);
    GeoExtent edit(profile->getSRS(),500010,4500310,500020,4500320);
    CHECK(pager->invalidate({edit}) == 1u);
    CHECK(children[0]->getNumChildren() == 0u);
    CHECK(children[3]->getChild(0) == untouched);
    REQUIRE(loadPage(children[0],manager));
    CHECK(children[3]->getChild(0) == untouched);
}

//! Catalog validation rejects ineffective tools and serialization preserves the extension definitions.
TEST_CASE("Procedural2 overlay catalog is configured independently of storage", "[procedural2][overlay]")
{
    ScatterGroup group; group.name = "trees";
    auto types = overlayTypes(); FeatureOverlay document(types,"edits");
    CHECK(document.validateCatalog(overlayRules(),{group}).isOK());
    CHECK_FALSE(document.validateCatalog({}, {group}).isOK());
    types[1].geometry = "point";
    CHECK_FALSE(FeatureOverlay(types,"edits").validateCatalog(overlayRules(),{group}).isOK());
    VegetationLayer2::Options options; options.overlayTypes() = overlayTypes(); options.overlaySource() = "local";
    VegetationLayer2::Options decoded(options.getConfig());
    REQUIRE(decoded.overlayTypes().size() == 2);
    CHECK(decoded.overlayTypes()[1].attributes.at("highway") == "path");
    CHECK(decoded.overlaySource().get() == "local");
}
