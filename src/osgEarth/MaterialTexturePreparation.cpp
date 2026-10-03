/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "MaterialTexturePreparation"
#include "PBRMaterial"
#include "ImageUtils"
#include "Progress"
#include "Threading"
#include "sha1.hpp"
#include <osg/Texture2D>
#include <osgDB/Registry>
#include <cmath>
#include <list>
#include <sstream>
#include <cstring>
#include <mutex>
#include <unordered_map>

#ifndef GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif

using namespace osgEarth;

namespace
{
    enum class Role { Color, Normal, Packed, Occlusion };

    //! Decode sRGB before averaging color samples; alpha and material data stay linear.
    float linearColor(float v)
    {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    }

    //! Encode a linear color sample for sRGB compressed storage.
    float srgbColor(float v)
    {
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }

    //! Normalize a tangent-space vector; opposing/degenerate samples fall back to +Z.
    osg::Vec4 normalizedNormal(const osg::Vec4& v)
    {
        osg::Vec3 n(v.x(), v.y(), v.z());
        if (n.normalize() == 0.0f) n.set(0, 0, 1);
        return osg::Vec4(n, 1.0f);
    }

    //! Area-filter a mip level, including odd edge texels without resizing the source image.
    std::vector<osg::Vec4> reduce(const std::vector<osg::Vec4>& pixels, int w, int h, Role role)
    {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<osg::Vec4> result(nw * nh);
        for (int y = 0; y < nh; ++y)
        {
            const double y0 = double(y) * h / nh, y1 = double(y + 1) * h / nh;
            for (int x = 0; x < nw; ++x)
            {
                const double x0 = double(x) * w / nw, x1 = double(x + 1) * w / nw;
                osg::Vec4 sum(0, 0, 0, 0);
                for (int sy = int(y0); sy < int(std::ceil(y1)); ++sy)
                    for (int sx = int(x0); sx < int(std::ceil(x1)); ++sx)
                    {
                        const double weight = (std::min(x1, double(sx + 1)) - std::max(x0, double(sx))) *
                            (std::min(y1, double(sy + 1)) - std::max(y0, double(sy)));
                        sum += pixels[sy * w + sx] * float(weight);
                    }
                sum /= float((x1 - x0) * (y1 - y0));
                result[y * nw + x] = role == Role::Normal ? normalizedNormal(sum) : sum;
            }
        }
        return result;
    }

