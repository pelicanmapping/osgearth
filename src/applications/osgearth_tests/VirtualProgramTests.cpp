/* osgEarth
 * Copyright 2026 Pelican Mapping
 * MIT License
 */
#include <osgEarth/catch.hpp>
#include <osgEarth/VirtualProgram>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/StringUtils>
#include <osgEarth/FileUtils>
#include <osg/GraphicsContext>
#include <osg/FrameStamp>
#include <osg/StateSet>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>
#include <fstream>
#include <thread>

using namespace osgEarth;

namespace
{
    osg::ref_ptr<osg::Program> simpleProgram()
    {
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX,
            "void main() { gl_Position = vec4(0.0); }"));
        return program;
    }

    struct Toggle : VirtualProgram::AcceptCallback
    {
        bool enabled = true;
        bool operator()(const osg::State&) override { return enabled; }
    };

    struct Context
    {
        osg::ref_ptr<osg::GraphicsContext> gc;
        std::vector<osg::ref_ptr<osg::StateSet>> stack;

        Context(osg::GraphicsContext* shared = nullptr)
        {
            Capabilities::get();
            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->readDISPLAY();
            traits->setUndefinedScreenDetailsToDefaultScreen();
            traits->width = traits->height = 16;
            traits->pbuffer = true;
            traits->doubleBuffer = false;
            traits->sharedContext = shared;
            gc = osg::GraphicsContext::createGraphicsContext(traits);
            REQUIRE(gc.valid());
            REQUIRE(gc->realize());
            REQUIRE(gc->makeCurrent());
            state().setUseVertexAttributeAliasing(true);
            state().setUseModelViewAndProjectionUniforms(true);
        }

        osg::State& state() { return *gc->getState(); }

        VirtualProgram* push()
        {
            osg::ref_ptr<osg::StateSet> ss = new osg::StateSet;
            auto vp = VirtualProgram::getOrCreate(ss);
            stack.push_back(ss);
            state().pushStateSet(ss);
            return vp;
        }

        osg::ref_ptr<const osg::Program> apply(VirtualProgram* vp)
        {
            vp->apply(state());
            auto pcp = state().getLastAppliedProgramObject();
            REQUIRE(pcp != nullptr);
            REQUIRE(pcp->isLinked());
            return pcp->getProgram();
        }

        ~Context()
        {
            state().popAllStateSets();
            state().setLastAppliedProgramObject(nullptr);
            for (auto& ss : stack)
                VirtualProgram::get(ss)->releaseGLObjects(&state());
            stack.clear();
            VirtualProgram::PolyShader::clearShaderCache();
            gc->releaseContext();
        }
    };

    std::string mainSource(const osg::Program* program, osg::Shader::Type type)
    {
        for (unsigned i = 0; i < program->getNumShaders(); ++i)
        {
            auto shader = program->getShader(i);
            if (shader->getType() == type && shader->getName().find("main(") == 0)
                return shader->getShaderSource();
        }
        return {};
    }

    bool containsSource(const osg::Program* program, const std::string& source)
    {
        for (unsigned i = 0; i < program->getNumShaders(); ++i)
            if (program->getShader(i)->getShaderSource().find(source) != std::string::npos)
                return true;
        return false;
    }

    const std::string firstSource = "void first(inout vec4 v) { v.x += 1.0; }";
    const std::string secondSource = "void second(inout vec4 v) { v.x *= 2.0; }";
}

TEST_CASE("ProgramRepo releases every alias of an unused program", "[virtualprogram]")
{
    ProgramRepo repo;
    auto a = simpleProgram();
    auto b = simpleProgram();
    repo.add({1}, a, 1, 10);
    repo.add({2}, b, 2, 20);
    REQUIRE(a == b);
    REQUIRE(repo.copy().size() == 2);
    REQUIRE(repo.copy().begin()->second->_frameLastUsed == 2);
    repo.release(10, nullptr);
    REQUIRE(repo.use({1}, 3, 20) == a);
    repo.release(20, nullptr);
    REQUIRE(repo.copy().empty());
    REQUIRE_FALSE(repo.use({1}, 4, 30).valid());
    REQUIRE_FALSE(repo.use({2}, 4, 30).valid());
}

TEST_CASE("ProgramRepo preserves existing ownership on repeated insertion", "[virtualprogram]")
{
    ProgramRepo repo;
    auto a = simpleProgram();
    repo.add({1}, a, 1, 10);
    auto b = simpleProgram();
    repo.add({1}, b, 2, 20);
    REQUIRE(a == b);
    repo.release(20, nullptr);
    REQUIRE(repo.use({1}, 3, 10) == a);
    repo.release(10, nullptr);
    REQUIRE(repo.copy().empty());
}

TEST_CASE("ProgramRepo distinguishes uniform block bindings", "[virtualprogram]")
{
    ProgramRepo repo;
    auto a = simpleProgram();
    auto b = simpleProgram();
    a->addBindUniformBlock("Block", 1);
    b->addBindUniformBlock("Block", 2);
    repo.add({1}, a, 1, 10);
    repo.add({2}, b, 1, 10);
    REQUIRE(a != b);
    repo.releaseGLObjects(nullptr);
    REQUIRE(repo.copy().empty());
    repo.add({1}, a, 2, 10);
    REQUIRE(repo.use({1}, 2, 10) == a);
}

