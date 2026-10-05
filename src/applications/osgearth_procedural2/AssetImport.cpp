/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "AssetImport.h"
#include <osgEarth/Chonk>
#include <osgEarth/Registry>
#include <osgEarth/JsonUtils>
#include <osgEarthPrestige/AssetCatalog>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osg/ComputeBoundsVisitor>
#include <fstream>
#include <iostream>
#include <map>

using namespace osgEarth;

namespace
{
    //! Counts scene-graph types without filtering inactive source children.
    struct Inventory : osg::NodeVisitor
    {
        std::map<std::string, unsigned> types;
        //! Inspect all nodes, including masks, so unsupported source structures are visible.
        Inventory() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
        //! Accumulate node class counts; source data is never modified.
        void apply(osg::Node& node) override { ++types[node.className()]; traverse(node); }
    };
}

int importVegetationAsset(osg::ArgumentParser& args)
{
    std::string input, output, dump, coarse, canopy;
    if (!args.read("--asset-source", input) || !args.read("--asset-json", output))
    {
        std::cerr << "--asset-source model --asset-json report.json [--asset-mesh] [--asset-dump scene.osgt]\n"
            << "  [--asset-coarse model --asset-canopy model] Validate the complete catalog bundle.\n";
        return 1;
    }
    args.read("--asset-dump", dump);
    const bool mesh = args.read("--asset-mesh");
    args.read("--asset-coarse", coarse);
    args.read("--asset-canopy", canopy);
    osgEarth::initialize(args);
    auto node = osgDB::readRefNodeFile(input);
    if (!node) return 1;
    Inventory inventory;
    node->accept(inventory);
    osg::ref_ptr<TextureArena> arena = new TextureArena();
    ChonkFactory factory(arena);
    auto chonk = factory.getOrCreateChonk(node);
    if (!chonk) return 1;
    Util::Json::Value report;
    report["source"] = input;
    report["triangles"] = unsigned(chonk->_ebo_store.size()/3);
    report["vertices"] = unsigned(chonk->_vbo_store.size());
    const auto& bounds = chonk->getBound();
    for (unsigned i=0; i<3; ++i)
    {
        report["low"].append(bounds._min[i]);
        report["high"].append(bounds._max[i]);
    }
    for (const auto& entry : inventory.types) report["node_types"][entry.first] = entry.second;
    if (mesh)
    {
        for (const auto& vertex : chonk->_vbo_store)
        {
            Util::Json::Value data;
            for (unsigned i = 0; i < 3; ++i) data["position"].append(vertex.position[i]);
            for (unsigned i = 0; i < 3; ++i) data["normal"].append(vertex.normal[i]);
            for (unsigned i = 0; i < 4; ++i) data["color"].append(vertex.color[i] / 255.0);
            for (unsigned i = 0; i < 2; ++i) data["uv"].append(vertex.uv[i]);
            data["material"] = unsigned(vertex.material_index);
            report["mesh"].append(data);
        }
        for (auto index : chonk->_ebo_store) report["indices"].append(unsigned(index));
    }
    for (const auto& material : chonk->_materials)
    {
        Util::Json::Value data;
        data["id"] = unsigned(material->index);
        for (int index : material->textures)
        {
            auto texture = index < 0 ? Texture::Ptr() : arena->find(unsigned(index));
            osg::Image* image = texture && texture->osgTexture() ? texture->osgTexture()->getImage(0) : nullptr;
            Util::Json::Value tex;
            if (image)
            {
                tex["file"] = image->getFileName();
                tex["width"] = image->s(); tex["height"] = image->t();
                tex["bytes"] = unsigned(image->getTotalSizeInBytesIncludingMipmaps());
                tex["compressed"] = image->isCompressed();
                tex["mips"] = image->getNumMipmapLevels();
            }
            data["textures"].append(tex);
        }
        report["materials"].append(data);
    }
    if (!coarse.empty())
    {
        Config asset("asset");
        asset.set("name", "import-check");
        asset.set("near", input);
        asset.set("coarse", coarse);
        if (!canopy.empty()) asset.set("canopy", canopy);
        osgEarthPrestige::AssetCatalog catalog({osgEarthPrestige::ScatterAsset(asset)}, 256u * 1024u * 1024u);
        osgEarthPrestige::ScatterGroup group;
        group.models = {"import-check"};
        auto individual = catalog.acquire(group, "import-check");
        auto aggregate = catalog.acquireStand(group, "import-check");
        auto residency = catalog.residency();
        if (!individual || !aggregate || residency.failedLoads || residency.budgetDenials)
        {
            std::cerr << "Catalog validation failed: " << residency.lastError << '\n';
            return 1;
        }
        report["catalog_bytes"] = double(residency.bytes);
        report["aggregate_triangles"] = unsigned(aggregate->_ebo_store.size() / 3);
    }
    std::ofstream out(output);
    out << Util::Json::StyledWriter().write(report);
    if (!dump.empty() && !osgDB::writeNodeFile(*node, dump)) return 1;
    std::cout << input << ": " << chonk->_ebo_store.size()/3 << " triangles\n";
    return out.good() ? 0 : 1;
}
