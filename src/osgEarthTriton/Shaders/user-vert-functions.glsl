// user-vert-functions.glsl
#extension GL_ARB_gpu_shader5: enable

float user_get_depth( in vec3 worldPos )
{
    return 1000.0;
}

#ifdef DOUBLE_PRECISION

dvec3 user_horizon(in dvec3 intersect)
{
    return intersect;
}

#else

vec3 user_horizon(in vec3 intersect)
{
    return intersect;
}

#endif

/* You may use this hook to set any varying parameters you need for the user-functions.glsl fragment program.
   provided are the ocean vertex in world, eye, and projected coordinates. */
void user_intercept(in vec3 worldPosition, in vec3 localPosition, in vec4 eyePosition, in vec4 projectedPosition)
{

}


// osgEarth log depth buffer
uniform mat4 osg_ProjectionMatrix;
vec4 oe_logdepth_vert(in vec4 clip)
{
    if (osg_ProjectionMatrix[3][3] == 0) // perspective only
    {
        // extract the far plane distance directly from the projection matrix
        // (much cheaper than inverting the matrix per-vertex)
        float FAR = osg_ProjectionMatrix[3][2] / (osg_ProjectionMatrix[2][2] + 1.0);
        float FC = 2.0 / log2(FAR + 1);
        clip.z = (log2(max(1e-6, clip.w+1.0))*FC - 1.0) * clip.w;
    }
    return clip;
}

// Provides a point to override the final value of gl_Position.
// Useful for implementing logarithmic depth buffers etc.
vec4 overridePosition(in vec4 position)
{
    return oe_logdepth_vert(position);
}
