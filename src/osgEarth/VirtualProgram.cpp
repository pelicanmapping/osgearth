/* osgEarth
* Copyright 2025 Pelican Mapping
* MIT License
*/
#include "VirtualProgram"
#include "Registry"
#include "ShaderFactory"
#include "ShaderLoader"
#include "ShaderUtils"
#include "ShaderMerger"
#include "StringUtils"
#include "Containers"
#include "Metrics"
#include "GLUtils"

#include <osg/Shader>
#include <osg/Program>
#include <osg/State>
#include <osg/Notify>
#include <osg/Version>
#include <osg/GL2Extensions>
#include <osg/GLExtensions>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>

#include <fstream>
#include <sstream>
#include <cstdlib> // getenv
#include <cstring>
#include <tuple>
#include <limits>

using namespace osgEarth;
using namespace osgEarth::Threading;

#define OE_TEST OE_NULL
//#define OE_TEST OE_NOTICE

#define MAX_CONTEXTS 16

#define MAX_PROGRAM_CACHE_SIZE 128

#define PREALLOCATE_APPLY_VARS

#define USE_PROGRAM_REPO

// Without a program repo, we need to store a refptr to the actual Program somewhere.
#ifndef USE_PROGRAM_REPO
#define USE_LAST_USED_PROGRAM
#endif

// Don't use this until we make it safe (combine with ProgramRepo or something) -gw
// MERGE: We can use this, right?
#define USE_POLYSHADER_CACHE

// Use typeid in lieu of dynamic_cast on inner loops
// Pro: faster. Con: cannot derive from VirtualProgram.
#define USE_TYPEID

#define MAKE_SHADER_ID(X) internShaderName( X )

//------------------------------------------------------------------------

namespace
{
    // Mutations are rare relative to draws. Invalidate conservatively across all
    // VPs, including shared PolyShaders, without maintaining reverse graphs.
    std::atomic<std::uint64_t> s_compositionGeneration{1};
    // Raw shader edits require an explicit dirty() notification. Check their
    // content once after that notification, never on a warm apply(). A global
    // generation also covers osg::Shaders registered through separate wrappers.
    std::atomic<std::uint64_t> s_shaderMutationGeneration{1};
    void compositionChanged()
    {
        s_compositionGeneration.fetch_add(1, std::memory_order_release);
    }

    // Observe even direct releaseGLObjects calls on a generated program. A PCP
    // retained by a draw cache must not outlive its membership in OSG's PCP list.
    class ComposedProgram : public osg::Program
    {
    public:
        ComposedProgram() = default;
        explicit ComposedProgram(const osg::Program& rhs) : osg::Program(rhs) { }
        mutable std::atomic<std::uint64_t> glGeneration{1};
        void releaseGLObjects(osg::State* state) const override
        {
            glGeneration.fetch_add(1, std::memory_order_release);
            osg::Program::releaseGLObjects(state);
        }
        void resizeGLObjectBuffers(unsigned size) override
        {
            glGeneration.fetch_add(1, std::memory_order_release);
            osg::Program::resizeGLObjectBuffers(size);
        }
    };

    osg::Shader* copyShaderContent(const osg::Shader& shader)
    {
        auto copy = new osg::Shader(shader);
        // OSG 3.6's copy constructor omits these and shares the binary buffer.
        copy->getShaderDefines() = shader.getShaderDefines();
        copy->getShaderRequirements() = shader.getShaderRequirements();
        if (shader.getShaderBinary())
            copy->setShaderBinary(new osg::ShaderBinary(*shader.getShaderBinary()));
        return copy;
    }

    bool sameShaderContent(const osg::Shader* a, const osg::Shader* b)
    {
        if (!a || !b) return a == b;
        if (a->getType() != b->getType() ||
            a->getShaderSource() != b->getShaderSource() ||
            a->getShaderDefinesMode() != b->getShaderDefinesMode() ||
            a->getShaderDefines() != b->getShaderDefines() ||
            a->getShaderRequirements() != b->getShaderRequirements() ||
            a->getCodeInjectionMap().size() != b->getCodeInjectionMap().size())
            return false;
        auto bi = b->getCodeInjectionMap().begin();
        for (const auto& ai : a->getCodeInjectionMap())
        {
            if (std::memcmp(&ai.first, &bi->first, sizeof(float)) != 0 || ai.second != bi->second)
                return false;
            ++bi;
        }
        auto ab = a->getShaderBinary(), bb = b->getShaderBinary();
        unsigned as = ab ? ab->getSize() : 0, bs = bb ? bb->getSize() : 0;
        return as == bs && (as == 0 || std::memcmp(ab->getData(), bb->getData(), as) == 0);
    }

    // Compare the exact text OSG would generate, without allocating that text.
    // The changed flag only refreshes effective state; it is not a cache token.
    bool definesMatch(osg::State& state, const osg::Program& program, const std::string& text)
    {
        auto& defines = state.getDefineMap();
        if (defines.changed)
        {
            // DefineMap::updateCurrentDefines is not exported by OSG's Windows
            // DLL. An empty query refreshes it through the supported entry point
            // without constructing a nonempty string.
            static const osg::ShaderDefines empty;
            state.getDefineString(empty);
        }
        auto sd = program.getShaderDefines().begin();
        auto cd = defines.currentDefines.begin();
        std::size_t offset = 0;
        auto match = [&](const char* data, std::size_t size) {
            if (size > text.size() - offset || std::memcmp(text.data() + offset, data, size) != 0)
                return false;
            offset += size;
            return true;
        };
        while (sd != program.getShaderDefines().end() && cd != defines.currentDefines.end())
        {
            const int order = sd->compare(cd->first);
            if (order < 0) ++sd;
            else if (order > 0) ++cd;
            else
            {
                const auto& value = cd->second.first;
                if (!match("#define ", 8) || !match(cd->first.data(), cd->first.size())) return false;
                if (!value.empty())
                {
                    if (value.front() != '(' && !match(" ", 1)) return false;
                    if (!match(value.data(), value.size())) return false;
                }
#ifdef WIN32
                if (!match("\r\n", 2)) return false;
#else
                if (!match("\n", 1)) return false;
#endif
                ++sd;
                ++cd;
            }
        }
        return offset == text.size();
    }

    std::uint64_t nextShaderKeyID()
    {
        static std::atomic<std::uint64_t> next{ 1 };
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    // Intern names only when installing, removing or querying a shader. Draw
    // traversal compares integers without treating a string hash as identity.
    VirtualProgram::ShaderID internShaderName(const std::string& name)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, VirtualProgram::ShaderID> names;
        std::lock_guard<std::mutex> lock(mutex);
        auto inserted = names.emplace(name, 0);
        if (inserted.second)
            inserted.first->second = nextShaderKeyID();
        return inserted.first->second;
    }

    // Length-prefix variable-sized fields; preserve order and multiplicity.
    void appendString(ProgramRepo::Key& key, const std::string& value)
    {
        key.push_back(value.size());
        for (std::size_t offset = 0; offset < value.size(); offset += 8)
        {
            std::uint64_t word = 0;
            for (std::size_t i = 0; i < 8 && offset + i < value.size(); ++i)
                word |= std::uint64_t(static_cast<unsigned char>(value[offset + i])) << (8 * i);
            key.push_back(word);
        }
    }

    template<typename Bindings>
    void appendBindings(ProgramRepo::Key& key, const Bindings& bindings)
    {
        key.push_back(bindings.size());
        for (const auto& binding : bindings)
        {
            appendString(key, binding.first);
            key.push_back(binding.second);
        }
    }

    void appendTemplate(ProgramRepo::Key& key, const osg::Program& program)
    {
        appendBindings(key, program.getFragDataBindingList());
        appendBindings(key, program.getUniformBlockBindingList());
        key.push_back(program.getParameter(GL_GEOMETRY_VERTICES_OUT_EXT));
        key.push_back(program.getParameter(GL_GEOMETRY_INPUT_TYPE_EXT));
        key.push_back(program.getParameter(GL_GEOMETRY_OUTPUT_TYPE_EXT));
    }

    void appendStrings(ProgramRepo::Key& key, const osg::ShaderDefines& strings)
    {
        key.push_back(strings.size());
        for (const auto& value : strings)
            appendString(key, value);
    }

    void appendShader(ProgramRepo::Key& key, const osg::Shader& shader)
    {
        key.push_back(shader.getType());
        appendString(key, shader.getShaderSource());
        key.push_back(shader.getShaderDefinesMode());
        appendStrings(key, shader.getShaderDefines());
        appendStrings(key, shader.getShaderRequirements());
        key.push_back(shader.getCodeInjectionMap().size());
        for (const auto& injection : shader.getCodeInjectionMap())
        {
            std::uint32_t position;
            static_assert(sizeof(position) == sizeof(injection.first), "Float key size");
            std::memcpy(&position, &injection.first, sizeof(position));
            key.push_back(position);
            appendString(key, injection.second);
        }
        const auto binary = shader.getShaderBinary();
        appendString(key, binary && binary->getSize() > 0 ?
            std::string(reinterpret_cast<const char*>(binary->getData()), binary->getSize()) : std::string());
    }

    void appendProgram(ProgramRepo::Key& key, const osg::Program& program)
    {
        key.push_back(program.getNumShaders());
        for (unsigned i = 0; i < program.getNumShaders(); ++i)
            appendShader(key, *program.getShader(i));
        appendBindings(key, program.getAttribBindingList());
        appendTemplate(key, program);
        appendStrings(key, program.getShaderDefines());
    }

    std::atomic_bool s_debugGroupPushed(false);

    /** Locate a function by name in the location map. */
    bool findFunction(
        const std::string& name,
        VirtualProgram::FunctionLocationMap& flm,
        VirtualProgram::Function** output)
    {
        for (auto& locations : flm)
        {
            for (auto& function : locations.second)
            {
                if (function.second._name.compare(name) == 0)
                {
                    (*output) = &function.second;
                    return true;
                }
            }
        }
        return false;
    }

    using PolyShaderCache = std::map<
        std::tuple<std::string, std::string, VirtualProgram::FunctionLocation>,
        std::pair<osg::ref_ptr<VirtualProgram::PolyShader>, std::uint64_t>>;

    std::mutex& getPolyShaderCacheMutex() {
        static std::mutex _cacheMutex;
        return _cacheMutex;
    }

    PolyShaderCache& getPolyShaderCache() {
        static PolyShaderCache _cache;
        return _cache;
    }
}

//------------------------------------------------------------------------

namespace osgEarth
{
    struct VirtualProgram::PolyShader::ShaderSnapshot
    {
        osg::ref_ptr<osg::Shader> nominal, geometry, tessellation;
        std::uint64_t keyID = 0;
        std::uint64_t mutationGeneration = 0;
    };

    struct VirtualProgramAccess
    {
        static osg::ref_ptr<osg::Shader> shader(const VirtualProgram::PolyShader& shader, unsigned stages)
        {
            shader.refreshShaderSnapshot();
            std::lock_guard<std::mutex> lock(shader._snapshotMutex);
            const auto& snapshot = shader._shaderSnapshot;
            // Share each immutable shader version between composed programs,
            // preserving OSG's reuse of compiled shader objects.
            auto selected = shader.selectShader(stages);
            if (selected == shader._geomShader.get() && selected) return snapshot->geometry;
            if (selected == shader._tessevalShader.get() && selected) return snapshot->tessellation;
            return snapshot->nominal;
        }
        static void refresh(const VirtualProgram::PolyShader& shader)
        {
            shader.refreshShaderSnapshot();
        }
    };
}