    //! Build and compress each mip independently; padding is confined to compression blocks.
    //! Input is immutable. Returns null on cancellation or compressor failure.
    osg::ref_ptr<osg::Image> prepareImage(const osg::Image* source, Role role, int layout, bool srgb,
        bool mipmaps, osgDB::ImageProcessor* processor, ProgressCallback* progress)
    {
        const bool alpha = role == Role::Color ? source->isImageTranslucent() :
            role == Role::Packed && layout == PBRMaterial::DRAM;
        const auto mode = role == Role::Occlusion ? osg::Texture::USE_RGTC1_COMPRESSION :
            alpha || role == Role::Normal ? osg::Texture::USE_S3TC_DXT5_COMPRESSION :
            osg::Texture::USE_S3TC_DXT1_COMPRESSION;
        const GLenum format = role == Role::Occlusion ? GL_COMPRESSED_RED_RGTC1_EXT :
            mode == osg::Texture::USE_S3TC_DXT5_COMPRESSION ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT :
            GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        const unsigned blockBytes = mode == osg::Texture::USE_S3TC_DXT5_COMPRESSION ? 16u : 8u;
        ImageUtils::PixelReader read(source);
        std::vector<osg::Vec4> pixels;
        std::vector<unsigned char> bytes;
        osg::Image::MipmapDataType offsets;
        int w = source->s(), h = source->t();
        const unsigned levels = mipmaps ? osg::Image::computeNumberOfMipmapLevels(w, h) : 1u;
        for (unsigned level = 0; level < levels; ++level)
        {
            if (progress && progress->isCanceled()) return nullptr;
            if (level < source->getNumMipmapLevels())
            {
                pixels.resize(w * h);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        osg::Vec4 value = read(x, y, 0, level);
                        if (srgb)
                            for (unsigned c = 0; c < 3; ++c) value[c] = linearColor(value[c]);
                        if (role == Role::Normal)
                            value = normalizedNormal(osg::Vec4(value.r()*2-1, value.g()*2-1, value.b()*2-1, 1));
                        pixels[y * w + x] = value;
                    }
            }
            const int pw = (w + 3) & ~3, ph = (h + 3) & ~3;
            osg::ref_ptr<osg::Image> blockImage = new osg::Image();
            blockImage->allocateImage(pw, ph, 1, GL_RGBA, GL_UNSIGNED_BYTE, 1);
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                {
                    osg::Vec4 value = pixels[std::min(y, h - 1) * w + std::min(x, w - 1)];
                    if (srgb)
                        for (unsigned c = 0; c < 3; ++c) value[c] = srgbColor(value[c]);
                    if (role == Role::Normal)
                    {
                        for (unsigned c = 0; c < 3; ++c) value[c] = value[c] * 0.5f + 0.5f;
                        value.a() = 1.0f; // XYZ normals must not trigger Chonk's octahedral decode.
                    }
                    auto* pixel = blockImage->data(x, y);
                    for (unsigned c = 0; c < 4; ++c)
                        pixel[c] = static_cast<unsigned char>(std::max(0.0f, std::min(1.0f, value[c])) * 255.0f + 0.5f);
                }
            processor->compress(*blockImage, mode, false, false, osgDB::ImageProcessor::USE_CPU,
                osgDB::ImageProcessor::PRODUCTION);
            const unsigned size = (pw / 4) * (ph / 4) * blockBytes;
            if (!blockImage->isCompressed() || blockImage->getTotalSizeInBytes() != size) return nullptr;
            if (level) offsets.push_back(static_cast<unsigned>(bytes.size()));
            bytes.insert(bytes.end(), blockImage->data(), blockImage->data() + size);
            if (level + 1 < levels)
                pixels = reduce(pixels, w, h, role);
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
        auto* data = new unsigned char[bytes.size()];
        std::memcpy(data, bytes.data(), bytes.size());
        osg::ref_ptr<osg::Image> result = new osg::Image();
        result->setImage(source->s(), source->t(), 1, format, format, GL_UNSIGNED_BYTE, data, osg::Image::USE_NEW_DELETE, 1);
        result->setMipmapLevels(offsets);
        result->setName(source->getName());
        result->setFileName(source->getFileName());
        result->setOrigin(source->getOrigin());
        result->setDataVariance(osg::Object::STATIC);
        return result;
    }
}

struct MaterialTexturePreparation::Impl
{
    struct Entry
    {
        osg::ref_ptr<osg::Image> image;
        std::list<std::string>::iterator position;
    };
    std::mutex mutex;
    Threading::Gate<std::string> gate;
    std::unordered_map<std::string, Entry> cache;
    std::unordered_map<std::string, osg::observer_ptr<osg::Image>> live;
    std::list<std::string> lru;
    std::size_t bytes = 0, limit;
    unsigned insertions = 0;

    //! Set the per-layer cache budget; expensive processing runs outside the cache mutex.
    explicit Impl(std::size_t cacheBytes) : limit(cacheBytes) { }

