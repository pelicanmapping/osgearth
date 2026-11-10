/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include "LabelTilesInternal.h"
#include <osgEarth/Horizon>
#include <osgEarth/Query>
#include <osgEarth/Threading>
#include <osg/Vec4d>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <thread>

using namespace osgEarthPrestige::Labeling;
using osgEarth::TileKey;

namespace
{
    //! Recomputes path glyph bounds on the preparation worker, preserving source geometry and shaped glyph order.
    void setPathFootprint(Record& record, const TextRun& run, const TextStyle& style)
    {
        auto path = std::make_shared<LabelPath>(*record.path);
        const double baseline = (run.lowerLeft.y() + run.upperRight.y()) * 0.5;
        const double padding = style.haloWidth + 1.5;
        path->glyphs.clear();
        path->width = run.advance * style.pixels;
        for (const auto& item : run.glyphs)
        {
            const double center = (item.glyph.bounds.x() + item.glyph.bounds.z()) * 0.5;
            PathGlyph glyph;
            glyph.distance = (item.offset.x() + center) * style.pixels;
            glyph.lowerLeft.set((item.glyph.bounds.x() - center) * style.pixels - padding,
                (item.glyph.bounds.y() + item.offset.y() - baseline) * style.pixels - padding);
            glyph.upperRight.set((item.glyph.bounds.z() - center) * style.pixels + padding,
                (item.glyph.bounds.w() + item.offset.y() - baseline) * style.pixels + padding);
            path->glyphs.push_back(glyph);
        }
        record.path = std::move(path);
    }

    //! Compares paint and footprint fields without relying on struct padding or float byte representations.
    bool sameStyle(const TextStyle& a, const TextStyle& b)
    {
        return a.pixels == b.pixels && a.color == b.color && a.halo == b.halo &&
            a.haloWidth == b.haloWidth && a.haloSoftness == b.haloSoftness &&
            a.rotation == b.rotation && a.calloutGap == b.calloutGap;
    }

    //! Clips multipart source lines to their native tile and retains the longest continuous piece, bounded to 256 vertices.
    //! Runs on the tile worker; invalid transforms and excessive geometry reject this feature without changing other labels.
    std::vector<osg::Vec3d> roadPath(const osgEarth::Feature& feature, const TileKey& key)
    {
        std::vector<osg::Vec3d> best, current;
        double bestLength = 0, length = 0;
        const auto& extent = key.getExtent();
        //! Finishes one continuous piece; never joins disjoint parts or tile exits across a gap.
        auto finish = [&]()
        {
            if (current.size() <= 256 && length > bestLength) { best = current; bestLength = length; }
            current.clear(); length = 0;
        };
        osgEarth::ConstGeometryIterator parts(feature.getGeometry(), false);
        std::size_t scanned = 0;
        while (parts.hasMore())
        {
            const auto* part = parts.next();
            if (!part->isLinear()) continue;
            osg::Vec3d previous;
            bool valid = false;
            for (const auto& vertex : *part)
            {
                if (++scanned > 4096) return {};
                const auto p = osgEarth::GeoPoint(feature.getSRS(), vertex).transform(extent.getSRS());
                if (!p.isValid()) return {};
                const osg::Vec3d next = p.vec3d();
                if (valid)
                {
                    const auto delta = next - previous;
                    double lo = 0, hi = 1;
                    bool inside = true;
                    for (unsigned axis = 0; axis < 2; ++axis)
                    {
                        const double minimum = axis == 0 ? extent.xMin() : extent.yMin();
                        const double maximum = axis == 0 ? extent.xMax() : extent.yMax();
                        if (delta[axis] == 0) { if (previous[axis] < minimum || previous[axis] > maximum) inside = false; }
                        else
                        {
                            double a = (minimum - previous[axis]) / delta[axis], b = (maximum - previous[axis]) / delta[axis];
                            if (a > b) std::swap(a, b);
                            lo = std::max(lo, a); hi = std::min(hi, b);
                        }
                    }
                    if (!inside || hi <= lo) finish();
                    else
                    {
                        const auto a = previous + delta * lo, b = previous + delta * hi;
                        if (!current.empty() && (current.back() - a).length2() > 1e-12) finish();
                        if (current.empty()) current.push_back(a);
                        if ((b - a).length2() > 1e-12) { current.push_back(b); length += (b - a).length(); }
                        if (hi < 1) finish();
                    }
                }
                previous = next; valid = true;
            }
            finish();
        }
        for (auto& vertex : best)
        {
            const auto p = osgEarth::GeoPoint(extent.getSRS(), vertex).transform(osgEarth::SpatialReference::get("wgs84"));
            if (!p.isValid() || !osgEarth::GeoPoint(p.getSRS(), p.x(), p.y(), 10).toWorld(vertex)) return {};
        }
        return best;
    }
}

