/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include "PBRMaterial"
#include "URI"
#include "ImageUtils"

#include <osg/Texture2D>

using namespace osgEarth;

#undef LC
#define LC "[PBRMaterial] "

namespace
{
    const float DEFAULT_DISPLACEMENT = 0.0f;
    const float DEFAULT_ROUGHNESS = 0.5f;
    const float DEFAULT_AO = 1.0f;
    const float DEFAULT_METAL = 0.0f;

    osg::ref_ptr<osg::Image> assemble_RGBA(
        osg::ref_ptr<osg::Image>& color,
        osg::ref_ptr<osg::Image>& opacity)
    {
        osg::ref_ptr<osg::Image> output;

        if (color.valid())
        {
            output = new osg::Image();
            output->allocateImage(color->s(), color->t(), 1, GL_RGBA, GL_UNSIGNED_BYTE);
            ImageUtils::PixelWriter write_output(output.get());
            ImageUtils::PixelReader read_color(color.get());
            ImageUtils::PixelReader read_opacity(opacity.get());
            ImageUtils::ImageIterator iter(output.get());
            osg::Vec4 a, b;
            write_output.forEachPixel([&](auto& iter)
                {
                    read_color(a, iter);
                    if (opacity.valid())
                    {
                        read_opacity(b, iter.u(), iter.v());
                        a.a() = b[0];
                    }
                    write_output(a, iter);
                });
        }
        else
        {
            // default to red
            output = new osg::Image();
            output->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            output->setColor(osg::Vec4(1, 0, 0, 1), 0, 0);
        }

        return output;
    }

    osg::ref_ptr<osg::Image> assemble_NORM(osg::ref_ptr<osg::Image> normal)
    {
        osg::ref_ptr<osg::Image> output;
        if (normal.valid())
        {
            return normal;

#if 0
            // disable compression for now b/c it's not working :(
            if (normal->getPixelFormat() == GL_COMPRESSED_RED_GREEN_RGTC2_EXT)
            {
                // DO nothing.
                output = normal;
            }
            else
            {
                output = new osg::Image();
                output->allocateImage(normal->s(), normal->t(), 1, GL_RG, GL_UNSIGNED_BYTE);
                output->setInternalTextureFormat(GL_COMPRESSED_RED_GREEN_RGTC2_EXT);
                ImageUtils::PixelReader read(normal.get());
                ImageUtils::PixelWriter write(output.get());
                osg::Vec4 a, packed;
                osg::Vec3 temp;
                ImageUtils::ImageIterator iter(output.get());
                iter.forEachPixel([&]()
                    {
                        read(a, iter.s(), iter.t());
                        temp.set(a.x() * 2.0f - 1.0f, a.y() * 2.0f - 1.0f, a.z() * 2.0f - 1.0f);
                        packed = NormalMapGenerator::pack(temp);
                        write(packed, iter.s(), iter.t());
                    });
            }
#endif
        }
        else
        {
            output = new osg::Image();
            output->allocateImage(1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE);
            output->setInternalTextureFormat(GL_RGB8);
            output->setColor(osg::Vec4(0.5, 0.5, 1.0, 1.0), 0, 0);
        }
        return output;
    }

    osg::ref_ptr<osg::Image> assemble_DRAM(
        osg::ref_ptr<osg::Image>& displacement,
        osg::ref_ptr<osg::Image>& roughness,
        osg::ref_ptr<osg::Image>& ao,
        osg::ref_ptr<osg::Image>& metal)
    {
        osg::ref_ptr<osg::Image> output;

        if (displacement.valid() || roughness.valid() || ao.valid() || metal.valid())
        {
            // use the dimensions of the largest image
            int s = 0, t = 0;
            for (auto& image : { displacement, roughness, ao, metal })
            {
                if (image.valid() && image->s() > s)
                    s = image->s();
                if (image.valid() && image->t() > t)
                    t = image->t();
            }

            if (s > 0 && t > 0)
            {
                output = new osg::Image();
                output->allocateImage(s, t, 1, GL_RGBA, GL_UNSIGNED_BYTE);

                ImageUtils::PixelReader
                    read_displacement(displacement.get()),
                    read_roughness(roughness.get()),
                    read_ao(ao.get()),
                    read_metal(metal.get());

                ImageUtils::PixelWriter write_output(output.get());

                osg::Vec4 in, out;
                write_output.forEachPixel([&](auto& iter)
                    {
                        if (displacement.valid())
                        {
                            read_displacement(in, iter.u(), iter.v());
                            out.r() = in.r();
                        }
                        else
                        {
                            out.r() = DEFAULT_DISPLACEMENT;
                        }

                        if (roughness.valid())
                        {
                            read_roughness(in, iter.u(), iter.v());
                            out.g() = in.r();
                        }
                        else
                        {
                            out.g() = DEFAULT_ROUGHNESS;
                        }

                        if (ao.valid())
                        {
                            read_ao(in, iter.u(), iter.v());
                            out.b() = in.r();
                        }
                        else
                        {
                            out.b() = DEFAULT_AO;
                        }

                        if (metal.valid())
                        {
                            read_metal(in, iter.u(), iter.v());
                            out.a() = in.r();
                        }
                        else
                        {
                            out.a() = DEFAULT_METAL;
                        }

                        write_output(out, iter);
                    });
            }
        }

        if (!output.valid())
        {
            // fallback - use defaults.
            output = new osg::Image();
            output->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            output->setColor(osg::Vec4(DEFAULT_DISPLACEMENT, DEFAULT_ROUGHNESS, DEFAULT_AO, DEFAULT_METAL), 0, 0);
        }

        return output;
    }

