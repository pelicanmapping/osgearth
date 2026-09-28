/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "CanopyTransition.h"
#include <osgEarth/CameraUtils>
#include <osgEarth/VirtualProgram>
#include <osgEarth/NodeUtils>
#include <osgUtil/CullVisitor>
#include <algorithm>
#include <cmath>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

double osgEarth::Procedural2::populationPixelSize(const osg::BoundingSphere& bound, osg::NodeVisitor& nv)
{
    auto* cv = Culling::asCullVisitor(nv);
    if (!cv || !cv->getViewport()) return -1.0;
    osg::Matrixd projection = *cv->getProjectionMatrix();
    osg::Vec3d position = osg::Vec3d(bound.center())*(*cv->getModelViewMatrix());
    float scale = cv->getLODScale();
    double height = cv->getViewport()->height();
    auto* camera = cv->getCurrentCamera();
    if (camera && CameraUtils::isShadowCamera(camera))
    {
        auto* ss = camera->getStateSet();
        auto* view = ss ? ss->getUniform("oe_shadowToPrimaryMatrix") : nullptr;
        auto* proj = ss ? ss->getUniform("oe_primaryProjectionMatrix") : nullptr;
        auto* size = ss ? ss->getUniform("oe_primaryViewport") : nullptr;
        auto* lod = ss ? ss->getUniform("oe_primaryLODScale") : nullptr;
        osg::Matrixd toPrimary;
        osg::Vec2 viewport;
        if (!view || !proj || !size || !lod || !view->get(toPrimary) || !proj->get(projection) ||
            !size->get(viewport) || !lod->get(scale)) return -1.0;
        position = position*toPrimary;
        height = viewport.y();
    }
    if (scale <= 0.0f) return -1.0;
    const double distance = ProjectionMatrix::isOrtho(projection) ?
        1.0 : std::max(1.0,-position.z()-bound.radius());
    return std::max(0.5,double(bound.radius()))*std::abs(projection(1,1))*height/distance/scale;
}

float osgEarth::Procedural2::populationError(osg::NodeVisitor& nv, const osg::Uniform* adjustment)
{
    float global = 25.0f;
    if (!nv.getUserValue("oe_sse", global))
    {
        osg::ref_ptr<Util::PagingManager> manager;
        ObjectStorage::get(&nv, manager);
        if (manager && manager->sse() > 0.0f) global = manager->sse();
    }
    osg::Vec2f value(0,1);
    if (adjustment) adjustment->get(value);
    return std::max(1.0f, global + value.x());
}

float osgEarth::Procedural2::populationVisibility(double pixels, float error)
{
    return pixels < 0.0 ? 1.0f : canopyBlend(pixels,error,0.25f,0.0,0.0f);
}

void osgEarth::Procedural2::installPopulationPageFade(osg::Node* node, osg::Uniform* adjustment,
    std::shared_ptr<CanopyTransitionStates> states)
{
    osg::ref_ptr<osg::Uniform> policy = adjustment;
    node->addCullCallback(new Util::LambdaCullCallback([policy,states](osg::Node* target, osg::NodeVisitor* nv)
    {
        const float visibility = populationVisibility(populationPixelSize(target->getBound(),*nv),
            populationError(*nv,policy));
        if (visibility <= 0.0f) return;
        auto* cv = Culling::asCullVisitor(*nv);
        if (visibility >= 1.0f || !cv) { nv->traverse(*target); return; }
        osg::Vec2f inherited(0,64);
        nv->getUserValue("oe_p2_coverage_interval",inherited);
        const unsigned low = unsigned(inherited.x()), high = unsigned(inherited.y());
        const unsigned end = low+unsigned(std::floor((high-low)*visibility+0.5f));
        nv->setUserValue("oe_p2_coverage_interval",osg::Vec2f(float(low),float(end)));
        cv->pushStateSet(states->get(low,end));
        nv->traverse(*target);
        cv->popStateSet();
        nv->setUserValue("oe_p2_coverage_interval",inherited);
    }));
}