BatchPtr osgEarthPrestige::Labeling::restyleTextBatch(const BatchPtr& batch, const std::vector<TextStyle>& styles)
{
    const auto* old = dynamic_cast<const TextBatchResources*>(batch->payload().get());
    if (!old || old->styles.size() != styles.size()) throw std::invalid_argument("Incompatible label style table");
    bool changed = false;
    for (const auto& record : batch->records())
        if (!sameStyle(old->styles.at(record.style), styles.at(record.style))) { changed = true; break; }
    if (!changed) return batch;
    auto payload = std::make_shared<TextBatchResources>(*old);
    payload->styles = styles;
    auto records = batch->records();
    for (auto& record : records)
    {
        const auto& before = old->styles[record.style];
        const auto& after = styles[record.style];
        if (before.pixels == after.pixels && before.haloWidth == after.haloWidth &&
            before.rotation == after.rotation && before.calloutGap == after.calloutGap) continue;
        const auto& run = *old->text->runs.at(record.shapedRun);
        setTextFootprint(record, run, after);
        if (record.path) setPathFootprint(record, run, after);
    }
    return std::make_shared<const Batch>(std::move(records), std::move(payload));
}

std::vector<TileKey> osgEarthPrestige::Labeling::selectLabelTiles(
    const CameraSnapshot& camera, const osgEarth::Profile* profile, const TileOptions& options)
{
    struct Cell { TileKey key; double pixels; };
    const auto matrix = camera.worldToView * camera.projection;
    const auto eye = osg::Matrixd::inverse(camera.worldToView).getTrans();
    osgEarth::Horizon horizon;
    horizon.setEye(eye);
    //! Conservatively rejects geographic tile spheres and estimates refinement size in this camera's pixels.
    auto score = [&](const TileKey& key)
    {
        // A projected extent's toWorld/bounds are planar. Paging here is geocentric even for a Mercator source.
        const auto sphere = key.getExtent().transform(osgEarth::SpatialReference::get("wgs84"))
            .createWorldBoundingSphere(0, 100);
        if (!horizon.isVisible(sphere.center(), sphere.radius())) return -1.0;
        const osg::Vec4d clip = osg::Vec4d(sphere.center(), 1) * matrix;
        for (unsigned axis = 0; axis < 2; ++axis)
            for (double sign : { -1.0, 1.0 })
            {
                const osg::Vec3d normal(matrix(0,3) + sign * matrix(0,axis),
                    matrix(1,3) + sign * matrix(1,axis), matrix(2,3) + sign * matrix(2,axis));
                if (clip.w() + sign * clip[axis] < -sphere.radius() * normal.length()) return -1.0;
            }
        const double distance = std::max(1.0, (sphere.center() - eye).length() - sphere.radius());
        return sphere.radius() * std::abs(camera.projection(1,1)) * camera.height / distance;
    };
    std::vector<TileKey> roots;
    profile->getRootKeys(roots);
    std::vector<Cell> cells;
    for (const auto& root : roots)
    {
        const double pixels = score(root);
        if (pixels >= 0) cells.push_back({root, pixels});
    }
    for (;;)
    {
        auto largest = std::max_element(cells.begin(), cells.end(),
            [](const Cell& a, const Cell& b) { return a.pixels < b.pixels; });
        if (largest == cells.end() || largest->pixels <= options.tilePixels * camera.devicePixelRatio) break;
        if (largest->key.getLOD() >= options.maxLevel) { largest->pixels = 0; continue; }
        std::vector<Cell> children;
        for (unsigned quadrant = 0; quadrant < 4; ++quadrant)
        {
            auto key = largest->key.createChildKey(quadrant);
            const double pixels = score(key);
            if (pixels >= 0) children.push_back({key, pixels});
        }
        if (cells.size() - 1 + children.size() > options.tilesPerView) { largest->pixels = 0; continue; }
        cells.erase(largest);
        cells.insert(cells.end(), children.begin(), children.end());
    }
    std::vector<TileKey> result;
    for (const auto& cell : cells) result.push_back(cell.key);
    std::sort(result.begin(), result.end());
    return result;
}

