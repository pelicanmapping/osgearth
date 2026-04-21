out vec2 oe_Text_texCoord;

#pragma vp_entryPoint oe_Text_VS
#pragma vp_location   vertex_view

void oe_Text_VS(inout vec4 position)
{
    oe_Text_texCoord = gl_MultiTexCoord0.xy;
}


[break]
#ifdef GL_ES
    #extension GL_OES_standard_derivatives : enable
    #ifndef GL_OES_standard_derivatives
        #undef SIGNED_DISTANCE_FIELD
    #endif
#endif

#if !defined(GL_ES)
    #if __VERSION__>=400
        #define osg_TextureQueryLOD textureQueryLod
    #else
        #extension GL_ARB_texture_query_lod : enable
        #ifdef GL_ARB_texture_query_lod
            #define osg_TextureQueryLOD textureQueryLOD
        #endif
    #endif
#endif

#pragma vp_entryPoint oe_Text_FS
#pragma vp_location   fragment_coloring

#pragma import_defines(BACKDROP_COLOR, SHADOW, OUTLINE)
#pragma import_defines(SIGNED_DISTANCE_FIELD, TEXTURE_DIMENSION, GLYPH_DIMENSION)
#pragma import_defines(OSGTEXT_GLYPH_ALPHA_FORMAT_IS_RED)

#if __VERSION__>=130
    #define TEXTURE texture
    #define TEXTURELOD textureLod
#else
    #define TEXTURE texture2D
    #define TEXTURELOD texture2DLod
#endif


//#if !defined(GL_ES) && __VERSION__>=130
#ifdef OSGTEXT_GLYPH_ALPHA_FORMAT_IS_RED
    #define ALPHA r
    #define SDF g
#else
    #define ALPHA a
    #define SDF r
#endif


uniform sampler2D glyphTexture;

in vec2 oe_Text_texCoord;
vec4 vertexColor;

#ifndef TEXTURE_DIMENSION
    #define TEXTURE_DIMENSION float(1024.0)
#endif

#ifndef GLYPH_DIMENSION
    #define GLYPH_DIMENSION float(32.0)
#endif

#ifdef SIGNED_DISTANCE_FIELD

float distanceFromEdge(vec2 tc)
{
    float center_alpha = TEXTURELOD(glyphTexture, tc, 0.0).SDF;
    if (center_alpha==0.0) return -1.0;

    //float distance_scale = (1.0/4.0)*1.41;
    float distance_scale = (1.0/6.0)*1.41;
    //float distance_scale = (1.0/8.0)*1.41;

    return (center_alpha-0.5)*distance_scale;
}

vec4 distanceFieldColorSample(float edge_distance, float blend_width, float  blend_half_width)
{
#ifdef OUTLINE
    float outline_width = OUTLINE*0.5;
    if (edge_distance>blend_half_width)
    {
        return vertexColor;
    }
    else if (edge_distance>-blend_half_width)
    {
        return mix(vertexColor, vec4(BACKDROP_COLOR.rgb, BACKDROP_COLOR.a*vertexColor.a), smoothstep(0.0, 1.0, (blend_half_width-edge_distance)/(blend_width)));
    }
    else if (edge_distance>(blend_half_width-outline_width))
    {
        return vec4(BACKDROP_COLOR.rgb, BACKDROP_COLOR.a*vertexColor.a);
    }
    else if (edge_distance>-(outline_width+blend_half_width))
    {
        return vec4(BACKDROP_COLOR.rgb, vertexColor.a * ((blend_half_width+outline_width+edge_distance)/blend_width));
    }
    else
    {
        return vec4(0.0, 0.0, 0.0, 0.0);
    }
#else
    if (edge_distance>blend_half_width)
    {
        return vertexColor;
    }
    else if (edge_distance>-blend_half_width)
    {
        return vec4(vertexColor.rgb, vertexColor.a * smoothstep(1.0, 0.0, (blend_half_width-edge_distance)/(blend_width)));
    }
    else
    {
        return vec4(0.0, 0.0, 0.0, 0.0);
    }
#endif
}

