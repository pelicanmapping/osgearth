/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "PlaceholderAssets.h"
#include <osg/Geometry>
#include <osg/Geode>
#include <cmath>
#include <algorithm>

namespace
{
    //! Accumulates procedural triangles without scene-state or texture dependencies.
    struct Mesh
    {
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array();
        osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array();
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array();

        //! Appends a triangle with a geometric normal and a uniform color.
        void triangle(const osg::Vec3& a, const osg::Vec3& b, const osg::Vec3& c, const osg::Vec4& color)
        {
            osg::Vec3 n = (b - a) ^ (c - a);
            n.normalize();
            for (const auto& v : {a, b, c})
            {
                vertices->push_back(v);
                normals->push_back(n);
                colors->push_back(color);
            }
        }

        //! Adds a faceted shrub or boulder from an elliptical ring and two poles.
        void mound(const osg::Vec3& center, const osg::Vec3& size, const osg::Vec4& color, unsigned sides = 9u)
        {
            for (unsigned i = 0; i < sides; ++i)
            {
                const float a = float(i) * 6.2831853f / sides;
                const float b = float(i + 1) * 6.2831853f / sides;
                const osg::Vec3 p = center + osg::Vec3(std::cos(a)*size.x(), std::sin(a)*size.y(), 0);
                const osg::Vec3 q = center + osg::Vec3(std::cos(b)*size.x(), std::sin(b)*size.y(), 0);
                triangle(p, q, center + osg::Vec3(0.12f*size.x(), 0, size.z()), color);
                triangle(q, p, center - osg::Vec3(0, 0, size.z()*0.6f),
                    osg::Vec4(color.r()*0.9f, color.g()*0.9f, color.b()*0.9f, color.a()));
            }
        }

        //! Adds crossed, untextured crown silhouettes as an original stand-in for a baked tree impostor.
        void crownSilhouette(float radius, float z, float height, const osg::Vec4& color)
        {
            for (unsigned plane = 0; plane < 2; ++plane)
            {
                const osg::Vec3 side = plane == 0 ? osg::Vec3(radius,0,0) : osg::Vec3(0,radius,0);
                triangle(osg::Vec3(0,0,z)-side, osg::Vec3(0,0,z)+side, osg::Vec3(0,0,z+height), color);
                // Approximate crown normals preserve solar response without a normal-map asset.
                (*normals)[normals->size()-3] = osg::Vec3(-side.x(),-side.y(),radius);
                (*normals)[normals->size()-2] = osg::Vec3(side.x(),side.y(),radius);
                (*normals)[normals->size()-1] = osg::Vec3(0,0,1);
                for (unsigned i = 0; i < 3; ++i) (*normals)[normals->size()-1-i].normalize();
            }
        }

        //! Adds a tapered trunk or conifer crown with its base on z.
        void cone(float radius, float z, float height, const osg::Vec4& color)
        {
            for (unsigned i = 0; i < 10; ++i)
            {
                const float a = float(i)*6.2831853f/10.0f, b = float(i+1)*6.2831853f/10.0f;
                triangle(osg::Vec3(radius*std::cos(a), radius*std::sin(a), z),
                    osg::Vec3(radius*std::cos(b), radius*std::sin(b), z), osg::Vec3(0,0,z+height), color);
            }
        }

        //! Adds radial bent leaves; broad leaves distinguish undergrowth from grass blades.
        void leaves(float radius, float height, float width, const osg::Vec4& color, unsigned count = 9u)
        {
            for (unsigned i = 0; i < count; ++i)
            {
                const float a = float(i)*2.399963f;
                osg::Vec3 direction(std::cos(a), std::sin(a), 0);
                osg::Vec3 side(-direction.y()*width, direction.x()*width, 0);
                osg::Vec3 middle = direction*(radius*0.45f) + osg::Vec3(0,0,height*0.85f);
                osg::Vec3 tip = direction*radius + osg::Vec3(0,0,height*(0.6f + 0.04f*i));
                triangle(osg::Vec3(), middle-side, middle+side, color);
                triangle(middle-side, tip, middle+side, color);
            }
        }

        //! Publishes immutable geometry for conversion to a shared Chonk.
        osg::ref_ptr<osg::Node> finish()
        {
            auto geometry = new osg::Geometry();
            geometry->setVertexArray(vertices);
            geometry->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, GLsizei(vertices->size())));
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);
            auto geode = new osg::Geode();
            geode->addDrawable(geometry);
            return geode;
        }
    };
}

osg::ref_ptr<osg::Node> osgEarthPrestige::createPlaceholderAsset(const std::string& name, unsigned lod)
{
    if (lod > 1u) return nullptr;
    Mesh mesh;
    if (name == "trees")
    {
        if (lod == 0u)
        {
            mesh.cone(0.3f, 0, 10, osg::Vec4(0.32f,0.20f,0.10f,1));
            mesh.cone(3.3f, 3.0f, 6.2f, osg::Vec4(0.15f,0.36f,0.24f,1));
            mesh.cone(2.6f, 5.5f, 6.0f, osg::Vec4(0.20f,0.44f,0.28f,1));
            mesh.cone(1.7f, 8.0f, 5.0f, osg::Vec4(0.26f,0.51f,0.32f,1));
        }
        else
        {
            mesh.crownSilhouette(0.3f, 0, 10, osg::Vec4(0.32f,0.20f,0.10f,1));
            mesh.crownSilhouette(3.3f, 3.0f, 6.2f, osg::Vec4(0.15f,0.36f,0.24f,1));
            mesh.crownSilhouette(2.6f, 5.5f, 6.0f, osg::Vec4(0.20f,0.44f,0.28f,1));
            mesh.crownSilhouette(1.7f, 8.0f, 5.0f, osg::Vec4(0.26f,0.51f,0.32f,1));
        }
    }
    else if (name == "shrubs")
    {
        mesh.mound(osg::Vec3(0,0,0.8f), osg::Vec3(1.5f,1.1f,1.4f), osg::Vec4(0.43f,0.56f,0.23f,1), lod ? 4u : 9u);
        if (lod == 0u)
            mesh.mound(osg::Vec3(1,0.3f,0.5f), osg::Vec3(1,0.8f,0.9f), osg::Vec4(0.51f,0.62f,0.29f,1));
    }
    else if (name == "grass")
        mesh.leaves(0.45f, 0.65f, 0.035f, osg::Vec4(0.66f,0.69f,0.29f,1), lod ? 3u : 9u);
    else if (name == "undergrowth")
        mesh.leaves(0.9f, 0.8f, 0.19f, osg::Vec4(0.23f,0.55f,0.40f,1), lod ? 3u : 9u);
    else if (name == "rocks")
        mesh.mound(osg::Vec3(0,0,0.6f), osg::Vec3(1.6f,1.2f,1.2f), osg::Vec4(0.49f,0.48f,0.44f,1), lod ? 4u : 9u);
    else
        return nullptr;
    return mesh.finish();
}
