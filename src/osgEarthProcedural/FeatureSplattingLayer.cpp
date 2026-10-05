#include "FeatureSplattingLayer"
#include <osgEarth/Capabilities>
#include <osgEarth/Registry>
#include <osgEarth/Shaders>
#include <osgEarth/ShaderLoader>
#include <osgEarth/TerrainEngineNode>

using namespace osgEarth;
using namespace osgEarth::Procedural;

REGISTER_OSGEARTH_LAYER(featuresplatting, osgEarth::Procedural::FeatureSplattingLayer);

#undef LC
#define LC "[FeatureSplattingLayer] "

namespace
{
    const char* vs = R"(
        vec4 oe_layer_tilec;
        out vec2 oe_feature_splatting_uv;
        vec2 oe_terrain_scaleCoordsToRefLOD(in vec2 tc, in float refLOD); // SDK

        void oe_feature_splatting_vs(inout vec4 vertex)
        {
            vec2 uv = oe_terrain_scaleCoordsToRefLOD(oe_layer_tilec.st, 22);
            oe_feature_splatting_uv = uv;
        }
    )";

    const char* fs = R"(
        #pragma import_defines(OE_HAS_ALBEDO_TEX, OE_HAS_NORMAL_TEX, OE_HAS_PBR_TEX, OE_HAS_OCCLUSION_TEX)
        #pragma include PBRMaterial.glsl

        in vec2 oe_feature_splatting_uv;
        in float oe_layer_opacity;
        in vec3 vp_VertexView;
        in vec3 oe_UpVectorView;

        uniform sampler2D oe_albedo_tex;
        uniform sampler2D oe_normal_tex;
        uniform sampler2D oe_pbr_tex;
        uniform sampler2D oe_occlusion_tex;
        uniform vec4 oe_feature_splatting_layoutAndFactors;

        uniform float oe_normal_boost = 1.0;

        uniform float uPOM = 1.0;
        uniform float uHeightScale = 0.010;
   
        mat3 oe_normalMapTBN;
        vec3 vp_Normal;
        struct OE_PBR { float displacement, roughness, ao, metal; } oe_pbr;

        // Andrey Mishkinis height+alpha blender.
        // https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting
        float heightAndEffectMix(in float h1, in float a1, in float h2, in float a2)
        {
            const float oe_displacement_depth = 0.1;
            float ma = max(h1 + a1, h2 + a2) - oe_displacement_depth;
            float b1 = max(h1 + a1 - ma, 0.0);
            float b2 = max(h2 + a2 - ma, 0.0);
            return b2 / (b1 + b2);
        }

        // Only DRAM contains displacement; other layouts must not drive parallax.
        float heightSample(vec2 uv)
        {
          #ifdef OE_HAS_PBR_TEX
            if (oe_feature_splatting_layoutAndFactors.x == OE_PBR_LAYOUT_DRAM)
                return texture(oe_pbr_tex, uv).r;
          #endif
            return 0.0;
        }

        // Sample optional maps, then normalize packing and factors for terrain blending.
        vec4 sampleMaterial(vec2 uv)
        {
            vec4 texel = oe_pbr_default(oe_feature_splatting_layoutAndFactors.x);
          #ifdef OE_HAS_PBR_TEX
            texel = texture(oe_pbr_tex, uv);
          #endif
            float ao = -1.0;
          #ifdef OE_HAS_OCCLUSION_TEX
            ao = texture(oe_occlusion_tex, uv).r;
          #endif
            return oe_pbr_decode(texel, oe_feature_splatting_layoutAndFactors, ao);
        }

        vec2 POM(in vec2 baseUV, in vec3 viewDirTS)
        {
            const float uMaxLayers = 32.0;
            const float uMinLayers = 8.0;

            // Avoid division blowups at grazing angles
            viewDirTS = normalize(viewDirTS);
            if (viewDirTS.z < 0.0) viewDirTS = -viewDirTS;
            float ndotv = clamp(viewDirTS.z, 0.05, 1.0);

            // More layers at grazing angles, fewer when looking straight on
            float numLayers = mix(uMaxLayers, uMinLayers, ndotv);
            float layerDepth = 1.0 / numLayers;

            // POM ray step in UV space.
            // Note: -viewDirTS.xy because we march *into* the surface along view direction.
            vec2  P = (viewDirTS.xy / viewDirTS.z) * uHeightScale;
            vec2  deltaUV = -P / numLayers;

            vec2  uv = baseUV;
            float currLayerDepth = 0.0;
            float currHeight = heightSample(uv);

            // Walk until the accumulated depth exceeds the heightfield value.
            // We interpret "height" as surface height; "depth" increases as we march inward.
            while (currLayerDepth < currHeight)
            {
                uv += deltaUV;
                currLayerDepth += layerDepth;
                currHeight = heightSample(uv);

                // Optional early-out if UV goes out of range
                //if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
                //    return baseUV;
            }

            // We stepped past the surface; do linear interpolation between last two steps
            vec2 prevUV = uv - deltaUV;
            float prevDepth = currLayerDepth - layerDepth;
            float prevHeight = heightSample(prevUV);

            // Solve for intersection fraction between prev and curr along the segment
            // We want depth == height. Consider function f = height - depth.
            float f0 = prevHeight - prevDepth;
            float f1 = currHeight - currLayerDepth;
            float t = f0 / (f0 - f1); // in [0,1] typically

            vec2 uvLinear = mix(prevUV, uv, clamp(t, 0.0, 1.0));

            // Binary refinement (a few steps is usually enough)
            vec2 a = prevUV;
            vec2 b = uv;
            vec2 mid = uvLinear;

            float depthA = prevDepth;
            float depthB = currLayerDepth;

            for (int i = 0; i < 5; ++i)
            {
                mid = (a + b) * 0.5;
                float depthMid = (depthA + depthB) * 0.5;
                float hMid = heightSample(mid);

                if (depthMid < hMid) {
                    a = mid;
                    depthA = depthMid;
                } else {
                    b = mid;
                    depthB = depthMid;
                }
            }

            return mid;
        }

        void oe_feature_splatting_fs(inout vec4 color)
        {
            vec2 uv = oe_feature_splatting_uv;
            float unquantized_blend = 1.0;
            float quantized_blend = 1.0;
            float displacement = 0.0;

            // parallaxify!  
            vec3 B = gl_NormalMatrix * vec3(0, 1, 0); // east in local tile
            vec3 N = vp_Normal; // up in local tile
            vec3 T = cross(B, N);
            mat3 TBN = mat3(normalize(T), normalize(B), -normalize(N)); // tangent->view (The -N ?? bug in the POM code??)

            vec3 viewVectorTS = transpose(TBN) * normalize(-vp_VertexView);
            
          #ifdef OE_HAS_PBR_TEX
            if (oe_feature_splatting_layoutAndFactors.x == OE_PBR_LAYOUT_DRAM)
                uv = POM(uv, viewVectorTS);
          #endif

          #ifdef OE_HAS_NORMAL_TEX            
            vec3 normalTS = texture(oe_normal_tex, uv).xyz * 2.0 - 1.0;
            normalTS.y = -normalTS.y; // adjust for different TBN space conventions
            normalTS.xy *= oe_normal_boost;
            vp_Normal = normalize(oe_normalMapTBN * normalize(normalTS));
          #endif

            vec4 pbr = sampleMaterial(uv);
            displacement = pbr.r;

          #ifdef OE_HAS_ALBEDO_TEX
            const float ground_height = 0.0;
            vec4 albedo = texture(oe_albedo_tex, uv);
            float albedo_blend = 1.0 - color.r;
            unquantized_blend = heightAndEffectMix(ground_height, 1.0, displacement, albedo_blend) * oe_layer_opacity;
            quantized_blend = smoothstep(0.5, 0.6, unquantized_blend);
            color = vec4(albedo.rgb, quantized_blend);
          #endif

            oe_pbr.displacement = mix(oe_pbr.displacement, displacement, quantized_blend);
            oe_pbr.roughness = mix(oe_pbr.roughness, pbr.g, quantized_blend);
            oe_pbr.ao = mix(0.0, pbr.b, unquantized_blend);
            oe_pbr.metal = mix(oe_pbr.metal, pbr.a, quantized_blend);
        }
    )";
}