    Result<osg::ref_ptr<osg::Image>> loadImage(const URI& uri, const osgDB::Options* o)
    {
        auto rr = uri.readImage(o);
        if (rr.failed())
            return Status(Status::ResourceUnavailable, rr.errorDetail());
        return osg::ref_ptr<osg::Image>(rr.getImage());
    }
}

Status
PBRTexture::load(const PBRMaterial& mat_const, const osgDB::Options* options)
{
    PBRMaterial mat = mat_const;

    if (!mat.colorImage && mat.color().isSet()) {
        auto rr = mat.color()->readImage(options);
        if (rr.failed()) return status = Status(Status::ResourceUnavailable, "Failed to load " + mat.color()->full() + " ... " + rr.errorDetail());
        else mat.colorImage = rr.getImage();
    }

    if (!mat.normalImage && mat.normal().isSet()) {
        auto rr = mat.normal()->readImage(options);
        if (rr.failed()) return status = Status(Status::ResourceUnavailable, "Failed to load " + mat.normal()->full() + " ... " + rr.errorDetail());
        else mat.normalImage = rr.getImage();
    }

    if (!mat.roughnessImage && mat.roughness().isSet()) {
        auto rr = mat.roughness()->readImage(options);
        if (rr.failed()) return status = Status(Status::ResourceUnavailable, "Failed to load " + mat.roughness()->full() + " ... " + rr.errorDetail());
        else mat.roughnessImage = rr.getImage();
    }

    if (!mat.metalImage && mat.metal().isSet()) {
        auto rr = mat.metal()->readImage(options);
        if (rr.failed()) return status = Status(Status::ResourceUnavailable, "Failed to load " + mat.metal()->full() + " ... " + rr.errorDetail());
        else mat.metalImage = rr.getImage();
    }

    if (!mat.aoImage && mat.ao().isSet()) {
        auto rr = mat.ao()->readImage(options);
        if (rr.failed()) return status = Status(Status::ResourceUnavailable, "Failed to load " + mat.ao()->full() + " ... " + rr.errorDetail());
        else mat.aoImage = rr.getImage();
    }

    if (!mat.displacementImage && mat.displacement().isSet()) {
        auto rr = mat.displacement()->readImage(options);
        if (rr.failed()) return status = Status(Status::ResourceUnavailable, "Failed to load " + mat.displacement()->full() + " ... " + rr.errorDetail());
        else mat.displacementImage = rr.getImage();
    }

    if (!mat.opacityImage && mat.opacity().isSet()) {
        auto rr = mat.opacity()->readImage(options);
        if (rr.failed()) return status = Status(Status::ResourceUnavailable, "Failed to load " + mat.opacity()->full() + " ... " + rr.errorDetail());
        else mat.opacityImage = rr.getImage();
    }


    albedo = new osg::Texture2D(assemble_RGBA(mat.colorImage, mat.opacityImage));
    albedo->setName(mat.name() + " albedo");

    normal = new osg::Texture2D(assemble_NORM(mat.normalImage));
    normal->setName(mat.name() + " normal");

    pbr = new osg::Texture2D(assemble_DRAM(mat.displacementImage, mat.roughnessImage, mat.aoImage, mat.metalImage));
    pbr->setName(mat.name() + " PBR");

    for (auto& tex : { albedo, normal, pbr })
    {
        tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        tex->setMaxAnisotropy(4.0f);
    }

    return status = Status::OK();
}
