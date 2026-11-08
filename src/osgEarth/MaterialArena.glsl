#ifndef OE_MATERIAL_ARENA_GLSL
#define OE_MATERIAL_ARENA_GLSL
// Shared 80-byte std430 record, matching MaterialArena::GPU.
struct ChonkMaterial
{
    uint64_t albedo;
    uint64_t normal;
    uint64_t pbr;
    uint64_t material1;
    uint64_t material2;
    ivec2 extended;
    uint64_t occlusion;
    vec4 layoutAndFactors;
};
layout(binding = 2, std430) readonly buffer ChonkMaterialArena {
    ChonkMaterial chonkMaterials[];
};

#endif
