
#ifdef OPENGL32
out vec4 fragColor;
#endif

// Light, view, and normal vectors are all in world space.
// This function may be used to modify the ambient, diffuse, and specular light computed by Triton's fragment shaders.
void user_lighting(in vec3 L
                   , in vec3 vVertex_World_Space, in vec3 vNormal_World_Space
                   , in vec4 vVertex_Projection_Space
                   , in float shininess
                   , inout vec3 ambient, inout vec3 diffuse, inout vec3 specular)
{

}

// View in world space. The final waterColor will be alpha-blended with the fogColor by fogBlend.
void user_fog(in vec3 vNorm, inout vec4 waterColor, inout vec4 fogColor, inout float fogBlend)
{

}

// The final computed color is normally just clamped to (0,1), but you may override this behavior here.
void user_tonemap(in vec4 preToneMapColor, inout vec4 postToneMapColor)
{

}

// Override spray particle colors. Note these are drawn with an additive blending
// mode, so darker colors just result in more transparent particles. You're given
// the position in eye and world coordinates, and the texture lookup results for the
// spray particle. "Transparency" represents the overall transparency of the particles.
// "Decay" is used to fade out the particle over time. The final output color should
// be written to additive Color.
// The default implementation is:
// additiveColor = texColor * lightColor * decay * transparency

void user_particle_color(in vec3 vEye, in vec3 vWorld, in vec4 texColor,
                         in vec4 lightColor, in float transparency, in float decay,
                         inout vec4 additiveColor)
{

}

#ifdef DECAL_SHADER
// Override the shading of volumetric decals on the water. You are given the texture lookup value,
// alpha value for the decal, and light color for the decal. The incoming default finalColor
// is the textureColor times the lightColor, with the alpha component further multiplied by alpha.
void user_decal_color(in vec4 textureColor, in float alpha, in vec4 lightColor, inout vec4 finalColor)
{

}

// Override the depth calculation for the decal fragment. May be needed if you are using a
// logarithmic depth buffer, as the depth texture used for decals is always linear.
// See also the overridePosition function in user-vert-functions.glsl if you are manipulating
// depth values.
void user_decal_depth(inout float depth)
{

}
#endif

//adjust the reflection color prior to it being used by triton.
void user_reflection_adjust(in vec4 envColor, in vec4 planarColor, in float planarReflectionBlend, inout vec4 reflectedColor)
{
}

// Shadows the fragment; 1.0 = no shadow, 0 = black.
float user_cloud_shadow_fragment()
{
    return 1.0;
}

// Adjust the water diffuse to include color from breaking waves, propeller wash, and some refraction
void user_diffuse_color( inout vec3 Cdiffuse, in vec3 CiNoLight, in vec3 Cwash, in vec4 reflectedColor,
                         in float reflectivity, in vec3 nNorm )
{

}

// osgEarth layer opacity uniform
uniform float oe_VisibleLayer_opacityUniform;

// Output to MRT
void writeFragmentData(in vec4 finalColor, in vec4 Cdiffuse, in vec3 lightColor, in vec3 nNorm )
{
    finalColor.a *= oe_VisibleLayer_opacityUniform;
    
#ifdef OPENGL32
    fragColor = finalColor;
#else
    gl_FragColor = finalColor;
#endif
}
