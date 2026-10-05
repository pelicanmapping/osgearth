/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "TreeCards.h"
#include <osgEarth/VirtualProgram>
#include <osgEarth/Shaders>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

osg::Vec2f osgEarth::Procedural2::treeCardInstanceUV(const TreeCardCluster& cluster, bool far)
{
    return osg::Vec2f(-float(cluster.count)-(far ? 0.25f : 0.0f),float(cluster.offset));
}

Status osgEarth::Procedural2::createTreeCardTemplate(const Chonk& source, unsigned slots, Chonk::Ptr& output)
{
    output.reset();
    if (slots < 8u || slots > 256u || source._lods.size() != 1u || source._ebo_store.empty() ||
        source._ebo_store.size()%3u || source._ebo_store.size() > 128u*3u)
        return Status(Status::ConfigurationError, "Tree cards require 8..256 slots and a 1..128 triangle coarse model");
    auto result = Chonk::create();
    result->_materialArena = source._materialArena;
    result->_materials = source._materials;
    result->_alphaMaterials = source._alphaMaterials;
    for (unsigned slot=0; slot<slots; ++slot)
    {
        std::map<unsigned,unsigned> remap;
        for (const auto index : source._ebo_store)
        {
            if (index >= source._vbo_store.size())
                return Status(Status::ConfigurationError, "Invalid coarse model index");
            auto found = remap.find(index);
            if (found == remap.end())
            {
                auto vertex = source._vbo_store[index];
                vertex.flex.set(float(slot),0,0);
                found = remap.emplace(index,unsigned(result->_vbo_store.size())).first;
                result->_vbo_store.push_back(vertex);
            }
            result->_ebo_store.push_back(found->second);
        }
    }
    result->_box.set(-1,-1,-1,1,1,1);
    result->_lods.push_back({0,result->_ebo_store.size(),0,FLT_MAX});
    result->_lods.back().alphaTested = result->hasAlphaTest(0,result->_vbo_store.size());
    output = result;
    return Status::NoError;
}

Status osgEarth::Procedural2::buildTreeCardClusters(const std::vector<TreeCardPlacement>& trees,
    const std::vector<Chonk::Ptr>& models, unsigned slots, std::vector<TreeCardCluster>& output,
    std::vector<osg::Vec4f>& records, ProgressCallback* progress)
{
    output.clear(); records.clear();
    if (slots < 8u || slots > 256u || trees.size() > 1000000u)
        return Status(Status::ConfigurationError, "Invalid tree cluster request size");
    std::vector<std::vector<unsigned>> species(models.size());
    for (unsigned i=0; i<trees.size(); ++i)
    {
        const auto& tree = trees[i];
        if (tree.model >= models.size() || !models[tree.model] || !models[tree.model]->_box.valid())
            return Status(Status::ConfigurationError, "Tree cluster model has no valid bound");
        for (unsigned element=0; element<16; ++element)
            if (!std::isfinite(tree.transform.ptr()[element]))
                return Status(Status::ConfigurationError, "Nonfinite tree transform");
        const auto& m = tree.transform;
        if (m(0,3) != 0 || m(1,3) != 0 || m(2,3) != 0 || m(3,3) != 1)
            return Status(Status::ConfigurationError, "Tree cluster requires affine transforms");
        const osg::Vec3d x(m(0,0),m(0,1),m(0,2)), y(m(1,0),m(1,1),m(1,2)), z(m(2,0),m(2,1),m(2,2));
        const double scale2 = x.length2();
        if (scale2 < 1e-12 || std::abs(y.length2()-scale2) > scale2*1e-6 ||
            std::abs(z.length2()-scale2) > scale2*1e-6 || std::abs(x*y) > scale2*1e-6 ||
            std::abs(x*z) > scale2*1e-6 || std::abs(y*z) > scale2*1e-6 || (x^y)*z <= 0.0)
            return Status(Status::ConfigurationError, "Tree cards require rotation and positive uniform scale");
        species[tree.model].push_back(i);
    }
    std::vector<TreeCardCluster> clusters;
    std::vector<osg::Vec4f> data;
    data.reserve(trees.size()*4u);
    //! Recursively bisects occupied space, bounding tree count without imposing a visible placement grid.
    std::function<bool(std::vector<unsigned>&,std::size_t,std::size_t,unsigned)> partition;
    partition = [&](std::vector<unsigned>& indices, std::size_t begin, std::size_t end, unsigned model)
    {
        if (progress && progress->isCanceled()) return false;
        osg::BoundingBoxd roots;
        for (std::size_t i=begin; i<end; ++i) roots.expandBy(trees[indices[i]].transform.getTrans());
        if (end-begin > slots)
        {
            const unsigned axis = roots.xMax()-roots.xMin() >= roots.yMax()-roots.yMin() ? 0u : 1u;
            const auto middle = begin+(end-begin)/2u;
            std::nth_element(indices.begin()+begin,indices.begin()+middle,indices.begin()+end,
                [&](unsigned a, unsigned b)
                {
                    const double x = trees[a].transform(3,axis), y = trees[b].transform(3,axis);
                    return x == y ? a < b : x < y;
                });
            return partition(indices,begin,middle,model) && partition(indices,middle,end,model);
        }
        osg::BoundingBoxd bounds;
        for (std::size_t i=begin; i<end; ++i)
            for (unsigned corner=0; corner<8; ++corner)
                bounds.expandBy(osg::Vec3d(models[model]->_box.corner(corner))*trees[indices[i]].transform);
        const double radius = std::max(0.01,bounds.radius());
        if (!std::isfinite(radius)) return false;
        const auto center = bounds.center();
        const auto frame = osg::Matrixd::scale(radius,radius,radius)*osg::Matrixd::translate(center);
        const auto inverse = osg::Matrixd::inverse(frame);
        TreeCardCluster cluster;
        cluster.transform = osg::Matrixf(frame);
        cluster.model = model; cluster.offset = unsigned(data.size()/4u); cluster.count = unsigned(end-begin);
        clusters.push_back(cluster);
        for (std::size_t i=begin; i<end; ++i)
        {
            const auto m = trees[indices[i]].transform*inverse;
            for (unsigned column=0; column<3; ++column)
                data.emplace_back(float(m(0,column)),float(m(1,column)),float(m(2,column)),float(m(3,column)));
            const auto center = osg::Vec3d(models[model]->_box.center())*m;
            const double scale = osg::Vec3d(m(0,0),m(0,1),m(0,2)).length();
            data.emplace_back(float(center.x()),float(center.y()),float(center.z()),float(models[model]->_box.radius()*scale));
        }
        return true;
    };
    for (unsigned model=0; model<models.size(); ++model)
        if (!species[model].empty() && !partition(species[model],0,species[model].size(),model))
            return Status(Status::ResourceUnavailable, "Tree cluster build canceled or invalid bounds");
    if (progress && progress->isCanceled()) return Status(Status::ResourceUnavailable, "Tree cluster build canceled");
    output.swap(clusters); records.swap(data);
    return Status::NoError;
}