TEST_CASE("ProgramRepo handles concurrent users with the caller lock", "[virtualprogram]")
{
    ProgramRepo repo;
    std::vector<std::thread> threads;
    for (unsigned user = 1; user <= 8; ++user)
        threads.emplace_back([&repo, user]() {
            auto program = simpleProgram();
            for (unsigned i = 0; i < 100; ++i)
            {
                std::lock_guard<ProgramRepo> lock(repo);
                repo.add({user}, program, i, user);
                repo.use({user}, i, user);
                repo.release(user, nullptr);
            }
        });
    for (auto& thread : threads)
        thread.join();
    REQUIRE(repo.copy().empty());
}

TEST_CASE("ProgramRepo shader edits preserve snapshots and program properties", "[virtualprogram]")
{
    ProgramRepo repo;
    auto original = simpleProgram();
    original->setName("editor test");
    original->addBindAttribLocation("position", 3);
    original->addBindFragDataLocation("color", 1);
    original->addBindUniformBlock("Block", 4);
    original->setParameter(GL_GEOMETRY_VERTICES_OUT_EXT, 6);
    original->getShader(0)->setShaderDefinesMode(osg::Shader::USE_MANUAL_SETTINGS);
    original->getShader(0)->getShaderDefines().insert("EDITOR_DEFINE");
    original->getShader(0)->getShaderRequirements().insert("EDITOR_REQUIREMENT");
    auto originalText = original->getShader(0)->getShaderSource();
    osg::ref_ptr<osg::Program> other = new osg::Program;
    other->addShader(original->getShader(0));
    repo.add({1}, original, 1, 10);
    auto alias = original;
    repo.add({2}, alias, 1, 20);
    repo.add({3}, other, 1, 30);
    const auto generation = repo.getGeneration();
    const std::string editedText = "void main() { gl_Position = vec4(1.0); }";
    auto edited = repo.editShader(original, 0, editedText);
    REQUIRE(edited.valid());
    REQUIRE(edited != original);
    REQUIRE(repo.getGeneration() != generation);
    REQUIRE(repo.use({1}, 2, 10) == edited);
    REQUIRE(repo.use({2}, 2, 20) == edited);
    REQUIRE(repo.use({3}, 2, 30) == other);
    REQUIRE(edited->getShader(0) != original->getShader(0));
    REQUIRE(original->getShader(0)->getShaderSource() == originalText);
    REQUIRE(other->getShader(0)->getShaderSource() == originalText);
    REQUIRE(edited->getShader(0)->getShaderSource() == editedText);
    REQUIRE(edited->getName() == original->getName());
    REQUIRE(edited->getAttribBindingList() == original->getAttribBindingList());
    REQUIRE(edited->getFragDataBindingList() == original->getFragDataBindingList());
    REQUIRE(edited->getUniformBlockBindingList() == original->getUniformBlockBindingList());
    REQUIRE(edited->getParameter(GL_GEOMETRY_VERTICES_OUT_EXT) == 6);
    REQUIRE(edited->getShader(0)->getShaderDefines().count("EDITOR_DEFINE") == 1);
    REQUIRE(edited->getShader(0)->getShaderRequirements().count("EDITOR_REQUIREMENT") == 1);
    REQUIRE(edited->getShaderDefines().count("EDITOR_DEFINE") == 1);
    const auto editedGeneration = repo.getGeneration();
    REQUIRE(repo.editShader(edited, 0, editedText) == edited);
    REQUIRE_FALSE(repo.editShader(original, 0, editedText).valid());
    REQUIRE_FALSE(repo.editShader(edited, 1, editedText).valid());
    REQUIRE(repo.getGeneration() == editedGeneration);

    // A new key requesting the original program must not acquire the debug edit.
    auto fresh = original;
    repo.add({4}, fresh, 2, 40);
    REQUIRE(fresh == original);
    REQUIRE(fresh != edited);
    repo.release(10, nullptr);
    repo.release(20, nullptr);
    REQUIRE_FALSE(repo.use({1}, 3, 50).valid());
    REQUIRE_FALSE(repo.use({2}, 3, 50).valid());
    repo.release(30, nullptr);
    repo.release(40, nullptr);
    REQUIRE(repo.copy().empty());
}

TEST_CASE("ProgramRepo shader edits merge equivalent programs and their owners", "[virtualprogram]")
{
    ProgramRepo repo;
    auto original = simpleProgram();
    auto target = simpleProgram();
    const std::string targetText = "void main() { gl_Position = vec4(2.0); }";
    target->getShader(0)->setShaderSource(targetText);
    repo.add({1}, original, 1, 10);
    auto alias = original;
    repo.add({2}, alias, 2, 20);
    repo.add({3}, target, 3, 20); // This user owns both entries before the merge.
    repo.add({4}, target, 4, 30);
    REQUIRE(repo.editShader(original, 0, targetText) == target);
    const auto entries = repo.copy();
    REQUIRE(entries.size() == 4);
    for (const auto& entry : entries)
    {
        REQUIRE(entry.second == entries.at({3}));
        REQUIRE(entry.second->_program == target);
        REQUIRE(entry.second->_frameLastUsed == 4);
        REQUIRE(entry.second->_users.size() == 3);
    }
    repo.release(10, nullptr);
    repo.release(20, nullptr);
    REQUIRE(repo.use({1}, 5, 30) == target);
    repo.release(30, nullptr);
    REQUIRE(repo.copy().empty());
}

