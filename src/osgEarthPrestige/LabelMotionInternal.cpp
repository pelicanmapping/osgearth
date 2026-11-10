/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include "LabelEngineInternal.h"
#include <osg/Vec4d>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

using namespace osgEarthPrestige::Labeling;

float Fade::opacity(double time, double adoptedTime) const
{
    const double elapsed = time - start - (relative ? adoptedTime : 0.0);
    const float initial = relative ? std::clamp(from + velocity * float(adoptedTime - sampledTime), 0.0f, 1.0f) : from;
    return initial + (to - initial) * float(std::clamp(elapsed / 0.2, 0.0, 1.0));
}

double Fade::end(double adoptedTime) const
{
    return start + (relative ? adoptedTime : 0.0) + (from == to ? 0.0 : 0.2);
}

bool Fade::operator==(const Fade& rhs) const
{
    return start == rhs.start && from == rhs.from && to == rhs.to && relative == rhs.relative &&
        velocity == rhs.velocity && sampledTime == rhs.sampledTime;
}

bool osgEarthPrestige::Labeling::samePose(const CameraSnapshot& a, const CameraSnapshot& b)
{
    //! Ignores floating-point noise while retaining subpixel motion at ordinary map distances.
    auto equal = [](double x, double y) { return std::abs(x - y) <= 1e-10 * std::max({ 1.0, std::abs(x), std::abs(y) }); };
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            if (!equal(a.worldToView(row, column), b.worldToView(row, column)) ||
                (column != 2 && !equal(a.projection(row, column), b.projection(row, column)))) return false;
    return true;
}

bool osgEarthPrestige::Labeling::compatible(const Result& result, const CameraSnapshot& camera, double stationaryTime)
{
    if (!result.input) return false;
    const auto& old = result.input->camera;
    if (camera.time < old.time || camera.x != old.x || camera.y != old.y || camera.width != old.width ||
        camera.height != old.height || camera.devicePixelRatio != old.devicePixelRatio ||
        camera.geocentric != old.geocentric) return false;
    if (samePose(old, camera)) return true; // Stationary layouts do not expire just because the viewer was idle.
    if (camera.time - std::max(old.time, stationaryTime) > 0.5) return false;
    const auto oldEye = osg::Matrixd::inverse(old.worldToView), eye = osg::Matrixd::inverse(camera.worldToView);
    osg::Vec3d oldDirection(oldEye(2, 0), oldEye(2, 1), oldEye(2, 2));
    osg::Vec3d direction(eye(2, 0), eye(2, 1), eye(2, 2));
    oldDirection.normalize(); direction.normalize();
    if (oldDirection * direction < 0.819152044289) return false; // cos(35 degrees).
    if ((old.projection(3, 3) == 0) != (camera.projection(3, 3) == 0)) return false;
    const auto matrix = camera.worldToView * camera.projection;
    const std::size_t count = std::min<std::size_t>(32, result.placements.size());
    const double distance = 0.35 * std::min(camera.width, camera.height);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& p = result.placements[i * result.placements.size() / count];
        const auto& record = result.input->batches[p.batch]->records()[p.record];
        const osg::Vec4d clip = osg::Vec4d(record.worldAnchor, 1) * matrix;
        if (!std::isfinite(clip.w()) || clip.w() <= 0) return false;
        const osg::Vec2d pixel(camera.x + (clip.x() / clip.w() + 1) * camera.width * 0.5,
            camera.y + (clip.y() / clip.w() + 1) * camera.height * 0.5);
        if (!std::isfinite(pixel.x()) || !std::isfinite(pixel.y()) ||
            (pixel - p.anchorPixels).length2() > distance * distance) return false;
    }
    return true;
}

