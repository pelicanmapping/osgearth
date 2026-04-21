#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Group>
#include <osg/Program>
#include <osg/Shader>
#include <osg/TextureCubeMap>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osgViewer/Viewer>
#include <osgDB/ReadFile>
#include <osgEarth/GLUtils>
#include <osgGA/StateSetManipulator>

using namespace osgEarth;

/**
* This is a demo of fake building interiors like you see in Marvel's Spiderman.
* Shader code is modified from https://github.com/kylebakerio/aframe-interior-parallax-mapping/tree/master/shaders
* Interior cube map is from https://www.humus.name/index.php?page=3D&ID=80
*/

osg::ref_ptr< osg::Geometry > createQuadGeometry(float size = 1.0f) {
    float h = size * 0.5f;

    auto verts = new osg::Vec3Array;
    auto norms = new osg::Vec3Array;
    auto dirs = new osg::Vec2Array;
    auto tangents = new osg::Vec3Array();

    verts->push_back(osg::Vec3(-h, 0, -h));
    verts->push_back(osg::Vec3(h, 0, -h));
    verts->push_back(osg::Vec3(h, 0, h));
    verts->push_back(osg::Vec3(-h, 0, h));

    norms->push_back(osg::Vec3(0, -1, 0));
    norms->push_back(osg::Vec3(0, -1, 0));
    norms->push_back(osg::Vec3(0, -1, 0));
    norms->push_back(osg::Vec3(0, -1, 0));

    dirs->push_back(osg::Vec2(0, 0));
    dirs->push_back(osg::Vec2(50, 0));
    dirs->push_back(osg::Vec2(50, 50));
    dirs->push_back(osg::Vec2(0, 50));

    osg::Vec3 tangent(1, 0, 0);
    tangents->push_back(tangent);
    tangents->push_back(tangent);
    tangents->push_back(tangent);
    tangents->push_back(tangent);

    auto geom = new osg::Geometry;
    geom->setVertexArray(verts);
    geom->setNormalArray(norms, osg::Array::BIND_PER_VERTEX);
    geom->setTexCoordArray(0, dirs, osg::Array::BIND_PER_VERTEX);
    geom->setVertexAttribArray(osg::Geometry::SECONDARY_COLORS, tangents, osg::Array::BIND_PER_VERTEX);

    osg::DrawElementsUByte* de = new osg::DrawElementsUByte(GL_TRIANGLES);
    de->push_back(0);
    de->push_back(1);
    de->push_back(2);

    de->push_back(0);
    de->push_back(2);
    de->push_back(3);
    geom->addPrimitiveSet(de);


    return geom;
}

osg::ref_ptr<osg::Geometry> createBuilding(float size = 1.0f)
{
    float h = size * 0.5f;

    auto verts = new osg::Vec3Array;
    auto norms = new osg::Vec3Array;
    auto uvs = new osg::Vec2Array;
    auto tangents = new osg::Vec3Array;
    auto de = new osg::DrawElementsUShort(GL_TRIANGLES);

    auto addWall = [&](const osg::Vec3& bl, const osg::Vec3& br,
                       const osg::Vec3& tr, const osg::Vec3& tl,
                       const osg::Vec3& n, const osg::Vec3& t)
    {
        unsigned short base = static_cast<unsigned short>(verts->size());

        verts->push_back(bl);
        verts->push_back(br);
        verts->push_back(tr);
        verts->push_back(tl);

        for (int i = 0; i < 4; ++i)
        {
            norms->push_back(n);
            tangents->push_back(t);
        }

        uvs->push_back(osg::Vec2(0, 0));
        uvs->push_back(osg::Vec2(10, 0));
        uvs->push_back(osg::Vec2(10, 10));
        uvs->push_back(osg::Vec2(0, 10));

        de->push_back(base + 0);
        de->push_back(base + 1);
        de->push_back(base + 2);
        de->push_back(base + 0);
        de->push_back(base + 2);
        de->push_back(base + 3);
    };

    // -Y wall: U along +X, V along +Z
    addWall(osg::Vec3(-h, -h, -h), osg::Vec3( h, -h, -h),
            osg::Vec3( h, -h,  h), osg::Vec3(-h, -h,  h),
            osg::Vec3(0, -1, 0), osg::Vec3(1, 0, 0));

    // +X wall: U along +Y, V along +Z
    addWall(osg::Vec3( h, -h, -h), osg::Vec3( h,  h, -h),
            osg::Vec3( h,  h,  h), osg::Vec3( h, -h,  h),
            osg::Vec3(1, 0, 0), osg::Vec3(0, 1, 0));

    // +Y wall: U along -X, V along +Z
    addWall(osg::Vec3( h,  h, -h), osg::Vec3(-h,  h, -h),
            osg::Vec3(-h,  h,  h), osg::Vec3( h,  h,  h),
            osg::Vec3(0, 1, 0), osg::Vec3(-1, 0, 0));

    // -X wall: U along -Y, V along +Z
    addWall(osg::Vec3(-h,  h, -h), osg::Vec3(-h, -h, -h),
            osg::Vec3(-h, -h,  h), osg::Vec3(-h,  h,  h),
            osg::Vec3(-1, 0, 0), osg::Vec3(0, -1, 0));

    auto geom = new osg::Geometry;
    geom->setVertexArray(verts);
    geom->setNormalArray(norms, osg::Array::BIND_PER_VERTEX);
    geom->setTexCoordArray(0, uvs, osg::Array::BIND_PER_VERTEX);
    geom->setVertexAttribArray(osg::Geometry::SECONDARY_COLORS, tangents, osg::Array::BIND_PER_VERTEX);
    geom->addPrimitiveSet(de);

    return geom;
}

