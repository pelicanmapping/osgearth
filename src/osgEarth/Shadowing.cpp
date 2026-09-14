/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include <osgEarth/Shadowing>
#include <osgEarth/Shaders>
#include <osgEarth/CullingUtils>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/CameraUtils>
#include <osgEarth/Math>
#include <osgShadow/ConvexPolyhedron>

#define LC "[ShadowCaster] "

using namespace osgEarth::Util;

//...................................................................

void
Shadowing::setIsShadowCamera(osg::Camera* camera)
{
    CameraUtils::setIsShadowCamera(camera);
}

bool
Shadowing::isShadowCamera(const osg::Camera* camera)
{
    return CameraUtils::isShadowCamera(camera);
}

//...................................................................

ShadowCaster::ShadowCaster() :
_enabled(true),
_size         ( 4096 ),
_texImageUnit ( 7 ),
_blurFactor   ( 0.001f ),
_color        ( 0.0f ),
_traversalMask( ~0 )
{
    _castingGroup = new osg::Group();

    _supported = Registry::capabilities().supportsGLSL();
    if ( _supported )
    {
        // default slices:
        _ranges.push_back(0.0f);
        _ranges.push_back(2500.0f);

        reinitialize();
    }
    else
    {
        OE_WARN << LC << "ShadowCaster not supported (no GLSL); disabled." << std::endl;
    }
}

void
ShadowCaster::setRanges(const std::vector<float>& ranges)
{
    _ranges = ranges;
    reinitialize();
}

void
ShadowCaster::setTextureImageUnit(int unit)
{
    _texImageUnit = unit;
    reinitialize();
}

void
ShadowCaster::setTextureSize(unsigned size)
{
    _size = size;
    reinitialize();
}

void
ShadowCaster::setBlurFactor(float value)
{
    _blurFactor = value;
    if ( _shadowBlurUniform.valid() )
        _shadowBlurUniform->set(value);
}

void
ShadowCaster::setShadowColor(float value)
{
    _color = value;
    if ( _shadowColorUniform.valid() )
        _shadowColorUniform->set(value);
}

