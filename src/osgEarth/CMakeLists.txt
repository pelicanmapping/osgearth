# osgEarth Core Library

if(OSGEARTH_BUILD_SHARED_LIBS)
    add_definitions(-DOSGEARTH_LIBRARY)
endif()

# Dependencies ...................................................

# required
find_package(CURL REQUIRED)
find_package(GDAL REQUIRED)
find_package(SQLite3 REQUIRED)

# optional
find_package(geos QUIET)
find_package(blend2d QUIET)
find_package(spdlog QUIET)
find_package(meshoptimizer QUIET)
if (OSGEARTH_ENABLE_S3)
    find_package(AWSSDK CONFIG COMPONENTS core s3 QUIET)
endif()

if(OSGEARTH_BUILD_SHARED_LIBS)
    find_package(blosc QUIET)
endif()

if(OSGEARTH_BUILD_ZIP_PLUGIN)
    find_package(LibZip QUIET)
endif()

if(OSGEARTH_PROTOBUF_FORCE_CONFIG_MODE)
    # https://stackoverflow.com/a/56896032/4218920
    set(protobuf_MODULE_COMPATIBLE ON CACHE BOOL "")
    find_package(Protobuf CONFIG QUIET)
else()
    find_package(Protobuf QUIET)
endif()


# Other options ...................................................

# support for the old Controls API (superceded by imgui)
if(OSGEARTH_BUILD_LEGACY_CONTROLS_API)
    set(OSGEARTH_HAVE_LEGACY_CONTROLS_API ON) # for BuildConfig
    add_definitions(-DOSGEARTH_HAVE_CONTROLS_API) # for backwards compat
endif()

# Builds the HTTPClient with WIN_INET instead of CURL
option(OSGEARTH_ENABLE_WININET_FOR_HTTP "Whether to use the WinInet library for HTTP requests (instead of cURL)" OFF)
mark_as_advanced(OSGEARTH_ENABLE_WININET_FOR_HTTP)
if (OSGEARTH_ENABLE_WININET_FOR_HTTP)
    add_definitions(-DOSGEARTH_USE_WININET_FOR_HTTP)
endif()


# Source code .....................................................

SET(LIB_NAME osgEarth)


set(TARGET_GLSL
    CascadeDraping.glsl
    Chonk.glsl
    Chonk.Culling.glsl
    Decals.h.glsl
    Decals.glsl
    Decals.Culling.glsl
    DepthOffset.glsl
    Draping.glsl
    DrawInstancedAttribute.glsl
    FrustumGrid.h.glsl
    FrustumGridComputer.glsl
    GPUClamping.glsl
    GPUClamping.lib.glsl
    HexTiling.glsl
    Instancing.glsl
    LineDrawable.glsl
    MetadataNode.glsl
    WireLines.glsl
    PhongLighting.glsl
    PointDrawable.glsl
    Text.glsl
    Text_legacy.glsl
    ContourMap.glsl
    GeodeticGraticule.glsl
    LogDepthBuffer.glsl
    LogDepthBuffer.VertOnly.glsl
    ShadowCaster.glsl
    SimpleOceanLayer.glsl
    RTTPicker.glsl
    WindLayer.CS.glsl
    PBR.glsl)

set(TARGET_IN
    BuildConfig.in    
    Version.in
    Shaders.cpp.in)
    
# auto-generate the shaders cpp:
set(SHADERS_CPP "${CMAKE_CURRENT_BINARY_DIR}/AutoGenShaders.cpp")
configure_shaders(Shaders.cpp.in ${SHADERS_CPP} ${TARGET_GLSL})

# Declare the dynamic headers -- we'll generate them later when all the HAVE vars are set.
set(OSGEARTH_VERSION_HEADER "${OSGEARTH_BUILDTIME_INCLUDE_DIR}/osgEarth/Version")
set(OSGEARTH_BUILDCONFIG_HEADER "${OSGEARTH_BUILDTIME_INCLUDE_DIR}/osgEarth/BuildConfig")

