/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include "LabelEngineInternal.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <stdexcept>
#include <tuple>

using namespace osgEarthPrestige::Labeling;

namespace
{
    using Identity = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::int32_t>;
    using Version = std::pair<Identity, std::uint64_t>;

    //! Orders stable feature identities independently of tile/batch arrival order.
    Identity identity(const Record& record)
    {
        return { record.key.source, record.key.feature, record.key.role, record.key.instance };
    }

    //! Tests open rectangle interiors; padded edges may touch without rejecting either label.
    bool overlaps(const Placement& a, const Placement& b)
    {
        return a.lowerLeft.x() < b.upperRight.x() && a.upperRight.x() > b.lowerLeft.x() &&
            a.lowerLeft.y() < b.upperRight.y() && a.upperRight.y() > b.lowerLeft.y();
    }

    struct Cells
    {
        int x0, y0, x1, y1;
        //! Bounds per-label index storage; larger footprints use the separate oversized list.
        bool oversized() const { return (x1 - x0 + 1) * (y1 - y0 + 1) > 64; }
    };
}

std::vector<Placement> osgEarthPrestige::Labeling::declutter(const Request& request,
    const std::atomic<bool>& canceled, const Result* previous, const DeclutterOptions& options, LayoutStats* stats)
{
    if (options.placements == 0 || options.placements > 16384 || options.collisionTests == 0 ||
        !std::isfinite(options.padding) || options.padding < 0)
        throw std::invalid_argument("Invalid label decluttering limits");
    LayoutStats work;
    auto candidates = project(request, canceled);
    work.candidates = candidates.size();
    std::vector<Version> history;
    std::map<Version, unsigned> alternatives;
    if (previous && previous->input && previous->input->view == request.view &&
        previous->input->configuration == request.configuration)
    {
        for (const auto& p : previous->placements)
        {
            const auto& record = previous->input->batches[p.batch]->records()[p.record];
            history.emplace_back(identity(record), record.revision);
            if (record.calloutGap > 0)
                alternatives.emplace(Version(identity(record), record.continuity ? record.continuity : record.revision),
                    p.alternative);
        }
        std::sort(history.begin(), history.end());
    }
    struct Candidate
    {
        std::size_t index;
        Identity key;
        std::uint64_t revision;
        float priority;
        bool retained;
    };
    std::vector<Candidate> order;
    order.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        const auto& p = candidates[i];
        const auto& record = request.batches[p.batch]->records()[p.record];
        auto key = identity(record);
        order.push_back({ i, key, record.revision, record.priority,
            std::binary_search(history.begin(), history.end(), Version(key, record.revision)) });
    }
    // Preference never promotes a retained low-priority label above a new high-priority label.
    std::sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b)
    {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.retained != b.retained) return a.retained;
        if (a.key != b.key) return a.key < b.key;
        if (a.revision != b.revision) return a.revision > b.revision;
        return a.index < b.index;
    });

    const auto& camera = request.camera;
    const double cellSize = std::max({ 64.0 * camera.devicePixelRatio, camera.width / 64.0, camera.height / 64.0 });
    const int columns = std::max(1, std::min(64, static_cast<int>(std::ceil(camera.width / cellSize))));
    const int rows = std::max(1, std::min(64, static_cast<int>(std::ceil(camera.height / cellSize))));
    std::vector<std::vector<std::size_t>> grid(columns * rows);
    std::vector<std::size_t> large, visited(options.placements, 0);
    std::vector<Placement> accepted;
    accepted.reserve(std::min(options.placements, candidates.size()));
    std::set<Identity> identities;
    const osg::Vec2d padding(options.padding * camera.devicePixelRatio, options.padding * camera.devicePixelRatio);

    //! Clamps index coverage to the viewport; every candidate has an anchor inside it, including oversized labels.
    auto cells = [&](const Placement& p)
    {
        //! Converts finite screen coordinates to a bounded grid index without unbounded integer conversion.
        auto index = [](double value, double origin, double size, int count)
        {
            const double fraction = std::clamp((value - origin) / size, 0.0, 1.0);
            return std::min(count - 1, static_cast<int>(fraction * count));
        };
        return Cells { index(p.lowerLeft.x(), camera.x, camera.width, columns),
            index(p.lowerLeft.y(), camera.y, camera.height, rows),
            index(p.upperRight.x(), camera.x, camera.width, columns),
            index(p.upperRight.y(), camera.y, camera.height, rows) };
    };
    std::size_t query = 0;
    for (const auto& candidate : order)
    {
        if (canceled.load())
        {
            if (stats) *stats = work;
            return {};
        }
        if (accepted.size() == options.placements)
        {
            work.budgetExhausted = true;
            break;
        }
        if (identities.count(candidate.key)) continue;
        const auto& base = candidates[candidate.index];
        const auto& record = request.batches[base.batch]->records()[base.record];
        const bool callout = record.calloutGap > 0 && !record.path;
        const auto retained = callout ? alternatives.find(Version(candidate.key,
            record.continuity ? record.continuity : record.revision)) : alternatives.end();
        const unsigned preferred = retained == alternatives.end() ? 0 : std::min(8u, retained->second);
        Placement p;
        Cells range {};
        bool found = false;
        // Try the last displayed alternative first. At most nine bounded grid queries per point label.
        for (unsigned attempt = 0; attempt < (callout ? 9u : 1u); ++attempt)
        {
            p = base;
            if (callout)
            {
                const unsigned option = attempt == 0 ? preferred : (attempt <= preferred ? attempt - 1 : attempt);
                p.alternative = option;
                if (option != 0)
                {
                    static const int directions[8][2] = { {1,0}, {-1,0}, {0,1}, {0,-1},
                        {1,1}, {-1,1}, {1,-1}, {-1,-1} };
                    for (unsigned axis = 0; axis < 2; ++axis)
                    {
                        const int direction = directions[option - 1][axis];
                        p.offset[axis] = direction > 0 ? record.calloutGap - record.lowerLeft[axis] :
                            direction < 0 ? -record.calloutGap - record.upperRight[axis] :
                            -0.5 * (record.lowerLeft[axis] + record.upperRight[axis]);
                    }
                    p.lowerLeft += p.offset * camera.devicePixelRatio;
                    p.upperRight += p.offset * camera.devicePixelRatio;
                }
                if (p.lowerLeft.x() - padding.x() < camera.x || p.lowerLeft.y() - padding.y() < camera.y ||
                    p.upperRight.x() + padding.x() > camera.x + camera.width ||
                    p.upperRight.y() + padding.y() > camera.y + camera.height) continue;
            }
            p.lowerLeft -= padding;
            p.upperRight += padding;
            if (!std::isfinite(p.lowerLeft.x()) || !std::isfinite(p.lowerLeft.y()) ||
                !std::isfinite(p.upperRight.x()) || !std::isfinite(p.upperRight.y())) continue;
            range = cells(p);
            ++query;
            //! Counts each exact comparison once; budget exhaustion conservatively rejects the untested candidate.
            auto blocked = [&](std::size_t index)
            {
                if (visited[index] == query) return false;
                visited[index] = query;
                if (work.collisionTests == options.collisionTests)
                {
                    work.budgetExhausted = true;
                    return true;
                }
                ++work.collisionTests;
                return overlaps(p, accepted[index]);
            };
            bool collision = false;
            if (range.oversized())
            {
                for (std::size_t i = 0; i < accepted.size() && !collision; ++i)
                    collision = blocked(i);
            }
            else
            {
                for (auto i : large)
                    if ((collision = blocked(i))) break;
                for (int y = range.y0; y <= range.y1 && !collision; ++y)
                    for (int x = range.x0; x <= range.x1 && !collision; ++x)
                        for (auto i : grid[y * columns + x])
                            if ((collision = blocked(i))) break;
            }
            if (work.budgetExhausted) break;
            if (!collision) { found = true; break; }
        }
        if (work.budgetExhausted) break;
        if (!found) continue;
        const auto index = accepted.size();
        accepted.push_back(p);
        identities.insert(candidate.key);
        if (range.oversized()) large.push_back(index);
        else
            for (int y = range.y0; y <= range.y1; ++y)
                for (int x = range.x0; x <= range.x1; ++x)
                    grid[y * columns + x].push_back(index);
    }
    work.accepted = accepted.size();
    if (stats) *stats = work;
    return canceled.load() ? std::vector<Placement>() : accepted;
}
