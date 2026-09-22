/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/ChonkRenderPass>
#include <osg/UserDataContainer>
#include <atomic>
#include <cmath>

using namespace osgEarth;
namespace
{
    const char* packetName = "osgEarth.ChonkRenderPass";
    std::atomic<std::uint64_t> nextViewID{0}, nextBatchSerial{0};

    //! Rejects nonfinite matrix coefficients before they can poison GPU culling or buffer placement keys.
    bool finite(const osg::Matrixd& matrix)
    {
        for (unsigned i=0; i<16; ++i) if (!std::isfinite(matrix.ptr()[i])) return false;
        return true;
    }
}

ChonkRenderPass::Batch::Batch(const Parameters& value, std::uint64_t id) : parameters(value), serial(id) { }
std::uint64_t ChonkRenderPass::createViewID() { return ++nextViewID; }
osg::ref_ptr<const ChonkRenderPass::Batch> ChonkRenderPass::createBatch(const Parameters& p)
{
    if (p.viewID == 0 || p.count == 0 || p.count > MAX_VIEWS || p.output < PER_VIEW || p.output > MERGED ||
        (p.activeViews >> p.count) != 0 || !finite(p.lodView) || !finite(p.lodProjection) ||
        !std::isfinite(p.lodViewport.x()) || !std::isfinite(p.lodViewport.y()) ||
        p.lodViewport.x() <= 0 || p.lodViewport.y() <= 0) return nullptr;
    for (unsigned i=0; i<p.count; ++i) if (!finite(p.clipFromWorld[i])) return nullptr;
    return new Batch(p,++nextBatchSerial);
}
ChonkRenderPass::ChonkRenderPass() { setName(packetName); }
ChonkRenderPass::ChonkRenderPass(const Batch* batch, unsigned index, const osg::Matrixd& renderView) :
    ChonkRenderPass()
{
    if (batch && index < batch->parameters.count && (batch->parameters.output == PER_VIEW || index == 0) &&
        finite(renderView) && _inverseRenderView.invert(renderView) && finite(_inverseRenderView))
    {
        _batch = batch;
        _index = index;
    }
}
ChonkRenderPass::ChonkRenderPass(const ChonkRenderPass& rhs, const osg::CopyOp& op) :
    osg::Object(rhs,op), _batch(rhs._batch), _index(rhs._index), _inverseRenderView(rhs._inverseRenderView) { }
osg::Object* ChonkRenderPass::cloneType() const { return new ChonkRenderPass; }
osg::Object* ChonkRenderPass::clone(const osg::CopyOp& op) const { return new ChonkRenderPass(*this,op); }
void ChonkRenderPass::set(osg::StateSet* state, const ChonkRenderPass* pass)
{
    if (!state) return;
    // StateSet copies may share their user-data container. Preserve all application data without modifying that copy.
    auto old = state->getUserDataContainer();
    osg::ref_ptr<osg::UserDataContainer> data = old ?
        static_cast<osg::UserDataContainer*>(old->clone(osg::CopyOp::SHALLOW_COPY)) : new osg::DefaultUserDataContainer;
    osg::ref_ptr<ChonkRenderPass> packet = pass ? new ChonkRenderPass(*pass) : new ChonkRenderPass;
    packet->setName(packetName);
    unsigned index = data->getUserObjectIndex(packetName);
    if (index < data->getNumUserObjects()) data->setUserObject(index,packet);
    else data->addUserObject(packet);
    state->setUserDataContainer(data);
    state->setDefine("OE_CHONK_MULTIVIEW",packet->getBatch() ? osg::StateAttribute::ON : osg::StateAttribute::OFF);
}
const ChonkRenderPass* ChonkRenderPass::find(osg::State& state)
{
    const auto& stack = state.getStateSetStack();
    for (auto i=stack.rbegin(); i!=stack.rend(); ++i)
    {
        auto data = (*i)->getUserDataContainer();
        if (!data) continue;
        auto pass = dynamic_cast<const ChonkRenderPass*>(data->getUserObject(packetName));
        if (pass) return pass->getBatch() ? pass : nullptr;
    }
    return nullptr;
}
