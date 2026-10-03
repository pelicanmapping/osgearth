# Environment Variables

### Configuration
| Variable | Description | Default |
| -------- | ----------- | ------- |
| OSGEARTH_DEFAULT_FONT | Name of the default font to use for annotations. | arial.ttf (Windows) |
| OSGEARTH_TERRAIN_CONCURRENCY | Number of threads to use for terrain tile loading. | `4` |

### Debugging

Set `OSGEARTH_TEXTURE_ARENA_WARN_GPU_WORK=1` before starting the application to warn whenever
a TextureArena texture compile requires GPU compression or mipmap generation. Warnings include
the texture name, source URI/filename when available, and allocated width x height x depth
(or layer count for arrays). Any set value enables this diagnostic;
the setting is read once on the first image upload. Disabled by default.

| Variable | Description | Default |
| -------- | ----------- | ------- |
| OSGEARTH_NOTIFY_LEVEL | Verbosity of console output. Options are `DEBUG`, `INFO`, `NOTICE`, and `FATAL`. `INFO` is usually sufficient for most debugging purposes. | `NOTICE` |
| OSGEARTH_GL_DEBUG | Set this to `1` to dump verbose GL error details to the console. If you get vague GL errors from OSG, this can help provide more information. ||
| OSGEARTH_VP_DEBUG | Set this to `1` to dump verbose information pertaining to VirtualProgram errors. ||
| OSGEARTH_VERBOSE_GDAL_ERRORS | Set this to `1` to get detailed information whenever GDAL generates and internal error. Could be helpful for finding issues with source data. ||
| OSGEARTH_HTTP_DEBUG | Set this to `1` and osgEarth will dump HTTP request strings to the console along with response code and timing information. This can be helpful for debugging networking problems. ||
| OSGEARTH_HTTP_DISABLE | Simulates no network by disabling outgoing HTTP/S connections. ||
| OSGEARTH_DUMP_SHADERS | Set to `1` to get a verbose console dump of shader composition source code. It's a lot. ||
| OSGEARTH_HEADLESS | Set this to `1` to simulate a headless environment in which no OpenGL graphics hardware is available. ||
| OSGEARTH_CACHE_DEBUG | Set `1` to see verbose cache activity reporting on the console. Not supported by all cache drivers. ||
| OSGEARTH_REX_DEBUG | Set to `1` and the terrain engine will render a bounding box for each terrain tile. ||
|||


### Caching
| Variable | Description | Default |
| -------- | ----------- | ------- |
| OSGEARTH_CACHE_DRIVER | Name of the cache implementation to use. Options include `filesystem` and `sqlite3`. | `filesystem` |
| OSGEARTH_CACHE_PATH | Path of a local folder in which to cache data. Setting this variable will automatically activate caching. ||
| OSGEARTH_CACHE_STATS | Enable cumulative cache telemetry. Set to `1` to write JSON to the osgEarth log, or set to a filename to append JSON Lines reports. Filenames may contain `{driver}` and `{pid}` placeholders. | disabled |
| OSGEARTH_NO_CACHE | Set this to `1` and osgEarth will ignore any configured cache setup, and force all requests to go directly to source. ||
| OSGEARTH_CACHE_ONLY | Set this to `1` and osgEarth will only attempt to read data from a configured cache, and will not attempt to read data from the source for remote layers. ||
| OSGEARTH_CACHE_MAX_AGE | Maximum age (in seconds) of valid cache entries. ||
| OSGEARTH_CACHE_MAX_SIZE_MB | Maximum size of the disk cache in MB. This only applies to the `rocksdb` cache driver, and is only a general target and NOT a guarantee. ||
|||

Cache telemetry can also be enabled in a cache configuration with `stats=true`,
`stats_path=<filename>`, and `stats_interval=<seconds>`. Reports are cumulative
JSON Lines snapshots containing hit/miss/error counts, latency percentiles,
bytes, queue high-water marks, and backend-specific measurements. An interval
of zero emits only the final shutdown report.

### Security & Networking
| Variable | Description | Default |
| -------- | ----------- | ------- |
| OSGEARTH_CESIUM_KEY | Authorization key string for accessing Cesium Ion services. ||
| OSGEARTH_BING_KEY | Authorization key string for accessing Microsoft Bing services. ||
| OSGEARTH_AZURE_KEY | Subscription key string for accessing Microsoft Azure Maps services. ||
| OSG_CURL_PROXY | Hostname of a proxy server to use. ||
| OSG_CURL_PROXYPORT | Port numver of the proxy server to use. ||
| OSGEARTH_CURL_PROXYAUTH | Authorization string in the form `username:password` to pass to a proxy server. ||
| OSGEARTH_HTTP_TIMEOUT | Timeout for HTTP responses, in seconds. ||
| OSGEARTH_HTTP_CONNECTTIMEOUT | Timeout for HTTP connection requests, in seconds. ||
|||

### 3rd Party
| Variable | Description | Default |
| -------- | ----------- | ------- |
| GDAL_DATA | Path of the folder containing GDAL's coordinate system data files. ||
| SILVERLINING_PATH | Path to the SunDog Silverlining SDK Resources folder. ||
| TRITON_PATH | Path to the SunDog Triton SDK Resources folder. ||

### Experimental / Advanced
| Variable | Description | Default |
| -------- | ----------- | ------- |
| OSGEARTH_USE_NVGL | Set to `1` to enable NVIDIA GL 4.6 extensions that activate bindless textures and buffers. ||
| OSGEARTH_ENABLE_WORK_STEALING | Set to `1` to turn on work-stealing in the jobs threading subsystem. ||
| OSGEARTH_L2_CACHE_SIZE | Sets the maximum number of rasters to store in a layer's L2 cache if it has one. The L2 cache is generally used to speed up reprojection and mosaicing when a layer's profile differs from that of the map. ||
| OSGEARTH_MEMORY_PROFILE | When set to `1` osgEarth will endeavor to disable internal memory-based caching mechanisms so you can get a better sense of memory usage over time. ||
| OSGEARTH_IGNORE_VERTICAL_DATUMS | When set to `1` osgEarth will quietly ignore any vertical datums present in source data. This exists only for backwards-compatibility with legacy systems. ||
