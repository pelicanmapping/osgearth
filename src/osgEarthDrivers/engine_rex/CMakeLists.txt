
set(TARGET_GLSL
    RexEngine.vert.glsl
    RexEngine.elevation.glsl
    RexEngine.gs.glsl
    RexEngine.ImageLayer.glsl
    RexEngine.NormalMap.glsl
    RexEngine.Morphing.glsl
    RexEngine.Tessellation.glsl
    RexEngine.SDK.glsl
    
    RexEngine.vert.GL4.glsl
    RexEngine.ImageLayer.GL4.glsl
    RexEngine.NormalMap.GL4.glsl
    RexEngine.Tessellation.GL4.glsl
    RexEngine.SDK.GL4.glsl
    RexEngine.GL4.glsl)

set(TARGET_IN
    Shaders.cpp.in)

set(SHADERS_CPP "${CMAKE_CURRENT_BINARY_DIR}/AutoGenShaders.cpp")

configure_shaders(
    Shaders.cpp.in
    ${SHADERS_CPP}
    ${TARGET_GLSL} )

# SET(TARGET_LIBRARIES_VARS OSG_LIBRARY OSGDB_LIBRARY OSGUTIL_LIBRARY OSGTEXT_LIBRARY OPENTHREADS_LIBRARY OPENGL_gl_LIBRARY) 

SET(TARGET_SRC
    CreateTileImplementation.cpp
    DrawState.cpp
    DrawTileCommand.cpp
    GeometryPool.cpp
    RexTerrainEngineNode.cpp
    RexTerrainEngineDriver.cpp
    LayerDrawable.cpp
    LoadTileData.cpp
	SelectionInfo.cpp
    SurfaceNode.cpp
    TerrainCuller.cpp
    TerrainRenderData.cpp
	TileDrawable.cpp
    EngineContext.cpp
    TileNode.cpp
    TileNodeRegistry.cpp
    Loader.cpp
    Unloader.cpp
    ${SHADERS_CPP}
)

SET(TARGET_H
    Common
    CreateTileImplementation
    DrawState
    DrawTileCommand
    GeometryPool
    Shaders
    RexTerrainEngineNode
    LayerDrawable
    LoadTileData
    RenderBindings
    SurfaceNode
    TerrainCuller
    TerrainRenderData
	TileDrawable
    TileRenderModel
    EngineContext
    TileNode
    TileNodeRegistry
    Loader
    Unloader
	SelectionInfo
)

add_osgearth_plugin(
    TARGET osgdb_osgearth_engine_rex
    HEADERS ${TARGET_H}
    SOURCES ${TARGET_SRC}
    SHADERS ${TARGET_GLSL}
    TEMPLATES ${TARGET_IN}
    LIBRARIES OpenGL::GL)