SET(TARGET_H    
    AGG.h
    AltitudeFilter
    AltitudeSymbol
    AnnotationData
    AnnotationLayer
    AnnotationNode
    AnnotationRegistry
    AnnotationSettings
    AnnotationUtils
    ArcGISServer
    ArcGISTilePackage
    AtlasBuilder
    AttributesFilter
    AutoClipPlaneHandler
    AutoScaleCallback
    AzureMaps
    BboxDrawable
    BBoxSymbol
    BillboardResource
    BillboardSymbol
    Bing
    Bounds
    BufferFilter
    BuildGeometryFilter
    BuildTextFilter
    Cache
    CacheBin
    CachePolicy
    CacheSeed
    Callbacks
    Callouts
    CameraUtils
    Capabilities
    CascadeDrapingDecorator
    CentroidFilter
    CesiumIon
    Chonk
    CircleNode
    ClampableNode
    ClampCallback
    Clamping
    ClampingTechnique
    ClipSpace
    ClusterNode
    Color
    ColorFilter
    Common
    Composite
    CompositeFeatureSource
    CompressedArray
    CompositeTiledModelLayer
    Config
    Containers
    ContourMap
    ConvertTypeFilter
    Coverage
    CoverageLayer
    CoverageSymbol
    CropFilter
    CssUtils
    Cube
    CullingUtils
    DateTime
    DateTimeRange
    DebugImageLayer
    DecalLayer
    Decals
    DepthOffset
    Draggers
    DrapeableNode
    DrapingCullSet
    DrapingTechnique
    DrawInstanced
    EarthManipulator
    ECEF
    Elevation
    ElevationLayer
    ElevationLOD
    ElevationPool
    ElevationQuery
    ElevationRanges
    EllipseNode
    Ellipsoid
    Endian
    Ephemeris
    ExampleResources
    Export
    Expression
    Extension
    ExtrudeGeometryFilter
    ExtrusionSymbol
    FadeEffect
    Feature
    FeatureCursor
    FeatureDisplayLayout
    FeatureElevationLayer
    FeatureImageLayer
    FeatureImageRTTLayer
    FeatureIndex
    FeatureModelGraph
    FeatureModelLayer
    FeatureModelSource
    FeatureNode
    FeatureRasterizer
    FeatureSDFLayer
    FeatureSource
    FeatureSourceIndexNode
    FeatureStyleSorter
    FileUtils
    Fill
    Filter
    FilterContext
    FilteredFeatureSource
    FlatteningLayer
    Formatter
    FractalElevationLayer
    FrameClock
    GARSGraticule
    GDAL
    GDALDEM
    GeoCommon
    GeoData
    GeodeticGraticule
    GeodeticLabelingEngine
    Geoid
    GeoMath
    Geometry
    GeometryClamper
    GeometryCloud
    GeometryCompiler
    GeometryFactory
    GeometryRasterizer
    GeometryUtils
    GeoPositionNode
    GeoPositionNodeAutoScaler
    GEOS
    GeoTransform
    GLSLChunker
    GLUtils
    GraticuleLabelingEngine
    HeightFieldUtils
    Horizon
    HorizonClipPlane
    HTM
    HTTPClient
    IconResource
    IconSymbol
    ImageLayer
    ImageMosaic
    ImageOverlay
    ImageOverlayEditor
    ImageToFeatureLayer
    ImageToHeightFieldConverter
    ImageUtils
    InstanceBuilder
    InstanceCloud
    InstanceResource
    InstanceSymbol
    IntersectionPicker
    IOTypes
    JoinLines
    JsonUtils
    LabelNode
    LandCover
    LandCoverLayer
    LatLongFormatter
    Layer
    LayerReference
    LayerShader
    Lighting
    LinearLineOfSight
    LineDrawable
    LineFunctor
    LineOfSight
    LineSymbol
    LoadableNode
    LocalGeometryNode
    LocalTangentPlane
    Locators
    LODGenerator
    LogarithmicDepthBuffer
    Map
    MapboxGLGlyphManager
    MapboxGLImageLayer
    MapCallback
    MapModelChange
    MapNode
    MapNodeObserver
    MaterialLoader
    Math
    MBTiles
    MeasureTool
    MemCache
    MemoryUtils
    MeshConsolidator
    MeshFlattener
    MeshSubdivider
    MetadataNode
    MetaTile
    Metrics
    MGRSFormatter
    MGRSGraticule
    ModelLayer
    ModelNode
    ModelResource
    ModelSource
    ModelSymbol
    MVT
    NativeProgramAdapter
    NetworkMonitor
    NodeUtils
    NoiseTextureFactory
    Notify
    ObjectIDPicker
    ObjectIndex
    OGRFeatureSource
    OgrUtils
    optional
    OverlayDecorator
    PagedNode
    PatchLayer
    PBRMaterial
    PhongLightingEffect
    Picker
    PlaceNode
    PluginLoader
    PMTiles
    PointDrawable
    PointSymbol
    PolyfillCPP14
    PolygonizeLines
    PolygonSymbol
    PowerlineLayer
    PrimitiveIntersector
    Profile
    Progress
    Query
    RadialLineOfSight
    Random
    RectangleNode
    RefinePolicy
    Registry
    RenderSymbol
    ResampleFilter
    Resource
    ResourceCache
    ResourceLibrary
    Revisioning
    RTTPicker
    ScaleFilter
    ScatterFilter
    SceneGraphCallback
    ScreenSpaceLayout
    ScreenSpaceLayoutCallout
    ScreenSpaceLayoutDeclutter
    ScreenSpaceLayoutImpl
    Script
    ScriptEngine
    ScriptFilter
    SDF
    SDF
    SelectExtentTool
    Session
    ShaderFactory
    ShaderGenerator
    ShaderLayer
    ShaderLoader
    ShaderMerger
    Shaders
    ShaderUtils
    Shadowing
    SimpleOceanLayer
    SimplePager
    SimplexNoise
    SimplifyFilter
    Skins
    Sky
    SkyView
    SpatialReference
    StarData
    StateSetCache
    StateTransition
    Status
    StringUtils
    Stroke
    Style
    StyleSelector
    StyleSheet
    SubstituteModelFilter
    Symbol
    Tags
    TDTiles
    Terrain
    TerrainConstraintLayer
    TerrainEffect
    TerrainEngineNode
    TerrainEngineRequirements
    TerrainLayer
    TerrainMeshLayer
    TerrainOptions
    TerrainProfile
    TerrainResources
    TerrainTileModel
    TerrainTileModelFactory
    TerrainTileNode
    TessellateOperator
    Tessellator
    Text
    TextSymbol
    TextSymbolizer
    TextureArena
    TextureBuffer
    TFS
    TFSPackager
    Threading
    ThreeDTilesLayer
    TileCache
    TiledFeatureModelLayer
    TiledModelLayer
    TileEstimator
    TileHandler
    TileIndex
    TileIndexBuilder    
    TileKey
    TileLayer
    TileMesher
    TileRasterizer
    TileSource
    TileSourceElevationLayer
    TileSourceImageLayer
    TileVisitor
    TimeControl
    TimeSeriesImage
    TMS
    TMSBackFiller
    TopologyGraph
    TrackNode
    TransformFilter
    Units
    URI
    Utils
    UTMGraticule
    UTMLabelingEngine
    VerticalDatum
    VideoLayer
    ViewFitter
    Viewpoint
    VirtualProgram
    VisibleLayer
    WFS
    WindLayer
    WireLines
    WMS
    XmlUtils
    XYZ
    XYZFeatureSource
    XYZModelLayer

    rtree.h
    weemesh.h
    weejobs.h
    tl_optional.hpp
    mpark_variant.hpp
    polyfill_string_view.hpp

    tinyxml/tinyxml.h
    tinyxml/tinystr.h

    ${OSGEARTH_BUILDCONFIG_HEADER}
    ${OSGEARTH_VERSION_HEADER}
)
set(TARGET_SRC
    AltitudeFilter.cpp
    AltitudeSymbol.cpp
    AnnotationData.cpp
    AnnotationLayer.cpp
    AnnotationNode.cpp
    AnnotationRegistry.cpp
    AnnotationSettings.cpp
    AnnotationUtils.cpp
    ArcGISServer.cpp
    ArcGISTilePackage.cpp
    AtlasBuilder.cpp
    AttributesFilter.cpp
    AutoClipPlaneHandler.cpp    
    AzureMaps.cpp
    BboxDrawable.cpp
    BBoxSymbol.cpp
    BillboardResource.cpp
    BillboardSymbol.cpp
    Bing.cpp
    Bounds.cpp
    BufferFilter.cpp
    BuildGeometryFilter.cpp
    BuildTextFilter.cpp
    Cache.cpp
    CacheBin.cpp
    CachePolicy.cpp
    CacheSeed.cpp
    Callouts.cpp
    CameraUtils.cpp
    Capabilities.cpp
    CascadeDrapingDecorator.cpp
    CentroidFilter.cpp
    CesiumIon.cpp
    Chonk.cpp
    CircleNode.cpp
    ClampableNode.cpp
    ClampCallback.cpp
    Clamping.cpp
    ClampingTechnique.cpp
    ClipSpace.cpp
    ClusterNode.cpp
    Color.cpp
    ColorFilter.cpp
    Composite.cpp
    CompositeFeatureSource.cpp
    CompositeTiledModelLayer.cpp
    Compressors.cpp
    CompressedArray.cpp
    Config.cpp
    ContourMap.cpp
    ConvertTypeFilter.cpp
    CoverageLayer.cpp
    CoverageSymbol.cpp
    CropFilter.cpp
    CssUtils.cpp
    Cube.cpp
    CullingUtils.cpp
    DateTime.cpp
    DateTimeRange.cpp
    DebugImageLayer.cpp
    DecalLayer.cpp
    Decals.cpp
    DepthOffset.cpp
    Draggers.cpp
    DrapeableNode.cpp
    DrapingCullSet.cpp
    DrapingTechnique.cpp
    DrawInstanced.cpp
    EarthManipulator.cpp
    ECEF.cpp
    Elevation.cpp
    ElevationLayer.cpp
    ElevationLOD.cpp
    ElevationPool.cpp
    ElevationQuery.cpp
    ElevationRanges.cpp
    EllipseNode.cpp
    Ellipsoid.cpp
    Ephemeris.cpp
    ExampleResources.cpp
    Expression.cpp
    Extension.cpp
    ExtrudeGeometryFilter.cpp
    ExtrusionSymbol.cpp
    FadeEffect.cpp
    Feature.cpp
    FeatureCursor.cpp
    FeatureDisplayLayout.cpp
    FeatureElevationLayer.cpp
    FeatureImageLayer.cpp
    FeatureImageRTTLayer.cpp
    FeatureModelGraph.cpp
    FeatureModelLayer.cpp
    FeatureModelSource.cpp
    FeatureNode.cpp
    FeatureRasterizer.cpp
    FeatureSDFLayer.cpp
    FeatureSource.cpp
    FeatureSourceIndexNode.cpp
    FeatureStyleSorter.cpp
    FileUtils.cpp
    Fill.cpp
    Filter.cpp
    FilterContext.cpp
    FilteredFeatureSource.cpp
    FlatteningLayer.cpp
    FractalElevationLayer.cpp
    FrameClock.cpp
    GARSGraticule.cpp
    GDAL.cpp
    GDALDEM.cpp
    GeoData.cpp
    GeodeticGraticule.cpp
    GeodeticLabelingEngine.cpp
    Geoid.cpp
    GeoMath.cpp
    Geometry.cpp
    GeometryClamper.cpp
    GeometryCloud.cpp
    GeometryCompiler.cpp
    GeometryFactory.cpp
    GeometryRasterizer.cpp
    GeometryUtils.cpp
    GeoPositionNode.cpp
    GeoPositionNodeAutoScaler.cpp
    GEOS.cpp
    GeoTransform.cpp
    GLSLChunker.cpp
    GLUtils.cpp
    GraticuleLabelingEngine.cpp
    HeightFieldUtils.cpp
    Horizon.cpp
    HorizonClipPlane.cpp
    HTM.cpp
    HTTPClient.cpp
    IconResource.cpp
    IconSymbol.cpp
    ImageLayer.cpp
    ImageMosaic.cpp
    ImageOverlay.cpp
    ImageOverlayEditor.cpp
    ImageToFeatureLayer.cpp
    ImageToHeightFieldConverter.cpp
    ImageUtils.cpp
    InstanceBuilder.cpp
    InstanceCloud.cpp
    InstanceResource.cpp
    InstanceSymbol.cpp
    IntersectionPicker.cpp
    IOTypes.cpp
    JoinLines.cpp
    JsonUtils.cpp
    LabelNode.cpp
    LandCover.cpp
    LandCoverLayer.cpp
    LatLongFormatter.cpp
    Layer.cpp
    LayerShader.cpp
    Lighting.cpp
    LinearLineOfSight.cpp
    LineDrawable.cpp
    LineSymbol.cpp
    LocalGeometryNode.cpp
    LocalTangentPlane.cpp
    LODGenerator.cpp
    LogarithmicDepthBuffer.cpp
    Map.cpp
    MapboxGLGlyphManager.cpp
    MapboxGLImageLayer.cpp
    MapCallback.cpp
    MapNode.cpp
    MaterialLoader.cpp
    Math.cpp
    MBTiles.cpp
    MeasureTool.cpp
    MemCache.cpp
    MemoryUtils.cpp
    MeshConsolidator.cpp
    MeshFlattener.cpp
    MeshSubdivider.cpp
    MetadataNode.cpp
    MetaTile.cpp
    Metrics.cpp
    MGRSFormatter.cpp
    MGRSGraticule.cpp
    ModelLayer.cpp
    ModelNode.cpp
    ModelResource.cpp
    ModelSource.cpp
    ModelSymbol.cpp
    MVT.cpp
    NetworkMonitor.cpp
    NodeUtils.cpp
    NoiseTextureFactory.cpp
    Notify.cpp
    ObjectIDPicker.cpp
    ObjectIndex.cpp
    OGRFeatureSource.cpp
    OgrUtils.cpp
    OverlayDecorator.cpp
    PagedNode.cpp
    PatchLayer.cpp
    PBRMaterial.cpp
    PhongLightingEffect.cpp
    PlaceNode.cpp
    PMTiles.cpp
    PointDrawable.cpp
    PointSymbol.cpp
    PolygonizeLines.cpp
    PolygonSymbol.cpp
    PowerlineLayer.cpp
    PrimitiveIntersector.cpp
    Profile.cpp
    Progress.cpp
    Query.cpp
    RadialLineOfSight.cpp
    Random.cpp
    RectangleNode.cpp
    Registry.cpp
    RenderSymbol.cpp
    ResampleFilter.cpp
    Resource.cpp
    ResourceCache.cpp
    ResourceLibrary.cpp
    Revisioning.cpp
    RTTPicker.cpp
    ScaleFilter.cpp
    ScatterFilter.cpp
    SceneGraphCallback.cpp
    ScreenSpaceLayout.cpp
    ScriptEngine.cpp
    ScriptFilter.cpp
    SDF.cpp
    SDF.cpp
    SelectExtentTool.cpp
    Session.cpp
    ShaderFactory.cpp
    ShaderGenerator.cpp
    ShaderLayer.cpp
    ShaderLoader.cpp
    ShaderMerger.cpp
    ShaderUtils.cpp
    Shadowing.cpp
    SimpleOceanLayer.cpp
    SimplePager.cpp
    SimplexNoise.cpp
    SimplifyFilter.cpp
    Skins.cpp
    Sky.cpp
    SkyView.cpp
    SpatialReference.cpp
    StateSetCache.cpp
    Status.cpp
    StringUtils.cpp
    Stroke.cpp
    Style.cpp
    StyleSelector.cpp
    StyleSheet.cpp
    SubstituteModelFilter.cpp
    Symbol.cpp
    TDTiles.cpp
    Terrain.cpp
    TerrainConstraintLayer.cpp
    TerrainEngineNode.cpp
    TerrainMeshLayer.cpp
    TerrainOptions.cpp
    TerrainProfile.cpp
    TerrainResources.cpp
    TerrainTileModel.cpp
    TerrainTileModelFactory.cpp
    TessellateOperator.cpp
    Tessellator.cpp
    Text.cpp
    TextSymbol.cpp
    TextSymbolizer.cpp
    TextureArena.cpp
    TextureBuffer.cpp
    TextureBufferSerializer.cpp
    TFS.cpp
    TFSPackager.cpp
    Threading.cpp
    ThreeDTilesLayer.cpp
    TileCache.cpp
    TiledFeatureModelLayer.cpp
    TiledModelLayer.cpp
    TileEstimator.cpp
    TileHandler.cpp
    TileIndex.cpp
    TileIndexBuilder.cpp
    TileKey.cpp
    TileLayer.cpp
    TileMesher.cpp
    TileRasterizer.cpp
    TileSource.cpp
    TileSourceElevationLayer.cpp
    TileSourceImageLayer.cpp
    TileVisitor.cpp
    TimeControl.cpp
    TimeSeriesImage.cpp
    TMS.cpp
    TMSBackFiller.cpp
    TopologyGraph.cpp
    TrackNode.cpp
    TransformFilter.cpp
    Units.cpp
    URI.cpp
    Utils.cpp
    UTMGraticule.cpp
    UTMLabelingEngine.cpp
    Version.cpp
    VerticalDatum.cpp
    VideoLayer.cpp
    ViewFitter.cpp
    Viewpoint.cpp
    VirtualProgram.cpp
    VisibleLayer.cpp
    WFS.cpp
    WindLayer.cpp
    WireLines.cpp
    WMS.cpp
    XmlUtils.cpp
    XYZ.cpp
    XYZFeatureSource.cpp
    XYZModelLayer.cpp
    
    tinyxml/tinyxml.cpp
    tinyxml/tinyxmlerror.cpp
    tinyxml/tinystr.cpp
    tinyxml/tinyxmlparser.cpp
    
    ${SHADERS_CPP}
)