vec4 textColor(vec2 src_texCoord)
{
    float sample_distance_scale = 0.75;
    vec2 dx = dFdx(src_texCoord)*sample_distance_scale;
    vec2 dy = dFdy(src_texCoord)*sample_distance_scale;


    float distance_across_pixel = length(dx+dy)*(TEXTURE_DIMENSION/GLYPH_DIMENSION);

    // compute the appropriate number of samples required to avoid aliasing.
    int maxNumSamplesAcrossSide = 4;

    int numSamplesX = int(TEXTURE_DIMENSION * length(dx));
    int numSamplesY = int(TEXTURE_DIMENSION * length(dy));
    if (numSamplesX<2) numSamplesX = 2;
    if (numSamplesY<2) numSamplesY = 2;
    if (numSamplesX>maxNumSamplesAcrossSide) numSamplesX = maxNumSamplesAcrossSide;
    if (numSamplesY>maxNumSamplesAcrossSide) numSamplesY = maxNumSamplesAcrossSide;


    vec2 delta_tx = dx/float(numSamplesX-1);
    vec2 delta_ty = dy/float(numSamplesY-1);

    float numSamples = float(numSamplesX)*float(numSamplesY);
    float scale = 1.0/numSamples;
    vec4 total_color = vec4(0.0,0.0,0.0,0.0);

    float blend_width = 1.5*distance_across_pixel/numSamples;
    float blend_half_width = blend_width*0.5;

    // check whether fragment is wholly within or outwith glyph body+outline
    float cd = distanceFromEdge(src_texCoord); // central distance (distance from center to edge)
    if (cd-blend_half_width>distance_across_pixel) return vertexColor; // pixel fully within glyph body

    #ifdef OUTLINE
    float outline_width = OUTLINE*0.5;
    if ((-cd-outline_width-blend_half_width)>distance_across_pixel) return vec4(0.0, 0.0, 0.0, 0.0); // pixel fully outside outline+glyph body
    #else
    if (-cd-blend_half_width>distance_across_pixel) return vec4(0.0, 0.0, 0.0, 0.0); // pixel fully outside glyph body
    #endif


    // use multi-sampling to provide high quality antialised fragments
    vec2 origin = src_texCoord - dx*0.5 - dy*0.5;
    for(;numSamplesY>0; --numSamplesY)
    {
        vec2 pos = origin;
        int numX = numSamplesX;
        for(;numX>0; --numX)
        {
            vec4 c = distanceFieldColorSample(distanceFromEdge(pos), blend_width, blend_half_width);
            total_color = total_color + c * c.a;
            pos += delta_tx;
        }
        origin += delta_ty;
    }

    if (total_color.a == 0.0)
        return vec4(0.0, 0.0, 0.0, 0.0);

    total_color.rgb /= total_color.a;
    total_color.a *= scale;

    return total_color;
}

#else

vec4 textColor(vec2 src_texCoord)
{

#ifdef OUTLINE

    float alpha = TEXTURE(glyphTexture, src_texCoord).ALPHA;
    float delta_tc = 1.6*OUTLINE*GLYPH_DIMENSION/TEXTURE_DIMENSION;

    float outline_alpha = alpha;
    vec2 origin = src_texCoord-vec2(delta_tc*0.5, delta_tc*0.5);

    float numSamples = 3.0;
    delta_tc = delta_tc/(numSamples-1.0);

    float background_alpha = 1.0;

    for(float i=0.0; i<numSamples; ++i)
    {
        for(float j=0.0; j<numSamples; ++j)
        {
            float local_alpha = TEXTURE(glyphTexture, origin + vec2(i*delta_tc, j*delta_tc)).ALPHA;
            outline_alpha = max(outline_alpha, local_alpha);
            background_alpha = background_alpha * (1.0-local_alpha);
        }
    }

    #ifdef osg_TextureQueryLOD
        float mipmapLevel = osg_TextureQueryLOD(glyphTexture, src_texCoord).x;
        if (mipmapLevel<1.0)
        {
            outline_alpha = mix(1.0-background_alpha, outline_alpha, mipmapLevel/1.0);
        }
    #endif

    if (outline_alpha<alpha) outline_alpha = alpha;
    if (outline_alpha>1.0) outline_alpha = 1.0;

    if (outline_alpha==0.0) return vec4(0.0, 0.0, 0.0, 0.0); // outside glyph and outline

    vec4 color = mix(BACKDROP_COLOR, vertexColor, smoothstep(0.0, 1.0, alpha));
    color.a = vertexColor.a * smoothstep(0.0, 1.0, outline_alpha);

    return color;

#else

    float alpha = TEXTURE(glyphTexture, src_texCoord).ALPHA;
    if (alpha==0.0) return vec4(0.0, 0.0, 0.0, 0.0);
    return vec4(vertexColor.rgb, vertexColor.a * alpha);

#endif
}

#endif

void oe_Text_FS(inout vec4 color)
{
    float originalAlpha = color.a;
    vertexColor = vec4(color.rgb, 1.0);

    if (oe_Text_texCoord.x<0.0 && oe_Text_texCoord.y<0.0)
    {
        return;
    }

#ifdef SHADOW
    float scale = -1.0*GLYPH_DIMENSION/TEXTURE_DIMENSION;
    vec2 delta_tc = SHADOW*scale;
    vec4 shadow_color = textColor(oe_Text_texCoord+delta_tc);
    shadow_color.rgb = BACKDROP_COLOR.rgb;

    vec4 glyph_color = textColor(oe_Text_texCoord);

    // lower the alpha_power value the greater the saturation, no need to be so aggressive with SDF than GREYSCALE
    #if SIGNED_DISTANCE_FIELD
        float alpha_power = 0.6;
    #else
        float alpha_power = 0.5;
    #endif

    // over saturate the alpha values to make sure the font and it's shadow are clear
    shadow_color.a = pow(shadow_color.a, alpha_power);
    glyph_color.a = pow(glyph_color.a, alpha_power);
    vec4 clr = mix(shadow_color, glyph_color, glyph_color.a);
#else
    vec4 clr = textColor(oe_Text_texCoord);
#endif

    if (clr.a==0.0) discard;

    color = clr;
    color.a *= originalAlpha;
}