TEST_CASE("PolyShader identity includes injection location", "[virtualprogram]")
{
    using VP = VirtualProgram;
    osg::ref_ptr<VP::PolyShader> model = VP::PolyShader::lookUpShader("first", firstSource, VP::LOCATION_VERTEX_MODEL);
    osg::ref_ptr<VP::PolyShader> view = VP::PolyShader::lookUpShader("first", firstSource, VP::LOCATION_VERTEX_VIEW);
    REQUIRE(model != view);
    REQUIRE(model->getLocation() == VP::LOCATION_VERTEX_MODEL);
    REQUIRE(view->getLocation() == VP::LOCATION_VERTEX_VIEW);
    REQUIRE(VP::PolyShader::lookUpShader("first", firstSource, VP::LOCATION_VERTEX_MODEL) == model);
    REQUIRE(view->getShader(VP::STAGE_GEOMETRY)->getType() == osg::Shader::GEOMETRY);
    VP::PolyShader::clearShaderCache();
    REQUIRE(VP::PolyShader::lookUpShader("first", firstSource, VP::LOCATION_VERTEX_MODEL) != model);
}

TEST_CASE("PolyShader lookup preserves requested content after public edits", "[virtualprogram]")
{
    using VP = VirtualProgram;
    osg::ref_ptr<VP::PolyShader> edited = VP::PolyShader::lookUpShader(
        "first", firstSource, VP::LOCATION_VERTEX_MODEL);
    edited->setShaderSource("void first(inout vec4 v) { v.x += 6.0; }");
    edited->prepare();
    osg::ref_ptr<VP::PolyShader> original = VP::PolyShader::lookUpShader(
        "first", firstSource, VP::LOCATION_VERTEX_MODEL);
    REQUIRE(original != edited);
    REQUIRE(original->getShaderSource().find("v.x += 1.0;") != std::string::npos);
    REQUIRE(edited->getShaderSource().find("v.x += 6.0;") != std::string::npos);
    VP::PolyShader::clearShaderCache();
}

TEST_CASE("VirtualProgram shader names survive legacy 32 bit hash collisions", "[virtualprogram]")
{
    const std::string a = "vp_44682", b = "vp_63996";
    REQUIRE(static_cast<unsigned>(hashString(a)) == static_cast<unsigned>(hashString(b)));
    osg::ref_ptr<VirtualProgram> vp = new VirtualProgram;
    vp->setShader(a, new osg::Shader(osg::Shader::VERTEX, "void a() {}"));
    vp->setShader(b, new osg::Shader(osg::Shader::VERTEX, "void b() {}"));
    VirtualProgram::ShaderMap shaders;
    vp->getShaderMap(shaders);
    REQUIRE(shaders.size() == 2);
    REQUIRE(vp->getPolyShader(a) != vp->getPolyShader(b));
    vp->removeShader(a);
    REQUIRE(vp->getPolyShader(a) == nullptr);
    REQUIRE(vp->getPolyShader(b) != nullptr);
}

TEST_CASE("VirtualProgram dirty preserves shader-cache content before the first draw", "[virtualprogram]")
{
    using VP = VirtualProgram;
    osg::ref_ptr<VP> vp = new VP;
    vp->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL);
    osg::ref_ptr<VP::PolyShader> edited = vp->getPolyShader("first");
    edited->getNominalShader()->setShaderSource("void first(inout vec4 v) { v.x += 7.0; }");
    vp->dirty();
    osg::ref_ptr<VP> other = new VP;
    other->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL);
    auto original = other->getPolyShader("first");
    REQUIRE(original != edited);
    REQUIRE(original->getNominalShader()->getShaderSource().find("v.x += 1.0;") != std::string::npos);
    REQUIRE(edited->getNominalShader()->getShaderSource().find("v.x += 7.0;") != std::string::npos);
    VP::PolyShader::clearShaderCache();
}

TEST_CASE("ProgramRepo keys preserve order and multiplicity", "[virtualprogram]")
{
    ProgramRepo repo;
    auto a = simpleProgram();
    auto b = simpleProgram();
    b->addBindAttribLocation("other", 1);
    repo.add({1, 2}, a, 1, 10);
    repo.add({2, 1}, b, 1, 10);
    repo.add({1, 1, 2}, a, 1, 10);
    REQUIRE(repo.copy().size() == 3);
    REQUIRE(repo.use({1, 2}, 2, 10) == a);
    REQUIRE(repo.use({2, 1}, 2, 10) == b);
    repo.release(10, nullptr);
    REQUIRE(repo.copy().empty());
}

TEST_CASE("VirtualProgram moves functions and copies extensions", "[virtualprogram]")
{
    using VP = VirtualProgram;
    osg::ref_ptr<VP> vp = new VP;
    vp->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL);
    vp->setFunction("first", firstSource, VP::LOCATION_VERTEX_VIEW);
    VP::FunctionLocationMap functions;
    vp->getFunctions(functions);
    REQUIRE(functions[VP::LOCATION_VERTEX_MODEL].empty());
    REQUIRE(functions[VP::LOCATION_VERTEX_VIEW].size() == 1);
    vp->addGLSLExtension("GL_ARB_shader_bit_encoding");
    osg::ref_ptr<VP> copy = new VP(*vp);
    REQUIRE(copy->hasGLSLExtension("GL_ARB_shader_bit_encoding"));
}