#undef  LC
#define LC "[ProgramRepo] "

ProgramRepo::ProgramRepo() :
    _releaseUnusedPrograms(true)
{
    const char* value = ::getenv("OSGEARTH_PROGRAM_BINARY_CACHE_PATH");
    if (value)
        setProgramBinaryCacheLocation(value);
}

ProgramRepo::~ProgramRepo()
{
    releaseGLObjects(NULL);
}

std::size_t
ProgramRepo::KeyHash::operator()(const Key& key) const
{
    // Mix whole words instead of bytes. A final avalanche brings high bits
    // (notably encoded float orders) into low bits used by power-of-two buckets.
    std::uint64_t hash = 14695981039346656037ULL ^ key.size();
    for (auto value : key)
        hash = (hash ^ value) * 1099511628211ULL;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return static_cast<std::size_t>(hash);
}

bool
ProgramRepo::ProgramLess::operator()(const osg::ref_ptr<osg::Program>& lhs,
    const osg::ref_ptr<osg::Program>& rhs) const
{
    if (lhs == rhs)
        return false;
    const int comparison = lhs->compare(*rhs);
    if (comparison != 0)
        return comparison < 0;
    // OSG versions omit some bindings and shader preprocessing settings from
    // compare(). Check those too before sharing an ostensibly equal program.
    Key left, right;
    appendProgram(left, *lhs);
    appendProgram(right, *rhs);
    return left < right;
}

void
ProgramRepo::touch(const Entry::Ptr& entry, unsigned frameNumber, UID user)
{
    entry->_frameLastUsed = frameNumber;
    if (entry->_users.insert(user).second)
        _entriesByUser[user].insert(entry);
}

void
ProgramRepo::setReleaseUnusedPrograms(bool value)
{
    lock();
    _releaseUnusedPrograms = value;
    unlock();
}

void
ProgramRepo::setProgramBinaryCacheLocation(const std::string& folder)
{
    lock();
    if (folder.empty())
    {
        _programBinaryCacheFolder.clear();
    }
    else if (osgDB::makeDirectory(folder) == true)
    {
        _programBinaryCacheFolder = folder;
    }
    else
    {
        OE_WARN << LC << "Failed to access/create program binary cache location " << folder << std::endl;
    }
    unlock();
}

bool
ProgramRepo::isProgramBinaryCachingActive() const
{
    return _programBinaryCacheFolder.empty() == false;
}

osg::ref_ptr<osg::Program>
ProgramRepo::use(const Key& key, unsigned frameNumber, UID user)
{
    ProgramMap::iterator i = _db.find(key);
    if (i != _db.end())
    {
        touch(i->second, frameNumber, user);
        return i->second->_program;
    }
    return 0L;
}

void
ProgramRepo::release(UID user, osg::State* state)
{
    if (user <= 0 || _releaseUnusedPrograms == false)
        return;

    _generation.fetch_add(1, std::memory_order_release);

    auto used = _entriesByUser.find(user);
    if (used == _entriesByUser.end())
        return;

    for (const auto& entry : used->second)
    {
        entry->_users.erase(user);
        if (entry->_users.empty())
        {
            if (state)
                state->setLastAppliedProgramObject(nullptr);
            entry->_program->releaseGLObjects(state);
            for (const auto& key : entry->_keys)
                _db.erase(key);
            _programs.erase(entry->_program);
        }
    }
    _entriesByUser.erase(used);
}

void
ProgramRepo::add(
    const Key& key,
    osg::ref_ptr<osg::Program>& in_out,
    unsigned frameNumber,
    UID user)
{
    // Another draw thread may have filled this key while we composed outside
    // the repository lock. Keep the existing entry and its ownership records.
    if (auto existing = use(key, frameNumber, user))
    {
        in_out = existing;
        return;
    }

    auto equivalent = _programs.find(in_out);
    Entry::Ptr entry;
    if (equivalent != _programs.end())
    {
        entry = equivalent->second;
        in_out = entry->_program;
    }
    else
    {
        entry = std::make_shared<Entry>();
        entry->_program = in_out;
        _programs.emplace(in_out, entry);
    }
    _db.emplace(key, entry);
    entry->_keys.push_back(key);
    touch(entry, frameNumber, user);
}

osg::ref_ptr<osg::Program>
ProgramRepo::editShader(osg::Program* program, unsigned shaderIndex, const std::string& source)
{
    std::lock_guard<ProgramRepo> lock(*this);
    if (!program || shaderIndex >= program->getNumShaders())
        return nullptr;
    auto current = _programs.find(program);
    if (current == _programs.end() || current->first.get() != program)
        return nullptr; // The editor's repository snapshot is out of date.
    if (program->getShader(shaderIndex)->getShaderSource() == source)
        return program;

    // Cached programs participate in a content-ordered index, and their shaders
    // can be shared with other compositions and PolyShader snapshots. Never edit
    // either object in place. Preserve shader order and all program properties.
    osg::ref_ptr<osg::Program> replacement = dynamic_cast<ComposedProgram*>(program) ?
        new ComposedProgram(*program) :
        dynamic_cast<osg::Program*>(program->clone(osg::CopyOp::SHALLOW_COPY));
    if (!replacement || typeid(*replacement) != typeid(*program))
        return nullptr;
    // OSG compares concrete program types. Preserve that type for equivalent
    // program lookup, and restore bindings omitted by its 3.6 copy constructor.
    for (const auto& binding : program->getUniformBlockBindingList())
        replacement->addBindUniformBlock(binding.first, binding.second);
    osg::ref_ptr<osg::Shader> edited = copyShaderContent(*program->getShader(shaderIndex));
    edited->setShaderSource(source);
    for (unsigned i = 0; i < program->getNumShaders(); ++i)
        replacement->removeShader(program->getShader(i));
    for (unsigned i = 0; i < program->getNumShaders(); ++i)
        replacement->addShader(i == shaderIndex ? edited.get() : program->getShader(i));

    auto previous = current->second;
    auto equivalent = _programs.find(replacement);
    if (equivalent == _programs.end())
    {
        _programs.erase(current);
        previous->_program = replacement;
        _programs.emplace(replacement, previous);
    }
    else
    {
        // Editing can produce an already-cached program. Merge aliases and
        // reverse ownership records so final-owner release still removes all.
        auto destination = equivalent->second;
        replacement = destination->_program;
        for (const auto& key : previous->_keys)
        {
            _db[key] = destination;
            destination->_keys.push_back(key);
        }
        const auto frame = std::max(previous->_frameLastUsed, destination->_frameLastUsed);
        for (auto user : previous->_users)
        {
            touch(destination, frame, user);
            _entriesByUser[user].erase(previous);
        }
        previous->_users.clear();
        previous->_keys.clear();
        _programs.erase(current);
    }
    // Every State must pick up the replacement on its next apply(). Existing
    // users of the old program can finish with its unchanged GL objects.
    _generation.fetch_add(1, std::memory_order_release);
    return replacement;
}

void
ProgramRepo::prune(unsigned frameNumber, osg::State* state)
{
    //todo
}

void
ProgramRepo::resizeGLObjectBuffers(unsigned maxSize)
{
    _generation.fetch_add(1, std::memory_order_release);
    for (auto i = _programs.begin(); i != _programs.end(); ++i)
    {
        i->second->_program->resizeGLObjectBuffers(maxSize);
    }
}

void
ProgramRepo::releaseGLObjects(osg::State* state) const
{
    _generation.fetch_add(1, std::memory_order_release);
    if (state)
        state->setLastAppliedProgramObject(nullptr);
    OE_TEST << LC << "Main release, size=" << _db.size() << std::endl;
    // First try to find an entry with an equivalent program:
    for (auto& i : _programs)
    {
        auto& e = i.second;
        e->_program->releaseGLObjects(state);
        OE_TEST << LC << "...released program " << e->_program->getName() << std::endl;
    }
    _db.clear();
    _entriesByUser.clear();
    _programs.clear();
}