bool TileView::submit(const CameraSnapshot& camera)
{
    if (!camera.valid()) return false;
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock || _closed.load()) return false;
    _camera = camera;
    _pending = true;
    _busy.store(true);
    return true;
}

std::shared_ptr<const TileSet> TileView::acquire(std::uint64_t after) const
{
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock || _closed.load() || _published < 0 || _results[_published]->revision <= after) return {};
    _consumed.store(_results[_published]->revision);
    return _results[_published];
}

struct TileSource::State
{
    struct Entry
    {
        BatchPtr batch;
        bool stale = false; // Retain old coverage until level-dependent source preparation succeeds.
        std::uint64_t touch = 0;
        std::chrono::steady_clock::time_point retry;
    };
    osg::ref_ptr<osgEarth::FeatureSource> source;
    osg::ref_ptr<const osgEarth::Profile> profile = osgEarth::Profile::create("global-geodetic");
    std::string font;
    std::vector<TextStyle> styles;
    TileOptions options;
    struct StyleUpdate
    {
        std::vector<TextStyle> styles;
        std::vector<TileStyleRule> rules;
    };
    std::shared_ptr<const StyleUpdate> pendingStyles;
    std::atomic<bool> stylePending { false };
    mutable std::mutex mutex;
    std::array<std::weak_ptr<TileView>, 8> views;
    std::map<TileKey, Entry> cache;
    TileStats counters;
    std::uint64_t clock = 0, cacheRevision = 1;
    std::atomic<bool> stopped { false }, failed { false };
    std::thread worker;

    //! Captures control-thread configuration; source I/O and font objects are confined to run().
    State(osgEarth::FeatureSource* s, std::string f, std::vector<TextStyle> st, TileOptions op) :
        source(s), font(std::move(f)), styles(std::move(st)), options(std::move(op)) { }

