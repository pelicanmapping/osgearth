/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "Grass.h"
#include <osgEarth/VirtualProgram>
#include <osg/Geometry>
#include <osg/Geode>
#include <cmath>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

osg::ref_ptr<osg::Node> osgEarth::Procedural2::createGrassPatch(const ScatterGroup& group, unsigned lod)
{
    if (!group.proceduralGrass || group.validate().isError() || lod > 1u) return {};
    const unsigned blades = lod == 0u ? group.grassBlades : (group.grassBlades + 3u) / 4u;
    const unsigned segments = lod == 0u ? 4u : 2u;
    // Includes maximum blade lean, wind, and the coarse representation's compensated width.
    const float reach = group.grassRadius + 0.45f * 1.3f * group.grassHeight + group.grassWind +
        4.0f * group.grassWidth;
    const float height = 1.3f * group.grassHeight;
    auto positions = new osg::Vec3Array();
    auto parameters = new osg::Vec3Array();
    auto uv = new osg::Vec2Array();
    auto indices = new osg::DrawElementsUInt(GL_TRIANGLES);
    for (unsigned blade = 0; blade < blades; ++blade)
    {
        const unsigned first = static_cast<unsigned>(positions->size());
        for (unsigned row = 0; row <= segments; ++row)
        for (unsigned side = 0; side < 2u; ++side)
        {
            const unsigned i = static_cast<unsigned>(positions->size());
            // Chonk derives CPU and GPU bounds from these positions. The shader replaces every vertex.
            positions->push_back(osg::Vec3((i & 1u) ? reach : -reach, (i & 2u) ? reach : -reach,
                (i & 4u) ? height : 0.0f));
            parameters->push_back(osg::Vec3(float(blade), float(row) / segments, side ? 1.0f : -1.0f));
            uv->push_back(osg::Vec2(float(side), float(row) / segments));
        }
        for (unsigned row = 0; row < segments; ++row)
        {
            const unsigned a = first + 2u * row;
            for (unsigned index : {a, a+1u, a+3u, a, a+3u, a+2u}) indices->push_back(index);
        }
    }
    auto geometry = new osg::Geometry();
    geometry->setVertexArray(positions);
    geometry->setTexCoordArray(0, uv);
    parameters->setBinding(osg::Array::BIND_PER_VERTEX);
    geometry->setTexCoordArray(3, parameters); // Chonk's flex attribute carries blade, longitudinal t, and side.
    auto normals = new osg::Vec3Array();
    normals->push_back(osg::Vec3(0,0,1));
    geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
    auto colors = new osg::Vec4Array();
    colors->push_back(osg::Vec4(1,1,1,1));
    geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
    geometry->addPrimitiveSet(indices);
    geometry->setUseDisplayList(false);
    geometry->setUseVertexBufferObjects(true);
    auto node = new osg::Geode();
    node->addDrawable(geometry);
    return node;
}

