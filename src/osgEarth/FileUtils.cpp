/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */

#include <osgEarth/FileUtils>
#include <osgEarth/Registry>
#include <osgEarth/StringUtils>
#include "Notify"
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>
#include <osg/Object>
#include <osgDB/ConvertUTF>
#include <stack>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <stdio.h>
#  include <stdlib.h>
#endif


#ifdef _WIN32
#  include <sys/utime.h>
#else
#  include <utime.h>
#endif



// currently this impl is for _all_ platforms, except as defined.
// the mac version will change soon to reflect the path scheme under osx, but
// for now, the above include is commented out, and the below code takes precedence.

#if defined(_WIN32) && !defined(__CYGWIN__)
    #include <io.h>
    #define WINBASE_DECLARE_GET_MODULE_HANDLE_EX
    #include <windows.h>
    #include <winbase.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <direct.h> // for _mkdir

    #define mkdir(x,y) _mkdir((x))
    #define stat64 _stati64

    // set up for windows so acts just like unix access().
#ifndef F_OK
    #define F_OK 4
#endif

#else // unix

#if defined( __APPLE__ )
    // I'm not sure how we would handle this in raw Darwin
    // without the AvailablilityMacros.
    #include <AvailabilityMacros.h>

    //>OSG_IOS
    //IOS includes
    #include "TargetConditionals.h"

    #if (TARGET_OS_IPHONE)
        #include <Availability.h>
        // workaround a bug which appears when compiling for SDK < 4.0 and for the simulator
        #if defined(__IPHONE_4_0) && (__IPHONE_OS_VERSION_MIN_REQUIRED >= __IPHONE_4_0)
            #define stat64 stat
        #else
            #if !TARGET_IPHONE_SIMULATOR
                #define stat64 stat
            #endif
        #endif
    #endif
    //<OSG_IPHONE

    // 10.5 defines stat64 so we can't use this #define
    // By default, MAC_OS_X_VERSION_MAX_ALLOWED is set to the latest
    // system the headers know about. So I will use this as the control
    // variable. (MIN_ALLOWED is set low by default so it is
    // unhelpful in this case.)
    // Unfortunately, we can't use the label MAC_OS_X_VERSION_10_4
    // for older OS's like Jaguar, Panther since they are not defined,
    // so I am going to hardcode the number.
    #if (MAC_OS_X_VERSION_MAX_ALLOWED <= 1040 )
        #define stat64 stat
    #endif

    #if MAC_OS_X_VERSION_MAX_ALLOWED >= 101300 && __DARWIN_ONLY_64_BIT_INO_T
        #define stat64 stat
    #endif

#elif defined(__CYGWIN__) || defined(__FreeBSD__) || (defined(__hpux) && !defined(_LARGEFILE64_SOURCE))
    #define stat64 stat
#endif

    #include <stdlib.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
#endif

    // set up _S_ISDIR()
#if !defined(S_ISDIR)
#  if defined( _S_IFDIR) && !defined( __S_IFDIR)
#    define __S_IFDIR _S_IFDIR
#  endif
#  define S_ISDIR(mode)    (mode&__S_IFDIR)
#endif

#if !defined(S_ISREG) && defined(_S_IFREG)
#  define S_ISREG(mode)    (mode&_S_IFREG)
#endif


#define LC "[FileUtils] "


using namespace osgEarth;
using namespace osgEarth::Util;

namespace osgEarth
{
#ifdef OSG_USE_UTF8_FILENAME
#define OSGDB_STRING_TO_FILENAME(s) osgDB::convertUTF8toUTF16(s)
#define OSGDB_FILENAME_TO_STRING(s) osgDB::convertUTF16toUTF8(s)
#define OSGDB_FILENAME_TEXT(x) L ## x
#define OSGDB_WINDOWS_FUNCT(x) x ## W
#define OSGDB_WINDOWS_FUNCT_STRING(x) #x "W"
    typedef wchar_t filenamechar;
    typedef std::wstring filenamestring;
#else
#define OSGDB_STRING_TO_FILENAME(s) s
#define OSGDB_FILENAME_TO_STRING(s) s
#define OSGDB_FILENAME_TEXT(x) x
#define OSGDB_WINDOWS_FUNCT(x) x ## A
#define OSGDB_WINDOWS_FUNCT_STRING(x) #x "A"
    typedef char filenamechar;
    typedef std::string filenamestring;
#endif
}