    //! Restyles paint/bounds without queries; changed level eligibility marks cached tiles for lazy worker reload.
    //! Existing record identities/revisions preserve each view's placement preferences and fade progress.
    bool restyle()
    {
        std::shared_ptr<const StyleUpdate> next;
        { std::lock_guard<std::mutex> lock(mutex); next = pendingStyles; }
        if (!next) return false;
        try
        {
            std::vector<std::pair<Entry*, BatchPtr>> changed;
            std::size_t bytes = 0;
            for (auto& item : cache)
            {
                auto& entry = item.second;
                if (!entry.batch) continue;
                auto replacement = restyleTextBatch(entry.batch, next->styles);
                bytes += replacement->bytes();
                if (replacement != entry.batch) changed.emplace_back(&entry, std::move(replacement));
            }
            if (bytes > options.residentBytes) throw std::runtime_error("Restyled tiles exceed the resident byte budget");
            std::lock_guard<std::mutex> lock(mutex);
            if (pendingStyles != next) return true; // A newer slider value supersedes the entire transaction.
            bool levelsChanged = false;
            for (auto& item : cache)
                for (std::size_t i = 0; i < options.rules.size(); ++i)
                {
                    const auto level = item.first.getLOD();
                    const auto& before = options.rules[i];
                    const auto& after = next->rules[i];
                    if ((level >= before.minLevel && level <= before.maxLevel) !=
                        (level >= after.minLevel && level <= after.maxLevel))
                    {
                        item.second.stale = true; item.second.retry = {};
                        levelsChanged = true; break;
                    }
                }
            styles = next->styles;
            options.rules = next->rules;
            for (auto& item : changed) item.first->batch = std::move(item.second);
            if (!changed.empty() || levelsChanged) ++cacheRevision;
            counters.residentBytes = bytes;
            counters.restyledTiles += changed.size();
            ++counters.styleUpdates;
            pendingStyles.reset(); stylePending.store(false);
        }
        catch (const std::exception& e)
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++counters.failures; counters.error = e.what();
            if (pendingStyles == next) { pendingStyles.reset(); stylePending.store(false); }
        }
        return true;
    }

    //! Loads one spatial tile, bounds candidates before shaping, and publishes only complete immutable resources.
    BatchPtr load(const TileKey& key, TextService& fonts, const std::shared_ptr<TileView>& view)
    {
        osg::ref_ptr<osgEarth::ProgressCallback> progress = new osgEarth::ProgressCallback(nullptr,
            [&]() { return stopped.load() || view->_closed.load(); });
        osgEarth::Query query;
        const auto* fp = source->getFeatureProfile();
        if (fp->isTiled()) query.tileKey() = key;
        else query.bounds() = key.getExtent().transform(fp->getSRS()).bounds();
        auto cursor = source->createFeatureCursor(query, {}, nullptr, progress);
        if (!cursor) throw std::runtime_error("Feature query failed: " + key.str());
        struct Raw { Record record; std::string text; };
        //! Keeps the lowest-priority/highest-ID candidate at the heap head for bounded top-K admission.
        auto better = [](const Raw& a, const Raw& b)
        {
            return a.record.priority != b.record.priority ? a.record.priority > b.record.priority :
                a.record.key.feature < b.record.key.feature;
        };
        std::priority_queue<Raw, std::vector<Raw>, decltype(better)> selected(better);
        const double threshold = key.getLOD() < 6 ? 12 : key.getLOD() < 8 ? 8 : 0;
        std::size_t scanned = 0, eligible = 0, skipped = 0;
        while (cursor->hasMore())
        {
            if (progress->isCanceled()) return {};
            if (++scanned > 50000) throw std::runtime_error("Tile query exceeded the 50,000-feature scan budget");
            auto* feature = cursor->nextFeature();
            const auto* geometry = feature ? feature->getGeometry() : nullptr;
            if (!geometry) { ++skipped; continue; }
            const bool line = geometry->isLinear();
            if (!line && (!geometry->isPointSet() || geometry->size() != 1)) { ++skipped; continue; }
            if (options.requireFeatureID && feature->getFID() == 0) { ++skipped; continue; }
            const double rank = feature->getDouble(options.priorityAttribute);
            if (!std::isfinite(rank)) { ++skipped; continue; }
            double priority = rank;
            unsigned style = rank >= 12 ? 0 : std::min<std::size_t>(1, styles.size() - 1);
            if (options.rules.empty())
            {
                if (line || rank < threshold) continue;
            }
            else
            {
                const auto category = feature->getString(options.classAttribute);
                //! Finds the first class/level rule eligible for this source tile.
                const auto rule = std::find_if(options.rules.begin(), options.rules.end(), [&](const TileStyleRule& rule)
                {
                    return rule.line == line && key.getLOD() >= rule.minLevel && key.getLOD() <= rule.maxLevel &&
                        std::find(rule.classes.begin(), rule.classes.end(), category) != rule.classes.end();
                });
                if (rule == options.rules.end()) continue;
                priority = rule->priority - rank;
                style = rule->style;
            }
            std::shared_ptr<LabelPath> path;
            osgEarth::GeoPoint point;
            if (line)
            {
                path = std::make_shared<LabelPath>();
                path->gpu = options.gpuPaths;
                path->world = roadPath(*feature, key);
                if (path->world.size() < 2) { ++skipped; continue; }
            }
            else
            {
                point = osgEarth::GeoPoint(feature->getSRS(), geometry->front()).transform(profile->getSRS());
                if (!point.isValid()) { ++skipped; continue; }
                const auto& extent = profile->getExtent();
                if (!extent.contains(point.x(), point.y())) { ++skipped; continue; }
                const double x = std::clamp(point.x(), extent.xMin(), std::nextafter(extent.xMax(), extent.xMin()));
                const double y = std::clamp(point.y(), std::nextafter(extent.yMin(), extent.yMax()), extent.yMax());
                if (profile->createTileKey(x, y, key.getLOD()) != key) continue;
            }
            Raw raw;
            raw.text = feature->getString(options.textAttribute);
            if (raw.text.empty() && !options.fallbackTextAttribute.empty())
                raw.text = feature->getString(options.fallbackTextAttribute);
            if (raw.text.empty() || raw.text.size() > 4096) { ++skipped; continue; }
            // The repository's Natural Earth sample has three CP1252 controls stored as Unicode controls.
            if (options.repairLegacyControls)
            for (const auto& repair : { std::pair<std::string,std::string>{"\xC2\x92", "\xE2\x80\x99"},
                {"\xC2\x96", "\xE2\x80\x93"}, {"\xC2\x9E", "\xC5\xBE"} })
            {
                std::size_t offset = 0;
                while ((offset = raw.text.find(repair.first, offset)) != std::string::npos)
                { raw.text.replace(offset, repair.first.size(), repair.second); offset += repair.second.size(); }
            }
            raw.record.key = { options.sourceID, static_cast<std::uint64_t>(feature->getFID()), line ? 1u : 0u, 0 };
            raw.record.revision = 1;
            raw.record.priority = static_cast<float>(priority);
            raw.record.style = style;
            // Always emit Earth-fixed anchors, independent of the tile grid's projected/geographic units.
            if (path)
            {
                raw.record.path = path;
                raw.record.worldAnchor = path->world[path->world.size() / 2];
            }
            else
            {
                const auto geographic = point.transform(osgEarth::SpatialReference::get("wgs84"));
                if (!geographic.isValid() || !osgEarth::GeoPoint(geographic.getSRS(), geographic.x(), geographic.y(), 10)
                    .toWorld(raw.record.worldAnchor)) { ++skipped; continue; }
            }
            ++eligible;
            if (selected.size() < options.recordsPerTile) selected.push(std::move(raw));
            else if (better(raw, selected.top())) { selected.pop(); selected.push(std::move(raw)); }
        }
        std::vector<Raw> rows;
        while (!selected.empty()) { rows.push_back(selected.top()); selected.pop(); }
        std::sort(rows.begin(), rows.end(), better);
        const auto admitted = rows.size();
        std::vector<std::string> strings;
        for (const auto& row : rows) strings.push_back(row.text);
        auto payload = std::make_shared<TextBatchResources>();
        try { payload->text = fonts.prepare(strings); }
        catch (const UnsupportedGlyph&)
        {
            // One unsupported script must not poison every other label in a global tile.
            std::vector<Raw> supported;
            strings.clear();
            for (auto& row : rows)
            {
                try { fonts.prepare({row.text}); }
                catch (const UnsupportedGlyph&) { ++skipped; continue; }
                strings.push_back(row.text);
                supported.push_back(std::move(row));
            }
            rows = std::move(supported);
            payload->text = fonts.prepare(strings);
        }
        payload->styles = styles;
        std::vector<Record> records;
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            auto record = rows[i].record;
            record.shapedRun = i;
            setTextFootprint(record, *payload->text->runs[i], styles[record.style]);
            if (record.path)
            {
                const auto& run = *payload->text->runs[i];
                if (run.glyphs.empty() || run.glyphs.size() > 256 || run.advance <= 0) { ++skipped; continue; }
                setPathFootprint(record, run, styles[record.style]);
            }
            records.push_back(record);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            counters.skipped += skipped;
            counters.truncated += eligible - admitted;
        }
        return std::make_shared<const Batch>(std::move(records), std::move(payload));
    }

    //! Reclaims unpinned snapshots on this worker and adopts the latest camera demand by value.
    bool demand(const std::shared_ptr<TileView>& view)
    {
        std::array<std::shared_ptr<const TileSet>, 4> retired;
        CameraSnapshot camera;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(view->_mutex);
            changed = view->_pending;
            if (changed) { camera = view->_camera; view->_pending = false; }
            for (std::size_t i = 0; i < view->_results.size(); ++i)
                if (static_cast<int>(i) != view->_published && view->_results[i] && view->_results[i].use_count() == 1)
                    retired[i] = std::move(view->_results[i]);
        }
        if (changed) view->_wanted = selectLabelTiles(camera, profile, options);
        return changed;
    }

    //! Resolves complete ready child coverage or retains its ready parent; never mixes both for the same region.
    std::shared_ptr<TileSet> coverage(const std::shared_ptr<TileView>& view)
    {
        auto output = std::make_shared<TileSet>();
        output->complete = true;
        std::map<TileKey, std::set<TileKey>> children;
        std::set<TileKey> roots;
        for (auto key : view->_wanted)
        {
            while (key.getLOD() > 0)
            {
                auto parent = key.createParentKey();
                children[parent].insert(key);
                key = parent;
            }
            roots.insert(key);
        }
        //! Builds a temporary subtree cut, falling back atomically if any needed child is not ready yet.
        std::function<bool(const TileKey&, std::vector<TileKey>&)> resolve = [&](const TileKey& key,
            std::vector<TileKey>& selected)
        {
            const auto start = selected.size();
            auto branch = children.find(key);
            if (branch != children.end())
            {
                bool ready = true;
                for (const auto& child : branch->second) ready = resolve(child, selected) && ready;
                if (ready) return true;
                selected.resize(start);
            }
            auto found = cache.find(key);
            if (found == cache.end() || !found->second.batch) return false;
            selected.push_back(key);
            if (found->second.stale) output->complete = false;
            if (branch != children.end()) output->complete = false;
            return true;
        };
        for (const auto& root : roots)
            if (!resolve(root, output->coverage)) output->complete = false;
        std::sort(output->coverage.begin(), output->coverage.end());
        for (const auto& key : output->coverage)
        {
            auto& entry = cache.at(key);
            entry.touch = ++clock;
            output->batches.push_back(entry.batch);
        }
        return output;
    }

    //! Publishes only content changes into one of four bounded snapshot slots; retains prior coverage on backpressure.
    bool publish(const std::shared_ptr<TileView>& view)
    {
        auto next = coverage(view);
        std::lock_guard<std::mutex> lock(view->_mutex);
        if (view->_closed.load()) return true;
        if (view->_published >= 0)
        {
            const auto& old = view->_results[view->_published];
            if (old->batches == next->batches && old->complete == next->complete) return true;
        }
        for (std::size_t i = 0; i < view->_results.size(); ++i)
            if (!view->_results[i])
            {
                next->revision = ++view->_revision;
                view->_results[i] = std::move(next);
                view->_published = static_cast<int>(i);
                view->_readyRevision.store(view->_revision);
                return true;
            }
        return false;
    }

    //! Loads at most one missing ancestor/leaf per view per pass, sharing bounded LRU content across cameras.
    bool service(const std::shared_ptr<TileView>& view, TextService& fonts)
    {
        const bool changed = demand(view);
        if (!changed && !view->_busy.load() && view->_cacheRevision == cacheRevision &&
            std::chrono::steady_clock::now() < view->_nextCheck) return false;
        std::set<TileKey> needed;
        for (auto key : view->_wanted)
        {
            auto leaf = cache.find(key);
            if (leaf != cache.end() && leaf->second.batch && !leaf->second.stale)
            { leaf->second.touch = ++clock; continue; }
            needed.insert(key);
            while (key.getLOD() > 0) { key = key.createParentKey(); needed.insert(key); }
        }
        bool loaded = false, pending = false;
        for (const auto& key : needed)
        {
            auto found = cache.find(key);
            if (found != cache.end() && found->second.batch && !found->second.stale)
            { found->second.touch = ++clock; continue; }
            if (found != cache.end() && std::chrono::steady_clock::now() < found->second.retry) continue;
            pending = true;
            if (loaded) break;
            loaded = true;
            BatchPtr batch;
            std::string error;
            try { batch = load(key, fonts, view); }
            catch (const std::exception& e) { error = e.what(); }
            if (stopped.load() || view->_closed.load()) return false;
            if (batch && batch->bytes() > options.residentBytes)
            { batch.reset(); error = "Prepared tile exceeds the resident byte budget"; }
            if (found != cache.end() && found->second.batch && !batch)
            {
                // A failed refresh must not erase currently displayed labels; retry on the usual backoff.
                found->second.retry = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                std::lock_guard<std::mutex> lock(mutex);
                ++counters.loads; ++counters.failures; counters.error = error;
                continue;
            }
            if (found != cache.end())
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (found->second.batch) counters.residentBytes -= found->second.batch->bytes();
                cache.erase(found);
            }
            //! Evicts the least recently used cache entry; existing publications keep immutable batch pins valid.
            auto evict = [&]()
            {
                auto oldest = std::min_element(cache.begin(), cache.end(),
                    [](const auto& a, const auto& b) { return a.second.touch < b.second.touch; });
                if (oldest == cache.end()) return;
                std::lock_guard<std::mutex> lock(mutex);
                if (oldest->second.batch) counters.residentBytes -= oldest->second.batch->bytes();
                cache.erase(oldest);
                ++counters.evictions;
            };
            std::size_t bytes;
            { std::lock_guard<std::mutex> lock(mutex); bytes = counters.residentBytes; }
            while (!cache.empty() && (cache.size() >= options.residentTiles ||
                (batch && bytes + batch->bytes() > options.residentBytes)))
            {
                evict();
                std::lock_guard<std::mutex> lock(mutex); bytes = counters.residentBytes;
            }
            auto& entry = cache[key];
            entry.batch = std::move(batch); entry.touch = ++clock;
            entry.retry = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            ++cacheRevision;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++counters.loads;
                counters.residentTiles = cache.size();
                if (entry.batch) counters.residentBytes += entry.batch->bytes();
                else { ++counters.failures; counters.error = error.empty() ? "Canceled tile query" : error; }
            }
        }
        // Recheck demand before publishing so a slow old request cannot replace coverage after a camera jump.
        pending = demand(view) || pending;
        pending = !publish(view) || pending;
        {
            std::lock_guard<std::mutex> lock(view->_mutex);
            view->_busy.store(pending || view->_pending || stylePending.load());
        }
        view->_cacheRevision = cacheRevision;
        view->_nextCheck = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        return loaded;
    }

    //! Owns font/source work and reclaims all cache/snapshot ownership here before terminating.
    void run()
    {
        osgEarth::setThreadName("oe.labeltiles");
        try
        {
            TextService fonts(font);
            std::string warm;
            for (unsigned c = 32; c < 127; ++c) warm.push_back(static_cast<char>(c));
            for (unsigned c = 160; c < 384; ++c)
            { warm.push_back(static_cast<char>(0xc0 | (c >> 6))); warm.push_back(static_cast<char>(0x80 | (c & 63))); }
            warm += "\xE2\x80\x93\xE2\x80\x99";
            fonts.prepare({warm}); // Seal common Latin glyphs together instead of one tiny page per tile.
            while (!stopped.load())
            {
                std::array<std::shared_ptr<TileView>, 8> live;
                { std::lock_guard<std::mutex> lock(mutex); for (unsigned i = 0; i < 8; ++i) live[i] = views[i].lock(); }
                bool worked = restyle();
                for (auto& view : live)
                    if (view && !view->_closed.load() && !stopped.load()) worked = service(view, fonts) || worked;
                if (!worked) std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        }
        catch (const std::exception& e)
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++counters.failures; counters.error = e.what();
            failed.store(true);
            for (auto& weak : views) if (auto view = weak.lock())
            { view->_busy.store(false); view->_closed.store(true); }
        }
        cache.clear();
    }
};