float osgEarth::Procedural2::canopyBlend(double pixels, double threshold, float overlap, double age, float seconds)
{
    const double start = threshold*(1.0-overlap), end = threshold*(1.0+overlap);
    double t = end > start ? std::max(0.0,std::min(1.0,(pixels-start)/(end-start))) : double(pixels >= threshold);
    t = t*t*(3.0-2.0*t);
    double arrival = seconds > 0.0f ? std::max(0.0,std::min(1.0,age/seconds)) : 1.0;
    arrival = arrival*arrival*(3.0-2.0*arrival);
    return float(std::min(t,arrival));
}

unsigned osgEarth::Procedural2::canopySplit(unsigned low, unsigned high, float childWeight)
{
    return low+unsigned(std::floor((high-low)*(1.0-std::max(0.0f,std::min(1.0f,childWeight)))+0.5));
}

osg::StateSet* CanopyTransitionStates::get(unsigned low, unsigned high)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto& state = states[low*65+high];
    if (!state)
    {
        state = new osg::StateSet();
        state->setDataVariance(osg::Object::STATIC);
        state->addUniform(new osg::Uniform("oe_chonk_coverage",osg::Vec2f(low/64.0f,high/64.0f)));
    }
    return state.get();
}

void osgEarth::Procedural2::configureCanopyTransition(Util::PagedNode2* node, double worldError,
    const ScatterGroup& group, std::shared_ptr<CanopyTransitionStates> states, osg::Uniform* adjustment)
{
    osg::ref_ptr<osg::Uniform> policy = adjustment;
    const double ratio = worldError / std::max(1.0,2.0*node->getRadius());
    node->setRefinementFunction([node,ratio,policy,group](osg::NodeVisitor& nv, bool suggested)
    {
        const double pixels = populationPixelSize(node->getBound(),nv);
        return pixels >= 0.0 ? pixels*ratio >= populationError(nv,policy)*(1.0f-group.canopyTransition) : suggested;
    });
    node->setReplacementFunction([node,ratio,policy,group,states](osg::NodeVisitor& nv, osg::Node* children, bool refine)
    {
        auto* cv = Culling::asCullVisitor(nv);
        const double pixels = populationPixelSize(node->getBound(),nv);
        const float threshold = populationError(nv,policy);
        const double now = nv.getFrameStamp() ? nv.getFrameStamp()->getReferenceTime() : node->getMergeTime();
        const float weight = children && refine ? (pixels >= 0.0 ?
            canopyBlend(pixels*ratio,threshold,group.canopyTransition,now-node->getMergeTime(),group.canopyFadeSeconds) :
            1.0f) : 0.0f;
        osg::Vec2f inherited(0,64);
        nv.getUserValue("oe_p2_coverage_interval",inherited);
        const unsigned low = unsigned(inherited.x()), high = unsigned(inherited.y());
        const unsigned split = canopySplit(low,high,weight);
        //! Traverses one interval without mutable per-camera Uniforms; visitor-local inheritance composes nested fades.
        auto visit = [&](osg::Node* target, unsigned a, unsigned b)
        {
            if (a == b || !target) return;
            nv.setUserValue("oe_p2_coverage_interval",osg::Vec2f(float(a),float(b)));
            if (cv) cv->pushStateSet(states->get(a,b));
            target->accept(nv);
            if (cv) cv->popStateSet();
            nv.setUserValue("oe_p2_coverage_interval",inherited);
        };
        for (unsigned i=0; i<node->getNumChildren(); ++i)
            if (node->getChild(i) != children) visit(node->getChild(i),low,split);
        visit(children,split,high);
        return split < high;
    });
}

