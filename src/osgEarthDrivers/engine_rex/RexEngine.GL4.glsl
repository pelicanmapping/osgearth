#define MAX_NUM_SHARED_SAMPLERS 16

struct oe_rex_Shared {
    vec2 morphConstants[49];
    float padding[2];
};
struct oe_rex_Tile {
    vec4 tileKey;
    mat4 modelViewMatrix;
    mat4 colorMat;
    mat4 parentMat;
    mat4 elevMat;
    mat4 normalMat;
    mat4 landcoverMat;
    mat4 sharedMat[MAX_NUM_SHARED_SAMPLERS];
    int colorIndex;
    int parentIndex;
    int elevIndex;
    int normalIndex;
    int landcoverIndex;
    int sharedIndex[MAX_NUM_SHARED_SAMPLERS];
    int drawOrder;
    float elevMin;
    float elevMax;
    float tileSpan;
    float padding[3];
    //float padding[2];
};
layout(binding = 29, std430) readonly buffer RexTextureArena {
    uint64_t oe_terrain_tex[];
};
layout(binding = 30, std430) readonly buffer RexSharedDataBuffer {
    oe_rex_Shared oe_shared;
};
layout(binding = 31, std430) readonly buffer RexTileBuffer {
    oe_rex_Tile oe_tile[];
};

#if defined(VP_STAGE_VERTEX) || defined(VP_STAGE_TESSEVALUATION)
flat out int oe_tileID;
#elif defined(VP_STAGE_FRAGMENT)
flat in int oe_tileID;
#else
int oe_tileID;
#endif
