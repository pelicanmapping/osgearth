/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include "CanopyAssets.h"
#include <osgEarthProcedural2/Canopy>
#include <algorithm>
#include <cmath>
#include <set>

using namespace osgEarth;
using namespace osgEarth::Procedural2;

Status osgEarth::Procedural2::assembleCanopyAsset(unsigned coverage, unsigned layout,
    const std::array<Chonk::Ptr,9>& sources, Chonk::Ptr& output)
{
    output.reset();
    auto result = Chonk::create();
    std::set<const ChonkMaterial*> retained;
    const auto& crowns = canopyTemplate(coverage,layout);
    for (unsigned crown=0; crown<9; ++crown)
    {
        const auto& source = sources[crown];
        if (!source || source->_lods.size() != 1 || source->_ebo_store.empty() || source->_ebo_store.size()%3 ||
            source->_ebo_store.size() > 128u*3u)
            return Status(Status::ConfigurationError,"Canopy source must contain 1..128 static triangles");
        if (result->_materialArena && result->_materialArena != source->_materialArena)
            return Status(Status::ConfigurationError,"Canopy sources must share a material arena");
        result->_materialArena = source->_materialArena;
        for (const auto& material : source->_materials)
            if (retained.insert(material.get()).second) result->_materials.push_back(material);
        result->_alphaMaterials.insert(source->_alphaMaterials.begin(),source->_alphaMaterials.end());
        const auto rotation = osg::Matrixf::rotate(((crown+layout)%4u)*1.57079632679f,osg::Vec3(0,0,1));
        osg::BoundingBoxf box;
        for (const auto index : source->_ebo_store) box.expandBy(source->_vbo_store[index].position*rotation);
        const osg::Vec3 span(box.xMax()-box.xMin(),box.yMax()-box.yMin(),box.zMax()-box.zMin());
        if (!box.valid() || span.z() < 1e-5f || std::max(span.x(),span.y()) < 1e-5f)
            return Status(Status::ConfigurationError,"Canopy source needs finite horizontal extent and positive height");
        const auto& shape = crowns[crown];
        // Emit referenced vertices only: unused source vertices cannot enlarge the certified footprint or the budget.
        for (const auto index : source->_ebo_store)
        {
            auto v = source->_vbo_store[index];
            const auto p = v.position*rotation;
            const osg::Vec3 unit((p.x()-box.center().x())/std::max(span.x(),1e-5f),
                (p.y()-box.center().y())/std::max(span.y(),1e-5f),(p.z()-box.zMin())/span.z());
            v.position.set(shape.center.x()+2.0f*shape.size.x()*unit.x(),
                shape.center.y()+2.0f*shape.size.y()*unit.y(),(shape.center.z()+shape.size.z())*unit.z());
            // Preserve authored foliage normals; ordinary carrier-card normals need a volume approximation.
            if (v.normal_technique == Chonk::NORMAL_TECHNIQUE_VOLUME)
                v.normal = osg::Matrixf::transform3x3(v.normal,rotation);
            else v.normal.set(unit.x(),unit.y(),std::max(0.35f,unit.z()-0.5f));
            v.normal.normalize();
            v.normal_technique = Chonk::NORMAL_TECHNIQUE_VOLUME;
            v.flex.set(float(crown),0,0);
            result->_box.expandBy(v.position);
            result->_ebo_store.push_back(Chonk::element_t(result->_vbo_store.size()));
            result->_vbo_store.push_back(v);
        }
    }
    Chonk::LOD lod;
    lod.offset = 0; lod.length = result->_ebo_store.size();
    lod.far_pixel_scale = 0; lod.near_pixel_scale = FLT_MAX;
    lod.alphaTested = result->hasAlphaTest(0,result->_vbo_store.size());
    result->_lods.push_back(lod);
    output = result;
    return Status::NoError;
}