void
ProgramRepo::linkProgram(
    const Key&,
    osg::Program* program, 
    osg::Program::PerContextProgram* pcp, 
    osg::State& state)
{
    OE_PROFILING_ZONE_NAMED("link");

    // The Program is shared by define variants and graphics contexts. Serialize
    // linking so another context cannot replace its binary while it is in use.
    std::lock_guard<ProgramRepo> lock(*this);

    // A binary belongs only to this link attempt. Start clean for cache misses
    // (including failed file opens or disabled caching), and detach it on exit
    // so another define variant cannot reuse it.
    struct ClearProgramBinary
    {
        osg::Program* program;
        ~ClearProgramBinary() { program->setProgramBinary(nullptr); }
    } clearProgramBinary{ program };

    program->setProgramBinary(nullptr);

    if (isProgramBinaryCachingActive())
    {
        bool readFromCache = false;
        std::fstream fStream;
        std::string programCacheName;

#if OSG_VERSION_LESS_THAN(3,7,0)
        const std::string& defineStr = state.getDefineString(program->getShaderDefines());
#else
        const std::string& defineStr = pcp->getDefineString();
#endif

        // Runtime identities are deliberately process-local. Disk binaries use
        // actual linked inputs, defines and driver identity, with an exact
        // descriptor in the file to reject even a filename-hash collision.
        Key descriptor;
        descriptor.push_back(2); // file format version
        appendProgram(descriptor, *program);
        appendString(descriptor, defineStr);
        descriptor.push_back(state.getUseVertexAttributeAliasing());
        descriptor.push_back(state.getUseModelViewAndProjectionUniforms());
        appendBindings(descriptor, state.getAttributeBindingList());
        auto appendAlias = [&](const osg::VertexAttribAlias& alias)
        {
            descriptor.push_back(alias._location);
            appendString(descriptor, alias._glName);
            appendString(descriptor, alias._osgName);
            appendString(descriptor, alias._declaration);
        };
        appendAlias(state.getVertexAlias());
        appendAlias(state.getNormalAlias());
        appendAlias(state.getColorAlias());
        appendAlias(state.getSecondaryColorAlias());
        appendAlias(state.getFogCoordAlias());
        descriptor.push_back(state.getTexCoordAliasList().size());
        for (const auto& alias : state.getTexCoordAliasList())
            appendAlias(alias);
        for (GLenum property : { GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION })
        {
            const GLubyte* value = glGetString(property);
            appendString(descriptor, value ? reinterpret_cast<const char*>(value) : "");
        }
        const std::uint64_t descriptorSize = descriptor.size();
        const std::streamoff headerSize = sizeof(descriptorSize) +
            descriptorSize * sizeof(std::uint64_t) + sizeof(GLenum);
        std::stringstream programCacheNameStream;
        programCacheNameStream << "vp2_" << std::hex << KeyHash{}(descriptor) << ".bin";

        programCacheName = osgDB::concatPaths(
            _programBinaryCacheFolder, 
            osgEarth::toLegalFileName(programCacheNameStream.str(), false, "-"));

        //currently set to be able to read and write to the binary file so only need to open file once.
        fStream.open(programCacheName.c_str(), std::fstream::in | std::fstream::out | std::fstream::app | std::fstream::binary);
        if (fStream.is_open())
        {
            // get length of file:
            fStream.seekg(0, fStream.end);
            std::streamoff length = fStream.tellg();
            fStream.seekg(0, fStream.beg);

            if (length > headerSize && length - headerSize <= (std::numeric_limits<unsigned>::max)())
            {
                OE_PROFILING_ZONE_NAMED("LoadShaderProgramBinary");
                OE_PROFILING_ZONE_TEXT(programCacheName);
                std::uint64_t storedSize = 0;
                fStream.read(reinterpret_cast<char*>(&storedSize), sizeof(storedSize));
                if (storedSize == descriptorSize)
                {
                    Key stored(descriptor.size());
                    fStream.read(reinterpret_cast<char*>(stored.data()), stored.size() * sizeof(std::uint64_t));
                    if (fStream && stored == descriptor)
                    {
                        GLenum format = 0;
                        std::vector<unsigned char> buffer(static_cast<std::size_t>(length - headerSize));
                        fStream.read(reinterpret_cast<char*>(&format), sizeof(format));
                        fStream.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
                        if (fStream)
                        {
                            osg::ref_ptr<osg::Program::ProgramBinary> binary = new osg::Program::ProgramBinary();
                            binary->setFormat(format);
                            binary->assign(static_cast<unsigned>(buffer.size()), buffer.data());
                            program->setProgramBinary(binary);
                            readFromCache = true;
                        }
                    }
                }
            }
            else
            {
                OE_PROFILING_ZONE_NAMED("LoadShaderNotFound");
                OE_PROFILING_ZONE_TEXT(programCacheName);
            }

            //If there is not a programBinary then we need to add one
            // This sets an openGL hint so we can grab it later.
            if (program->getProgramBinary() == NULL)
            {
                program->setProgramBinary(new osg::Program::ProgramBinary());
            }

            if (!readFromCache)
            {
                fStream.close();
                fStream.clear();
                fStream.open(programCacheName.c_str(), std::fstream::out | std::fstream::trunc | std::fstream::binary);
            }
        }

        program->compileGLObjects(state);

        if (readFromCache && !pcp->loadedBinary())
        {
            // The driver rejected the payload. Some OSG versions fall back to
            // source themselves; otherwise request that fallback explicitly.
            program->setProgramBinary(new osg::Program::ProgramBinary());
            if (!pcp->isLinked())
            {
                pcp->requestLink();
                program->compileGLObjects(state);
            }
            readFromCache = false;
            fStream.close();
            fStream.clear();
            fStream.open(programCacheName.c_str(), std::fstream::out | std::fstream::trunc | std::fstream::binary);
        }

        if (fStream.is_open())
        {
            if (pcp->isLinked())
            {
                if (!readFromCache)
                {
                    osg::ref_ptr<osg::Program::ProgramBinary> binary = pcp->compileProgramBinary(state);
                    if (binary && binary->getSize() > 0)
                    {
                        OE_PROFILING_ZONE_NAMED("SaveShaderProgramBinary");
                        GLenum format = binary->getFormat();
                        fStream.write(reinterpret_cast<const char*>(&descriptorSize), sizeof(descriptorSize));
                        fStream.write(reinterpret_cast<const char*>(descriptor.data()), descriptor.size() * sizeof(std::uint64_t));
                        fStream.write((char*)&format, sizeof(GLenum));
                        fStream.write((char*)binary->getData(), binary->getSize());
                        fStream.close();
                        OE_DEBUG << LC << "Wrote a shader binary from the cache (" << programCacheName << ")" << std::endl;
                    }
                    else
                    {
                        //Should we save off something so we know it is a bad shader instead of just deleting the empty cache file?
                        fStream.close();
                        OE_WARN << LC << "Failed to compile program binary (" << programCacheName << ")" << std::endl;
                        remove(programCacheName.c_str());
                    }
                }
            }
            else
            {
                OE_WARN << LC << "Failed to link program binary (" << programCacheName << ")" << std::endl;
                fStream.close();
                remove(programCacheName.c_str());
            }
        }
    }
    else
    {
        program->compileGLObjects(state);
    }
}

osg::ref_ptr<VirtualProgram>
ProgramRepo::getOrCreateVirtualProgram(const std::string& name, std::function<VirtualProgram*()> create)
{
    lock();

    auto& vp = _virtualProgramLUT[name];
    if (!vp.valid())
    {
        vp = create();
    }

    unlock();

    return vp;
}

//------------------------------------------------------------------------

#undef  LC
#define LC "[VirtualProgram] "

// environment variable control
#define OSGEARTH_DUMP_SHADERS  "OSGEARTH_DUMP_SHADERS"
#define OSGEARTH_MERGE_SHADERS "OSGEARTH_MERGE_SHADERS"

#define OSGEARTH_DISABLE_GLRELEASE "OSGEARTH_VP_DISABLE_GL_RELEASE"
static bool s_disableVPRelease = false;

namespace
{
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE)
    // GLES requires all shader code be merged into a since source
    bool s_mergeShaders = true;
#else
    bool s_mergeShaders = false;
#endif

    bool s_dumpShaders = false;        // debugging

    /** A device that lets us do a const search on the State's attribute map. OSG does not yet
    have a const way to do this. It has getAttributeVec() but that is non-const (it creates
    the vector if it doesn't exist); Newer versions have getAttributeMap(), but that does not
    go back to OSG 3.0. */
    struct StateEx : public osg::State
    {
        static const VirtualProgram::AttrStack* getProgramStack(const osg::State& state)
        {
            static osg::StateAttribute::TypeMemberPair pair(VirtualProgram::SA_TYPE, 0);
            const StateEx* sh = reinterpret_cast<const StateEx*>(&state);
            AttributeMap::const_iterator i = sh->_attributeMap.find(pair);
            return i != sh->_attributeMap.end() ? &(i->second.attributeVec) : 0L;
        }
    };

    // removes leading and trailing whitespace, and replaces all other
    // whitespace with single spaces
    std::string trimAndCompress(const std::string& in)
    {
        bool inwhite = true;
        std::stringstream buf;
        for (unsigned i = 0; i < in.length(); ++i)
        {
            char c = in[i];
            if (::isspace(c))
            {
                if (!inwhite)
                {
                    buf << ' ';
                    inwhite = true;
                }
            }
            else
            {
                inwhite = false;
                buf << c;
            }
        }
        std::string r;
        r = buf.str();
        trim2(r);
        return r;
    }

    bool s_attribAliasSortFunc(const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
        return a.first.size() > b.first.size();
    }


    /**
    * Replaces a shader's attribute values with their aliases
    */
    void applyAttributeAliases(
        osg::Shader*                             shader,
        const VirtualProgram::AttribAliasVector& sortedAliases)
    {
        std::string src = shader->getShaderSource();
        for (VirtualProgram::AttribAliasVector::const_iterator i = sortedAliases.begin(); i != sortedAliases.end(); ++i)
        {
            //OE_DEBUG << LC << "Replacing " << i->first << " with " << i->second << std::endl;
            osgEarth::replaceIn(src, i->first, i->second);
        }
        shader->setShaderSource(src);
    }


    /**
    * Adds a new shader entry to the accumulated shader map, respecting the
    * override policy of both the existing entry (if there is one) and the
    * new entry.
    */
    void addToAccumulatedMap(VirtualProgram::ShaderMap&         accumShaderMap,
        const VirtualProgram::ShaderID&    shaderID,
        const VirtualProgram::ShaderEntry& newEntry)
    {

        // see if we're trying to disable a previous entry:
        if ((newEntry._overrideValue & osg::StateAttribute::ON) == 0) //TODO: check for higher override
        {
            // yes? remove it!
            accumShaderMap.erase(shaderID);
        }

        else
        {
            // see if there's a higher-up entry with the same ID:
            VirtualProgram::ShaderEntry& accumEntry = accumShaderMap[shaderID];

            // make sure we can add the new one:
            if ((accumEntry._shader.get() == 0L) ||                                   // empty slot, fill it
                ((accumEntry._overrideValue & osg::StateAttribute::PROTECTED) != 0) || // new entry is protected
                ((accumEntry._overrideValue & osg::StateAttribute::OVERRIDE) == 0))   // old entry does NOT override
            {
                accumEntry = newEntry;
            }
        }
    }

    /**
    * Apply the data binding information from a template program to the
    * target program.
    */
    void addTemplateDataToProgram(const osg::Program* templateProgram, osg::Program* program)
    {
        const osg::Program::FragDataBindingList& fbl = templateProgram->getFragDataBindingList();
        for (osg::Program::FragDataBindingList::const_iterator i = fbl.begin(); i != fbl.end(); ++i)
            program->addBindFragDataLocation(i->first, i->second);

        const osg::Program::UniformBlockBindingList& ubl = templateProgram->getUniformBlockBindingList();
        for (osg::Program::UniformBlockBindingList::const_iterator i = ubl.begin(); i != ubl.end(); ++i)
            program->addBindUniformBlock(i->first, i->second);

        // dont' need unless we're using shader4 ext??
        program->setParameter(GL_GEOMETRY_VERTICES_OUT_EXT, templateProgram->getParameter(GL_GEOMETRY_VERTICES_OUT_EXT));
        program->setParameter(GL_GEOMETRY_INPUT_TYPE_EXT, templateProgram->getParameter(GL_GEOMETRY_INPUT_TYPE_EXT));
        program->setParameter(GL_GEOMETRY_OUTPUT_TYPE_EXT, templateProgram->getParameter(GL_GEOMETRY_OUTPUT_TYPE_EXT));
    }

    struct SortByType {
        bool operator()(const osg::ref_ptr<osg::Shader>& lhs, const osg::ref_ptr<osg::Shader>& rhs) {
            return (int)lhs->getType() < (int)rhs->getType();
        }
    };

    bool shaderInStageMask(osg::Shader* shader, const VirtualProgram::StageMask& mask)
    {
        if (shader->getType() == shader->VERTEX && (mask & VirtualProgram::STAGE_VERTEX)) return true;
        if (shader->getType() == shader->GEOMETRY && (mask & VirtualProgram::STAGE_GEOMETRY)) return true;
        if (shader->getType() == shader->TESSCONTROL && (mask & VirtualProgram::STAGE_TESSCONTROL)) return true;
        if (shader->getType() == shader->TESSEVALUATION && (mask & VirtualProgram::STAGE_TESSEVALUATION)) return true;
        if (shader->getType() == shader->FRAGMENT && (mask & VirtualProgram::STAGE_FRAGMENT)) return true;
        if (shader->getType() == shader->COMPUTE && (mask & VirtualProgram::STAGE_COMPUTE)) return true;
        return false;
    }

    std::string getNameForType(osg::Shader::Type type)
    {
        return
            type == osg::Shader::VERTEX ? "VERTEX" :
            type == osg::Shader::FRAGMENT ? "FRAGMENT" :
            type == osg::Shader::GEOMETRY ? "GEOMETRY" :
            type == osg::Shader::TESSCONTROL ? "TESSCONTROL" :
            "TESSEVAL";
    }

    /**
    * Populates the specified Program with passed-in shaders.
    */
    void addShadersToProgram(
        const VirtualProgram::ShaderVector&      shaders,
        const VirtualProgram::AttribBindingList& attribBindings,
        const VirtualProgram::AttribAliasMap&    attribAliases,
        osg::Program*                            program,
        VirtualProgram::StageMask                stages)
    {
        if (s_mergeShaders)
        {
            ShaderMerger merger;

            for (auto& shader : shaders)
            {
                if (shaderInStageMask(shader.get(), stages))
                {
                    merger.add(shader.get());
                }
            }

            merger.merge(program);

            if (s_dumpShaders)
            {
                for (unsigned i = 0; i < program->getNumShaders(); ++i)
                {
                    osg::Shader* shader = program->getShader(i);
                    OE_NOTICE << "\n---------MERGED "
                        << getNameForType(shader->getType())
                        << " SHADER------------\n"
                        << shader->getShaderSource()
                        << std::endl;
                }
            }
        }
        else
        {
            if (!s_dumpShaders)
            {
                for (auto& shader : shaders)
                {
                    if (shaderInStageMask(shader.get(), stages))
                    {
                        program->addShader(shader.get());
                    }
                }
            }

            else
            {
                VirtualProgram::ShaderVector copy(shaders);
                std::sort(copy.begin(), copy.end(), SortByType());

                int c = 1;

                for (auto& shader : copy)
                {
                    if (shaderInStageMask(shader.get(), stages))
                    {
                        program->addShader(shader.get());

                        OE_NOTICE
                            << "--- [ " << (c++) << "/" << shaders.size() << " " 
                            << shader.get()->getTypename() << " ] ------------------\n\n"
                            << shader.get()->getShaderSource() << std::endl;
                    }
                }
            }
        }

        // add the attribute bindings
        for (auto& binding : attribBindings)
        {
            program->addBindAttribLocation(binding.first, binding.second);
        }
    }


    /**
    * Assemble a new OSG shader Program from the provided components.
    * Outputs the uniquely-identifying "key vector" and returns the new program.
    */
    osg::Program* buildProgram(
        const std::string&                  programName,
        osg::State&                         state,
        VirtualProgram::FunctionLocationMap& accumFunctions,
        VirtualProgram::ShaderMap&          accumShaderMap,
        const VirtualProgram::ExtensionsSet& extensionsSet,
        VirtualProgram::AttribBindingList&  accumAttribBindings,
        VirtualProgram::AttribAliasMap&     accumAttribAliases,
        osg::Program*                       templateProgram)
    {
        // create new MAINs for this function stack.
        VirtualProgram::ShaderVector mains;

        VirtualProgram::StageMask stages = Registry::shaderFactory()->createMains(
            state,
            accumFunctions,
            accumShaderMap,
            extensionsSet,
            mains);

        VirtualProgram::ShaderVector buildVector;
        buildVector.reserve(accumShaderMap.size() + mains.size());

        for (auto& iter : accumShaderMap)
        {
            // Repository programs remain immutable if a caller later edits a
            // supplied shader or one obtained from getNominalShader/getShader.
            auto shader = VirtualProgramAccess::shader(*iter.second._shader, stages);
            buildVector.push_back(shader);
        }

        buildVector.insert(buildVector.end(), mains.begin(), mains.end());

        if (s_dumpShaders)
        {
            if (!programName.empty())
            {
                OE_NOTICE << LC << "\n\n=== [ Program \"" << programName << "\" ] =============================\n\n" << std::endl;
            }
            else
            {
                OE_NOTICE << LC << "\n\n=== [ Program (unnamed) ] =============================\n\n" << std::endl;
            }
        }

        // Create the new program.
        osg::Program* program = new ComposedProgram();
        program->setName(programName);
        addShadersToProgram(buildVector, accumAttribBindings, accumAttribAliases, program, stages);
        addTemplateDataToProgram(templateProgram, program);

        return program;
    }
}