namespace
{
    // getFullPath needs an absolute referrer, not filesystem canonicalization.
    // In particular, do not query the file or expand 8.3 names on Windows.
    std::string resolveReferrer(const std::string& path)
    {
#if defined(_WIN32) && !defined(__CYGWIN__)
        const filenamestring& nativePath = OSGDB_STRING_TO_FILENAME(path);
        filenamechar stackBuffer[MAX_PATH + 1];
        filenamechar* buffer = stackBuffer;
        DWORD capacity = MAX_PATH + 1;
        std::vector<filenamechar> storage;

        DWORD length = OSGDB_WINDOWS_FUNCT(GetFullPathName)(
            nativePath.c_str(), capacity, buffer, nullptr);
        while (length >= capacity)
        {
            // An insufficient-buffer result includes the terminating null.
            // Never consume a truncated or uninitialized path.
            capacity = length;
            storage.resize(capacity);
            buffer = storage.data();
            length = OSGDB_WINDOWS_FUNCT(GetFullPathName)(
                nativePath.c_str(), capacity, buffer, nullptr);
        }
        if (length == 0)
            return path;

        // Preserve OSG's drive-letter convention without changing path casing.
        if (length >= 2 && buffer[1] == ':' && buffer[0] >= 'a' && buffer[0] <= 'z')
            buffer[0] = static_cast<filenamechar>(buffer[0] - ('a' - 'A'));
#ifdef OSG_USE_UTF8_FILENAME
        return osgDB::convertUTF16toUTF8(buffer, length);
#else
        return std::string(buffer, length);
#endif
#else
        // POSIX realpath also resolves symbolic links; retain that behavior.
        return osgDB::getRealPath(path);
#endif
    }
}


std::string
osgEarth::Util::getAbsolutePath(const std::string& path)
{
    return osgDB::convertFileNameToUnixStyle( osgDB::getRealPath(path) );
}

bool
osgEarth::Util::isRelativePath(const std::string& fileName)
{
    //If it is a URL, it is not relative
    if (osgDB::containsServerAddress( fileName ) ) return false;

    std::string native = osgDB::convertFileNameToNativeStyle(fileName);
#if defined(_WIN32)  && !defined(__CYGWIN__)
    //Check to see if the path begins with a drive letter like "C:\data"
    if (native.size() >= 3 && native[1] == ':' && native[2] == '\\') return false;

    //Check to see if the path is network path like "\\server1\data"
    if (native.size() >= 2 && native[0] == '\\' && native[1] == '\\') return false;

    //The path must be relative
    return true;
#else
    //Absolute paths in Unix will start with a '/'
    return !(native.size() >= 1 && native[0] == '/');
#endif
}

std::map<std::string, std::string>
osgEarth::Util::extractQueryParams(const std::string& url) {
    std::map<std::string, std::string> queryParams;

    size_t pos = url.find('?');
    if (pos != std::string::npos) {
        std::string queryParameters = url.substr(pos + 1);
        std::stringstream ss(queryParameters);
        std::string param;
        while (std::getline(ss, param, '&')) {
            pos = param.find('=');
            if (pos != std::string::npos) {
                std::string key = param.substr(0, pos);
                std::string value = param.substr(pos + 1);
                queryParams[key] = value;
            }
        }
    }

    return queryParams;
}