// Hidden from the default suite so headless runners need no graphics context.
// Run explicitly with [virtualprogram] to include these real OpenGL checks.
TEST_CASE("VirtualProgram composition cache follows runtime state", "[virtualprogram][.gl]")
{
    using VP = VirtualProgram;
    Context context;
    auto parent = context.push();
    parent->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL, 1.0f);
    auto child = context.push();
    child->setFunction("second", secondSource, VP::LOCATION_VERTEX_MODEL, 2.0f);
    auto original = context.apply(child);
    const auto source = mainSource(original, osg::Shader::VERTEX);
    REQUIRE(source.find("first(vp_Vertex)") < source.find("second(vp_Vertex)"));
    REQUIRE(context.apply(child) == original);

    SECTION("order edits on an ancestor rebuild main")
    {
        parent->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL, 3.0f);
        auto changed = context.apply(child);
        REQUIRE(changed != original);
        auto changedSource = mainSource(changed, osg::Shader::VERTEX);
        REQUIRE(changedSource.find("second(vp_Vertex)") < changedSource.find("first(vp_Vertex)"));
    }
    SECTION("repeated schedule edits reuse existing keys")
    {
        const auto initialSize = Registry::programRepo().copy().size();
        for (unsigned i = 0; i < 20; ++i)
        {
            parent->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL, 3.0f);
            context.apply(child);
            parent->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL, 1.0f);
            REQUIRE(context.apply(child) == original);
        }
        REQUIRE(Registry::programRepo().copy().size() == initialSize + 1);
    }
    SECTION("vertex bindings participate in the key")
    {
        child->addBindAttribLocation("customAttribute", 7);
        auto changed = context.apply(child);
        REQUIRE(changed != original);
        REQUIRE(changed->getAttribBindingList().at("customAttribute") == 7);
        child->removeBindAttribLocation("customAttribute");
        REQUIRE(context.apply(child) == original);
    }
    SECTION("template changes participate in the key")
    {
        child->getTemplate()->addBindUniformBlock("Block", 4);
        child->getTemplate()->addBindFragDataLocation("customOutput", 1);
        auto changed = context.apply(child);
        REQUIRE(changed != original);
        REQUIRE(changed->getUniformBlockBindingList().at("Block") == 4);
        REQUIRE(changed->getFragDataBindingList().at("customOutput") == 1);
    }
    SECTION("extensions participate in the key")
    {
        parent->addGLSLExtension("GL_ARB_shader_bit_encoding");
        auto changed = context.apply(child);
        REQUIRE(changed != original);
        REQUIRE(mainSource(changed, osg::Shader::VERTEX).find("GL_ARB_shader_bit_encoding") != std::string::npos);
        parent->removeGLSLExtension("GL_ARB_shader_bit_encoding");
        REQUIRE(context.apply(child) == original);
    }
    SECTION("callbacks are reevaluated within the same frame")
    {
        osg::ref_ptr<Toggle> toggle = new Toggle;
        parent->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL, toggle, 1.0f);
        auto enabled = context.apply(child);
        toggle->enabled = false;
        auto disabled = context.apply(child);
        REQUIRE(enabled != disabled);
        REQUIRE(mainSource(disabled, osg::Shader::VERTEX).find("first(vp_Vertex)") == std::string::npos);
        toggle->enabled = true;
        REQUIRE(context.apply(child) == enabled);
    }
    SECTION("inheritance and masks select the correct ancestor functions")
    {
        child->setInheritShaders(false);
        auto isolated = context.apply(child);
        REQUIRE(mainSource(isolated, osg::Shader::VERTEX).find("first(vp_Vertex)") == std::string::npos);
        child->setInheritShaders(true);
        parent->setMask(1);
        child->setMask(2);
        REQUIRE(mainSource(context.apply(child), osg::Shader::VERTEX) ==
            mainSource(isolated, osg::Shader::VERTEX));
        child->setMask(1);
        REQUIRE(mainSource(context.apply(child), osg::Shader::VERTEX) == source);
    }
    SECTION("identical source in different shader stages is distinct")
    {
        child->setShader("helper", new osg::Shader(osg::Shader::VERTEX, "void helper() {}"));
        auto vertex = context.apply(child);
        child->setShader("helper", new osg::Shader(osg::Shader::FRAGMENT, "void helper() {}"));
        auto fragment = context.apply(child);
        REQUIRE(vertex != fragment);
    }
    SECTION("releasing GL objects allows the program to be rebuilt")
    {
        child->releaseGLObjects(&context.state());
        auto rebuilt = context.apply(child);
        REQUIRE(mainSource(rebuilt, osg::Shader::VERTEX) == source);
    }
}