osg::ref_ptr<osg::TextureCubeMap> createCubeMap()
{
    auto cubemap = new osg::TextureCubeMap;

    std::string rootDir = "../data/InteriorMapping/CubeMaps/Interior";
    std::string ext = "dds";
    
    cubemap->setImage(osg::TextureCubeMap::POSITIVE_X, osgDB::readRefImageFile(rootDir + "/posx." + ext));
    cubemap->setImage(osg::TextureCubeMap::NEGATIVE_X, osgDB::readRefImageFile(rootDir + "/negx." + ext));
    cubemap->setImage(osg::TextureCubeMap::POSITIVE_Y, osgDB::readRefImageFile(rootDir + "/posy." + ext));
    cubemap->setImage(osg::TextureCubeMap::NEGATIVE_Y, osgDB::readRefImageFile(rootDir + "/negy." + ext));
    cubemap->setImage(osg::TextureCubeMap::POSITIVE_Z, osgDB::readRefImageFile(rootDir + "/posz." + ext));
    cubemap->setImage(osg::TextureCubeMap::NEGATIVE_Z, osgDB::readRefImageFile(rootDir + "/negz." + ext));

    cubemap->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    cubemap->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    cubemap->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);

    cubemap->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
    cubemap->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);

    return cubemap;
}

osg::ref_ptr<osg::Program> createParallaxCubeMapProgram()
{
    static const char* vertSource = R"glsl(
        #version 330 core

        in vec4 tangent;
        out vec3 vViewDirTangent;
        out vec2 vUv;

        void main()
        {
            vUv =  gl_MultiTexCoord0.xy;
            vec3 vNormal = gl_NormalMatrix * gl_Normal;
            vec3 vTangent = gl_NormalMatrix * tangent.xyz;
            vec3 vBitangent = normalize( cross( vNormal, vTangent ) * tangent.w );

            mat3 mTBN = transpose(mat3(vTangent, vBitangent, vNormal));

            vec4 mvPos = gl_ModelViewMatrix * vec4( gl_Vertex.xyz, 1.0 );
            vec3 viewDir = -mvPos.xyz;
            vViewDirTangent = mTBN * viewDir;

            gl_Position = gl_ProjectionMatrix * mvPos;
        }
    )glsl";

    static const char* fragSource = R"glsl(
        #version 330 core

        uniform samplerCube uCubeMap;
        uniform sampler2D uWallTexture;
        in vec3 vViewDirTangent;
        in vec2 vUv;

        out vec4 fragColor;

        float min3 (vec3 v) {
          return min (min (v.x, v.y), v.z);
        }

        void main()
        {
            vec2 uv = fract(vUv);
            vec3 sampleDir = normalize(vViewDirTangent);

            sampleDir *= vec3(-1.,-1.,1.);
            vec3 viewInv = 1. / sampleDir;

            vec3 pos = vec3(uv * 2.0 - 1.0, -1.0);
    
            float fmin = min3(abs(viewInv) - viewInv * pos);
            sampleDir = sampleDir * fmin + pos;

            vec4 cubeTexel = texture(uCubeMap, sampleDir);
            vec4 wallTexel = texture(uWallTexture, uv);
            fragColor = vec4(mix(cubeTexel.rgb, wallTexel.rgb, wallTexel.a), 1.0);
        }
    )glsl";

    auto program = new osg::Program;
    program->addShader(new osg::Shader(osg::Shader::VERTEX, vertSource));
    program->addShader(new osg::Shader(osg::Shader::FRAGMENT, fragSource));
    program->addBindAttribLocation("tangent", osg::Geometry::SECONDARY_COLORS);
    return program;
}

int main(int argc, char** argv)
{
    osg::Group* root = new osg::Group;

    auto cubemap = createCubeMap();

    {
        //auto quad = createQuadGeometry(2.0f);
        auto quad = createBuilding(2.0f);
        root->addChild(quad);
        auto parallaxCubeMapProgram = createParallaxCubeMapProgram();
        auto ss = quad->getOrCreateStateSet();
        ss->setAttributeAndModes(parallaxCubeMapProgram, osg::StateAttribute::ON);
        
        ss->setTextureAttributeAndModes(0, cubemap, osg::StateAttribute::ON);
        ss->addUniform(new osg::Uniform("uCubeMap", 0));
        std::string wallImage = "../data/InteriorMapping/commercial_building.png";
        //std::string wallImage = "../data/InteriorMapping/Window.dds"
        osg::Texture2D* wallTexture = new osg::Texture2D(osgDB::readRefImageFile(wallImage));

        wallTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        wallTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        ss->setTextureAttributeAndModes(1, wallTexture, osg::StateAttribute::ON);
        ss->addUniform(new osg::Uniform("uWallTexture", 1));
    }

    osgViewer::Viewer viewer;
    viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()));

    GL3RealizeOperation* realizer = new GL3RealizeOperation();
    realizer->setSyncToVBlank(false);
    viewer.setRealizeOperation(realizer);


    viewer.setSceneData(root);
    return viewer.run();
}