std::string
osgEarth::Util::getFullPath(const std::string& relativeTo, const std::string &relativePath)
{
    static std::unordered_map<std::string, std::string> s_cache;
    static std::mutex s_cache_mutex;

    // A length prefix distinguishes inputs containing the old '&' delimiter.
    std::string cacheKey = std::to_string(relativeTo.size());
    cacheKey.reserve(cacheKey.size() + 1 + relativeTo.size() + relativePath.size());
    cacheKey += ':';
    cacheKey += relativeTo;
    cacheKey += relativePath;
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        auto i = s_cache.find(cacheKey);
        if (i != s_cache.end())
            return i->second;
    }

    // result that will go into the cache:
    std::string result;

    std::string relativeToMinusQueryParams = relativeTo;
    std::string relativePathMinusQueryParams = relativePath;
    std::map< std::string, std::string> queryParams;
    // We are going to merge query parameters on both the relativeTo and relativePath arguments for the final URL.
    if (osgDB::containsServerAddress(relativeTo))
    {
        queryParams = extractQueryParams(relativeTo);
        auto queryParams2 = extractQueryParams(relativePath);
        for (auto& p : queryParams2)
        {
            queryParams.insert(p);
        }

        relativeToMinusQueryParams = removeQueryParams(relativeTo);
        relativePathMinusQueryParams = removeQueryParams(relativePath);
    }

    // If the relativePath starts with a / then it's really relative to the root server address of the relativeTo.
    // For example if relativeTo is http://example.com/assets/1 and the relativePath is /models/7 then the final URL should be
    // http://example.com/models/7
    if (osgDB::containsServerAddress(relativeToMinusQueryParams) && osgEarth::startsWith(relativePathMinusQueryParams, "/"))
    {
        std::string serverAddress = osgDB::getServerProtocol(relativeToMinusQueryParams) + "://" + osgDB::getServerAddress(relativeToMinusQueryParams);
        std::string filename = serverAddress + relativePathMinusQueryParams;
        std::string path = stripRelativePaths(filename);                     
        result = path;
    }

    // If they passed in an absolute path, strip any relative paths that might be embedded in the path.
    // So if they passed in c:/data/models/../../file.jpg this will return the resolved path of c:/file.jpg
    else if (!isRelativePath(relativePathMinusQueryParams))
    {
        result = stripRelativePaths(relativePathMinusQueryParams);
    }
    // They passed in a relative path but no relativeTo, so just return the relativePath as is.
    else if (relativeToMinusQueryParams.empty())
    {
        result = relativePathMinusQueryParams;
    }

    //If they didn't specify a relative path, just return the relativeTo
    else if (relativePathMinusQueryParams.empty())
    {
        result = relativeToMinusQueryParams;
    }

    else
    {
        //Note:  Modified from VPB

        //Concatinate the paths together
        std::string filename;
        if ( !osgDB::containsServerAddress(relativeToMinusQueryParams) )
            filename = osgDB::concatPaths( osgDB::getFilePath( resolveReferrer(relativeToMinusQueryParams)), relativePathMinusQueryParams);
        else
            filename = osgDB::concatPaths( osgDB::getFilePath(relativeToMinusQueryParams), relativePathMinusQueryParams);

        std::string path = stripRelativePaths(filename);

        //OE_NOTICE << "FullPath " << path << std::endl;
        result = path;
    }

    // Append the merged query parameters to the result
    if (!queryParams.empty())
    {
        std::stringstream buf;
        bool firstParam = true;
        for (auto& p : queryParams)
        {
            if (!firstParam)
            {
                buf << "&";
            }
            buf << p.first << "=" << p.second;
            firstParam = false;
        }

        std::string queryParamsStr = buf.str();
        if (!queryParamsStr.empty())
        {
            result = result + "?" + queryParamsStr;
        }
    }

    // cache the result and return it.
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        // Keep path resolution outside the lock, and enforce the limit here
        // since another caller may have inserted entries during resolution.
        if (s_cache.size() >= 20000)
            s_cache.clear();
        s_cache.emplace(std::move(cacheKey), result);
    }
    return result;
}

std::string
osgEarth::Util::stripRelativePaths(const std::string& filename)
{
    // If they pass in a relative path just return it unmodified
    if (isRelativePath(filename))
    {
        return filename;
    }

    std::list<std::string> directories;
    int start = 0;
    for (unsigned int i = 0; i < filename.size(); ++i)
    {
        if (filename[i] == '\\' || filename[i] == '/')
        {
            //Get the current directory
            std::string dir = filename.substr(start, i - start);

            if (dir != "..")
            {
                if (dir != ".")
                {
                    directories.push_back(dir);
                }
            }
            else if (!directories.empty())
            {
                directories.pop_back();
            }
            start = i + 1;
        }
    }

    std::string path;
    for (std::list<std::string>::iterator itr = directories.begin();
        itr != directories.end();
        ++itr)
    {
        path += *itr;
        path += "/";
    }

    path += filename.substr(start, std::string::npos);
    return path;
}