void
FeatureSplattingLayer::init()
{
    super::init();
    setRenderType(RENDERTYPE_TERRAIN_SURFACE);
}

void
FeatureSplattingLayer::prepareForRendering(TerrainEngine* engine)
{
    super::prepareForRendering(engine);

    if (Capabilities::get().supportsInt64() == false)
    {
        setStatus(Status::ResourceUnavailable, "GLSL int64 support required but not available");
        OE_WARN << LC << getStatus().message() << std::endl;
        return;
    }

    if (Capabilities::get().getGLSLVersion() < 4.6f || Capabilities::get().supportsNVGL() == false)
    {
        setStatus(Status::ResourceUnavailable, "GLSL 4.6+ required but not available");
        OE_WARN << LC << getStatus().message() << std::endl;
        return;
    }

    //TODO error check these.
    engine->getResources()->reserveTextureImageUnitForLayer(_res_albedo, this);
    engine->getResources()->reserveTextureImageUnitForLayer(_res_normal, this);
    engine->getResources()->reserveTextureImageUnitForLayer(_res_pbr, this);
    const auto& material = options().material().value();
    if ((material.aoImage || material.ao().isSet()) &&
        !engine->getResources()->reserveTextureImageUnitForLayer(_res_occlusion, this))
    {
        setStatus(Status::ResourceUnavailable, "Failed to reserve occlusion texture image unit");
        return;
    }

    buildStateSet();
}