void osgEarth::Procedural2::installTreeCardShader(osg::StateSet* state)
{
    Util::Shaders shaders;
    const auto sizing = ShaderLoader::load(shaders.ChonkScreenSize,shaders);
    VirtualProgram::getOrCreate(state)->setFunction("oe_p2_tree_cards",sizing+R"glsl(
        #pragma import_defines(OE_IS_SHADOW_CAMERA)
        #pragma import_defines(OE_IS_DEPTH_CAMERA)
        #pragma import_defines(OE_CHONK_SSE_ADJUST)
        #pragma import_defines(OE_CHONK_SSE_PIXEL_CUTOFF)
        #pragma import_defines(OE_CHONK_SSE_LOD_ONLY)
        #pragma import_defines(OE_LOD_SCALE_UNIFORM)
        uniform vec3 oe_Camera;
        uniform float oe_sse;
        uniform vec2 oe_chonk_sse_adjust = vec2(0.0,1.0);
        uniform vec4 oe_lod_scale = vec4(1.0);
        uniform vec4 oe_chonk_member_lod = vec4(0.0);
        uniform vec2 oe_chonk_member_range = vec2(0.0);
        uniform float oe_chonk_lod_transition_factor = 0.0;
        #ifdef OE_LOD_SCALE_UNIFORM
        uniform float OE_LOD_SCALE_UNIFORM;
        #else
        #define OE_LOD_SCALE_UNIFORM oe_Camera.z
        #endif
        #ifdef OE_IS_SHADOW_CAMERA
        uniform mat4 oe_shadowToPrimaryMatrix;
        uniform mat4 oe_primaryProjectionMatrix;
        uniform vec2 oe_primaryViewport;
        #endif
        uniform vec3 oe_p2_cluster_debug = vec3(0.0); // mode (off/tiers/clusters), medium enabled, far enabled
        flat out vec4 oe_p2_cluster_color;
        out float oe_fade;
        struct P2CardInstance { mat4 xform; vec2 local_uv; float radius; uint first_lod_cmd_index; };
        layout(binding = 31, std430) readonly buffer P2CardInstances { P2CardInstance p2CardInstances[]; };
        struct P2CardVisible { uint source_index; uint lod; float fade; float alpha_cutoff; };
        layout(binding = 0, std430) readonly buffer P2CardVisibility { P2CardVisible p2CardVisible[]; };
        layout(binding = 3, std430) readonly buffer P2CardTransforms { vec4 p2CardTransforms[]; };
        layout(location = 0) in vec3 position;
        layout(location = 1) in vec3 normal;
        layout(location = 5) in vec3 flex;
        out vec3 vp_Normal;
        out vec3 oe_position_view;
        out vec3 oe_position_vec;
        // Integer avalanche gives each spatial group a repeatable color, never keyed to its visible draw index.
        uint oe_p2_cluster_hash(uint value)
        {
            value ^= value >> 16u; value *= 0x7feb352du;
            value ^= value >> 15u; value *= 0x846ca68bu;
            return value ^ (value >> 16u);
        }
        // Draw-local SSBO data remains stable when Chonk compacts or reorders visible cluster instances.
        void oe_p2_tree_cards(inout vec4 vertex)
        {
            oe_p2_cluster_color = vec4(0.0);
            uint source = p2CardVisible[gl_BaseInstance+gl_InstanceID].source_index;
            P2CardInstance instance = p2CardInstances[source];
            if (instance.local_uv.x >= 0.0) return;
            #if !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA)
            bool far = fract(-instance.local_uv.x) > 0.125;
            if (oe_p2_cluster_debug.x > 0.0 && (far ? oe_p2_cluster_debug.z : oe_p2_cluster_debug.y) > 0.0)
            {
                vec3 tint = far ? vec3(1.0,0.3,0.04) : vec3(0.03,0.8,1.0);
                if (oe_p2_cluster_debug.x > 1.5)
                {
                    uint hash = oe_p2_cluster_hash(uint(round(instance.local_uv.y)) ^
                        floatBitsToUint(instance.xform[3].x) ^ oe_p2_cluster_hash(floatBitsToUint(instance.xform[3].y)));
                    float hue = (far ? 0.0 : 0.46)+float(hash & 255u)/255.0*(far ? 0.15 : 0.29);
                    vec3 rgb = clamp(abs(fract(vec3(hue)+vec3(0.0,2.0/3.0,1.0/3.0))*6.0-3.0)-1.0,0.0,1.0);
                    tint = mix(vec3(1.0),rgb,0.8)*(0.65+0.35*float((hash >> 8u) & 255u)/255.0);
                }
                oe_p2_cluster_color = vec4(tint,1.0);
            }
            #endif
            uint slot = uint(round(flex.x));
            uint count = uint(round(-instance.local_uv.x));
            if (slot >= count) { vertex = instance.xform[3]; return; }
            uint offset = 4u*(uint(round(instance.local_uv.y))+slot);
            vec4 a = p2CardTransforms[offset], b = p2CardTransforms[offset+1u], c = p2CardTransforms[offset+2u];
            if (oe_chonk_member_lod.x > 0.0)
            {
                vec4 sphere = p2CardTransforms[offset+3u];
                vec4 center = gl_ModelViewMatrix*instance.xform*vec4(sphere.xyz,1.0);
                mat4 projection = gl_ProjectionMatrix;
                vec2 viewport = oe_Camera.xy;
                #ifdef OE_IS_SHADOW_CAMERA
                center = oe_shadowToPrimaryMatrix*center;
                projection = oe_primaryProjectionMatrix;
                viewport = oe_primaryViewport;
                #endif
                float pixelError = oe_sse, budget = oe_sse;
                #ifdef OE_CHONK_SSE_ADJUST
                pixelError = max(1.0,pixelError+oe_chonk_sse_adjust.x);
                budget = pixelError*oe_chonk_sse_adjust.y;
                #endif
                float cutoff = oe_chonk_visibility_cutoff(budget,pixelError,
                    oe_chonk_member_lod.z,oe_lod_scale[uint(oe_chonk_member_lod.w)]);
                float fade = oe_chonk_member_fade(center,sphere.w*length(instance.xform[0].xyz),projection,
                    viewport,cutoff,oe_chonk_lod_transition_factor,oe_chonk_member_range,OE_LOD_SCALE_UNIFORM);
                // Collapse rejected slots before rasterization; the surviving card uses the ordinary A2C ramp.
                if (fade <= 0.0) { vertex = instance.xform[3]; return; }
                #if !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA)
                oe_fade *= fade;
                #endif
            }
            vec4 p = vec4(position,1.0);
            vertex = instance.xform*vec4(dot(a,p),dot(b,p),dot(c,p),1.0);
            mat3 rotation = transpose(mat3(a.xyz,b.xyz,c.xyz));
            vp_Normal = normalize(mat3(instance.xform)*rotation*normal);
            oe_position_view = (gl_ModelViewMatrix*vertex).xyz;
            oe_position_vec = gl_NormalMatrix*normalize(mat3(instance.xform)*rotation*position);
        }
    )glsl",VirtualProgram::LOCATION_VERTEX_MODEL,0.2f);
    VirtualProgram::getOrCreate(state)->setFunction("oe_p2_cluster_debug_fragment",R"glsl(
        #pragma import_defines(OE_IS_SHADOW_CAMERA)
        #pragma import_defines(OE_IS_DEPTH_CAMERA)
        flat in vec4 oe_p2_cluster_color;
        // Replace only shaded RGB; retain all cutouts, A2C fades, depth and visibility decisions.
        void oe_p2_cluster_debug_fragment(inout vec4 color)
        {
            #if !defined(OE_IS_SHADOW_CAMERA) && !defined(OE_IS_DEPTH_CAMERA)
            if (oe_p2_cluster_color.a > 0.0) color.rgb = oe_p2_cluster_color.rgb;
            #endif
        }
    )glsl",VirtualProgram::LOCATION_FRAGMENT_LIGHTING,1000001.0f);
}