# include the old legacy API controls?
if(OSGEARTH_BUILD_LEGACY_CONTROLS_API)
    list(APPEND TARGET_H   ActivityMonitorTool     MouseCoordsTool     Controls)
    list(APPEND TARGET_SRC ActivityMonitorTool.cpp MouseCoordsTool.cpp Controls.cpp)
endif()

# include the geocoder (built into gdal)?
if(OSGEARTH_ENABLE_GEOCODER)
    set(OSGEARTH_HAVE_GEOCODER ON)
    list(APPEND TARGET_H   Geocoder)
    list(APPEND TARGET_SRC Geocoder.cpp)
endif()


# generate the google protocol buffers headers and sources
if(Protobuf_FOUND AND Protobuf_PROTOC_EXECUTABLE)    
    protobuf_generate_cpp(PROTO_VECTOR_TILE_CPP PROTO_VECTOR_TILE_H vector_tile.proto) 
    protobuf_generate_cpp(PROTO_GLYPHS_CPP PROTO_GLYPHS_H glyphs.proto)    
    list(APPEND TARGET_H ${PROTO_VECTOR_TILE_H} ${PROTO_GLYPHS_H})
    list(APPEND TARGET_SRC ${PROTO_VECTOR_TILE_CPP} ${PROTO_GLYPHS_CPP})
    
    if (OSGEARTH_OUT_OF_SOURCE_BUILD)
        # for an out-of-source build, the binary folder will include any protobuf-generated
        # header files; therefore we need to include them:
        include_directories(${CMAKE_CURRENT_BINARY_DIR})
    endif()