std::string osgEarth::Util::removeQueryParams(const std::string& uri) {
    std::string::size_type pos = uri.find('?');
    if (pos != std::string::npos) {
        return uri.substr(0, pos);
    }
    return uri;
}

// force getFullPath's statics to be initialized at startup
//OSG_INIT_SINGLETON_PROXY(osgEarthFileUtilsGetFullPathInitSingletonProxy, osgEarth::Util::getFullPath(std::string(), std::string()));

bool
osgEarth::Util::isArchive(const std::string& path)
{
    osgDB::Registry::ArchiveExtensionList list = osgDB::Registry::instance()->getArchiveExtensions();
    for( osgDB::Registry::ArchiveExtensionList::const_iterator i = list.begin(); i != list.end(); ++i )
    {
        if ( osgEarth::Util::ciEndsWith(path, ("."+*i)) )
            return true;
    }
    return false;
}

bool
osgEarth::Util::isPathToArchivedFile(const std::string& path)
{
    osgDB::Registry::ArchiveExtensionList list = osgDB::Registry::instance()->getArchiveExtensions();
    for( osgDB::Registry::ArchiveExtensionList::const_iterator i = list.begin(); i != list.end(); ++i )
    {
        if (path.find("."+*i+"/")  != std::string::npos ||
            path.find("."+*i+"\\") != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

std::string
osgEarth::Util::getTempPath()
{
#if defined(_WIN32)  && !defined(__CYGWIN__)
    BOOL fSuccess  = FALSE;

    TCHAR lpTempPathBuffer[MAX_PATH];

    //  Gets the temp path env string (no guarantee it's a valid path).
    DWORD dwRetVal = ::GetTempPath(MAX_PATH,          // length of the buffer
        lpTempPathBuffer); // buffer for path

    if (dwRetVal > MAX_PATH || (dwRetVal == 0))
    {
        OE_NOTICE << "GetTempPath failed" << std::endl;
        return ".";
    }

    return std::string(lpTempPathBuffer);
#else
    return "/tmp/";
#endif
}

std::string
osgEarth::Util::getTempName(const std::string& prefix, const std::string& suffix)
{
    //tmpname is kind of busted on Windows, it always returns a file of the form \blah which gets put in your root directory but
    //oftentimes can't get opened by some drivers b/c it doesn't have a drive letter in front of it.
    while (true)
    {
        std::stringstream ss;
        ss << prefix << "~" << std::this_thread::get_id() << "_" << rand() << suffix;
        if (!osgDB::fileExists(ss.str()))
            return ss.str();
    }
}

bool
osgEarth::Util::makeDirectory( const std::string &path )
{
    if (path.empty())
    {
        OSG_DEBUG << "osgDB::makeDirectory(): cannot create an empty directory" << std::endl;
        return false;
    }

    struct stat64 stbuf;
#ifdef OSG_USE_UTF8_FILENAME
    if( _wstat64( OSGDB_STRING_TO_FILENAME(path).c_str(), &stbuf ) == 0 )
#else
    if( stat64( path.c_str(), &stbuf ) == 0 )
#endif
    {
        if( S_ISDIR(stbuf.st_mode))
            return true;
        else
        {
            OSG_DEBUG << "osgDB::makeDirectory(): "  <<
                    path << " already exists and is not a directory!" << std::endl;
            return false;
        }
    }

    std::string dir = path;
    std::stack<std::string> paths;
    while( true )
    {
        if( dir.empty() )
            break;

#ifdef OSG_USE_UTF8_FILENAME
        if( _wstat64( OSGDB_STRING_TO_FILENAME(dir).c_str(), &stbuf ) < 0 )
#else
        if( stat64( dir.c_str(), &stbuf ) < 0 )
#endif
        {
            switch( errno )
            {
                case ENOENT:
                case ENOTDIR:
                    paths.push( dir );
                    break;

                default:
                    OSG_DEBUG << "osgDB::makeDirectory(): "  << strerror(errno) << std::endl;
                    return false;
            }
        }
        dir = osgDB::getFilePath(std::string(dir));
    }

    while( !paths.empty() )
    {
        std::string dir = paths.top();

        #if defined(_WIN32)
            //catch drive name
            if (dir.size() == 2 && dir.c_str()[1] == ':') {
                paths.pop();
                continue;
            }
        #endif

#ifdef OSG_USE_UTF8_FILENAME
        if ( _wmkdir(OSGDB_STRING_TO_FILENAME(dir).c_str())< 0 )
#else
        if( mkdir( dir.c_str(), 0755 )< 0 )
#endif
        {
            if (osgDB::fileExists(dir))
            {
                OE_DEBUG << "Attempt to create directory that already exists " << dir << std::endl;
            }
            else
            {
                OSG_DEBUG << "osgDB::makeDirectory(): "  << strerror(errno) << std::endl;
                return false;
            }
        }
        paths.pop();
    }
    return true;
}

bool
osgEarth::Util::makeDirectoryForFile( const std::string &path )
{
    return makeDirectory( osgDB::getFilePath( path ));
}


std::uint64_t
osgEarth::Util::getFileSize(const std::string& path)
{
    struct stat64 stbuf;
#ifdef OSG_USE_UTF8_FILENAME
    const int result = _wstat64(OSGDB_STRING_TO_FILENAME(path).c_str(), &stbuf);
#else
    const int result = stat64(path.c_str(), &stbuf);
#endif
    return result == 0 && S_ISREG(stbuf.st_mode) ?
        static_cast<std::uint64_t>(stbuf.st_size) : 0u;
}


bool
osgEarth::Util::removeDirectory(const std::string& path)
{
    if (!osgDB::fileExists(path))
        return true;

    if (osgDB::fileType(path) != osgDB::DIRECTORY)
        return false;

    const osgDB::DirectoryContents contents = osgDB::getDirectoryContents(path);
    for (osgDB::DirectoryContents::const_iterator i = contents.begin(); i != contents.end(); ++i)
    {
        if (*i == "." || *i == "..")
            continue;

        const std::string child = osgDB::concatPaths(path, *i);
        bool removed = false;
        if (osgDB::fileType(child) == osgDB::DIRECTORY)
        {
            removed = removeDirectory(child);
        }
        else
        {
#ifdef OSG_USE_UTF8_FILENAME
            removed = _wunlink(OSGDB_STRING_TO_FILENAME(child).c_str()) == 0;
#else
            removed = ::unlink(child.c_str()) == 0;
#endif
        }

        if (!removed)
            return false;
    }

#ifdef OSG_USE_UTF8_FILENAME
    return _wrmdir(OSGDB_STRING_TO_FILENAME(path).c_str()) == 0;
#else
    return ::rmdir(path.c_str()) == 0;
#endif
}


bool
osgEarth::Util::touchFile(const std::string& path)
{
    DateTime now;
    struct ::utimbuf ut;
    ut.actime = now.asTimeStamp();
    ut.modtime = now.asTimeStamp();
    return 0 == ::utime( path.c_str(), &ut );
}


TimeStamp
osgEarth::Util::getLastModifiedTime(const std::string& path)
{
    struct stat buf;
    if ( stat(path.c_str(), &buf) == 0 )
        return buf.st_mtime;
    else
        return 0;
}


/**************************************************/
DirectoryVisitor::DirectoryVisitor()
{
}

void DirectoryVisitor::handleFile( const std::string& filename )
{
}

bool DirectoryVisitor::handleDir( const std::string& path )
{
	return true;
}

void DirectoryVisitor::traverse(const std::string& path )
{
	if ( osgDB::fileType(path) == osgDB::DIRECTORY )
	{
		if (handleDir( path ))
		{
			osgDB::DirectoryContents files = osgDB::getDirectoryContents(path);
			for( osgDB::DirectoryContents::const_iterator f = files.begin(); f != files.end(); ++f )
			{
				if ( f->compare(".") == 0 || f->compare("..") == 0 )
					continue;

				std::string filepath = osgDB::concatPaths( path, *f );
				traverse( filepath );
			}
		}
	}
	else if ( osgDB::fileType(path) == osgDB::REGULAR_FILE )
	{
		handleFile( path );
	}
}

/**************************************************/
CollectFilesVisitor::CollectFilesVisitor()
{
}

void CollectFilesVisitor::handleFile( const std::string& filename )
{
	filenames.push_back( filename );
}