TileSource::TileSource(osgEarth::FeatureSource* source, std::string font, std::vector<TextStyle> styles,
    const TileOptions& options)
{
    if (!source || !source->getFeatureProfile() || styles.empty() || options.maxLevel > 16 ||
        options.tilesPerView < 2 || options.tilesPerView > 64 || options.recordsPerTile == 0 ||
        options.recordsPerTile > 256 || options.residentTiles < options.tilesPerView ||
        options.residentBytes == 0 || !std::isfinite(options.tilePixels) || options.tilePixels <= 0)
        throw std::invalid_argument("Invalid tiled label source configuration");
    if (source->options().autoFID() == true)
        throw std::invalid_argument("Tiled labels require stable source feature IDs, not per-query autoFID");
    Record validation;
    for (const auto& style : styles)
    {
        setTextFootprint(validation, TextRun(), style);
        for (unsigned component = 0; component < 4; ++component)
            if (!std::isfinite(style.color[component]) || !std::isfinite(style.halo[component]))
                throw std::invalid_argument("Invalid tiled label color");
    }
    _state = std::make_unique<State>(source, std::move(font), std::move(styles), options);
    const auto* fp = source->getFeatureProfile();
    for (const auto& rule : options.rules)
        if (rule.classes.empty() || rule.minLevel > rule.maxLevel || rule.maxLevel > 16 ||
            rule.style >= _state->styles.size() || !std::isfinite(rule.priority))
            throw std::invalid_argument("Invalid tiled label class rule");
    if (fp->isTiled())
    {
        const auto* native = fp->getTilingProfile();
        if (!native || (!native->getSRS()->isGeographic() && !native->getSRS()->isMercator()) ||
            fp->getFirstLevel() != 0)
            throw std::invalid_argument("Tiled labels require a geographic/Web Mercator profile starting at level zero");
        _state->profile = native;
        if (fp->getMaxLevel() >= 0)
            _state->options.maxLevel = std::min(options.maxLevel, static_cast<unsigned>(fp->getMaxLevel()));
    }
    _state->worker = std::thread([this]() { _state->run(); });
}