endif()

# configure the library:
add_osgearth_library(
    TARGET ${LIB_NAME}
    PUBLIC_HEADERS ${TARGET_H}
    SOURCES ${TARGET_SRC}
    SHADERS ${TARGET_GLSL}
    TEMPLATES ${TARGET_IN} )


# google protocol buffers
if(Protobuf_FOUND AND Protobuf_PROTOC_EXECUTABLE)
    message(STATUS "Found protobuf")
    set(OSGEARTH_HAVE_PROTOBUF ON)
    set(OSGEARTH_HAVE_MVT ON)
    
    set(PROTOBUF_USE_DLLS FALSE CACHE BOOL "Set this to true if Protobuf is compiled as dll")
    if(PROTOBUF_USE_DLLS)
        add_definitions(-DPROTOBUF_USE_DLLS)
    endif()
    
    #include_directories(${Protobuf_INCLUDE_DIR})
    target_link_libraries(${LIB_NAME} PRIVATE protobuf::libprotobuf)
endif()

# spdlog support?
if (spdlog_FOUND)
    message(STATUS "Found spdlog")
    set(OSGEARTH_HAVE_SPDLOG ON)
    target_link_libraries(${LIB_NAME} PRIVATE spdlog::spdlog)
endif()

# MBTiles support?
if(SQLite3_FOUND)
    message(STATUS "Found SQLite3 - will support MBTiles")
    set(OSGEARTH_HAVE_SQLITE3 ON)
    set(OSGEARTH_HAVE_MBTILES ON)
    include_directories(${SQLite3_INCLUDE_DIR})
    target_link_libraries(${LIB_NAME} PRIVATE ${SQLite3_LIBRARIES})
