#pragma vp_entryPoint oe_logDepth_vert
#pragma vp_location   vertex_clip
#pragma vp_order      0.99

void oe_logDepth_vert(inout vec4 clip)
{
    if (gl_ProjectionMatrix[3][3] == 0.0) // perspective only
    {
        // extract the far plane distance directly from the projection matrix
        // (much cheaper than inverting the matrix per-vertex)
        float FAR = gl_ProjectionMatrix[3][2] / (gl_ProjectionMatrix[2][2] + 1.0);

        float FC = 2.0 / log2(FAR + 1);
        clip.z = (log2(max(1e-6, clip.w+1.0))*FC - 1.0) * clip.w;
    }
}