TEST_CASE("VirtualProgram binary cache uses stable validated content keys", "[virtualprogram][.gl]")
{
    Context context;
    struct BinaryCacheScope
    {
        std::string folder = "virtualprogram_binary_test_" + std::to_string(createUID());
        BinaryCacheScope() { VirtualProgram::setProgramBinaryCacheLocation(folder); }
        ~BinaryCacheScope()
        {
            VirtualProgram::setProgramBinaryCacheLocation("");
            Util::removeDirectory(folder);
        }
    } cache;
    auto vp = context.push();
    vp->setFunction("first", firstSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    auto original = context.apply(vp);
    auto files = osgDB::getDirectoryContents(cache.folder);
    std::string binaryFile;
    for (const auto& file : files)
        if (osgDB::getFileExtension(file) == "bin")
            binaryFile = osgDB::concatPaths(cache.folder, file);
    REQUIRE_FALSE(binaryFile.empty());

    // Recreate identical shader content with new process-local identities.
    context.state().setLastAppliedProgramObject(nullptr);
    vp->releaseGLObjects(&context.state());
    VirtualProgram::PolyShader::clearShaderCache();
    vp->setFunction("first", firstSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    auto cached = context.apply(vp);
    REQUIRE(cached->getPCP(context.state())->loadedBinary());
    REQUIRE(cached->getProgramBinary() == nullptr);

    // A truncated or mismatched descriptor must compile from source and repair
    // the file, without attaching a binary from a previous link attempt.
    context.state().setLastAppliedProgramObject(nullptr);
    vp->releaseGLObjects(&context.state());
    {
        std::ofstream corrupt(binaryFile, std::ios::binary | std::ios::trunc);
        corrupt << 'x';
    }
    auto repaired = context.apply(vp);
    REQUIRE_FALSE(repaired->getPCP(context.state())->loadedBinary());
    REQUIRE(repaired->getProgramBinary() == nullptr);
    REQUIRE(mainSource(repaired, osg::Shader::VERTEX) == mainSource(original, osg::Shader::VERTEX));

    // Keep a full file but alter its descriptor; filename equality alone must
    // never authorize using this binary.
    context.state().setLastAppliedProgramObject(nullptr);
    vp->releaseGLObjects(&context.state());
    {
        std::fstream corrupt(binaryFile, std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekp(sizeof(std::uint64_t));
        corrupt.put('x');
    }
    auto mismatched = context.apply(vp);
    REQUIRE_FALSE(mismatched->getPCP(context.state())->loadedBinary());

    // A driver-rejected binary must also fall back and repair the cache during
    // the same apply, rather than leaving an unusable PerContextProgram.
    context.state().setLastAppliedProgramObject(nullptr);
    vp->releaseGLObjects(&context.state());
    {
        std::fstream corrupt(binaryFile, std::ios::binary | std::ios::in | std::ios::out);
        std::uint64_t count = 0;
        corrupt.read(reinterpret_cast<char*>(&count), sizeof(count));
        REQUIRE(corrupt.good());
        corrupt.seekp(sizeof(count) + count * sizeof(std::uint64_t));
        GLenum invalidFormat = 0xffffffffu;
        corrupt.write(reinterpret_cast<const char*>(&invalidFormat), sizeof(invalidFormat));
    }
    auto fallback = context.apply(vp);
    REQUIRE_FALSE(fallback->getPCP(context.state())->loadedBinary());
    REQUIRE(fallback->getProgramBinary() == nullptr);
    context.state().setLastAppliedProgramObject(nullptr);
    vp->releaseGLObjects(&context.state());
    REQUIRE(context.apply(vp)->getPCP(context.state())->loadedBinary());
}

TEST_CASE("VirtualProgram preserves earlier occurrences of a shared attribute", "[virtualprogram][.gl]")
{
    Context context;
    auto shared = context.push();
    shared->setFunction("first", firstSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    shared->addBindAttribLocation("custom", 1);
    auto middle = context.push();
    middle->setFunction("second", secondSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    middle->addBindAttribLocation("custom", 2);
    osg::ref_ptr<osg::StateSet> leaf = new osg::StateSet;
    leaf->setAttributeAndModes(shared, osg::StateAttribute::ON);
    context.stack.push_back(leaf);
    context.state().pushStateSet(leaf);
    auto program = context.apply(shared);
    REQUIRE(program->getAttribBindingList().at("custom") == 1);
}

TEST_CASE("VirtualProgram applies notified shader edits and retained template edits", "[virtualprogram][.gl]")
{
    using VP = VirtualProgram;
    Context context;
    auto vp = context.push();
    vp->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL);

    SECTION("a retained template pointer does not need another getter call")
    {
        auto properties = vp->getTemplate();
        auto original = context.apply(vp);
        REQUIRE(context.apply(vp) == original);
        properties->addBindUniformBlock("LaterBlock", 6);
        auto changed = context.apply(vp);
        REQUIRE(changed != original);
        REQUIRE(changed->getUniformBlockBindingList().at("LaterBlock") == 6);
        properties->removeBindUniformBlock("LaterBlock");
        REQUIRE(context.apply(vp) == original);
    }

    SECTION("caller-owned shaders and cached programs have independent content")
    {
        osg::ref_ptr<osg::Shader> shader = new osg::Shader(osg::Shader::VERTEX,
            "float external_helper() { return 1.0; }");
        vp->setShader("external_helper", shader);
        auto original = context.apply(vp);
        context.apply(vp);
        context.apply(vp);
        // Same-length edits through retained pointers require a notification.
        shader->setShaderSource("float external_helper() { return 2.0; }");
        REQUIRE(context.apply(vp) == original);
        vp->dirty();
        auto changed = context.apply(vp);
        REQUIRE(changed != original);
        REQUIRE(containsSource(changed, "return 2.0;"));
        REQUIRE(containsSource(original, "return 1.0;"));
        REQUIRE_FALSE(containsSource(original, "return 2.0;"));
        REQUIRE(context.apply(vp) == changed);

        shader->setShaderDefinesMode(osg::Shader::USE_MANUAL_SETTINGS);
        shader->getShaderDefines().insert("EXTERNAL_DEFINE");
        vp->dirty();
        auto metadataChanged = context.apply(vp);
        REQUIRE(metadataChanged != changed);
        REQUIRE(metadataChanged->getShaderDefines().count("EXTERNAL_DEFINE") == 1);
    }

    SECTION("an exposed prepared shader changes after dirty notification")
    {
        auto poly = vp->getPolyShader("first");
        auto shader = poly->getNominalShader();
        auto original = context.apply(vp);
        context.apply(vp);
        context.apply(vp);
        const auto keyCount = Registry::programRepo().copy().size();
        for (unsigned i = 0; i < 10; ++i) REQUIRE(context.apply(vp) == original);
        REQUIRE(Registry::programRepo().copy().size() == keyCount);
        shader->setShaderSource("void first(inout vec4 v) { v.x += 9.0; }");
        REQUIRE(context.apply(vp) == original);
        vp->dirty();
        auto changed = context.apply(vp);
        REQUIRE(changed != original);
        REQUIRE(containsSource(changed, "v.x += 9.0;"));
        REQUIRE(containsSource(original, "v.x += 1.0;"));
    }

    SECTION("preparing a shared PolyShader invalidates all of its owners")
    {
        osg::ref_ptr<osg::StateSet> otherStateSet = new osg::StateSet;
        auto other = VP::getOrCreate(otherStateSet);
        other->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL);
        auto poly = vp->getPolyShader("first");
        REQUIRE(poly == other->getPolyShader("first"));
        auto original = context.apply(vp);
        context.state().popStateSet();
        context.state().pushStateSet(otherStateSet);
        REQUIRE(context.apply(other) == original);
        poly->setShaderSource("void first(inout vec4 v) { v.x += 8.0; }");
        poly->prepare();
        auto changed = context.apply(other);
        REQUIRE(changed != original);
        context.state().popStateSet();
        context.state().pushStateSet(context.stack.back());
        REQUIRE(context.apply(vp) == changed);
        other->releaseGLObjects(&context.state());
    }
}

TEST_CASE("VirtualProgram dirty invalidates every owner of a shared shader", "[virtualprogram][.gl]")
{
    using VP = VirtualProgram;
    Context context;
    auto vp = context.push();
    vp->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL);
    vp->setFunction("second", secondSource, VP::LOCATION_VERTEX_MODEL);
    osg::ref_ptr<osg::StateSet> otherStateSet = new osg::StateSet;
    auto other = VP::getOrCreate(otherStateSet);
    other->setFunction("first", firstSource, VP::LOCATION_VERTEX_MODEL);
    other->setFunction("second", secondSource, VP::LOCATION_VERTEX_MODEL);
    osg::ref_ptr<osg::Shader> shader;
    std::string source, oldText, newText;

    SECTION("setFunction shares a PolyShader")
    {
        REQUIRE(vp->getPolyShader("first") == other->getPolyShader("first"));
        shader = vp->getPolyShader("first")->getNominalShader();
        source = "void first(inout vec4 v) { v.x += 5.0; }";
        oldText = "v.x += 1.0;";
        newText = "v.x += 5.0;";
    }
    SECTION("setShader shares a raw shader through separate PolyShaders")
    {
        shader = new osg::Shader(osg::Shader::VERTEX, "float helper() { return 1.0; }");
        vp->setShader("helper", shader);
        other->setShader("helper", shader);
        REQUIRE(vp->getPolyShader("helper") != other->getPolyShader("helper"));
        source = "float helper() { return 5.0; }";
        oldText = "return 1.0;";
        newText = "return 5.0;";
    }

    auto original = context.apply(vp);
    context.apply(vp);
    context.state().popStateSet();
    context.state().pushStateSet(otherStateSet);
    REQUIRE(context.apply(other) == original);
    context.apply(other);
    shader->setShaderSource(source);
    vp->dirty(); // Notify one owner, then draw the other owner first.
    auto changed = context.apply(other);
    REQUIRE(changed != original);
    REQUIRE(containsSource(changed, newText));
    REQUIRE(containsSource(original, oldText));
    REQUIRE_FALSE(containsSource(original, newText));
    context.state().popStateSet();
    context.state().pushStateSet(context.stack.back());
    REQUIRE(context.apply(vp) == changed);

    // Unchanged shaders retain the compiled version shared with old programs.
    const osg::Shader* unchanged = nullptr;
    for (unsigned i = 0; i < original->getNumShaders(); ++i)
        if (original->getShader(i)->getName() == "second") unchanged = original->getShader(i);
    REQUIRE(unchanged != nullptr);
    bool shared = false;
    for (unsigned i = 0; i < changed->getNumShaders(); ++i)
        shared |= changed->getShader(i) == unchanged;
    REQUIRE(shared);

    // Repeated notifications without content changes do not create aliases.
    const auto keyCount = Registry::programRepo().copy().size();
    for (unsigned i = 0; i < 3; ++i)
    {
        vp->dirty();
        REQUIRE(context.apply(vp) == changed);
        REQUIRE(context.apply(vp) == changed);
        REQUIRE(Registry::programRepo().copy().size() == keyCount);
    }
    other->releaseGLObjects(&context.state());
}

TEST_CASE("VirtualProgram shares immutable shader versions between compositions", "[virtualprogram][.gl]")
{
    Context context;
    auto parent = context.push();
    parent->setFunction("first", firstSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    auto a = context.apply(parent);
    auto child = context.push();
    child->setFunction("second", secondSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    auto b = context.apply(child);
    REQUIRE(a != b);
    const osg::Shader* shared = nullptr;
    for (unsigned i = 0; i < a->getNumShaders(); ++i)
        if (a->getShader(i)->getName() == "first") shared = a->getShader(i);
    REQUIRE(shared != nullptr);
    bool found = false;
    for (unsigned i = 0; i < b->getNumShaders(); ++i)
        found |= b->getShader(i) == shared;
    REQUIRE(found);
}

TEST_CASE("VirtualProgram PCP reuse matches OSG define selection", "[virtualprogram][.gl]")
{
    Context context;
    auto vp = context.push();
    vp->setFunction("define_test",
        "#pragma import_defines(A,EMPTY,FLAG,FN)\n"
        "void define_test(inout vec4 v) {\n#ifdef A\nv.x += A;\n#endif\n}",
        VirtualProgram::LOCATION_VERTEX_MODEL);
    auto program = context.apply(vp);
    osg::ref_ptr<const osg::Program::PerContextProgram> initial = context.state().getLastAppliedProgramObject();
    osg::ref_ptr<osg::StateSet> defines = new osg::StateSet;
    defines->setDefine("A", "2.0");
    defines->setDefine("EMPTY");
    defines->setDefine("FN", "(x) ((x) + 1.0)");
    context.state().pushStateSet(defines);
    auto check = [&]() {
        // Consume the dirty flag before apply; this must not hide a change.
        auto text = context.state().getDefineString(program->getShaderDefines());
        auto expected = program->getPCP(context.state());
        REQUIRE(context.apply(vp) == program);
        REQUIRE(context.state().getLastAppliedProgramObject() == expected);
        REQUIRE(expected->getDefineString() == text);
        REQUIRE_FALSE(expected->needsLink());
        return expected;
    };
    auto defined = check();
    REQUIRE(defined != initial);
    REQUIRE(check() == defined);

    context.state().popStateSet();
    defines->setDefine("A", "3.0");
    context.state().pushStateSet(defines);
    auto changed = check();
    REQUIRE(changed != defined);

    context.state().popStateSet();
    defines->setDefine("UNRELATED", "123");
    context.state().pushStateSet(defines);
    REQUIRE(check() == changed);

    context.state().popStateSet();
    defines->setDefine("A", "2.0", osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
    context.state().pushStateSet(defines);
    REQUIRE(check() == defined);
    osg::ref_ptr<osg::StateSet> child = new osg::StateSet;
    child->setDefine("A", "3.0");
    context.state().pushStateSet(child);
    REQUIRE(check() == defined);
    context.state().popStateSet();
    child->setDefine("A", "3.0", osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
    context.state().pushStateSet(child);
    REQUIRE(check() == changed);

    context.state().popStateSet();
    context.state().popStateSet();
    REQUIRE(check() == initial);
    auto other = simpleProgram();
    other->apply(context.state());
    REQUIRE(context.state().getLastAppliedProgramObject() != initial);
    REQUIRE(check() == initial);
}

TEST_CASE("VirtualProgram cached PCPs follow GL lifetime and dirty programs", "[virtualprogram][.gl]")
{
    Context context;
    auto vp = context.push();
    vp->setFunction("first", firstSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    auto program = context.apply(vp);
    osg::ref_ptr<const osg::Program::PerContextProgram> pcp = context.state().getLastAppliedProgramObject();
    context.apply(vp);

    SECTION("dirtying an already bound program relinks it")
    {
        const_cast<osg::Program*>(program.get())->dirtyProgram();
        REQUIRE(pcp->needsLink());
        REQUIRE(context.apply(vp) == program);
        REQUIRE_FALSE(pcp->needsLink());
    }
    SECTION("direct program release invalidates retained PCPs")
    {
        program->releaseGLObjects(&context.state());
        REQUIRE(context.apply(vp) == program);
        REQUIRE(context.state().getLastAppliedProgramObject() != pcp);
    }
    SECTION("local caches do not retain released GL handles")
    {
        osg::observer_ptr<const osg::Program::PerContextProgram> observed(pcp.get());
        pcp = nullptr;
        context.state().setLastAppliedProgramObject(nullptr);
        program->releaseGLObjects(&context.state());
        REQUIRE_FALSE(observed.valid());
        REQUIRE(context.apply(vp) == program);
    }
    SECTION("repository clear forces ownership registration and a fresh program")
    {
        {
            std::lock_guard<ProgramRepo> lock(Registry::programRepo());
            Registry::programRepo().releaseGLObjects(&context.state());
        }
        REQUIRE(context.apply(vp) != program);
        REQUIRE_FALSE(Registry::programRepo().copy().empty());
    }
    SECTION("releasing one owner does not lose another cached owner's registration")
    {
        auto parent = vp;
        auto child = context.push();
        child->setInheritShaders(true);
        auto shared = context.apply(child);
        REQUIRE(shared == program);
        parent->releaseGLObjects(&context.state());
        REQUIRE(context.apply(child) == program);
        child->releaseGLObjects(&context.state());
        REQUIRE(Registry::programRepo().copy().empty());
    }
}

TEST_CASE("VirtualProgram applies ShaderGUI edits without mutating shared shaders", "[virtualprogram][.gl]")
{
    Context context;
    auto parent = context.push();
    parent->setFunction("first", firstSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    auto original = context.apply(parent);
    osg::ref_ptr<const osg::Program::PerContextProgram> originalPCP = context.state().getLastAppliedProgramObject();
    auto child = context.push();
    child->setFunction("second", secondSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    auto childProgram = context.apply(child);
    context.apply(child);
    REQUIRE(childProgram != original);
    unsigned shaderIndex = original->getNumShaders();
    std::string source;
    SECTION("editing a shared function changes only the selected composition")
    {
        for (unsigned i = 0; i < original->getNumShaders(); ++i)
            if (original->getShader(i)->getName() == "first") shaderIndex = i;
        source = "void first(inout vec4 v) { v.x += 4.0; }";
    }
    SECTION("generated main shaders can also be edited")
    {
        for (unsigned i = 0; i < original->getNumShaders(); ++i)
            if (original->getShader(i)->getType() == osg::Shader::VERTEX &&
                original->getShader(i)->getName().find("main(") == 0) shaderIndex = i;
        REQUIRE(shaderIndex < original->getNumShaders());
        source = original->getShader(shaderIndex)->getShaderSource() + "\n// ShaderGUI edit\n";
    }
    REQUIRE(shaderIndex < original->getNumShaders());
    const auto previousSource = original->getShader(shaderIndex)->getShaderSource();
    auto edited = Registry::programRepo().editShader(
        const_cast<osg::Program*>(original.get()), shaderIndex, source);
    REQUIRE(edited.valid());
    REQUIRE(edited != original);
    REQUIRE(original->getShader(shaderIndex)->getShaderSource() == previousSource);
    REQUIRE(context.apply(child) == childProgram);
    REQUIRE(parent->getPolyShader("first")->getNominalShader()->getShaderSource().find("v.x += 1.0;") != std::string::npos);
    context.state().popStateSet();
    REQUIRE(context.apply(parent) == edited);
    REQUIRE(context.state().getLastAppliedProgramObject() != originalPCP);
    REQUIRE(edited->getShader(shaderIndex)->getShaderSource() == source);
    REQUIRE(context.apply(parent) == edited);
    // The editor's Compile button still uses this supported release path.
    edited->releaseGLObjects(nullptr);
    REQUIRE(context.apply(parent) == edited);
}

TEST_CASE("VirtualProgram local hits retain frame usage bookkeeping", "[virtualprogram][.gl]")
{
    Context context;
    auto vp = context.push();
    vp->setFunction("first", firstSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    osg::ref_ptr<osg::FrameStamp> frame = new osg::FrameStamp;
    context.state().setFrameStamp(frame);
    frame->setFrameNumber(100);
    auto program = context.apply(vp);
    context.apply(vp);
    frame->setFrameNumber(101);
    REQUIRE(context.apply(vp) == program);
    for (const auto& entry : Registry::programRepo().copy())
        if (entry.second->_program == program)
            REQUIRE(entry.second->_frameLastUsed == 101);
}

TEST_CASE("VirtualProgram separates States that share a GL context ID", "[virtualprogram][.gl]")
{
    Context first;
    auto vp = first.push();
    vp->setFunction("first", "#pragma import_defines(SHARED_VALUE)\n" + firstSource,
        VirtualProgram::LOCATION_VERTEX_MODEL);
    auto program = first.apply(vp);
    osg::ref_ptr<const osg::Program::PerContextProgram> firstPCP = first.state().getLastAppliedProgramObject();
    {
        Context second(first.gc);
        REQUIRE(first.state().getContextID() == second.state().getContextID());
        osg::ref_ptr<osg::StateSet> ss = new osg::StateSet;
        ss->setAttributeAndModes(vp, osg::StateAttribute::ON);
        ss->setDefine("SHARED_VALUE", "1");
        second.stack.push_back(ss);
        second.state().pushStateSet(ss);
        REQUIRE(second.apply(vp) == program);
        auto secondPCP = second.state().getLastAppliedProgramObject();
        REQUIRE(secondPCP != firstPCP);
        for (unsigned i = 0; i < 5; ++i)
        {
            REQUIRE(first.gc->makeCurrent());
            REQUIRE(first.apply(vp) == program);
            REQUIRE(first.state().getLastAppliedProgramObject() == firstPCP);
            REQUIRE(second.gc->makeCurrent());
            REQUIRE(second.apply(vp) == program);
            REQUIRE(second.state().getLastAppliedProgramObject() == secondPCP);
        }
        second.gc->releaseContext();
        std::atomic<unsigned> ready{0};
        std::atomic_bool correct{true};
        auto draw = [&](Context& context, const osg::Program::PerContextProgram* expected) {
            const bool current = context.gc->makeCurrent();
            if (!current) correct = false;
            ++ready;
            while (ready.load() != 2) std::this_thread::yield();
            if (current)
            {
                for (unsigned i = 0; i < 100; ++i)
                {
                    vp->apply(context.state());
                    if (context.state().getLastAppliedProgramObject() != expected)
                        correct = false;
                }
                context.gc->releaseContext();
            }
        };
        std::thread a(draw, std::ref(first), firstPCP.get());
        std::thread b(draw, std::ref(second), secondPCP);
        a.join();
        b.join();
        REQUIRE(correct.load());
        REQUIRE(second.gc->makeCurrent());
    }
    REQUIRE(first.gc->makeCurrent());
    REQUIRE(first.apply(vp).valid());
}

TEST_CASE("VirtualProgram resolves alternating parent paths with the same leaf", "[virtualprogram][.gl]")
{
    Context context;
    auto root = context.push();
    root->setFunction("first", firstSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    auto leaf = context.push();
    leaf->setFunction("second", secondSource, VirtualProgram::LOCATION_VERTEX_MODEL);
    auto original = context.apply(leaf);
    osg::ref_ptr<osg::StateSet> alternate = new osg::StateSet;
    auto otherRoot = VirtualProgram::getOrCreate(alternate);
    otherRoot->setFunction("first", "void first(inout vec4 v) { v.x += 7.0; }",
        VirtualProgram::LOCATION_VERTEX_MODEL);
    for (unsigned i = 0; i < 5; ++i)
    {
        context.state().popAllStateSets();
        context.state().pushStateSet(alternate);
        context.state().pushStateSet(context.stack.back());
        auto other = context.apply(leaf);
        REQUIRE(other != original);
        REQUIRE(containsSource(other, "v.x += 7.0;"));
        context.state().popAllStateSets();
        for (auto& ss : context.stack) context.state().pushStateSet(ss);
        REQUIRE(context.apply(leaf) == original);
    }
    otherRoot->releaseGLObjects(&context.state());
}

