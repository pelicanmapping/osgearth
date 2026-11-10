/* osgEarth - Copyright 2026 Pelican Mapping - MIT License */
#include <osgEarthPrestige/LabelNode>
#include <osgEarthPrestige/LabelLayer>
#include <osg/Transform>
#include <osg/View>
#include <osg/observer_ptr>
#include <osgUtil/CullVisitor>
#include <cmath>
#include <stdexcept>
#ifdef OSGEARTH_HAVE_SLUGHORN
#include "LabelSceneInternal.h"
#include "LabelStyleInternal.h"
#endif

using namespace osgEarthPrestige;

struct LabelNode::Impl
{
    osg::observer_ptr<LabelLayer> layer;
    std::string text;
    osgEarth::Config style;
    osg::Vec3d position;
    float priority = 0;
#ifdef OSGEARTH_HAVE_SLUGHORN
    std::weak_ptr<Labeling::SceneSource> source;
    Labeling::DynamicLabel value;
    std::uint64_t id = 0, revision = 1;
#endif
};

LabelNode::LabelNode() : _impl(new Impl)
{
    // A pixel-sized label has no fixed world-space radius for OSG small-feature culling.
    // Use traversal masks/LOD and the explicit anchor projection below instead.
    setCullingActive(false);
    setStyle(osgEarth::Config());
}

LabelNode::LabelNode(LabelLayer* layer, const std::string& text, const osgEarth::Style& style) : LabelNode()
{
    setText(text); setStyle(style); setLayer(layer);
}

LabelNode::LabelNode(const LabelNode& rhs, const osg::CopyOp& copy) : osg::Node(rhs, copy), _impl(new Impl)
{
    setText(rhs.getText()); setStyle(rhs.getStyleConfig()); setPosition(rhs.getPosition());
    setPriority(rhs.getPriority()); setLayer(rhs.getLayer());
}

LabelNode::~LabelNode()
{
#ifdef OSGEARTH_HAVE_SLUGHORN
    if (auto source = _impl->source.lock()) source->remove(_impl->id);
#endif
}

void LabelNode::setLayer(LabelLayer* layer)
{
    if (_impl->layer == layer) return;
#ifdef OSGEARTH_HAVE_SLUGHORN
    auto next = layer ? layer->_scene : nullptr;
    const auto id = next ? next->add() : 0;
    if (auto old = _impl->source.lock()) old->remove(_impl->id);
    _impl->source = next; _impl->id = id;
#endif
    _impl->layer = layer;
}

LabelLayer* LabelNode::getLayer() const { return _impl->layer.get(); }
const std::string& LabelNode::getText() const { return _impl->text; }
const osgEarth::Config& LabelNode::getStyleConfig() const { return _impl->style; }
const osg::Vec3d& LabelNode::getPosition() const { return _impl->position; }
float LabelNode::getPriority() const { return _impl->priority; }

void LabelNode::setText(const std::string& text)
{
    if (text.size() > 4096) throw std::invalid_argument("LabelNode text exceeds 4096 UTF-8 bytes");
    if (_impl->text == text) return;
    _impl->text = text;
#ifdef OSGEARTH_HAVE_SLUGHORN
    _impl->value.text = text; ++_impl->revision;
#endif
}

void LabelNode::setStyle(const osgEarth::Style& style) { setStyle(style.getConfig()); }

void LabelNode::setStyle(const osgEarth::Config& style)
{
#ifdef OSGEARTH_HAVE_SLUGHORN
    const auto parsed = Labeling::readTextStyle(style);
    _impl->value.style = parsed; ++_impl->revision;
#endif
    _impl->style = style;
}

void LabelNode::setPosition(const osg::Vec3d& position)
{
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(position[i])) throw std::invalid_argument("LabelNode position must be finite");
    _impl->position = position;
    dirtyBound();
}

void LabelNode::setPriority(float priority)
{
    if (!std::isfinite(priority)) throw std::invalid_argument("LabelNode priority must be finite");
    if (_impl->priority == priority) return;
    _impl->priority = priority;
#ifdef OSGEARTH_HAVE_SLUGHORN
    _impl->value.priority = priority; ++_impl->revision;
#endif
}

osg::BoundingSphere LabelNode::computeBound() const { return osg::BoundingSphere(_impl->position, 0.0f); }

void LabelNode::traverse(osg::NodeVisitor& visitor)
{
#ifdef OSGEARTH_HAVE_SLUGHORN
    if (_impl->text.empty() || visitor.getVisitorType() != osg::NodeVisitor::CULL_VISITOR) return;
    auto* cv = dynamic_cast<osgUtil::CullVisitor*>(&visitor);
    if (!cv || !cv->getFrameStamp()) return;
    auto* camera = cv->getCurrentCamera();
    if (!camera || !camera->getView() || camera->getView()->getCamera() != camera) return;
    const auto clip = osg::Vec4d(_impl->position, 1.0) * *cv->getModelViewMatrix() * *cv->getProjectionMatrix();
    if (clip.w() <= 0.0 || std::abs(clip.x()) > clip.w() || std::abs(clip.y()) > clip.w()) return;
    auto source = _impl->source.lock();
    if (!source) return;
    const auto world = _impl->position * osg::computeLocalToWorld(visitor.getNodePath());
    for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(world[i])) return;
    source->visit(camera, cv->getFrameStamp()->getFrameNumber(), _impl->id, _impl->revision, _impl->value, world);
#endif
}
