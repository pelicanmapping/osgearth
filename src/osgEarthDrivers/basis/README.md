# Basis image reader

The optional `osgdb_basis` plugin uses the `basisu` CMake package (tested with
vcpkg's Basis Universal 1.50.0 and 2.0.2). The exported `basisu::basisu_encoder` library
contains the transcoder as well as the encoder.

The reader loads the first image in a `.basis` file, including its mipmaps.
It also loads 2D ETC1S and UASTC `.ktx2` files, including Zstd-supercompressed
UASTC. Arrays, cubemaps, HDR, and non-identity KTX swizzles are unsupported.
Supported LDR encodings are transcoded to BC1 for opaque images and BC3 for
images with alpha. Image dimensions exclude the compression block padding.
The file's Y-flip flag is honored, producing bottom-left image data for OSG.
Top-down images with non-power-of-two dimensions use RGBA8 because OSG cannot
vertically flip those DXT images correctly.
Single-level images with partial DXT blocks also use RGBA8 to avoid OSG's
truncated byte-size calculation when copying those images.

To request uncompressed pixels (for CPU image processing or hardware without
S3TC support), set plugin string data on the reader options:

```cpp
osg::ref_ptr<osgDB::Options> options = new osgDB::Options;
options->setPluginStringData("BASIS_FORMAT", "rgba8");
auto image = osgDB::readRefImageFile("texture.basis", options.get());
```

`BASIS_FORMAT=auto` restores the default behavior. This option uses
`setPluginStringData`, not the general options string. The reader does not
detect GPU capabilities, change color-space interpretation,
write images, or decode HDR textures. Unsupported encodings and failed reads
return an error instead of an incomplete image.

`BASIS_ORIGIN` accepts `bottom_left` (default) or `top_left`. The glTF reader
uses `top_left` for `KHR_texture_basisu`, avoiding a redundant vertical flip.
External KTX2 URIs, `data:image/ktx2;base64` URIs, and GLB buffer views all
delegate to this plugin. Color textures retain their compressed blocks and
mipmaps with an sRGB texture format; normal and material maps decode to RGBA8
for osgEarth's channel conversion. An optional extension can use its core
image fallback; failure to decode a required Basis texture fails the model load.

For OSG 3.6, the glTF reader supplies a texture upload callback for sRGB BC1/BC3,
which OSG's compressed-size helper does not recognize. Both this callback and
TextureArena calculate byte counts from the source's BC block format while
retaining the sRGB format on the GPU.

Available input encodings depend on the installed Basis version. For example,
ASTC LDR 6x6 requires the newer library; ETC1S and UASTC work with both versions.