//------------------------------------------------------------------------

bool VirtualProgram::_gldebug = false;

struct VirtualProgram::ApplyVars
{
    ShaderMap accumShaderMap;
    ProgramRepo::Key programKey;
    AttribBindingList accumAttribBindings;
    AttribAliasMap accumAttribAliases;
    ExtensionsSet accumExtensions;
    osg::observer_ptr<osg::State> state;
    AttrStack stack;
    std::uint64_t compositionGeneration = 0;
    std::uint64_t repoGeneration = 0;
    bool reusable = false;
    unsigned frameLastUsed = 0;
    osg::ref_ptr<osg::Program> program;
    // Observing the PCP must not keep released GL handles alive.
    osg::observer_ptr<osg::Program::PerContextProgram> pcp;
    std::uint64_t pcpGeneration = 0;
    osg::Program::FragDataBindingList templateFragBindings;
    osg::Program::UniformBlockBindingList templateUniformBindings;
    GLint templateGeometry[3] = {};
};

void VirtualProgram::enableGLDebugging()
{
    _gldebug = true;
}

//------------------------------------------------------------------------

VirtualProgram::ShaderEntry::ShaderEntry() :
    _overrideValue(0)
{
    //nop
}

bool
VirtualProgram::ShaderEntry::accept(const osg::State& state) const
{
    return (!_accept.valid()) || (_accept->operator()(state) == true);
}

bool
VirtualProgram::ShaderEntry::operator < (const VirtualProgram::ShaderEntry& rhs) const
{
    int c = _shader->getShaderSource().compare(rhs._shader->getShaderSource());
    if (c < 0) return true;
    if (c > 0) return false;

    if (_shader->getShaderSource().compare(rhs._shader->getShaderSource()) < 0) return true;

    if (_overrideValue < rhs._overrideValue) return true;
    if (_overrideValue > rhs._overrideValue) return false;

    if (_accept.valid() && !rhs._accept.valid()) return true;
    return false;
}

//------------------------------------------------------------------------

// same type as PROGRAM (for proper state sorting)
const osg::StateAttribute::Type VirtualProgram::SA_TYPE = osg::StateAttribute::PROGRAM;

VirtualProgram*
VirtualProgram::getOrCreate(osg::StateSet* stateset)
{
    if (!stateset)
        return 0L;

    VirtualProgram* vp = dynamic_cast<VirtualProgram*>(stateset->getAttribute(SA_TYPE));
    if (!vp)
    {
        vp = new VirtualProgram();
        vp->_inherit = true;
        vp->_inheritSet = true;
        vp->setName(stateset->getName());
        stateset->setAttributeAndModes(vp, osg::StateAttribute::ON);
    }
    return vp;
}

VirtualProgram*
VirtualProgram::get(osg::StateSet* stateset)
{
    if (!stateset)
        return 0L;

    return dynamic_cast<VirtualProgram*>(stateset->getAttribute(SA_TYPE));
}

const VirtualProgram*
VirtualProgram::get(const osg::StateSet* stateset)
{
    if (!stateset)
        return 0L;

    return dynamic_cast<const VirtualProgram*>(stateset->getAttribute(SA_TYPE));
}

VirtualProgram*
VirtualProgram::cloneOrCreate(const osg::StateSet* src, osg::StateSet* dest)
{
    if (!dest)
        return 0L;

    const VirtualProgram* vp = 0L;

    if (src)
    {
        vp = get(src);
    }

    if (!vp)
    {
        return getOrCreate(dest);
    }

    else
    {
        VirtualProgram* cloneVP = osg::clone(vp, osg::CopyOp::DEEP_COPY_ALL);
        cloneVP->setInheritShaders(true);
        dest->setAttributeAndModes(cloneVP, osg::StateAttribute::ON);
        return cloneVP;
    }
}

VirtualProgram*
VirtualProgram::cloneOrCreate(osg::StateSet* stateset)
{
    return cloneOrCreate(stateset, stateset);
}

void
VirtualProgram::setReleaseUnusedPrograms(bool value)
{
    Registry::programRepo().setReleaseUnusedPrograms(value);
}

void
VirtualProgram::setProgramBinaryCacheLocation(const std::string& folder)
{
    Registry::programRepo().setProgramBinaryCacheLocation(folder);
}

//------------------------------------------------------------------------

VirtualProgram::VirtualProgram(unsigned mask) :
    _mask(mask),
    _active(true),
    _inherit(true),
    _inheritSet(false),
    _logShaders(false),
    _logPath(""),
    _acceptCallbacksVaryPerFrame(false),
    _isAbstract(false)
{
    // Note: we cannot set _active here. Wait until apply().
    // It will cause a conflict in the Registry.

    _id = osgEarth::createUID();
    compositionChanged();

    // check the the dump env var
    if (::getenv(OSGEARTH_DUMP_SHADERS) != 0L)
    {
        s_dumpShaders = true;
    }

    // check the merge env var
    if (::getenv(OSGEARTH_MERGE_SHADERS) != 0L)
    {
        s_mergeShaders = true;
    }

    if (::getenv(OSGEARTH_DISABLE_GLRELEASE) != 0L)
    {
        s_disableVPRelease = true;
    }

    // a template object to hold program data (so we don't have to dupliate all the 
    // osg::Program methods..)
    _template = new osg::Program();


#ifdef USE_LAST_USED_PROGRAM
    _lastUsedProgram.resize(MAX_CONTEXTS);
#endif


#if 0 // Good, but needs more testing
#ifdef OSGEARTH_SINGLE_THREADED_OSG
    _useDataModelMutex = false;
#endif
#endif
}


VirtualProgram::VirtualProgram(const VirtualProgram& rhs, const osg::CopyOp& copyop) :
    osg::StateAttribute(rhs, copyop),
    _shaderMap(rhs._shaderMap),
    _mask(rhs._mask),
    _functions(rhs._functions),
    _functionKey(rhs._functionKey),
    _globalExtensions(rhs._globalExtensions),
    _inherit(rhs._inherit),
    _inheritSet(rhs._inheritSet),
    _logShaders(rhs._logShaders),
    _logPath(rhs._logPath),
    _template(osg::clone(rhs._template.get())),
    _acceptCallbacksVaryPerFrame(rhs._acceptCallbacksVaryPerFrame),
    _isAbstract(rhs._isAbstract)

{
    _id = osgEarth::createUID();
    compositionChanged();

    // Attribute bindings.
    for(auto& binding : rhs.getAttribBindingList())
    {
        addBindAttribLocation(binding.first, binding.second);
    }


#ifdef USE_LAST_USED_PROGRAM
    _lastUsedProgram.resize(MAX_CONTEXTS);
#endif

}

VirtualProgram::~VirtualProgram()
{
    // Invalidates non-owning stack identities before an address can be reused.
    compositionChanged();

#ifdef USE_PROGRAM_REPO
    if (Registry::instance())
    {
        Registry::programRepo().lock();
        Registry::programRepo().release(_id, 0L);
        Registry::programRepo().unlock();
    }
#endif

    OE_TEST << LC << "~VP (" << _id << ") " << getName() << std::endl;
}

