/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#pragma once
#include <osgEarth/Chonk>
#include <osgEarth/TextureArena>
#include <osgEarth/VirtualProgram>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osg/Texture>
#include <cstring>

namespace osgEarth { namespace ShadowDetail
{
    /** Snapshot of explicitly static casters. Custom shader inputs and application-owned GL resources
     * require explicit invalidation; callbacks, dynamic objects, and pagers always bypass reuse.
     * Construct during synchronized cull. Retained objects prevent address reuse from hiding replacement.
     */
    struct Snapshot : osg::NodeVisitor
    {
        bool safe = true;
        double time = 0.0;
        std::vector<std::uint64_t> key;
        std::vector<osg::ref_ptr<const osg::Object>> retained;
        //! Includes every registered child, including masked and currently out-of-frustum casters.
        Snapshot() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
        //! Appends exact binary values; comparisons do not depend on hash collision probability.
        template<typename T> void value(const T& item)
        {
            std::uint64_t words[(sizeof(T)+7)/8]{};
            std::memcpy(words,&item,sizeof(T));
            key.insert(key.end(),std::begin(words),std::end(words));
        }
        //! Records variable-length names and defines without ambiguous concatenations.
        void text(const std::string& item)
        {
            value(item.size());
            for (unsigned char c : item) value(c);
        }
        //! Retains identity and rejects explicitly dynamic objects.
        void object(const osg::Object* item, bool permitDynamic = false)
        {
            value(item);
            if (item)
            {
                retained.emplace_back(item);
                if (!permitDynamic && item->getDataVariance() == osg::Object::DYNAMIC)
                    safe = false;
            }
        }
        //! Tracks exposed texture contents and rejects asynchronous/animated images.
        void texture(const osg::Texture& tex)
        {
            for (unsigned i=0; i<tex.getNumImages(); ++i)
            {
                auto image = tex.getImage(i);
                object(image);
                if (image)
                {
                    value(image->getModifiedCount());
                    if (image->requiresUpdateCall()) safe = false;
                }
            }
        }
        //! Records state membership, ordinary uniform writes and image edits; custom attributes need invalidation.
        void state(const osg::StateSet* ss)
        {
            // Camera state and uniforms may alternate between equivalent frame buffers. Compare their contents.
            value(ss != nullptr);
            if (!ss) return;
            retained.emplace_back(ss);
            if (ss->getUpdateCallback() || ss->getEventCallback()) safe = false;
            value(ss->getModeList().size());
            for (const auto& mode : ss->getModeList()) { value(mode.first); value(mode.second); }
            value(ss->getUniformList().size());
            for (const auto& uniform : ss->getUniformList())
            {
                retained.emplace_back(uniform.second.first.get());
                text(uniform.first);
                value(uniform.second.second);
                auto u = uniform.second.first.get();
                value(u->getType());
                value(u->getNumElements());
                // Static-caster opt-in excludes clock-driven shaders. OSG installs these clocks on every view,
                // even when no caster reads them; Chonk's finite birthday fade is handled explicitly below.
                if (uniform.first == "osg_FrameNumber" || uniform.first == "osg_FrameTime" ||
                    uniform.first == "osg_DeltaFrameTime" || uniform.first == "osg_SimulationTime" ||
                    uniform.first == "osg_DeltaSimulationTime") continue;
                if (u->getFloatArray()) for (auto x : *u->getFloatArray()) value(x);
                if (u->getDoubleArray()) for (auto x : *u->getDoubleArray()) value(x);
                if (u->getIntArray()) for (auto x : *u->getIntArray()) value(x);
                if (u->getUIntArray()) for (auto x : *u->getUIntArray()) value(x);
                if (u->getInt64Array()) for (auto x : *u->getInt64Array()) value(x);
                if (u->getUInt64Array()) for (auto x : *u->getUInt64Array()) value(x);
                if (uniform.second.first->getUpdateCallback() || uniform.second.first->getEventCallback()) safe = false;
            }
            value(ss->getDefineList().size());
            for (const auto& define : ss->getDefineList())
            {
                text(define.first);
                text(define.second.first);
                value(define.second.second);
            }
            value(ss->getAttributeList().size());
            for (const auto& attr : ss->getAttributeList())
            {
                auto arena = dynamic_cast<const TextureArena*>(attr.second.first.get());
                object(attr.second.first,arena != nullptr);
                value(attr.second.second);
                if (!arena && (attr.second.first->getUpdateCallback() || attr.second.first->getEventCallback())) safe = false;
                if (arena)
                {
                    if (arena->getAutoPaging() || arena->getAutoRelease()) safe = false;
                    for (const auto& tex : arena->getTextures()) if (tex) texture(*tex->osgTexture());
                }
                if (auto vp = dynamic_cast<const VirtualProgram*>(attr.second.first.get()))
                {
                    VirtualProgram::ShaderMap shaders;
                    vp->getShaderMap(shaders);
                    for (const auto& shader : shaders)
                    {
                        value(shader.second._shader->getKeyID());
                    }
                }
            }
            value(ss->getTextureAttributeList().size());
            for (const auto& unit : ss->getTextureAttributeList())
            {
                value(unit.size());
                for (const auto& attr : unit)
                {
                    object(attr.second.first);
                    value(attr.second.second);
                    if (auto tex = dynamic_cast<const osg::Texture*>(attr.second.first.get())) texture(*tex);
                }
            }
            value(ss->getTextureModeList().size());
            for (const auto& unit : ss->getTextureModeList())
            {
                value(unit.size());
                for (const auto& mode : unit) { value(mode.first); value(mode.second); }
            }
        }
        //! Detects graph/placement/mesh edits; unknown node subclasses conservatively disable caching.
        void apply(osg::Node& node) override
        {
            object(&node);
            value(node.getNodeMask());
            if (node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback()) safe = false;
            if (auto drawable = node.asDrawable())
                if (drawable->getDrawCallback()) safe = false;
            if (dynamic_cast<osg::PagedLOD*>(&node) || dynamic_cast<osg::ProxyNode*>(&node)) safe = false;
            state(node.getStateSet());
            if (auto transform = dynamic_cast<osg::MatrixTransform*>(&node))
                for (unsigned i=0; i<16; ++i) value(transform->getMatrix().ptr()[i]);
            if (auto chonk = dynamic_cast<ChonkDrawable*>(&node))
            {
                value(chonk->getContentRevision());
                if (time < chonk->getBirthday()+2.0) safe = false;
            }
            else if (auto geometry = node.asGeometry())
            {
                osg::Geometry::ArrayList arrays;
                geometry->getArrayList(arrays);
                for (auto array : arrays) { object(array); value(array->getModifiedCount()); }
                for (auto& primitive : geometry->getPrimitiveSetList())
                {
                    object(primitive);
                    value(primitive->getModifiedCount());
                    value(primitive->getMode());
                    value(primitive->getNumIndices());
                    value(primitive->getNumInstances());
                    if (auto draw = dynamic_cast<osg::DrawArrays*>(primitive.get())) value(draw->getFirst());
                }
            }
            else if (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode) &&
                typeid(node) != typeid(osg::MatrixTransform)) safe = false;
            if (auto group = node.asGroup()) value(group->getNumChildren());
            traverse(node);
        }
    };
} }