endif()

# Blosc support?
if(blosc_FOUND)
    message(STATUS "Found blosc")
    set(OSGEARTH_HAVE_BLOSC ON)
    include_directories(${BLOSC_INCLUDE_DIR})
    string (TOLOWER ${OSGEARTH_DYNAMIC_OR_STATIC} shared_or_static)
    target_link_libraries(${LIB_NAME} PRIVATE blosc_${shared_or_static})
endif()

# GEOS support?
if(geos_FOUND)
    message(STATUS "Found geos")
    set(OSGEARTH_HAVE_GEOS ON)
    #include_directories(${GEOS_INCLUDE_DIR})
    target_link_libraries(${LIB_NAME} PRIVATE GEOS::geos_c)
endif()

# Blend2D?
if(blend2d_FOUND)
    message(STATUS "Found blend2d")
    set(OSGEARTH_HAVE_BLEND2D ON)
    #include_directories(${BLEND2D_INCLUDES})
    target_link_libraries(${LIB_NAME} PRIVATE blend2d::blend2d)

    # Blend2D 0.21.2 has a different include path; detect this
    get_target_property(_BLEND2D_INCLUDE_PATHS blend2d::blend2d INTERFACE_INCLUDE_DIRECTORIES)
    if(_BLEND2D_INCLUDE_PATHS)
        list(GET _BLEND2D_INCLUDE_PATHS 0 _FIRST_INCLUDE_DIR)
        if(EXISTS "${_FIRST_INCLUDE_DIR}/blend2d/blend2d.h")
            target_compile_definitions(${LIB_NAME} PRIVATE BLEND2D_USE_SCOPED_INCLUDE)
        endif()
    endif()