int
VirtualProgram::compare(const osg::StateAttribute& sa) const
{
    // check the types are equal and then create the rhs variable
    // used by the COMPARE_StateAttribute_Parameter macros below.
    COMPARE_StateAttribute_Types(VirtualProgram, sa);

    // Safely compare the objects. Note, this function 
    // treats the argument as the RHS, so we need to invert
    // the result before returning it since the argument is
    // actually our LHS.
    return -rhs.compare_safe(*this);
}

void
VirtualProgram::addBindAttribLocation(const std::string& name, GLuint index)
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    _attribBindingList[name] = index;
    compositionChanged();
}

void
VirtualProgram::removeBindAttribLocation(const std::string& name)
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    _attribBindingList.erase(name);
    compositionChanged();
}

void
VirtualProgram::compileGLObjects(osg::State& state) const
{
    // Don't do this here. compileGLObjects() runs from a pre-compilation visitor,
    // and the state is not complete enough to create fully formed programs; so
    // this is not only pointless but can result in shader linkage errors 
    // (albeit harmless)
    //this->apply(state);
}

void
VirtualProgram::resizeGLObjectBuffers(unsigned maxSize)
{
#ifdef USE_PROGRAM_REPO
    Registry::programRepo().lock();
    Registry::programRepo().resizeGLObjectBuffers(maxSize);
    Registry::programRepo().unlock();
#endif

    // Resize shaders in the PolyShader
    for (ShaderMap::iterator i = _shaderMap.begin(); i != _shaderMap.end(); ++i)
    {
        if (i->second._shader.valid())
        {
            i->second._shader->resizeGLObjectBuffers(maxSize);
        }
    }
}

void
VirtualProgram::releaseGLObjects(osg::State* state) const
{
    if (s_disableVPRelease)
        return;

    OE_TEST << LC << "VP::RGLO (" << _id << ") " << getName() << " (" << (_lastUsedProgram[0].get()) << ") state=" << (uintptr_t)state << std::endl;

#ifdef USE_PROGRAM_REPO
    Registry::programRepo().lock();
    Registry::programRepo().release(_id, state);
    Registry::programRepo().unlock();
#endif

#ifdef USE_LAST_USED_PROGRAM
    if (state)
    {
        auto cid = GLUtils::getSharedContextID(*state);
        const osg::Program* p = _lastUsedProgram[cid].get();
        if (p)
            p->releaseGLObjects(state);
    }
    else
    {
        for (unsigned i = 0; i < _lastUsedProgram.size(); ++i)
        {
            const osg::Program* p = _lastUsedProgram[i].get();
            if (p)
                p->releaseGLObjects(state);
        }
    }
    _lastUsedProgram.setAllElementsTo(NULL);
#endif
}

VirtualProgram::PolyShader*
VirtualProgram::getPolyShader(const std::string& shaderID) const
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    ShaderMap::const_iterator i = _shaderMap.find(MAKE_SHADER_ID(shaderID));
    const ShaderEntry* entry = i != _shaderMap.end() ? &i->second : NULL;
    return entry ? entry->_shader.get() : 0L;
}


osg::Shader*
VirtualProgram::setShader(
    const std::string& shaderID,
    osg::Shader* shader,
    osg::StateAttribute::OverrideValue ov)
{
    if (!shader || shader->getType() == osg::Shader::UNDEFINED)
        return NULL;

    // set the inherit flag if it's not initialized
    if (!_inheritSet)
    {
        setInheritShaders(true);
    }

    checkSharing();

    // set the name to the ID:
    shader->setName(shaderID);

    PolyShader* pshader = new PolyShader(shader);
    pshader->prepare();

    // lock the data model and insert the new shader.
    {
        scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
        ShaderEntry& entry = _shaderMap[MAKE_SHADER_ID(shaderID)];
        entry._shader = pshader;
        entry._overrideValue = ov;
        entry._accept = nullptr;
        compositionChanged();
    }

    return shader;
}


osg::Shader*
VirtualProgram::setShader(
    osg::Shader* shader,
    osg::StateAttribute::OverrideValue ov)
{
    if (!shader || shader->getType() == osg::Shader::UNDEFINED)
        return NULL;

    if (shader->getName().empty())
    {
        OE_WARN << LC << "setShader called but the shader name is not set" << std::endl;
        return 0L;
    }

    // set the inherit flag if it's not initialized
    if (!_inheritSet)
    {
        setInheritShaders(true);
    }

    PolyShader* pshader = new PolyShader(shader);
    pshader->prepare();

    // lock the data model while changing it.
    {
        scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);

        checkSharing();

        ShaderEntry& entry = _shaderMap[MAKE_SHADER_ID(shader->getName())];
        entry._shader = pshader;
        entry._overrideValue = ov;
        entry._accept = nullptr;
        compositionChanged();
    }

    return shader;
}


void
VirtualProgram::dirty()
{
    s_shaderMutationGeneration.fetch_add(1, std::memory_order_release);
    compositionChanged();
}

void
VirtualProgram::setFunction(
    const std::string& functionName,
    const std::string& shaderSource,
    FunctionLocation location,
    float ordering)
{
    setFunction(functionName, shaderSource, location, nullptr, ordering);
}

void
VirtualProgram::setFunction(
    const std::string& functionName,
    const std::string& shaderSource,
    FunctionLocation location,
    AcceptCallback*  accept,
    float ordering)
{
    // set the inherit flag if it's not initialized
    if (!_inheritSet)
    {
        setInheritShaders(true);
    }

    // lock the functions map while iterating and then modifying it:
    {
        scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);

        checkSharing();

        // A function can move to a different injection location. Remove its
        // previous schedule entry as well as entries at the new location.
        for (auto& functions : _functions)
        {
            auto& ordered = functions.second;
            for (auto i = ordered.begin(); i != ordered.end(); )
                i = i->second._name == functionName ? ordered.erase(i) : std::next(i);
        }
        OrderedFunctionMap& ofm = _functions[location];

        Function function;
        function._name = functionName;
        function._accept = accept;
        ofm.insert(OrderedFunction(ordering, function));
        updateFunctionKey();

        // final cleanup on the shader source before applying it
        std::string finalized_source(shaderSource);
        ShaderLoader::finalize(finalized_source);

        // assemble the poly shader. but check a map first for existing shaders.
        PolyShader* shader = PolyShader::lookUpShader(functionName, finalized_source, location);

        ShaderEntry& entry = _shaderMap[MAKE_SHADER_ID(functionName)];
        entry._shader = shader;
        entry._overrideValue = osg::StateAttribute::ON;
        entry._accept = accept;
        compositionChanged();

    } // release lock
}

bool
VirtualProgram::addGLSLExtension(const std::string& extension)
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    std::pair<ExtensionsSet::const_iterator, bool> insertPair = _globalExtensions.insert(extension);
    if (insertPair.second) compositionChanged();
    return insertPair.second;
}

bool
VirtualProgram::hasGLSLExtension(const std::string& extension) const
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    bool doesHave = _globalExtensions.find(extension) != _globalExtensions.end();
    return doesHave;
}

bool
VirtualProgram::removeGLSLExtension(const std::string& extension)
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    ExtensionsSet::size_type erased = _globalExtensions.erase(extension);
    if (erased) compositionChanged();
    return erased > 0;
}

void
VirtualProgram::removeShader(const std::string& shaderID)
{
    // lock te functions map while making changes:
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);

    _shaderMap.erase(MAKE_SHADER_ID(shaderID));
    compositionChanged();

    for (FunctionLocationMap::iterator i = _functions.begin(); i != _functions.end(); ++i)
    {
        OrderedFunctionMap& ofm = i->second;
        for (OrderedFunctionMap::iterator j = ofm.begin(); j != ofm.end(); ++j)
        {
            if (j->second._name.compare(shaderID) == 0)
            {
                ofm.erase(j);

                // if the function map for this location is now empty,
                // remove the location map altogether.
                if (ofm.size() == 0)
                {
                    _functions.erase(i);
                }
                updateFunctionKey();
                return;
            }
        }
    }
}


void
VirtualProgram::setInheritShaders(bool value)
{
    if (_inherit != value || !_inheritSet)
    {
        _inherit = value;

#ifdef USE_PROGRAM_REPO
        // clear the program cache please
        {
            Registry::programRepo().lock();
            Registry::programRepo().release(_id, 0L);
            Registry::programRepo().unlock();
        }
#endif


        _inheritSet = true;
        compositionChanged();
    }
}

void VirtualProgram::setMask(unsigned value)
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    if (_mask != value) { _mask = value; compositionChanged(); }
}

void VirtualProgram::setIsAbstract(bool value)
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    if (_isAbstract != value) { _isAbstract = value; compositionChanged(); }
}