void
ShadowCaster::reinitialize()
{
    if ( !_supported )
        return;

    _shadowmap = 0L;
    _rttCameras.clear();

    int numSlices = (int)_ranges.size() - 1;
    if ( numSlices < 1 )
    {
        OE_WARN << LC << "Illegal. Must have at least one range slice." << std::endl;
        return ;
    }

    // create the projected texture:
    _shadowmap = new osg::Texture2DArray();
    _shadowmap->setTextureSize( _size, _size, numSlices );
    _shadowmap->setInternalFormat( GL_DEPTH_COMPONENT );
    _shadowmap->setFilter( osg::Texture::MIN_FILTER, osg::Texture::LINEAR );
    _shadowmap->setFilter( osg::Texture::MAG_FILTER, osg::Texture::LINEAR );
    _shadowmap->setWrap( osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_BORDER );
    _shadowmap->setWrap( osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_BORDER );
    _shadowmap->setBorderColor(osg::Vec4(1,1,1,1));

    // set up the RTT camera:
    for(int i=0; i<numSlices; ++i)
    {
        osg::Camera* rtt = new osg::Camera();
        Shadowing::setIsShadowCamera(rtt);
        rtt->setReferenceFrame( osg::Camera::ABSOLUTE_RF_INHERIT_VIEWPOINT );
        rtt->setClearDepth( 1.0 );
        rtt->setClearMask( GL_DEPTH_BUFFER_BIT );
        rtt->setComputeNearFarMode( osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR );
        rtt->setViewport( 0, 0, _size, _size );
        rtt->setRenderOrder( osg::Camera::PRE_RENDER );
        rtt->setRenderTargetImplementation( osg::Camera::FRAME_BUFFER_OBJECT );
        rtt->setImplicitBufferAttachmentMask(0, 0);
        rtt->attach( osg::Camera::DEPTH_BUFFER, _shadowmap.get(), 0, i );
        rtt->addChild( _castingGroup.get() );
        rtt->addCullCallback(new InstallCameraUniform());
        _rttCameras.push_back(rtt);
    }

    _rttStateSet = new osg::StateSet();

    // only draw back faces to the shadow depth map
    // TODO: consider this. Commented out for now because it doesn't
    // look right for trees.
#if 0
    _rttStateSet->setAttributeAndModes( 
        new osg::CullFace(osg::CullFace::FRONT),
        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
#endif

    // install a shadow-to-primary xform matrix (per frame) so verts match up
    _shadowToPrimaryMatrix = _rttStateSet->getOrCreateUniform(
        "oe_shadowToPrimaryMatrix", osg::Uniform::FLOAT_MAT4);

    _primaryProjectionMatrix = _rttStateSet->getOrCreateUniform(
        "oe_primaryProjectionMatrix", osg::Uniform::FLOAT_MAT4);

    _primaryViewportUniform = _rttStateSet->getOrCreateUniform(
        "oe_primaryViewport", osg::Uniform::FLOAT_VEC2);

    _renderStateSet = new osg::StateSet();

    // Establish a Virtual Program on the stateset.
    VirtualProgram* vp = VirtualProgram::getOrCreate(_renderStateSet.get());
    vp->setName(typeid(*this).name());

    // Load the shadowing shaders.
    Shaders package;
    package.replace("$OE_SHADOW_NUM_SLICES", Stringify()<<numSlices);
    package.load(vp, package.ShadowCaster);
    _renderStateSet->setDefine("OE_SHADOWING");

    // the texture coord generator matrix array (from the caster):
    _shadowMapTexGenUniform = _renderStateSet->getOrCreateUniform(
        "oe_shadow_matrix",
        osg::Uniform::FLOAT_MAT4,
        numSlices );

    // bind the shadow map texture itself:
    _renderStateSet->setTextureAttribute(_texImageUnit, _shadowmap.get(), osg::StateAttribute::ON );
    _renderStateSet->addUniform( new osg::Uniform("oe_shadow_map", _texImageUnit) );

    // blur factor:
    _shadowBlurUniform = _renderStateSet->getOrCreateUniform("oe_shadow_blur", osg::Uniform::FLOAT);
    _shadowBlurUniform->set(_blurFactor);

    // shadow color:
    _shadowColorUniform = _renderStateSet->getOrCreateUniform("oe_shadow_color", osg::Uniform::FLOAT);

    _shadowColorUniform->set(_color);

    _renderStateSet->getOrCreateUniform("oe_shadow_maxrange", osg::Uniform::FLOAT)->set(_ranges.back());
}

void
ShadowCaster::resizeGLObjectBuffers(unsigned maxSize)
{
    if (_light.valid())
        _light->resizeGLObjectBuffers(maxSize);
    if (_shadowmap.valid())
        _shadowmap->resizeGLObjectBuffers(maxSize);
    if (_rttStateSet.valid())
        _rttStateSet->resizeGLObjectBuffers(maxSize);
    if (_renderStateSet.valid())
        _renderStateSet->resizeGLObjectBuffers(maxSize);
    for(unsigned i=0; i<_rttCameras.size(); ++i)
        _rttCameras[i]->resizeGLObjectBuffers(maxSize);
}

void
ShadowCaster::releaseGLObjects(osg::State* state) const
{
    if (_light.valid())
        _light->releaseGLObjects(state);
    if (_shadowmap.valid())
        _shadowmap->releaseGLObjects(state);
    if (_rttStateSet.valid())
        _rttStateSet->releaseGLObjects(state);
    if (_renderStateSet.valid())
        _renderStateSet->releaseGLObjects(state);
    for(unsigned i=0; i<_rttCameras.size(); ++i)
        _rttCameras[i]->releaseGLObjects(state);
}

void
ShadowCaster::traverse(osg::NodeVisitor& nv)
{
    if (_supported                             && 
        _enabled                               &&
        _light.valid()                         &&
        nv.getVisitorType() == nv.CULL_VISITOR && 
        _castingGroup->getNumChildren() > 0    && 
        _shadowmap.valid() )
    {
        osgUtil::CullVisitor* cv = Culling::asCullVisitor(nv);
        osg::Camera* camera = cv->getCurrentCamera();
        if ( camera )
        {
            if (ProjectionMatrix::isOrtho(*cv->getProjectionMatrix()))
                return;

            osg::Matrix MV = *cv->getModelViewMatrix();
            osg::Matrix inverseMV;
            inverseMV.invert(MV);

            osg::Vec3d camEye, camTo, camUp;
            MV.getLookAt( camEye, camTo, camUp, 1.0 );

            // position the light. We only really care about the directional vector.
            osg::Vec4d lp4 = _light->getPosition();
            osg::Vec3d lightVectorWorld( -lp4.x(), -lp4.y(), -lp4.z() );
            lightVectorWorld.normalize();
            osg::Vec3d lightPosWorld = osg::Vec3d(0,0,0) * inverseMV;
            //osg::Vec3d lightPosWorld( lp4.x(), lp4.y(), lp4.z() ); // jitter..

            // construct the view matrix for the light. The up vector doesn't really
            // matter so we'll just use the camera's.
            osg::Matrix lightViewMat;
            osg::Vec3d lightUp(0,0,1);
            osg::Vec3d side = lightVectorWorld ^ lightUp;
            lightUp = side ^ lightVectorWorld;
            lightUp.normalize();
            lightViewMat.makeLookAt(lightPosWorld, lightPosWorld+lightVectorWorld, lightUp);

            // set the shadow-camera-to-primary-camera transformation matrix,
            // which lets you perform vertex shader operations from the perspective
            // of the primary camera (morphing, etc.) so that things match up
            // between the two cameras.
            osg::Matrix lightViewMatInv = osg::Matrix::inverse(lightViewMat);
            _shadowToPrimaryMatrix->set( lightViewMatInv * MV);

            // and a projection matrix, so we can do clip-space work if necessary:
            _primaryProjectionMatrix->set(*cv->getProjectionMatrix());

            // and the primary viewport
            _primaryViewportUniform->set(osg::Vec2f(cv->getViewport()->width(), cv->getViewport()->height()));

            if (_prevProjMatrix.isIdentity())
            {
                _prevProjMatrix = *cv->getProjectionMatrix();
            }
            int i;
            for(i=0; i < (int) _ranges.size()-1; ++i)
            {
                double n = _ranges[i];
                double f = _ranges[i+1];

                // take the camera's projection matrix and clamp it's near and far planes
                // to our shadow map slice range.
                osg::Matrix proj =
                    _prevProjMatrix.isIdentity() ? camera->getProjectionMatrix() :
                    _prevProjMatrix;

                double fovy,ar,zn,zf;
                ProjectionMatrix::getPerspective(proj, fovy, ar, zn, zf);
                ProjectionMatrix::setPerspective(proj, fovy, ar, std::max(n, zn), std::min(f, zf));
                
                // extract the corner points of the camera frustum in world space.
                osg::Matrix MVP = MV * proj;
                osg::Matrix inverseMVP;
                inverseMVP.invert(MVP);
                osgShadow::ConvexPolyhedron frustumPH;
                frustumPH.setToUnitFrustum(true, true);
                frustumPH.transform( inverseMVP, MVP );
                std::vector<osg::Vec3d> verts;
                frustumPH.getPoints( verts );

                // include the eyepoint in the shadowing volume.
                if (i == 0)
                    verts.push_back(camEye);

                // project those on to the plane of the light camera and fit them
                // to a bounding box. That box will form the extent of our orthographic camera.
                osg::BoundingBoxd bbox;
                for( std::vector<osg::Vec3d>::iterator v = verts.begin(); v != verts.end(); ++v )
                    bbox.expandBy( (*v) * lightViewMat );

                osg::Matrix lightProjMat;
                n = -osg::maximum(bbox.zMin(), bbox.zMax());
                f = -osg::minimum(bbox.zMin(), bbox.zMax());
                // TODO: consider extending "n" so that objects outside the main view can still cast shadows
                ProjectionMatrix::setOrtho(lightProjMat,
                    bbox.xMin(), bbox.xMax(), bbox.yMin(), bbox.yMax(), n, f);

                // configure the RTT camera for this slice:
                _rttCameras[i]->setViewMatrix( lightViewMat );
                _rttCameras[i]->setProjectionMatrix( lightProjMat );

                // this xforms from clip [-1..1] to texture [0..1] space
                static osg::Matrix s_scaleBiasMat = 
                    osg::Matrix::translate(1.0,1.0,1.0) * 
                    osg::Matrix::scale(0.5,0.5,0.5);
                
                // set the texture coordinate generation matrix that the shadow
                // receiver will use to sample the shadow map. Doing this on the CPU
                // prevents nasty precision issues!
                osg::Matrix VPS = lightViewMat * lightProjMat * s_scaleBiasMat;
                _shadowMapTexGenUniform->setElement(i, inverseMV * VPS);
            }

            // install the shadow-casting traversal mask:
            unsigned saveMask = cv->getTraversalMask();
            cv->setTraversalMask( _traversalMask & saveMask );

            // render the shadow maps.
            cv->pushStateSet( _rttStateSet.get() );
            for(i=0; i < (int) _rttCameras.size(); ++i)
            {
                _rttCameras[i]->accept( nv );
            }
            cv->popStateSet();

            // restore the previous mask
            cv->setTraversalMask( saveMask );
            
            // render the shadowed subgraph.
            cv->pushStateSet( _renderStateSet.get() );
            osg::Group::traverse( nv );
            cv->popStateSet();

            // save the projection matrix for the next frame.
            _prevProjMatrix = *cv->getProjectionMatrix();

            return;
        }
    }

    osg::Group::traverse(nv);
}