endif()

# Tracy?
if (Tracy_FOUND)
    message(STATUS "Found Tracy")
    set(OSGEARTH_HAVE_TRACY ON)
    target_link_libraries(${LIB_NAME} PRIVATE Tracy::TracyClient)
endif()

# mesh optimizer optional library?
if(meshoptimizer_FOUND)
    message(STATUS "Found meshoptimizer")
    set(OSGEARTH_HAVE_MESH_OPTIMIZER ON)
    target_link_libraries(${LIB_NAME} PRIVATE meshoptimizer::meshoptimizer)
endif()

if(SuperluminalAPI_FOUND)
    message(STATUS "Found SuperLuminalAPI.")
    set(OSGEARTH_HAVE_SUPERLUMINALAPI ON)
endif()

if(aws-cpp-sdk-core_FOUND)
    message(STATUS "Found AWS SDK.")
    set(OSGEARTH_HAVE_AWS_SDK_CORE ON)
    target_include_directories(${LIB_NAME} PRIVATE ${AWSSDK_INCLUDE_DIRS})
    target_link_libraries(${LIB_NAME} PRIVATE ${AWSSDK_LIBRARIES})

endif()

# pthreads?
if(UNIX)
    target_link_libraries(${LIB_NAME} PUBLIC Threads::Threads)