void
VirtualProgram::apply(osg::State& state) const
{
    OE_TEST << LC << "Applying (" << this << ") " << getName() << std::endl;

    if (_active.isSetTo(false))
    {
        return;
    }
    else if (!_active.isSet())
    {
        // cannot use capabilities here; it breaks serialization.
        _active = true; //Registry::capabilities().supportsGLSL();
    }

    // An abstract (pure virtual) program cannot be applied.
    if (_isAbstract)
    {
        return;
    }

    const unsigned contextID = GLUtils::getSharedContextID(state);

    if (_shaderMap.empty() && !_inheritSet)
    {
        // If there's no data in the VP, and never has been, unload any existing program.
        // NOTE: OSG's State processor creates a "global default attribute" for each type.
        // Sine we have no way of knowing whether the user created the VP or OSG created it
        // as the default fallback, we use the "_inheritSet" flag to differeniate. This
        // prevents any shader leakage from a VP-enabled node.

        // The following "if" helps performance a bit (based on profiler results) but a user
        // reported state corruption in the OSG stats display. The underlying cause is likely
        // in external code, but leave it commented out for now -gw 20150721

        //if ( state.getLastAppliedProgramObject() != 0L )
        {
            const osg::GL2Extensions* extensions = osg::GL2Extensions::Get(contextID, false);
            if (extensions)
            {
                extensions->glUseProgram(0);
            }
            state.setLastAppliedProgramObject(0);
        }
        return;
    }

    osg::ref_ptr<osg::Program> program;

    OE_PROFILING_ZONE_NAMED("vp:apply");
    OE_PROFILING_ZONE_TEXT(getName());

    // OSG's last-attribute tracking cannot detect a VP reached through a
    // different parent stack. Keep OSG's bookkeeping on our cache hits too.
    state.haveAppliedAttribute(this->SA_TYPE);

    // Acceptance can change on any draw, including within the same frame.
    bool acceptCallbacksVary = false;

#ifdef PREALLOCATE_APPLY_VARS
    ApplyVars* vars;
    {
        std::lock_guard<std::mutex> lock(_applyMutex);
        auto found = _apply.find(&state);
        if (found == _apply.end() || found->second->state.get() != &state)
        {
            // Reclaim caches for expired States, including address reuse. A
            // live draw owns its State, so its heap-stable ApplyVars survive.
            for (auto i = _apply.begin(); i != _apply.end(); )
                i = !i->second->state.valid() ? _apply.erase(i) : std::next(i);
            auto& entry = _apply[&state];
            entry.reset(new ApplyVars);
            entry->state = &state;
            vars = entry.get();
        }
        else vars = found->second.get();
    }
    ApplyVars& local = *vars;
#else
    ApplyVars local;
#endif
    ProgramRepo::Key& key = local.programKey;

    auto& repo = Registry::programRepo();
    const auto generation = s_compositionGeneration.load(std::memory_order_acquire);
    const auto repoGeneration = repo.getGeneration();
    const auto stack = _inherit ? StateEx::getProgramStack(state) : nullptr;
    const unsigned frameNumber = state.getFrameStamp() ? state.getFrameStamp()->getFrameNumber() : 0;
    const GLenum geometryParams[] = { GL_GEOMETRY_VERTICES_OUT_EXT,
        GL_GEOMETRY_INPUT_TYPE_EXT, GL_GEOMETRY_OUTPUT_TYPE_EXT };
    bool hit = local.reusable && local.state.get() == &state &&
        local.compositionGeneration == generation && local.repoGeneration == repoGeneration &&
        (stack ? local.stack == *stack : local.stack.empty()) &&
        local.templateFragBindings == _template->getFragDataBindingList() &&
        local.templateUniformBindings == _template->getUniformBlockBindingList();
    for (unsigned i = 0; hit && i < 3; ++i)
        hit = local.templateGeometry[i] == _template->getParameter(geometryParams[i]);

    if (hit)
    {
        program = local.program;
        // Preserve ownership/expiry bookkeeping once per frame, not per draw.
        if (frameNumber != local.frameLastUsed)
        {
            std::lock_guard<ProgramRepo> lock(repo);
            program = repo.use(key, frameNumber, _id);
            local.frameLastUsed = frameNumber;
        }
    }

    if (!program.valid())
    {
        local.reusable = false;
        local.pcp = nullptr;
        // Retain the capacity of all per-context vectors, including the key.
        local.accumShaderMap.clear();
        local.accumAttribBindings.clear();
        local.accumAttribAliases.clear();
        local.programKey.clear();
        local.accumExtensions.clear();

        // Delimit the variable-sized stack portion from the effective shader
        // and binding data appended below.
        key.push_back(0);

        // If we are inheriting, build the active shader map up to this point
        // (but not including this VP).
        if (_inherit)
        {
            accumulateShaders(
                state,
                _mask,
                local.accumShaderMap,
                local.accumAttribBindings,
                local.accumAttribAliases,
                local.accumExtensions,
                acceptCallbacksVary,
                this,
                &key);
        }

        // Next, add the data from this VP.
        {
            scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);

            accumulateLocalShaders(local.accumShaderMap, state, &key);
            for (const auto& shader : _shaderMap)
                acceptCallbacksVary |= shader.second._accept.valid();

            const AttribBindingList& abl = this->getAttribBindingList();
            local.accumAttribBindings.insert(abl.begin(), abl.end());

            local.accumExtensions.insert(_globalExtensions.begin(), _globalExtensions.end());
        }

        key.front() = key.size() - 1;
        key.push_back(local.accumShaderMap.size());
        for (auto& iter : local.accumShaderMap)
        {
            VirtualProgramAccess::refresh(*iter.second._shader);
            key.push_back(iter.first);
            key.push_back(iter.second._shader->getKeyID());
        }
        appendBindings(key, local.accumAttribBindings);
        key.push_back(local.accumExtensions.size());
        for (const auto& extension : local.accumExtensions)
            appendString(key, extension);
        appendTemplate(key, *_template);

#ifdef USE_PROGRAM_REPO
        // LOCK the program repo to look up the program.
        Registry::programRepo().lock();

        program = Registry::programRepo().use(local.programKey, frameNumber, _id);
        Registry::programRepo().unlock();
#endif

        if (!program.valid())
        {
            // build a new set of accumulated functions, to support the creation of main()
            FunctionLocationMap accumFunctions;
            accumulateFunctions(state, accumFunctions);

            //OE_NOTICE << LC << "Building new Program for VP " << getName() << std::endl;

            program = buildProgram(
                getName(),
                state,
                accumFunctions,
                local.accumShaderMap,
                local.accumExtensions,
                local.accumAttribBindings,
                local.accumAttribAliases,
                _template.get());

            if (_logShaders && program.valid())
            {
                std::stringstream buf;
                for (unsigned i = 0; i < program->getNumShaders(); i++)
                {
                    buf << program->getShader(i)->getShaderSource() << std::endl << std::endl;
                }

                if (_logPath.length() > 0)
                {
                    std::fstream outStream;
                    outStream.open(_logPath.c_str(), std::ios::out);
                    if (outStream.fail())
                    {
                        OE_WARN << LC << "Unable to open " << _logPath << " for logging shaders." << std::endl;
                    }
                    else
                    {
                        outStream << buf.str();
                        outStream.close();
                    }
                }
                else
                {
                    OE_NOTICE << LC << "Shader source: " << getName() << std::endl << "===============" << std::endl << buf.str() << std::endl << "===============" << std::endl;
                }
            }

#ifdef USE_PROGRAM_REPO
            // Adds this program to the repo, or finds an equivalent pre-existing program
            // in the repo and associates this program key with it.
            std::lock_guard<ProgramRepo> lock(Registry::programRepo());
            Registry::programRepo().add(local.programKey, program, frameNumber, _id);

            // purge expired programs.
            Registry::programRepo().prune(frameNumber, &state);
#endif
        }

        local.program = program;
        local.state = &state;
        if (stack) local.stack = *stack;
        else local.stack.clear();
        local.templateFragBindings = _template->getFragDataBindingList();
        local.templateUniformBindings = _template->getUniformBlockBindingList();
        for (unsigned i = 0; i < 3; ++i)
            local.templateGeometry[i] = _template->getParameter(geometryParams[i]);
        local.frameLastUsed = frameNumber;
        local.compositionGeneration = generation;
        local.repoGeneration = repoGeneration;
        // Never publish a reusable result across a tracked mutation.
        local.reusable = program.valid() && !acceptCallbacksVary &&
            generation == s_compositionGeneration.load(std::memory_order_acquire) &&
            repoGeneration == repo.getGeneration();
    }

    // finally, apply the program attribute.
    if (program.valid())
    {
        osg::Program::PerContextProgram* pcp;

        auto composed = dynamic_cast<const ComposedProgram*>(program.get());
        const auto glGeneration = composed ? composed->glGeneration.load(std::memory_order_acquire) : 0;
        if (composed && local.pcp.valid() && local.pcp->getProgram() == program.get() &&
            local.pcpGeneration == glGeneration &&
            definesMatch(state, *program, local.pcp->getDefineString()))
        {
            pcp = local.pcp.get();
        }
        else
        {
            // OSG mutates its PCP vector here; shared-context draw threads may
            // miss concurrently even though their composition caches differ.
            std::lock_guard<ProgramRepo> lock(repo);
            pcp = program->getPCP(state);
            local.pcp = pcp;
            local.pcpGeneration = glGeneration;
        }

        // dirtyProgram() can request a relink of the already-bound PCP.
        bool useProgram = state.getLastAppliedProgramObject() != pcp || pcp->needsLink();
        if (useProgram)
        {
            OE_PROFILING_ZONE_NAMED("use");
            OE_PROFILING_ZONE_TEXT(program->getName());

            bool needsLink = pcp->needsLink();

            if (needsLink)
            {
                Registry::programRepo().linkProgram(key, program.get(), pcp, state);
            }

            if (pcp->isLinked())
            {
                if (osg::isNotifyEnabled(osg::INFO))
                    pcp->validateProgram();

                if (_gldebug && s_debugGroupPushed.exchange(true))
                    OE_GL_POP;

                pcp->useProgram();

                if (_gldebug)
                {
                    std::string zone("[VP] " + program->getName());
                    OE_GL_PUSH(zone.c_str());
                }

                state.setLastAppliedProgramObject(pcp);
            }
            else
            {
                // program not usable, fallback to fixed function.
                const osg::GL2Extensions* extensions = osg::GL2Extensions::Get(contextID, true);
                extensions->glUseProgram(0);
                state.setLastAppliedProgramObject(0);

                if (needsLink)
                {
                    OE_WARN << LC << "Program will not link!" << std::endl;
                    ShaderInfoLog x(pcp->getProgram(), "");
                    x.dumpErrors(state);
                }
            }
        }

#ifdef USE_LAST_USED_PROGRAM
        _lastUsedProgram[contextID] = program.get();
#endif
    }
}

bool
VirtualProgram::checkSharing()
{
    if (::getenv("OSGEARTH_SHARED_VP_WARNING") && getNumParents() > 1)
    {
        OE_WARN << LC << "Modified VirtualProgram may be shared." << std::endl;
        return true;
    }

    return false;
}

void
VirtualProgram::getFunctions(
    VirtualProgram::FunctionLocationMap& out) const
{
    // make a safe copy of the functions map.
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    out = _functions;
}

void
VirtualProgram::getShaderMap(ShaderMap& out) const
{
    // make a safe copy of the functions map.
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    out = _shaderMap;
}

void
VirtualProgram::accumulateFunctions(
    const osg::State& state,
    FunctionLocationMap& result) const
{
    // This method searches the state's attribute stack and accumulates all 
    // the user functions (including those in this program).
    if (_inherit)
    {
        const AttrStack* av = StateEx::getProgramStack(state);
        if (av && av->size() > 0)
        {
            // find the closest VP that doesn't inherit:
            unsigned start;
            const osg::StateAttribute* sa;
            for (start = (int)av->size() - 1; start > 0; --start)
            {
                sa = (*av)[start].first;
#ifdef USE_TYPEID
                if (typeid(*sa) != typeid(VirtualProgram))
                    continue;
                const VirtualProgram* vp = static_cast<const VirtualProgram*>(sa);
#else
                const VirtualProgram* vp = dynamic_cast<const VirtualProgram*>(sa);
                if (!vp)
                    continue;
#endif

                if ((vp->_mask & _mask) && vp->_inherit == false)
                    break;
            }

            // collect functions from there on down.
            for (unsigned i = start; i < av->size(); ++i)
            {
                sa = (*av)[i].first;
#ifdef USE_TYPEID
                if (typeid(*sa) != typeid(VirtualProgram))
                    continue;
                const VirtualProgram* vp = static_cast<const VirtualProgram*>(sa);
#else
                const VirtualProgram* vp = dynamic_cast<const VirtualProgram*>(sa);
                if (!vp)
                    continue;
#endif

                if ((vp->_mask & _mask) && (vp != this))
                {
                    FunctionLocationMap rhs;
                    vp->getFunctions(rhs);

                    for(auto& j : rhs)
                    {
                        const OrderedFunctionMap& source = j.second;
                        OrderedFunctionMap&       dest = result[j.first];

                        for (auto k = source.begin(); k != source.end(); ++k)
                        {
                            if (k->second.accept(state))
                            {
                                // remove/override an existing function with the same name
                                for (auto exists = dest.begin(); exists != dest.end(); ++exists)
                                {
                                    if (exists->second._name.compare(k->second._name) == 0)
                                    {
                                        dest.erase(exists);
                                        break;
                                    }
                                }
                                dest.insert(*k);
                            }
                        }
                    }
                }
            }
        }
    }

    // add the local ones too:
    {
        scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);

        for (auto& j : _functions)
        {
            const OrderedFunctionMap& source = j.second;
            OrderedFunctionMap&       dest = result[j.first];

            for (auto k = source.begin(); k != source.end(); ++k)
            {
                if (k->second.accept(state))
                {
                    // remove/override an existing function with the same name
                    for (auto exists = dest.begin(); exists != dest.end(); ++exists)
                    {
                        if (exists->second._name.compare(k->second._name) == 0)
                        {
                            dest.erase(exists);
                            break;
                        }
                    }
                    dest.insert(*k);
                }
            }
        }
    }
}



