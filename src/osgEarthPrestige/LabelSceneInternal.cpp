/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include "LabelSceneInternal.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace osgEarthPrestige::Labeling;

struct SceneSource::State
{
    struct Entry
    {
        std::uint64_t id = 0, revision = 0;
        osg::Vec3d world;
    };
    struct Camera
    {
        osg::observer_ptr<osg::Camera> camera;
        bool bound = false;
        std::array<std::bitset<1024>, 2> visible;
        std::array<std::uint64_t, 2> frames {{~std::uint64_t(0), ~std::uint64_t(0)}};
        std::shared_ptr<const SceneVisibility> snapshot;
        bool dirty = false;
    };
    mutable std::mutex mutex;
    std::array<Entry, 1024> entries;
    std::array<Camera, 8> cameras;
    std::shared_ptr<DynamicSource> source;
    std::string font;
    std::uint64_t sourceID = 0, generation = 0;
    bool active = false;

    //! Starts one shared font worker on open/registration, never during cull.
    void start()
    {
        if (active && !source)
        {
            DynamicOptions options; options.sourceID = sourceID;
            source = std::make_shared<DynamicSource>(font, options);
        }
    }

    //! Finds or reclaims a bounded camera entry; raw addresses are never reused without observer validation.
    Camera* camera(osg::Camera* camera)
    {
        for (auto& slot : cameras)
            if (slot.bound && slot.camera == camera) return &slot;
        for (auto& slot : cameras)
            if (!slot.bound || !slot.camera.valid())
            {
                slot = Camera(); slot.camera = camera; slot.bound = true;
                return &slot;
            }
        return nullptr;
    }
};

SceneSource::SceneSource() : _state(new State) { }
SceneSource::~SceneSource() = default;

void SceneSource::open(const std::string& font, std::uint64_t sourceID)
{
    close();
    std::lock_guard<std::mutex> lock(_state->mutex);
    _state->font = font; _state->sourceID = sourceID; _state->active = true;
    for (const auto& entry : _state->entries)
        if (entry.id) { _state->start(); break; }
}

void SceneSource::close()
{
    std::shared_ptr<DynamicSource> retired;
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        retired = std::move(_state->source);
        _state->active = false;
        for (auto& entry : _state->entries) entry.revision = 0;
        for (auto& camera : _state->cameras) camera = State::Camera();
    }
    // Join outside the snapshot mutex. Callers retire view ownership before closing this source.
    retired.reset();
}

std::uint64_t SceneSource::add()
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    auto found = std::find_if(_state->entries.begin(), _state->entries.end(),
        [](const State::Entry& entry) { return entry.id == 0; });
    if (found == _state->entries.end()) throw std::runtime_error("LabelLayer supports at most 1024 live LabelNodes");
    _state->start();
    found->id = (++_state->generation * 1024) + (found - _state->entries.begin());
    return found->id;
}

void SceneSource::remove(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    auto& entry = _state->entries[id % 1024];
    if (!id || entry.id != id) return;
    if (_state->source) _state->source->erase(id);
    entry = State::Entry();
    for (auto& camera : _state->cameras) if (camera.bound)
    {
        for (auto& visible : camera.visible) visible.reset(id % 1024);
        camera.dirty = true;
    }
}

bool SceneSource::visit(osg::Camera* camera, std::uint64_t frame, std::uint64_t id, std::uint64_t revision,
    const DynamicLabel& value, const osg::Vec3d& world)
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    auto& entry = _state->entries[id % 1024];
    if (!id || entry.id != id || !_state->source) return false;
    auto* view = _state->camera(camera);
    if (!view) return false;
    const auto index = frame % 2;
    if (view->frames[index] != frame)
    { view->frames[index] = frame; view->visible[index].reset(); }
    view->visible[index].set(id % 1024);
    if (!view->snapshot || view->snapshot->ids[id % 1024] != id) view->dirty = true;
    if (entry.revision != revision || entry.world != world)
    {
        auto copy = value; copy.worldAnchor = world;
        if (!_state->source->set(id, std::move(copy))) return false;
        entry.revision = revision; entry.world = world;
    }
    return true;
}

std::shared_ptr<const SceneVisibility> SceneSource::visibility(osg::Camera* camera, std::uint64_t frame)
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    auto* view = _state->camera(camera);
    if (!view) return {};
    const auto previous = frame - 1, index = previous % 2;
    const bool valid = frame != 0 && view->frames[index] == previous;
    bool changed = !view->snapshot;
    for (std::size_t i = 0; !changed && i < 1024; ++i)
    {
        const auto id = valid && view->visible[index][i] ? _state->entries[i].id : 0;
        changed = id != view->snapshot->ids[i];
    }
    if (changed)
    {
        auto snapshot = std::make_shared<SceneVisibility>(); snapshot->source = _state->sourceID;
        if (valid) for (std::size_t i = 0; i < 1024; ++i)
            if (view->visible[index][i]) snapshot->ids[i] = _state->entries[i].id;
        view->snapshot = std::move(snapshot);
    }
    view->dirty = false;
    // Detect a disappearance even if the layer was traversed before its label nodes in the previous frame.
    const auto current = frame % 2;
    if (view->frames[current] != frame)
    { view->frames[current] = frame; view->visible[current].reset(); }
    return view->snapshot;
}

std::shared_ptr<DynamicSource> SceneSource::source() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->source;
}

bool SceneSource::needsFrame() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);
    for (const auto& camera : _state->cameras)
    {
        if (!camera.camera.valid()) continue;
        if (camera.dirty) return true;
        if (!camera.snapshot) continue;
        const auto last = camera.frames[0] == ~std::uint64_t(0) ? camera.frames[1] :
            camera.frames[1] == ~std::uint64_t(0) ? camera.frames[0] : std::max(camera.frames[0], camera.frames[1]);
        if (last == ~std::uint64_t(0)) continue;
        for (std::size_t i = 0; i < 1024; ++i)
            if ((camera.snapshot->ids[i] != 0) != camera.visible[last % 2][i]) return true;
    }
    return false;
}