TileSource::~TileSource()
{
    _state->stopped.store(true);
    _state->worker.join();
}

std::shared_ptr<TileView> TileSource::registerView()
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (_state->failed.load()) return {};
    for (auto& weak : _state->views)
        if (weak.expired())
        {
            auto view = std::make_shared<TileView>();
            weak = view;
            return view;
        }
    return {};
}

TileStats TileSource::stats() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->counters;
}

bool TileSource::setStyles(std::vector<TextStyle> styles, std::vector<TileStyleRule> rules)
{
    Record validation;
    for (const auto& style : styles)
    {
        setTextFootprint(validation, TextRun(), style);
        for (unsigned i = 0; i < 4; ++i)
            if (!std::isfinite(style.color[i]) || !std::isfinite(style.halo[i]))
                throw std::invalid_argument("Invalid tiled label color");
    }
    auto next = std::make_shared<State::StyleUpdate>();
    next->styles = std::move(styles);
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (_state->failed.load() || _state->stopped.load()) return false;
    if (next->styles.size() != _state->styles.size())
        throw std::invalid_argument("Cannot change the label rule count at runtime");
    const auto& previous = _state->pendingStyles ? _state->pendingStyles->styles : _state->styles;
    const auto& previousRules = _state->pendingStyles ? _state->pendingStyles->rules : _state->options.rules;
    next->rules = rules.empty() ? previousRules : std::move(rules);
    if (next->rules.size() != previousRules.size()) throw std::invalid_argument("Cannot change the label rule count");
    bool sameLevels = true;
    for (std::size_t i = 0; i < next->rules.size(); ++i)
    {
        const auto& a = next->rules[i]; const auto& b = previousRules[i];
        if (a.minLevel > a.maxLevel || a.maxLevel > 16 || a.classes != b.classes || a.style != b.style ||
            a.priority != b.priority || a.line != b.line)
            throw std::invalid_argument("Only rule levels 0-16 can change at runtime");
        sameLevels = sameLevels && a.minLevel == b.minLevel && a.maxLevel == b.maxLevel;
    }
    if (sameLevels && std::equal(next->styles.begin(), next->styles.end(), previous.begin(), sameStyle)) return true;
    _state->pendingStyles = std::move(next);
    _state->stylePending.store(true);
    for (auto& weak : _state->views) if (auto view = weak.lock()) view->_busy.store(true);
    return true;
}