    //! Prepare an immutable static 2D texture. Cache identity includes pixels and processing policy.
    osg::ref_ptr<osg::Texture> texture(osg::Texture* source, Role role, int layout, ProgressCallback* progress)
    {
        auto* texture2D = dynamic_cast<osg::Texture2D*>(source);
        if (!texture2D || texture2D->getSubloadCallback() || source->getDataVariance() == osg::Object::DYNAMIC)
            return source;
        const osg::Image* input = source->getImage(0);
        if (!input || !input->data() || input->r() != 1 || input->s() < 1 || input->t() < 1 ||
            input->isCompressed() || input->getDataVariance() == osg::Object::DYNAMIC || input->requiresUpdateCall() ||
            input->getDataType() != GL_UNSIGNED_BYTE || !ImageUtils::PixelReader::supports(input))
            return source;
        const GLenum pixelFormat = input->getPixelFormat();
        if (pixelFormat != GL_RGB && pixelFormat != GL_RGBA && !(role == Role::Occlusion && pixelFormat == GL_RED))
            return source;
        if (role == Role::Packed && (layout < PBRMaterial::DRAM || layout > PBRMaterial::MTL_GLS_AO)) return source;
        const GLenum internal = source->getInternalFormat();
        const bool srgb = internal == GL_SRGB || internal == GL_SRGB_ALPHA ||
            internal == GL_SRGB8 || internal == GL_SRGB8_ALPHA8;
        if (srgb && role != Role::Color) return source;
        const auto filter = source->getFilter(osg::Texture::MIN_FILTER);
        const bool mipmaps = filter != osg::Texture::NEAREST && filter != osg::Texture::LINEAR;
        auto* processor = osgDB::Registry::instance()->getImageProcessorForExtension("stbdxt");
        if (!processor) return source;

        // Content identity survives distinct loader allocations and detects changed source data.
        // Include source metadata so shared output images retain accurate diagnostics.
        std::ostringstream identity;
        identity << int(role) << ':' << layout << ':' << srgb << ':' << mipmaps << ':' << input->s() << ':' << input->t()
            << ':' << pixelFormat << ':' << input->getPacking() << ':' << input->getRowLength() << ':' << input->getOrigin()
            << ':' << input->getFileName().size() << ':' << input->getFileName() << ':' << input->getName();
        for (auto offset : input->getMipmapLevels()) identity << ':' << offset;
        sha1 hash(identity.str().c_str());
        hash.add(input->data(), input->getTotalSizeInBytesIncludingMipmaps());
        char digest[SHA1_HEX_SIZE];
        hash.finalize().print_hex(digest);
        const std::string key(digest);
        Threading::ScopedGate<std::string> singleFlight(gate, key);
        if (progress && progress->isCanceled()) return nullptr;
        osg::ref_ptr<osg::Image> prepared;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = cache.find(key);
            if (found != cache.end())
            {
                prepared = found->second.image;
                lru.splice(lru.begin(), lru, found->second.position);
            }
            else
            {
                auto active = live.find(key);
                if (active != live.end() && !active->second.lock(prepared)) live.erase(active);
            }
        }
        if (!prepared)
        {
            prepared = prepareImage(input, role, layout, srgb, mipmaps, processor, progress);
            if (!prepared) return source;
            const auto size = prepared->getTotalSizeInBytesIncludingMipmaps();
            std::lock_guard<std::mutex> lock(mutex);
            // Eviction releases our ownership, not sharing with textures still used by loaded tiles.
            if ((++insertions % 256) == 0)
                for (auto i = live.begin(); i != live.end(); )
                    i = i->second.valid() ? std::next(i) : live.erase(i);
            live[key] = prepared.get();
            if (size <= limit)
            {
                while (!lru.empty() && (bytes + size > limit || cache.size() >= 4096))
                {
                    auto old = cache.find(lru.back());
                    bytes -= old->second.image->getTotalSizeInBytesIncludingMipmaps();
                    cache.erase(old);
                    lru.pop_back();
                }
                lru.push_front(key);
                cache.emplace(key, Entry{prepared, lru.begin()});
                bytes += size;
            }
        }
        osg::ref_ptr<osg::Texture> result = osg::clone(source, osg::CopyOp::SHALLOW_COPY);
        result->setImage(0, prepared);
        GLenum format = prepared->getInternalTextureFormat();
        if (srgb)
            format = format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ?
                GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
        result->setInternalFormat(format);
        result->setResizeNonPowerOfTwoHint(false);
        return result;
    }

};

MaterialTexturePreparation::MaterialTexturePreparation(std::size_t cacheBytes) : _impl(new Impl(cacheBytes)) { }
MaterialTexturePreparation::~MaterialTexturePreparation() = default;

osg::ref_ptr<PBRTexture>
MaterialTexturePreparation::prepare(PBRTexture* material, ProgressCallback* progress)
{
    if (!material || (progress && progress->isCanceled())) return nullptr;
    osg::ref_ptr<PBRTexture> result = new PBRTexture(*material, osg::CopyOp::SHALLOW_COPY);
    result->albedo = _impl->texture(material->albedo, Role::Color, 0, progress);
    result->normal = _impl->texture(material->normal, Role::Normal, 0, progress);
    result->pbr = _impl->texture(material->pbr, Role::Packed, int(material->layoutAndFactors.x()), progress);
    result->occlusion = _impl->texture(material->occlusion, Role::Occlusion, 0, progress);
    return progress && progress->isCanceled() ? nullptr : result;
}
