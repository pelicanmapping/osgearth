#pragma vp_entryPoint oe_logDepth_vert
#pragma vp_location   vertex_clip
#pragma vp_order      0.99

out float oe_LogDepth_logz;

void oe_logDepth_vert(inout vec4 clip)
{
    if (gl_ProjectionMatrix[3][3] == 0) // perspective only
    {
        // extract the far plane distance directly from the projection matrix
        // (much cheaper than inverting the matrix per-vertex)
        float FAR = gl_ProjectionMatrix[3][2] / (gl_ProjectionMatrix[2][2] + 1.0);

        const float C = 0.001;
        float FC = 1.0 / log(FAR*C + 1);
        oe_LogDepth_logz = log(max(1e-6, clip.w*C + 1.0))*FC;
        clip.z = (2.0*oe_LogDepth_logz - 1.0)*clip.w;
    }
    else
    {
        oe_LogDepth_logz = -1.0;
    }
}

[break]
#pragma vp_entryPoint oe_logDepth_frag
#pragma vp_location   fragment_lighting
#pragma vp_order      0.99

in float oe_LogDepth_logz;

void oe_logDepth_frag(inout vec4 color)
{
    gl_FragDepth = oe_LogDepth_logz >= 0? oe_LogDepth_logz : gl_FragCoord.z;
}