void
FeatureSplattingLayer::buildStateSet()
{
    // fire up the shaders:
    auto* ss = getOrCreateStateSet();
    auto vp = VirtualProgram::getOrCreate(ss);
    vp->setFunction("oe_feature_splatting_vs", vs, ShaderComp::LOCATION_VERTEX_VIEW);
    osgEarth::Util::Shaders shaders;
    shaders.add("FeatureSplatting.frag", fs);
    vp->setFunction("oe_feature_splatting_fs", ShaderLoader::load("FeatureSplatting.frag", shaders),
        ShaderComp::LOCATION_FRAGMENT_COLORING);

    // load up the PBR material:
    osg::ref_ptr<PBRTexture> tex = new PBRTexture();
    auto load_status = tex->load(options().material().value(), getReadOptions());
    if (load_status.isError())
    {
        setStatus(load_status);
        return;
    }

    ss->getOrCreateUniform("oe_feature_splatting_layoutAndFactors", osg::Uniform::FLOAT_VEC4)->set(tex->layoutAndFactors);

    tex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
    tex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

    if (tex->albedo.valid())
    {
        ss->setTextureAttribute(_res_albedo.unit(), tex->albedo);
        ss->addUniform(new osg::Uniform("oe_albedo_tex", _res_albedo.unit()));
        ss->setDefine("OE_HAS_ALBEDO_TEX");
    }

    if (tex->normal.valid())
    {
        ss->setTextureAttribute(_res_normal.unit(), tex->normal);
        ss->addUniform(new osg::Uniform("oe_normal_tex", _res_normal.unit()));
        ss->setDefine("OE_HAS_NORMAL_TEX");
    }

    if (tex->pbr.valid())
    {
        ss->setTextureAttribute(_res_pbr.unit(), tex->pbr);
        ss->addUniform(new osg::Uniform("oe_pbr_tex", _res_pbr.unit()));
        ss->setDefine("OE_HAS_PBR_TEX");
    }
    if (tex->occlusion.valid())
    {
        ss->setTextureAttribute(_res_occlusion.unit(), tex->occlusion);
        ss->addUniform(new osg::Uniform("oe_occlusion_tex", _res_occlusion.unit()));
        ss->setDefine("OE_HAS_OCCLUSION_TEX");
    }
}