void
VirtualProgram::accumulateShaders(
    const osg::State&  state,
    unsigned           mask,
    ShaderMap&         accumShaderMap,
    AttribBindingList& accumAttribBindings,
    AttribAliasMap&    accumAttribAliases,
    ExtensionsSet&     accumExtensions,
    bool&              acceptCallbacksVary,
    const VirtualProgram* exclude,
    ProgramRepo::Key* key)
{
    acceptCallbacksVary = false;

    const AttrStack* av = StateEx::getProgramStack(state);
    if (av && av->size() > 0)
    {
        // find the deepest VP that doesn't inherit:
        unsigned start = 0;
        const osg::StateAttribute* sa;
        for (start = (int)av->size() - 1; start > 0; --start)
        {
            sa = (*av)[start].first;
#ifdef USE_TYPEID
            if (typeid(*sa) != typeid(VirtualProgram))
                continue;
            const VirtualProgram* vp = static_cast<const VirtualProgram*>(sa);
#else
            const VirtualProgram* vp = dynamic_cast<const VirtualProgram*>(sa);
            if (!vp)
                continue;
#endif
            if ((vp->_mask & mask) && vp->_inherit == false)
                break;
        }

        // collect shaders from there to here:
        for (unsigned i = start; i < av->size(); ++i)
        {
            sa = (*av)[i].first;
#ifdef USE_TYPEID
            if (typeid(*sa) != typeid(VirtualProgram))
                continue;
            const VirtualProgram* vp = static_cast<const VirtualProgram*>(sa);
#else
            const VirtualProgram* vp = dynamic_cast<const VirtualProgram*>(sa);
            if (!vp)
                continue;
#endif
            // Only skip the trailing occurrence that apply() adds locally.
            // The same VP may also appear earlier in this path, where its
            // bindings and override precedence still matter.
            if ((vp->_mask & mask) && !(vp == exclude && i + 1 == av->size()))
            {
                // Read shaders and their associated metadata under the same lock.
                scoped_lock_if lock(vp->_dataModelMutex, vp->_useDataModelMutex);
                for (const auto& shader : vp->_shaderMap)
                    acceptCallbacksVary |= shader.second._accept.valid();
                vp->accumulateLocalShaders(accumShaderMap, state, key);

                const AttribBindingList& abl = vp->getAttribBindingList();
                accumAttribBindings.insert(abl.begin(), abl.end());

                const ExtensionsSet& es = vp->_globalExtensions;
                accumExtensions.insert(es.begin(), es.end());
            }
        }
    }
}

void
VirtualProgram::addShadersToAccumulationMap(
    VirtualProgram::ShaderMap& accumMap,
    const osg::State&          state) const
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);
    accumulateLocalShaders(accumMap, state);
}

void
VirtualProgram::accumulateLocalShaders(ShaderMap& accumMap,
    const osg::State& state, ProgramRepo::Key* key) const
{
    std::size_t callbackCountIndex = 0;
    if (key)
    {
        key->push_back(_functionKey.size());
        key->insert(key->end(), _functionKey.begin(), _functionKey.end());
        callbackCountIndex = key->size();
        key->push_back(0);
    }

    for (auto& iter : _shaderMap)
    {
        const bool accepted = iter.second.accept(state);
        if (key && iter.second._accept.valid())
        {
            ++(*key)[callbackCountIndex];
            key->push_back(accepted ? 1 : 0);
        }
        if (accepted)
        {
            addToAccumulatedMap(accumMap, iter.first, iter.second);
        }
    }
}

void
VirtualProgram::updateFunctionKey()
{
    _functionKey.clear();
    for (const auto& location : _functions)
    {
        if (location.second.empty())
            continue;
        _functionKey.push_back(location.first);
        _functionKey.push_back(location.second.size());
        for (const auto& function : location.second)
        {
            std::uint32_t order;
            static_assert(sizeof(order) == sizeof(function.first), "Float key size");
            std::memcpy(&order, &function.first, sizeof(order));
            _functionKey.push_back(order);
            _functionKey.push_back(MAKE_SHADER_ID(function.second._name));
        }
    }
}

int
VirtualProgram::getShaders(
    const osg::State& state,
    std::vector<osg::ref_ptr<osg::Shader> >& output)
{
    ShaderMap         shaders;
    AttribBindingList bindings;
    AttribAliasMap    aliases;
    ExtensionsSet     extensions;
    bool              acceptCallbacksVary;

    // build the collection:
    accumulateShaders(state, ~0, shaders, bindings, aliases, extensions, acceptCallbacksVary);

    // pre-allocate space:
    output.reserve(shaders.size());
    output.clear();

    // copy to output.
    for (auto& iter : shaders)
    {
        output.push_back(iter.second._shader->getNominalShader());
    }

    return output.size();
}

int
VirtualProgram::getPolyShaders(
    const osg::State& state,
    std::vector<osg::ref_ptr<PolyShader> >& output)
{
    ShaderMap         shaders;
    AttribBindingList bindings;
    AttribAliasMap    aliases;
    ExtensionsSet     extensions;
    bool              acceptCallbacksVary;

    // build the collection:
    accumulateShaders(state, ~0, shaders, bindings, aliases, extensions, acceptCallbacksVary);

    // pre-allocate space:
    output.reserve(shaders.size());
    output.clear();

    // copy to output.
    for (auto& iter : shaders)
    {
        output.push_back(iter.second._shader.get());
    }

    return output.size();
}

void VirtualProgram::setShaderLogging(bool log)
{
    setShaderLogging(log, "");
}

void VirtualProgram::setShaderLogging(bool log, const std::string& filepath)
{
    _logShaders = log;
    _logPath = filepath;
}

bool VirtualProgram::getAcceptCallbacksVaryPerFrame() const
{
    return _acceptCallbacksVaryPerFrame;
}

void VirtualProgram::setAcceptCallbacksVaryPerFrame(bool acceptCallbacksVaryPerFrame)
{
    _acceptCallbacksVaryPerFrame = acceptCallbacksVaryPerFrame;
}

int
VirtualProgram::compare_safe(const VirtualProgram& rhs) const
{
    scoped_lock_if lock(_dataModelMutex, _useDataModelMutex);

    // compare each parameter 
    COMPARE_StateAttribute_Parameter(_mask);
    COMPARE_StateAttribute_Parameter(_inherit);
    COMPARE_StateAttribute_Parameter(_isAbstract);

    if (_shaderMap.size() < rhs._shaderMap.size()) return -1;
    if (_shaderMap.size() > rhs._shaderMap.size()) return +1;

    ShaderMap::const_iterator lhsIter = _shaderMap.begin();
    ShaderMap::const_iterator rhsIter = rhs._shaderMap.begin();

    while (lhsIter != _shaderMap.end())
    {
        if (lhsIter->first < rhsIter->first) return -1;
        if (lhsIter->first > rhsIter->first) return +1;

        const ShaderEntry& lhsEntry = lhsIter->second;
        const ShaderEntry& rhsEntry = rhsIter->second;

        if (lhsEntry < rhsEntry) return -1;
        if (rhsEntry < lhsEntry) return +1;

        lhsIter++;
        rhsIter++;
    }

    // compare the template settings.
    if (_template.valid() && rhs.getTemplate())
    {
        int r = _template->compare(*(rhs.getTemplate()));
        if (r != 0) return r;
    }

    return 0;
}

//.........................................................................

void VirtualProgram::PolyShader::refreshShaderSnapshot() const
{
    std::lock_guard<std::mutex> lock(_snapshotMutex);
    const auto generation = s_shaderMutationGeneration.load(std::memory_order_acquire);
    if (_shaderSnapshot && _shaderSnapshot->keyID == _keyID)
    {
        if (_shaderSnapshot->mutationGeneration == generation)
            return;
        if (sameShaderContent(_nominalShader, _shaderSnapshot->nominal) &&
            sameShaderContent(_geomShader, _shaderSnapshot->geometry) &&
            sameShaderContent(_tessevalShader, _shaderSnapshot->tessellation))
        {
            _shaderSnapshot->mutationGeneration = generation;
            return;
        }
        // dirty() already invalidated all composition caches. Assign a fresh
        // identity only to changed content; unchanged shaders keep their shared
        // compiled versions and do not add repository aliases.
        _keyID = nextShaderKeyID();
    }
    _shaderSnapshot = std::make_shared<ShaderSnapshot>();
    _shaderSnapshot->nominal = _nominalShader ? copyShaderContent(*_nominalShader) : nullptr;
    _shaderSnapshot->geometry = _geomShader ? copyShaderContent(*_geomShader) : nullptr;
    _shaderSnapshot->tessellation = _tessevalShader ? copyShaderContent(*_tessevalShader) : nullptr;
    _shaderSnapshot->keyID = _keyID;
    _shaderSnapshot->mutationGeneration = generation;
}

osg::Shader* VirtualProgram::PolyShader::getNominalShader() const
{
    return _nominalShader.get();
}

VirtualProgram::PolyShader::PolyShader() :
    _keyID(nextShaderKeyID()),
    _dirty(true),
    _location(VirtualProgram::LOCATION_UNDEFINED)
{
    //nop
}

VirtualProgram::PolyShader::PolyShader(osg::Shader* shader) :
    _keyID(nextShaderKeyID()),
    _location(VirtualProgram::LOCATION_UNDEFINED),
    _nominalShader(shader)
{
    _dirty = shader != 0L;
    if (shader)
    {
        _name = shader->getName();

        // extract the source before preprocessing:
        _source = shader->getShaderSource();
    }
}

void
VirtualProgram::PolyShader::setShaderSource(const std::string& source)
{
    _source = source;
    _dirty = true;
    _hash.unset();
    compositionChanged();
}

void
VirtualProgram::PolyShader::setLocation(VirtualProgram::FunctionLocation location)
{
    _location = location;
    _dirty = true;
    _hash.unset();
    compositionChanged();
}

osg::Shader*
VirtualProgram::PolyShader::getShader(unsigned mask) const
{
    return selectShader(mask);
}

osg::Shader*
VirtualProgram::PolyShader::selectShader(unsigned mask) const
{
    if (_location == VirtualProgram::LOCATION_VERTEX_VIEW || 
        _location == VirtualProgram::LOCATION_VERTEX_CLIP ||
        _location == VirtualProgram::LOCATION_VERTEX_TRANSFORM_MODEL_TO_VIEW)
    {
        // geometry stage has priority (runs last)
        if (mask & VirtualProgram::STAGE_GEOMETRY)
        {
            //OE_DEBUG << "Installing GS for VIEW/CLIP shader!\n";
            return _geomShader.get();
        }

        else if (mask & VirtualProgram::STAGE_TESSEVALUATION)
        {
            //OE_DEBUG << "Installing TES for VIEW/CLIP shader!\n";
            return _tessevalShader.get();
        }
    }

    return _nominalShader.get();
}

