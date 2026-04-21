#pragma vp_name       REX Engine TCS
#pragma vp_entryPoint oe_rex_TCS
#pragma vp_location   tess_control
#pragma vp_order      last

#pragma include RexEngine.GL4.glsl

// Vertex Markers:
#define VERTEX_VISIBLE  1
#define VERTEX_BOUNDARY 2
#define VERTEX_HAS_ELEVATION 4
#define VERTEX_SKIRT 8
#define VERTEX_CONSTRAINT 16

layout(vertices=3) out;

uniform float oe_terrain_tessResolutionMeters = 5000;
uniform float oe_terrain_tessMinLevel = 0.1;
uniform float oe_terrain_tessMaxLevel = 7.0;

varying vec4 vp_Vertex;
varying vec3 vp_Normal;
int oe_terrain_vertexMarker;

void VP_LoadVertex(in int);
float oe_terrain_getElevation();

float quantize(float t)
{
    return floor(t + 0.5);
}

// note: we are in MODEL space
void oe_rex_TCS()
{
    if (gl_InvocationID == 0)
    {
        if ((oe_terrain_vertexMarker & VERTEX_CONSTRAINT) != 0)
        {
            // For boundary vertices, we want to ensure they are not tessellated away.
            // Set a high tessellation level to preserve them.
            gl_TessLevelOuter[0] = 1.0;
            gl_TessLevelOuter[1] = 1.0;
            gl_TessLevelOuter[2] = 1.0;
            gl_TessLevelInner[0] = 1.0;
        }
        else
        {
            mat4 mvm = oe_tile[oe_tileID].modelViewMatrix;
            vec4 p[3];
            for (int i = 2; i >= 0; --i)
            {
                VP_LoadVertex(i);
                p[i] = mvm * vp_Vertex; // elevation sampling not really necessary for this
            }

            // Edge lengths in world meters (assuming your world units are meters)
            float L01 = length(p[1] - p[0]);
            float L12 = length(p[2] - p[1]);
            float L20 = length(p[0] - p[2]);

            // Per-edge tess factors (recommended)
            float t01 = clamp(quantize(L01 / max(oe_terrain_tessResolutionMeters, 1e-6)), oe_terrain_tessMinLevel, oe_terrain_tessMaxLevel);
            float t12 = clamp(quantize(L12 / max(oe_terrain_tessResolutionMeters, 1e-6)), oe_terrain_tessMinLevel, oe_terrain_tessMaxLevel);
            float t20 = clamp(quantize(L20 / max(oe_terrain_tessResolutionMeters, 1e-6)), oe_terrain_tessMinLevel, oe_terrain_tessMaxLevel);

            // IMPORTANT: gl_TessLevelOuter[i] corresponds to edge opposite vertex i:
            // Outer[0] -> edge (1,2)
            // Outer[1] -> edge (2,0)
            // Outer[2] -> edge (0,1)
            gl_TessLevelOuter[0] = t12;
            gl_TessLevelOuter[1] = t20;
            gl_TessLevelOuter[2] = t01;

            // Inner level: use something representative (avg or max)
            float tInner = clamp(quantize((t01 + t12 + t20) / 3.0), oe_terrain_tessMinLevel, oe_terrain_tessMaxLevel);
            gl_TessLevelInner[0] = tInner;
        }
    }
}


[break]
#pragma vp_name       REX Engine TES
#pragma vp_entryPoint oe_rex_TES
#pragma vp_location   tess_eval

// osgEarth terrain is always CCW winding
layout(triangles, equal_spacing, ccw) in;

// Internal helpers:
void VP_Interpolate3();
void VP_EmitVertex();

float VP_Interpolate3(float a, float b, float c) 
{
    return dot(gl_TessCoord.xyz, vec3(a,b,c));
}

vec2 VP_Interpolate3(vec2 a, vec2 b, vec2 c) 
{
    return vec2(
        dot(gl_TessCoord.xyz, vec3(a.x,b.x,c.x)),
        dot(gl_TessCoord.xyz, vec3(a.y,b.y,c.y)));
}

vec3 VP_Interpolate3(vec3 a, vec3 b, vec3 c) 
{
    return vec3(
        dot(gl_TessCoord.xyz, vec3(a.x,b.x,c.x)),
        dot(gl_TessCoord.xyz, vec3(a.y,b.y,c.y)),
        dot(gl_TessCoord.xyz, vec3(a.z,b.z,c.z)));
}

vec4 VP_Interpolate3(vec4 a, vec4 b, vec4 c) 
{
    return vec4(
        dot(gl_TessCoord.xyz, vec3(a.x,b.x,c.x)),
        dot(gl_TessCoord.xyz, vec3(a.y,b.y,c.y)),
        dot(gl_TessCoord.xyz, vec3(a.z,b.z,c.z)),
        dot(gl_TessCoord.xyz, vec3(a.w,b.w,c.w)));
}

varying vec3 oe_UpVectorView;
varying vec3 vp_Normal;

void oe_rex_TES()
{
    VP_Interpolate3();
    VP_EmitVertex();
}