void osgEarth::Procedural2::installGrassShader(osg::StateSet* state, const ScatterGroup& group)
{
    state->addUniform(new osg::Uniform("oe_p2_grass_shape",
        osg::Vec4(group.grassRadius, group.grassHeight, group.grassWidth, group.grassWind)));
    state->addUniform(new osg::Uniform("oe_p2_grass_coarse_width",
        std::sqrt(float(group.grassBlades) / float((group.grassBlades + 3u) / 4u))));
    VirtualProgram::getOrCreate(state)->setFunction("oe_p2_grass_vertex", R"glsl(
        // Read the original source record, never the unstable compacted draw index.
        struct P2GrassInstance { mat4 xform; vec2 local_uv; float radius; uint first_lod_cmd_index; };
        layout(binding = 31, std430) readonly buffer P2GrassInstances { P2GrassInstance p2GrassInstances[]; };
        struct P2GrassVisible { uint source_index; uint lod; float fade; float alpha_cutoff; };
        layout(binding = 0, std430) readonly buffer P2GrassVisibleInstances { P2GrassVisible p2GrassVisible[]; };
        layout(location = 5) in vec3 flex;
        uniform vec4 oe_p2_grass_shape;
        uniform float oe_p2_grass_coarse_width;
        uniform float osg_FrameTime;
        uniform float oe_p2_grass_time = -1.0; // Optional fixed time for repeatable captures; negative uses the frame clock.
        out vec3 vp_Normal;
        out vec4 vp_Color;
        out vec3 oe_position_view;

        // Independent integer streams preserve blade roots across LOD, batching, and visibility compaction.
        float oe_p2_grass_random(uint seed, uint stream)
        {
            uint h = seed ^ (stream * 0x9e3779b9u);
            h ^= h >> 16u; h *= 0x7feb352du;
            h ^= h >> 15u; h *= 0x846ca68bu;
            h ^= h >> 16u;
            return (float(h >> 8u) + 0.5) / 16777216.0;
        }

        // Expands a shared ribbon template into a rooted, curved blade before view-space lighting and shadowing.
        void oe_p2_grass_vertex(inout vec4 vertex)
        {
            P2GrassVisible visible = p2GrassVisible[gl_BaseInstance + gl_InstanceID];
            P2GrassInstance instance = p2GrassInstances[visible.source_index];
            uint seed = uint(instance.local_uv.y) ^ ((uint(flex.x) + 1u) * 0x85ebca6bu);
            float angle = 6.2831853 * oe_p2_grass_random(seed, 1u);
            float radius = oe_p2_grass_shape.x * sqrt(oe_p2_grass_random(seed, 2u));
            vec2 root = radius * vec2(cos(angle), sin(angle));
            float facing = 6.2831853 * oe_p2_grass_random(seed, 3u);
            vec2 direction = vec2(cos(facing), sin(facing));
            vec3 side = vec3(-direction.y, direction.x, 0.0);
            float height = oe_p2_grass_shape.y * mix(0.7, 1.3, oe_p2_grass_random(seed, 4u));
            float t = flex.y;
            float phase = 6.2831853 * oe_p2_grass_random(uint(instance.local_uv.y), 9u);
            // Bounded demonstration wind; the same frame time and source seed are used by shadow passes.
            float time = oe_p2_grass_time >= 0.0 ? oe_p2_grass_time : osg_FrameTime;
            vec2 wind = normalize(vec2(1.0, 0.35)) * oe_p2_grass_shape.w *
                sin(time * 1.7 + phase);
            vec2 bend = direction * (0.45 * height) + wind;
            float width = oe_p2_grass_shape.z * mix(0.6, 1.4, oe_p2_grass_random(seed, 5u));
            if ((visible.lod & 0xffffu) != 0u) width *= oe_p2_grass_coarse_width;
            vec3 center = vec3(root + bend * t*t, height * t);
            vec3 local = center + side * (flex.z * width * (1.0-t));
            vec3 tangent = vec3(2.0*t*bend, height);
            vec3 normal = normalize(cross(side, tangent));
            vertex = instance.xform * vec4(local, 1.0);
            vp_Normal = normalize(transpose(inverse(mat3(instance.xform))) * normal);
            oe_position_view = (gl_ModelViewMatrix * vertex).xyz;
            float tint = oe_p2_grass_random(seed, 6u);
            vec3 base = mix(vec3(0.13,0.23,0.055), vec3(0.28,0.34,0.075), tint);
            vec3 tip = mix(vec3(0.40,0.53,0.13), vec3(0.64,0.61,0.25), tint);
            vp_Color = vec4(mix(base, tip, sqrt(t)), 1.0);
        }
    )glsl", VirtualProgram::LOCATION_VERTEX_MODEL, 0.2f);
    VirtualProgram::getOrCreate(state)->setFunction("oe_p2_grass_normal", R"glsl(
        in vec3 vp_Normal;
        // Shade both sides of the geometric ribbons using their generated normals.
        void oe_p2_grass_normal(inout vec4 color) { if (!gl_FrontFacing) vp_Normal = -vp_Normal; }
    )glsl", VirtualProgram::LOCATION_FRAGMENT_LIGHTING, -1.0f);
}