void osgEarth::Procedural2::installCanopyShader(osg::StateSet* state)
{
    auto* vp = VirtualProgram::getOrCreate(state);
    vp->setFunction("oe_p2_canopy_crowns",R"glsl(
        struct P2CanopyInstance { mat4 xform; vec2 local_uv; float radius; uint first_lod_cmd_index; };
        layout(binding = 31, std430) readonly buffer P2CanopyInstances { P2CanopyInstance p2CanopyInstances[]; };
        struct P2CanopyVisible { uint source_index; uint lod; float fade; float alpha_cutoff; };
        layout(binding = 0, std430) readonly buffer P2CanopyVisibility { P2CanopyVisible p2CanopyVisible[]; };
        layout(location = 0) in vec3 position;
        layout(location = 5) in vec3 flex;
        layout(location = 1) in vec3 normal;
        out vec3 vp_Normal;
        out vec4 p2CanopyClipDistances;
        // Negative local_uv.x identifies aggregate art; natural placements keep their ordinary rank/seed data.
        void oe_p2_canopy_crowns(inout vec4 vertex)
        {
            uint source = p2CanopyVisible[gl_BaseInstance+gl_InstanceID].source_index;
            P2CanopyInstance instance = p2CanopyInstances[source];
            p2CanopyClipDistances = vec4(1.0);
            if (instance.local_uv.x < 0.0)
            {
                // The shape represents many upright crowns, not a stretched solid mound. Its Z column retains
                // geographic up; remove footprint scale and terrain-fit shear from the foliage lighting frame.
                // The parent's normal matrix still supplies the local-to-world/view rotation afterward.
                vec3 up = normalize(instance.xform[2].xyz);
                vec3 east = normalize(instance.xform[0].xyz-up*dot(instance.xform[0].xyz,up));
                vec3 north = normalize(cross(up,east));
                vp_Normal = normalize(mat3(east,north,up)*normal);
                uint clip = uint(round(instance.local_uv.y));
                if (clip != 0u)
                {
                    uint code = clip-1u;
                    vec2 low = vec2(code & 3u,(code >> 2u) & 3u)*0.25-0.5;
                    vec2 high = low+float((code >> 4u)+1u)*0.25;
                    p2CanopyClipDistances = vec4(position.xy-low,high-position.xy);
                }
                uint mask = uint(-instance.local_uv.x);
                uint crown = uint(round(flex.x));
                // Collapse all triangles of a rejected crown; no per-fragment boundary mask or separate draw is needed.
                if ((mask & (1u << crown)) == 0u) vertex = instance.xform[3];
            }
        }
    )glsl",VirtualProgram::LOCATION_VERTEX_MODEL,0.15f);
    // Apply the same spatial cut in color, depth, and shadow passes, including opaque placeholder materials.
    vp->setFunction("oe_p2_canopy_clip",R"glsl(
        in vec4 p2CanopyClipDistances;
        void oe_p2_canopy_clip(inout vec4 color)
        {
            if (any(lessThan(p2CanopyClipDistances,vec4(0.0)))) discard;
        }
    )glsl",VirtualProgram::LOCATION_FRAGMENT_COLORING,-0.6f);
    installPopulationFadeShader(state);
}

void osgEarth::Procedural2::installPopulationFadeShader(osg::StateSet* state)
{
    auto* vp = VirtualProgram::getOrCreate(state);
    vp->setFunction("oe_p2_canopy_transition",R"glsl(
        #pragma import_defines(OE_CHONK_OPAQUE)
        uniform vec2 oe_chonk_coverage = vec2(0.0,1.0);
        // Complementary intervals share the same pixel pattern in color, depth, and each shadow cascade.
        void oe_p2_canopy_transition(inout vec4 color)
        {
            #ifndef OE_CHONK_OPAQUE
            // The culler puts only complete coverage on the opaque path, preserving its discard-free shader.
            if (oe_chonk_coverage.x <= 0.0 && oe_chonk_coverage.y >= 1.0) return;
            uvec2 pixel = uvec2(gl_FragCoord.xy);
            uint h = pixel.x*0x9e3779b9u ^ pixel.y*0x85ebca6bu;
            h ^= h >> 16u; h *= 0x7feb352du; h ^= h >> 15u;
            float rank = (float(h & 65535u)+0.5)/65536.0;
            if (rank < oe_chonk_coverage.x || rank >= oe_chonk_coverage.y) discard;
            #endif
        }
    )glsl",VirtualProgram::LOCATION_FRAGMENT_COLORING,-0.5f);
}
