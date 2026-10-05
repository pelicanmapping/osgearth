/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarth/SimplePager>

namespace osgEarthPrestige
{
    using namespace osgEarth;
    //! SimplePager extension for local edits. Retains its normal loading, culling, cancellation and merge machinery.
    class OverlayPager : public osgEarth::Util::SimplePager
    {
        struct Page : osgEarth::Util::PagedNode2
        {
            TileKey key;
            //! Associates geographic identity with both loaded and in-flight paging branches.
            explicit Page(const TileKey& value) : key(value) { }
        };
    public:
        //! Creates keyed paging nodes without modifying the shared SimplePager implementation.
        OverlayPager(const Map* map, const Profile* profile) : SimplePager(map,profile)
        {
            setCreatePagedNodeFunction([](const TileKey& key) { return new Page(key); });
        }
        //! Update-thread only: drops intersecting first-content sibling blocks and pending ancestors.
        //! unload() increments PagedNode2's revision, so old worker results cannot merge after this edit.
        //! Coarsest content and all descendants disappear together; other resident regions remain intact.
        unsigned invalidate(const std::vector<GeoExtent>& regions)
        {
            struct Visitor : osg::NodeVisitor
            {
                const std::vector<GeoExtent>& regions;
                unsigned firstLevel, count = 0;
                //! Visits masked and ordinary children; invalidation is independent of current visibility.
                Visitor(const std::vector<GeoExtent>& value, unsigned level) :
                    NodeVisitor(TRAVERSE_ALL_CHILDREN), regions(value), firstLevel(level) { setNodeMaskOverride(~0u); }
                //! Stops at an unaffected branch or the smallest replaceable block containing coarse content.
                void apply(osg::Group& group) override
                {
                    auto* page = dynamic_cast<Page*>(&group);
                    if (page)
                    {
                        bool affected = false;
                        for (const auto& region : regions)
                            if (page->key.getExtent().intersects(region)) { affected = true; break; }
                        if (!affected) return;
                        if (page->key.getLOD()+1u >= firstLevel || !page->isLoadComplete())
                        {
                            page->unload(); ++count; return;
                        }
                    }
                    traverse(group);
                }
            } visitor(regions,getMinLevel());
            accept(visitor);
            return visitor.count;
        }
    };
}