void
VirtualProgram::PolyShader::prepare()
{
    if (_dirty)
    {
        _keyID = nextShaderKeyID();
        osg::Shader::Type nominalType;
        switch (_location)
        {
        case VirtualProgram::LOCATION_VERTEX_TRANSFORM_MODEL_TO_VIEW:
        case VirtualProgram::LOCATION_VERTEX_MODEL:
        case VirtualProgram::LOCATION_VERTEX_VIEW:
        case VirtualProgram::LOCATION_VERTEX_CLIP:
            nominalType = osg::Shader::VERTEX;
            break;
        case VirtualProgram::LOCATION_TESS_CONTROL:
            nominalType = osg::Shader::TESSCONTROL;
            break;
        case VirtualProgram::LOCATION_TESS_EVALUATION:
            nominalType = osg::Shader::TESSEVALUATION;
            break;
        case VirtualProgram::LOCATION_GEOMETRY:
            nominalType = osg::Shader::GEOMETRY;
            break;
        case VirtualProgram::LOCATION_FRAGMENT_COLORING:
        case VirtualProgram::LOCATION_FRAGMENT_LIGHTING:
        case VirtualProgram::LOCATION_FRAGMENT_OUTPUT:
            nominalType = osg::Shader::FRAGMENT;
            break;
        default:
            nominalType = osg::Shader::UNDEFINED;
        }

        if (nominalType != osg::Shader::UNDEFINED)
        {
            _nominalShader = new osg::Shader(nominalType, _source);
            if (!_name.empty())
                _nominalShader->setName(_name);
        }

        ShaderPreProcessor::runPost(_nominalShader.get());

        // for a VERTEX_VIEW or VERTEX_CLIP shader, these might get moved to another stage.
        if (_location == VirtualProgram::LOCATION_VERTEX_VIEW ||
            _location == VirtualProgram::LOCATION_VERTEX_CLIP ||
            _location == VirtualProgram::LOCATION_VERTEX_TRANSFORM_MODEL_TO_VIEW)
        {
            _geomShader = new osg::Shader(osg::Shader::GEOMETRY, _source);
            if (!_name.empty())
                _geomShader->setName(_name);
            ShaderPreProcessor::runPost(_geomShader.get());

            _tessevalShader = new osg::Shader(osg::Shader::TESSEVALUATION, _source);
            if (!_name.empty())
                _tessevalShader->setName(_name);
            ShaderPreProcessor::runPost(_tessevalShader.get());
        }
    }
    _dirty = false;
    // Capture the registered content before a caller can edit it, including
    // functions in the shared source cache that have not yet been applied.
    refreshShaderSnapshot();
    compositionChanged();
}

unsigned
VirtualProgram::PolyShader::getHash()
{
    if (_hash.isSet() == false)
    {
        _hash = osgEarth::hashString(_source);
    }
    return _hash.get();
}

void
VirtualProgram::PolyShader::resizeGLObjectBuffers(unsigned maxSize)
{
    if (_nominalShader.valid())
    {
        _nominalShader->resizeGLObjectBuffers(maxSize);
    }

    if (_geomShader.valid())
    {
        _geomShader->resizeGLObjectBuffers(maxSize);
    }

    if (_tessevalShader.valid())
    {
        _tessevalShader->resizeGLObjectBuffers(maxSize);
    }
}

void
VirtualProgram::PolyShader::releaseGLObjects(osg::State* state) const
{
    if (_nominalShader.valid())
    {
        _nominalShader->releaseGLObjects(state);
    }

    if (_geomShader.valid())
    {
        _geomShader->releaseGLObjects(state);
    }

    if (_tessevalShader.valid())
    {
        _tessevalShader->releaseGLObjects(state);
    }
}

VirtualProgram::PolyShader*
VirtualProgram::PolyShader::lookUpShader(
    const std::string& functionName,
    const std::string& shaderSource,
    VirtualProgram::FunctionLocation location)
{
    PolyShader* shader = NULL;

#ifdef USE_POLYSHADER_CACHE

    std::lock_guard<std::mutex> lock(getPolyShaderCacheMutex());

    auto hashKey = std::make_tuple(functionName, shaderSource, location);

    PolyShaderCache::iterator iter = getPolyShaderCache().find(hashKey);

    if (iter != getPolyShaderCache().end())
    {
        auto candidate = iter->second.first.get();
        candidate->refreshShaderSnapshot();
        // Public PolyShader edits must not change what a subsequent lookup of
        // the original source returns. Existing owners retain the edited object.
        if (!candidate->_dirty && candidate->getKeyID() == iter->second.second)
            shader = candidate;
    }
#endif

    if (!shader)
    {
        std::string source(shaderSource);
        ShaderLoader::configureHeader(source);

        shader = new PolyShader();
        shader->setName(functionName);
        shader->setLocation(location);
        shader->setShaderSource(source);
        shader->prepare();

#ifdef USE_POLYSHADER_CACHE
        getPolyShaderCache()[hashKey] = std::make_pair(shader, shader->getKeyID());
#endif
    }

    return shader;
}


void
VirtualProgram::PolyShader::clearShaderCache()
{
    std::lock_guard<std::mutex> lock(getPolyShaderCacheMutex());

    getPolyShaderCache().clear();
}

//.......................................................................
// SERIALIZERS for VIRTUALPROGRAM

#include <osgDB/ObjectWrapper>
#include <osgDB/InputStream>
#include <osgDB/OutputStream>

namespace
{
#define PROGRAM_LIST_FUNC( PROP, TYPE, DATA ) \
    static bool check##PROP(const osgEarth::VirtualProgram& attr) \
    { return attr.get##TYPE().size()>0; } \
    static bool read##PROP(osgDB::InputStream& is, osgEarth::VirtualProgram& attr) { \
        unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET; \
        for ( unsigned int i=0; i<size; ++i ) { \
            std::string key; unsigned int value; \
            is >> key >> value; attr.add##DATA(key, value); \
        } \
        is >> is.END_BRACKET; \
        return true; \
    } \
    static bool write##PROP( osgDB::OutputStream& os, const osgEarth::VirtualProgram& attr ) \
    { \
        const osg::Program::TYPE& plist = attr.get##TYPE(); \
        os.writeSize(plist.size()); os << os.BEGIN_BRACKET << std::endl; \
        for ( osg::Program::TYPE::const_iterator itr=plist.begin(); \
              itr!=plist.end(); ++itr ) { \
            os << itr->first << itr->second << std::endl; \
        } \
        os << os.END_BRACKET << std::endl; \
        return true; \
    }

    PROGRAM_LIST_FUNC(AttribBinding, AttribBindingList, BindAttribLocation);

    // functions
    static bool checkFunctions(const osgEarth::VirtualProgram& attr)
    {
        osgEarth::VirtualProgram::FunctionLocationMap functions;
        attr.getFunctions(functions);

        unsigned count = 0;
        for (osgEarth::VirtualProgram::FunctionLocationMap::const_iterator loc = functions.begin(); loc != functions.end(); ++loc)
            count += loc->second.size();
        return count > 0;
    }

    static bool readFunctions(osgDB::InputStream& is, osgEarth::VirtualProgram& attr)
    {
        unsigned int size = is.readSize();
        is >> is.BEGIN_BRACKET;

        for (unsigned int i = 0; i < size; ++i)
        {
            std::string name;
            is >> name >> is.BEGIN_BRACKET;
            {
                unsigned location;
                is >> is.PROPERTY("Location") >> location;

                float order;
                is >> is.PROPERTY("Order") >> order;

                std::string source;
                is >> is.PROPERTY("Source");
                unsigned lines = is.readSize();
                is >> is.BEGIN_BRACKET;
                {
                    for (unsigned j = 0; j < lines; ++j)
                    {
                        std::string line;
                        is.readWrappedString(line);
                        source.append(line); source.append(1, '\n');
                    }
                }
                is >> is.END_BRACKET;

                attr.setFunction(name, source, (osgEarth::VirtualProgram::FunctionLocation)location, order);
            }
            is >> is.END_BRACKET;
        }
        is >> is.END_BRACKET;
        return true;
    }

    static bool writeFunctions(osgDB::OutputStream& os, const osgEarth::VirtualProgram& attr)
    {
        osgEarth::VirtualProgram::FunctionLocationMap functions;
        attr.getFunctions(functions);

        osgEarth::VirtualProgram::ShaderMap shaders;
        attr.getShaderMap(shaders);

        unsigned count = 0;
        for (osgEarth::VirtualProgram::FunctionLocationMap::const_iterator loc = functions.begin(); loc != functions.end(); ++loc)
            count += loc->second.size();

        os.writeSize(count);
        os << os.BEGIN_BRACKET << std::endl;
        {
            for (osgEarth::VirtualProgram::FunctionLocationMap::const_iterator loc = functions.begin(); loc != functions.end(); ++loc)
            {
                const osgEarth::VirtualProgram::OrderedFunctionMap& ofm = loc->second;
                for (osgEarth::VirtualProgram::OrderedFunctionMap::const_iterator k = ofm.begin(); k != ofm.end(); ++k)
                {
                    os << k->second._name << os.BEGIN_BRACKET << std::endl;
                    {
                        os << os.PROPERTY("Location") << (unsigned)loc->first << std::endl;
                        os << os.PROPERTY("Order") << k->first << std::endl;

                        osgEarth::VirtualProgram::ShaderID shaderId = MAKE_SHADER_ID(k->second._name);
                        osgEarth::VirtualProgram::ShaderMap::const_iterator miter = shaders.find(shaderId);
                        const osgEarth::VirtualProgram::ShaderEntry* m = miter != shaders.end() ? &miter->second : NULL;
                        if (m)
                        {
                            std::vector<std::string> lines;
                            std::istringstream iss(m->_shader->getShaderSource());
                            std::string line;
                            while (std::getline(iss, line))
                                lines.push_back(line);

                            os << os.PROPERTY("Source");
                            os.writeSize(lines.size());
                            os << os.BEGIN_BRACKET << std::endl;
                            {
                                for (std::vector<std::string>::const_iterator itr = lines.begin(); itr != lines.end(); ++itr)
                                {
                                    os.writeWrappedString(*itr);
                                    os << std::endl;
                                }
                            }
                            os << os.END_BRACKET << std::endl;
                        }
                    }
                    os << os.END_BRACKET << std::endl;
                }
            }
        }
        os << os.END_BRACKET << std::endl;

        return true;
    }

    REGISTER_OBJECT_WRAPPER(
        VirtualProgram,
        new osgEarth::VirtualProgram,
        osgEarth::VirtualProgram,
        "osg::Object osg::StateAttribute osgEarth::VirtualProgram")
    {
        ADD_BOOL_SERIALIZER(InheritShaders, true);
        ADD_UINT_SERIALIZER(Mask, ~0);

        ADD_USER_SERIALIZER(AttribBinding);
        ADD_USER_SERIALIZER(Functions);

        ADD_BOOL_SERIALIZER(IsAbstract, false);
    }
}