void osgEarthPrestige::Labeling::stabilize(Result& result, const Result* displayed, const Limits& limits)
{
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::int32_t, std::uint64_t>;
    //! Explicit lifetimes preserve opacity across edits; other records retain revision-based animation identity.
    auto key = [](const PresentedLabel& label)
    {
        const auto& r = label.batch->records()[label.record];
        return Key(r.key.source, r.key.feature, r.key.role, r.key.instance, r.continuity ? r.continuity : r.revision);
    };
    const auto& input = *result.input;
    result.presentation.clear();
    result.settledTime = 0;
    std::map<Key, PresentedLabel> old;
    if (input.animate && displayed && displayed->input && input.view == displayed->input->view &&
        input.configuration == displayed->input->configuration &&
        input.displayedSequence == displayed->input->sequence)
    {
        for (auto label : displayed->presentation)
        {
            if (label.fade.relative)
            {
                label.fade.from = std::clamp(label.fade.from +
                    label.fade.velocity * float(input.adoptedTime - label.fade.sampledTime), 0.0f, 1.0f);
                label.fade.velocity = 0;
                label.fade.sampledTime = 0;
                label.fade.start += input.adoptedTime;
                label.fade.relative = false;
            }
            old.emplace(key(label), std::move(label));
        }
    }
    std::set<Key> desired;
    //! Reverses at adoption using the prior slope, preserving opacity even when preparation/publication is delayed.
    auto reverse = [&](const Fade& fade, float target)
    {
        const float alpha = fade.opacity(input.camera.time, 0);
        const float velocity = alpha > 0 && alpha < 1 ? (fade.to - fade.from) / 0.2f : 0.0f;
        return Fade { 0, alpha, target, true, velocity, input.camera.time };
    };
    std::set<const Batch*> batches;
    std::size_t bytes = 0;
    for (const auto& batch : input.batches)
        if (batches.insert(batch.get()).second) bytes += batch->bytes();
    for (const auto& p : result.placements)
    {
        PresentedLabel label { input.batches[p.batch], p.record, {}, p.path, p.offset };
        const auto id = key(label);
        desired.insert(id);
        const auto found = old.find(id);
        if (input.animate)
        {
            if (found == old.end()) label.fade = { 0, 0, 1, true };
            else
            {
                label.fade = found->second.fade;
                const float alpha = label.fade.opacity(input.camera.time, 0);
                if (label.fade.to == 0) label.fade = reverse(label.fade, 1);
                else if (alpha >= 1) label.fade = {};
            }
        }
        result.presentation.push_back(std::move(label));
    }
    double reservation = 0;
    std::size_t outgoing = 0;
    for (auto& entry : old)
    {
        if (desired.count(entry.first)) continue;
        auto label = entry.second;
        const float alpha = label.fade.opacity(input.camera.time, 0);
        if (alpha <= 0 || outgoing == limits.declutter.placements) continue;
        // Old tile ownership is charged explicitly; animation cannot retain an unlimited chain of batches/results.
        if (!batches.count(label.batch.get()))
        {
            if (batches.size() >= limits.batches || bytes > limits.inputBytes ||
                label.batch->bytes() > limits.inputBytes - bytes) continue;
            batches.insert(label.batch.get());
            bytes += label.batch->bytes();
        }
        if (label.fade.to != 0) label.fade = reverse(label.fade, 0);
        const double remaining = label.fade.end(0) - (label.fade.relative ? 0 : input.camera.time);
        reservation = std::max(reservation, remaining);
        result.presentation.push_back(std::move(label));
        ++outgoing;
    }
    for (auto& label : result.presentation)
    {
        // A conservative per-view reservation avoids an extra collision pass or unbounded transition index.
        // Already visible winners continue; new/pending entries wait until every outgoing fade has completed.
        if (label.fade.to == 1 && label.fade.opacity(input.camera.time, input.camera.time) <= 0 && reservation > 0)
            label.fade = { reservation, 0, 1, true };
        result.settledTime = std::max(result.settledTime,
            label.fade.end(0) - (label.fade.relative ? 0 : input.camera.time));
    }
}