endif()

# dbghelp?
if(WIN32)
    target_link_libraries(${LIB_NAME} PRIVATE dbghelp)
endif()

# special purpose - do not remove
option(NRL_STATIC_LIBRARIES "Link osgEarth against static GDAL and cURL, including static OpenSSL, Proj4, JPEG, PNG, and TIFF." OFF)

if(NOT NRL_STATIC_LIBRARIES)
    # normal link:
    target_link_libraries(${LIB_NAME} 
        PUBLIC
            ${OPENSCENEGRAPH_LIBRARIES}
        PRIVATE
            OpenGL::GL CURL::libcurl GDAL::GDAL)
else()
    # special-purpose custom link:
    target_link_libraries(${LIB_NAME} 
        PUBLIC
            ${OPENSCENEGRAPH_LIBRARIES}
        PRIVATE
            OpenGL::GL CURL::libcurl GDAL::GDAL
            ${SSL_EAY_RELEASE} ${LIB_EAY_RELEASE} ${TIFF_LIBRARY} ${PROJ4_LIBRARY} ${PNG_LIBRARY} ${JPEG_LIBRARY})
endif()


# Generate the dynamic headers, now that all the OSGEARTH_HAVE_* vars are set:

message(STATUS "Creating ${OSGEARTH_BUILDCONFIG_HEADER}")
configure_file(BuildConfig.in ${OSGEARTH_BUILDCONFIG_HEADER})

message(STATUS "Creating ${OSGEARTH_VERSION_HEADER}")
configure_file(Version.in ${OSGEARTH_VERSION_HEADER})
