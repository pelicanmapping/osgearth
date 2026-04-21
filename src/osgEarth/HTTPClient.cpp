/* osgEarth
 * Copyright 2025 Pelican Mapping
 * MIT License
 */
#include <osgEarth/HTTPClient>
#include <osgEarth/Progress>
#include <osgEarth/Metrics>
#include <osgEarth/Version>
#include <osgEarth/ImageUtils>
#include <osgEarth/Cache>
#include <osgEarth/CacheBin>
#include <osgEarth/URI>
#include <osgEarth/FileUtils>
#include "Notify"
#include <osgDB/ReadFile>
#include <osgDB/FileNameUtils>
#include <curl/curl.h>

#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
#include <Superluminal/PerformanceAPI.h>
#endif

// Whether to use WinInet instead of cURL - CMAKE option
#ifdef OSGEARTH_USE_WININET_FOR_HTTP
#include <WinInet.h>
#pragma comment(lib, "wininet.lib")
#endif

#define LC "[HTTPClient] "

//#define OE_TEST OE_NOTICE
#define OE_TEST OE_NULL

// Uncomment this to test curl connecting sharing
//#define OE_CURL_SHARE

using namespace osgEarth;
using namespace osgEarth::Util;

namespace osgEarth
{
    static int s_simResponseCode = -1;

#ifdef OE_CURL_SHARE
    static CURLSH* CURL_SHARE = nullptr;
    static Threading::RecursiveMutex CURL_SHARE_MUTEX;

    static void share_lock_cb(CURL* handle, curl_lock_data data,
        curl_lock_access access, void* userptr)
    {
        CURL_SHARE_MUTEX.lock();
    }

    static void share_unlock_cb(CURL* handle, curl_lock_data data,
        void* userptr)
    {
        CURL_SHARE_MUTEX.unlock();
    }
#endif

    struct StreamObject
    {
        StreamObject(std::ostream* stream) : _stream(stream) { }

        void write(const char* ptr, size_t realsize)
        {
            if (_stream) _stream->write(ptr, realsize);
        }

        void writeHeader(const char* ptr, size_t realsize)
        {
            std::string header(ptr);
            std::size_t colon = header.find_first_of(':');
            if (colon != std::string::npos && colon > 0 && colon < header.length() - 1)
            {
                _headers[header.substr(0, colon)] = header.substr(colon + 1);
            }
        }

        std::ostream* _stream;
        Headers _headers;
    };

    static size_t
    StreamObjectReadCallback(void* ptr, size_t size, size_t nmemb, void* data)
    {
        size_t realsize = size* nmemb;
        StreamObject* sp = (StreamObject*)data;
        sp->write((const char*)ptr, realsize);
        return realsize;
    }

    static size_t
    StreamObjectHeaderCallback(void* ptr, size_t size, size_t nmemb, void* data)
    {
        size_t realsize = size* nmemb;
        StreamObject* sp = (StreamObject*)data;
        sp->writeHeader((const char*)ptr, realsize);
        return realsize;
    }

    TimeStamp
    getCurlFileTime(void* curl)
    {
        long filetime;
        if (CURLE_OK != curl_easy_getinfo(curl, CURLINFO_FILETIME, &filetime))
            return TimeStamp(0);
        else if (filetime < 0)
            return TimeStamp(0);
        else
            return TimeStamp(filetime);
    }

    static int CurlProgressCallback(void *clientp,double dltotal,double dlnow,double ultotal,double ulnow)
    {
        ProgressCallback* callback = (ProgressCallback*)clientp;
        bool cancelled = false;
        if (callback)
        {
            cancelled = callback->isCanceled() || callback->reportProgress(dlnow, dltotal);
            if (cancelled)
                OE_TEST << "An HTTP request was canceled mid-stream" << std::endl;
        }
        return cancelled;
    }

    bool decodeMultipartStream(const std::string&   boundary,
                               HTTPResponse::Part*  input,
                               HTTPResponse::Parts& output)
    {
        std::string bstr = std::string("--") + boundary;
        std::string line;
        char tempbuf[256];

        // first thing in the stream should be the boundary.
        input->_stream.read( tempbuf, bstr.length() );
        tempbuf[bstr.length()] = 0;
        line = tempbuf;
        if ( line != bstr )
        {
            OE_INFO << LC
                << "decodeMultipartStream: protocol violation; "
                << "expecting boundary; instead got: \""
                << line
                << "\"" << std::endl;
            return false;
        }

        for( bool done=false; !done; )
        {
            osg::ref_ptr<HTTPResponse::Part> next_part = new HTTPResponse::Part();

            // first finish off the boundary.
            std::getline( input->_stream, line );
            if ( line == "--" )
            {
                done = true;
            }
            else
            {
                // read all headers. this ends with a blank line.
                line = " ";
                while( line.length() > 0 && !done )
                {
                    std::getline( input->_stream, line );

                    // check for EOS:
                    if ( line == "--" )
                    {
                        done = true;
                    }
                    else
                    {
                        auto tokens = StringTokenizer()
                            .delim(":")
                            .standardQuotes()
                            .tokenize(line);

                        if (tokens.size() >= 2 )
                            next_part->_headers[tokens[0]] = tokens[1];
                    }
                }
            }

            if ( !done )
            {
                // read data until we reach the boundary
                unsigned int bstr_ptr = 0;
                std::string temp;
                //unsigned int c = 0;
                while( bstr_ptr < bstr.length() )
                {
                    char b;
                    input->_stream.read( &b, 1 );
                    if ( b == bstr[bstr_ptr] )
                    {
                        bstr_ptr++;
                    }
                    else
                    {
                        for( unsigned int i=0; i<bstr_ptr; i++ )
                        {
                            next_part->_stream << bstr[i];
                        }
                        next_part->_stream << b;
                        next_part->_size += bstr_ptr + 1;
                        bstr_ptr = 0;
                    }
                }
                output.push_back( next_part.get() );
            }
        }

        return true;
    }

    void setMetadata(ReadResult& result, const HTTPRequest& request, const HTTPResponse& response)
    {
        // be sure to install the response headers at the top level:
        Config meta = response.getHeadersAsConfig();
        meta.key() = "HTTP GET";

        auto& r = meta.add("osgEarth Request");
        
        r.add("URI", request.getURL());

        if (response.getCode() == 0)
        {
            if (!response.getMessage().empty())
                r.add("Request Error", response.getMessage());
            else
                r.add("Request Error (UNKNOWN)");
        }
        else
        {
            r.add("HTTP Response Code", std::to_string(response.getCode()));
            r.add("HTTP Request Headers", request.getHeadersAsConfig());
        }
        result.setMetadata(meta);
    }
}

/****************************************************************************/

HTTPRequest::HTTPRequest( const std::string& url )
: _url( url )
{
    // Convert spaces to %20.
    osgEarth::replaceIn(_url, " ", "%20");
}

HTTPRequest::HTTPRequest( const HTTPRequest& rhs ) :
_parameters( rhs._parameters ),
_headers(rhs._headers),
_url( rhs._url )
{
    //nop
}

void
HTTPRequest::addParameter( const std::string& name, const std::string& value )
{
    _parameters[name] = value;
}

void
HTTPRequest::addParameter( const std::string& name, int value )
{
    std::stringstream buf;
    buf << value;
     std::string bufStr;
    bufStr = buf.str();
    _parameters[name] = bufStr;
}

void
HTTPRequest::addParameter( const std::string& name, double value )
{
    std::stringstream buf;
    buf << value;
     std::string bufStr;
    bufStr = buf.str();
    _parameters[name] = bufStr;
}

const HTTPRequest::Parameters&
HTTPRequest::getParameters() const
{
    return _parameters;
}

void
HTTPRequest::addHeader( const std::string& name, const std::string& value )
{
    _headers[name] = value;
}

const Headers&
HTTPRequest::getHeaders() const
{
    return _headers;
}

Headers&
HTTPRequest::getHeaders()
{
    return _headers;
}

Config
HTTPRequest::getHeadersAsConfig() const
{
    Config conf("HTTP Request Headers");
    for (auto& header : _headers)
        conf.add(header.first, header.second);
    return conf;

}

void HTTPRequest::setLastModified( const DateTime &lastModified)
{
    addHeader("If-Modified-Since", lastModified.asRFC1123());
}


std::string
HTTPRequest::getURL() const
{
    if ( _parameters.size() == 0 )
    {
        return _url;
    }
    else
    {
        std::stringstream buf;
        buf << _url;
        for( Parameters::const_iterator i = _parameters.begin(); i != _parameters.end(); i++ )
        {
            buf << ( i == _parameters.begin() && _url.find('?') == std::string::npos? "?" : "&" );
            buf << i->first << "=" << i->second;
        }
         std::string bufStr;
         bufStr = buf.str();
        return bufStr;
    }
}

/****************************************************************************/

HTTPResponse::HTTPResponse( long _code ) :
_response_code( _code ),
_canceled(false),
_duration_s(0.0),
_lastModified(0u),
_fromCache(false)
{
    _parts.reserve(1);
}

HTTPResponse::HTTPResponse( const HTTPResponse& rhs ) :
_response_code( rhs._response_code ),
_parts( rhs._parts ),
_mimeType( rhs._mimeType ),
_canceled( rhs._canceled ),
_duration_s(0.0),
_lastModified(0u),
_fromCache(rhs._fromCache)
{
    //nop
}

unsigned
HTTPResponse::getCode() const {
    return _response_code;
}

unsigned
HTTPResponse::getCodeCategory() const 
{
    return
        getCode() < 100 ? CATEGORY_UNKNOWN :
        getCode() < 200 ? CATEGORY_INFORMATIONAL :
        getCode() < 300 ? CATEGORY_SUCCESS :
        getCode() < 400 ? CATEGORY_REDIRECTION :
        getCode() < 500 ? CATEGORY_CLIENT_ERROR :
                          CATEGORY_SERVER_ERROR;
}

bool
HTTPResponse::isOK() const {
    return _response_code == 200L && !isCanceled();
}

unsigned int
HTTPResponse::getNumParts() const {
    return _parts.size();
}

unsigned int
HTTPResponse::getPartSize( unsigned int n ) const {
    return _parts[n]->_size;
}

const std::string&
HTTPResponse::getPartHeader( unsigned int n, const std::string& name ) const {
    return _parts[n]->_headers[name];
}

std::istream&
HTTPResponse::getPartStream( unsigned int n ) const {
    return _parts[n]->_stream;
}

std::string
HTTPResponse::getPartAsString( unsigned int n ) const {
    std::string streamStr;
    if (n < _parts.size())
        streamStr = _parts[n]->_stream.str();
    return streamStr;
}

const std::string&
HTTPResponse::getMimeType() const {
    return _mimeType;
}

Config
HTTPResponse::getHeadersAsConfig() const
{
    Config conf("HTTP Response Headers");
    if ( _parts.size() > 0 )
    {
        for(auto& header : _parts[0]->_headers)
            conf.add(header.first, header.second);
    }
    return conf;
}

void
HTTPResponse::setHeadersFromConfig(const Config& conf)
{
    if (_parts.size() > 0)
    {
        for (auto& i : conf.children())
        {
            _parts[0]->_headers[i.key()] = i.value();
        }
    }
}

/****************************************************************************/

#define QUOTE_(X) #X
#define QUOTE(X) QUOTE_(X)
#define USER_AGENT "osgearth" QUOTE(OSGEARTH_MAJOR_VERSION) "." QUOTE(OSGEARTH_MINOR_VERSION)


namespace
{
    static optional<ProxySettings>     s_proxySettings;

    static std::string                 s_userAgent = USER_AGENT;

    static long                        s_timeout = 0;
    static long                        s_connectTimeout = 0;
    static float                       s_retryDelay_s = 0.5f;

    // HTTP debugging.
    static bool                        s_HTTP_DEBUG = false;
    static std::mutex                  s_HTTP_DEBUG_mutex;
    static int                         s_HTTP_DEBUG_request_count;
    static double                      s_HTTP_DEBUG_total_duration;

    static osg::ref_ptr< URLRewriter > s_rewriter;

    static osg::ref_ptr< ConfigHandler > s_curlConfigHandler;
}

//.........................................................................

namespace
{
    class CURLImplementation : public HTTPClient::Implementation
    {
    public:
        CURLImplementation() : _curl_handle(0), _previousHttpAuthentication(0) { }

        void initialize()
        {
            _previousHttpAuthentication = 0L;

            _curl_handle = curl_easy_init();

            curl_easy_setopt( _curl_handle, CURLOPT_WRITEFUNCTION, StreamObjectReadCallback );
            curl_easy_setopt( _curl_handle, CURLOPT_HEADERFUNCTION, StreamObjectHeaderCallback );
            curl_easy_setopt( _curl_handle, CURLOPT_FOLLOWLOCATION, (void*)1 );
            curl_easy_setopt( _curl_handle, CURLOPT_MAXREDIRS, (void*)5 );
            //curl_easy_setopt( _curl_handle, CURLOPT_PROGRESSFUNCTION, &CurlProgressCallback);
            curl_easy_setopt( _curl_handle, CURLOPT_XFERINFOFUNCTION, &CurlProgressCallback);
            curl_easy_setopt( _curl_handle, CURLOPT_NOPROGRESS, (void*)0 ); //0=enable.
            curl_easy_setopt( _curl_handle, CURLOPT_FILETIME, true );

#ifdef OE_CURL_SHARE
            curl_easy_setopt( _curl_handle, CURLOPT_SHARE, CURL_SHARE);
#endif

            // Enable automatic CURL decompression of known types. An empty string will automatically add all supported encoding types that are built into curl.
            // Note that you must have curl built against zlib to support gzip or deflate encoding.
            curl_easy_setopt( _curl_handle, CURLOPT_ENCODING, "");

            osg::ref_ptr< ConfigHandler > curlConfigHandler = HTTPClient::getConfigHandler();
            if (curlConfigHandler.valid()) {
                curlConfigHandler->onInitialize(_curl_handle);
            }
        }

        ~CURLImplementation()
        {
            if (_curl_handle)
                curl_easy_cleanup( _curl_handle );
            _curl_handle = 0;
        }

        HTTPResponse doGet(
            const HTTPRequest&    request,
            const osgDB::Options* options,
            ProgressCallback*     progress ) const
        {            
            OE_START_TIMER(http_get);

            std::string url = request.getURL();

            const osgDB::AuthenticationMap* authenticationMap = (options && options->getAuthenticationMap()) ?
                options->getAuthenticationMap() :
                osgDB::Registry::instance()->getAuthenticationMap();

            std::string proxy_host;
            std::string proxy_port = "8080";
            std::string proxy_auth;

            //TODO: don't do all this proxy setup on every GET. Just do it once per client, or only when
            // the proxy information changes.

            //Try to get the proxy settings from the global settings
            if (s_proxySettings.isSet())
            {
                proxy_host = s_proxySettings.get().hostName();
                std::stringstream buf;
                buf << s_proxySettings.get().port();
                proxy_port = buf.str();

                std::string proxy_username = s_proxySettings.get().userName();
                std::string proxy_password = s_proxySettings.get().password();
                if (!proxy_username.empty() && !proxy_password.empty())
                {
                    proxy_auth = proxy_username + std::string(":") + proxy_password;
                }
            }

            //Try to get the proxy settings from the local options that are passed in.
            readOptions( options, proxy_host, proxy_port );

            optional< ProxySettings > proxySettings;
            ProxySettings::fromOptions( options, proxySettings );
            if (proxySettings.isSet())
            {
                proxy_host = proxySettings.get().hostName();
                proxy_port = std::to_string(proxySettings.get().port());
                OE_TEST << LC << "Read proxy settings from options " << proxy_host << " " << proxy_port << std::endl;
            }

            //Try to get the proxy settings from the environment variable
            const char* proxyEnvAddress = getenv("OSG_CURL_PROXY");
            if (proxyEnvAddress) //Env Proxy Settings
            {
                proxy_host = std::string(proxyEnvAddress);

                const char* proxyEnvPort = getenv("OSG_CURL_PROXYPORT"); //Searching Proxy Port on Env
                if (proxyEnvPort)
                {
                    proxy_port = std::string( proxyEnvPort );
                }
            }

            const char* proxyEnvAuth = getenv("OSGEARTH_CURL_PROXYAUTH");
            if (proxyEnvAuth)
            {
                proxy_auth = std::string(proxyEnvAuth);
            }

            // Set up proxy server:
            std::string proxy_addr;
            if ( !proxy_host.empty() )
            {
                std::stringstream buf;
                buf << proxy_host << ":" << proxy_port;
                std::string bufStr;
                bufStr = buf.str();
                proxy_addr = bufStr;

                if ( s_HTTP_DEBUG )
                {
                    OE_NOTICE << LC << "Using proxy: " << proxy_addr << std::endl;
                }

                //curl_easy_setopt( _curl_handle, CURLOPT_HTTPPROXYTUNNEL, 1 );
                curl_easy_setopt( _curl_handle, CURLOPT_PROXY, proxy_addr.c_str() );

                //Setup the proxy authentication if setup
                if (!proxy_auth.empty())
                {
                    if ( s_HTTP_DEBUG )
                    {
                        OE_NOTICE << LC << "Using proxy authentication " << proxy_auth << std::endl;
                    }

                    curl_easy_setopt( _curl_handle, CURLOPT_PROXYUSERPWD, proxy_auth.c_str());
                }
            }
            else
            {
                curl_easy_setopt( _curl_handle, CURLOPT_PROXY, 0 );
            }

            // Rewrite the url if the url rewriter is available
            osg::ref_ptr< URLRewriter > rewriter = HTTPClient::getURLRewriter();
            if ( rewriter.valid() )
            {
                std::string oldURL = url;
                url = rewriter->rewrite( oldURL );
                OE_TEST << LC << "Rewrote URL " << oldURL << " to " << url << std::endl;
            }

            const osgDB::AuthenticationDetails* details = authenticationMap ?
                authenticationMap->getAuthenticationDetails( url ) :
                0;

            if (details)
            {
                const std::string colon(":");
                std::string password(details->username + colon + details->password);
                curl_easy_setopt(_curl_handle, CURLOPT_USERPWD, password.c_str());
                _previousPassword = password;

                // use for https.
                // curl_easy_setopt(_curl, CURLOPT_KEYPASSWD, password.c_str());

#if LIBCURL_VERSION_NUM >= 0x070a07
                if (details->httpAuthentication != _previousHttpAuthentication)
                {
                    curl_easy_setopt(_curl_handle, CURLOPT_HTTPAUTH, details->httpAuthentication);
                    _previousHttpAuthentication = details->httpAuthentication;
                }
#endif
            }
            else
            {
                if (!_previousPassword.empty())
                {
                    curl_easy_setopt(_curl_handle, CURLOPT_USERPWD, 0);
                    _previousPassword.clear();
                }

#if LIBCURL_VERSION_NUM >= 0x070a07
                // need to reset if previously set.
                if (_previousHttpAuthentication!=0)
                {
                    curl_easy_setopt(_curl_handle, CURLOPT_HTTPAUTH, 0);
                    _previousHttpAuthentication = 0;
                }
#endif
            }


            // Set any headers
            struct curl_slist *headers=NULL;
            if (!request.getHeaders().empty())
            {
                for (HTTPRequest::Parameters::const_iterator itr = request.getHeaders().begin(); itr != request.getHeaders().end(); ++itr)
                {
                    std::stringstream buf;
                    buf << osgEarth::toLower(itr->first) << ": " << itr->second;
                    headers = curl_slist_append(headers, buf.str().c_str());
                }
            }

            // Disable the default Pragma: no-cache that curl adds by default.
            headers = curl_slist_append(headers, "pragma: ");
            curl_easy_setopt(_curl_handle, CURLOPT_HTTPHEADER, headers);

            osg::ref_ptr<HTTPResponse::Part> part = new HTTPResponse::Part();
            StreamObject sp( &part->_stream );

            //Take a temporary ref to the callback (why? dangerous.)
            //osg::ref_ptr<ProgressCallback> progressCallback = callback;
            curl_easy_setopt( _curl_handle, CURLOPT_URL, url.c_str() );
            if (progress)
            {
                curl_easy_setopt(_curl_handle, CURLOPT_PROGRESSDATA, progress);
            }

            CURLcode res;
            long response_code = 0L;

            OE_START_TIMER(get_duration);

            //if ( _simResponseCode < 0 )
            //{
            char errorBuf[CURL_ERROR_SIZE];
            errorBuf[0] = 0;
            curl_easy_setopt( _curl_handle, CURLOPT_ERRORBUFFER, (void*)errorBuf );
            curl_easy_setopt( _curl_handle, CURLOPT_WRITEDATA, (void*)&sp);
            curl_easy_setopt( _curl_handle, CURLOPT_HEADERDATA, (void*)&sp);

            //Disable peer certificate verification to allow us to access in https servers where the peer certificate cannot be verified.
            curl_easy_setopt( _curl_handle, CURLOPT_SSL_VERIFYPEER, (void*)0 );

            osg::ref_ptr< ConfigHandler > configHandler = HTTPClient::getConfigHandler();
            if (configHandler.valid()) {
                configHandler->onGet(_curl_handle);
            }

            res = curl_easy_perform(_curl_handle);

            curl_easy_setopt( _curl_handle, CURLOPT_WRITEDATA, (void*)0 );
            curl_easy_setopt( _curl_handle, CURLOPT_PROGRESSDATA, (void*)0);

            // check for cancel or timeout:
            if (res == CURLE_ABORTED_BY_CALLBACK || res == CURLE_OPERATION_TIMEDOUT)
            {
                // CURLE_ABORTED_BY_CALLBACK means ProgressCallback cancelation.
                HTTPResponse response;
                response.setCanceled(true);
                response.setMessage(std::string(curl_easy_strerror(res)));
                return response;
            }

            if (!proxy_addr.empty())
            {
                long connect_code = 0L;
                CURLcode r = curl_easy_getinfo(_curl_handle, CURLINFO_HTTP_CONNECTCODE, &connect_code);
                if ( r != CURLE_OK )
                {
                    std::string msg = "Proxy connect error   " + std::string(curl_easy_strerror(r));
                    OE_WARN << LC << msg << std::endl;

                    // Free the headers
                    if (headers)
                    {
                        curl_slist_free_all(headers);
                    }

                    HTTPResponse response(0);
                    response.setMessage(msg);
                    return response;
                }
            }

            curl_easy_getinfo( _curl_handle, CURLINFO_RESPONSE_CODE, &response_code );

            if (s_simResponseCode > 0)
            {
                unsigned hash = std::hash<double>()(osg::Timer::instance()->tick()) % 10;
                if (hash == 0)
                    response_code = s_simResponseCode;
            }

            HTTPResponse response( response_code );



            // read the response content type:
            char* content_type_cp;

            curl_easy_getinfo( _curl_handle, CURLINFO_CONTENT_TYPE, &content_type_cp );

            if ( content_type_cp != NULL )
            {
                response.setMimeType(content_type_cp);
            }

            // read the file time:
            response.setLastModified(getCurlFileTime( _curl_handle ));

            if (res == CURLE_OK)
            {
                // check for multipart content
                if (response.getMimeType().length() > 9 &&
                    ::strstr( response.getMimeType().c_str(), "multipart" ) == response.getMimeType().c_str() )
                {
                    OE_TEST << LC << "detected multipart data; decoding..." << std::endl;

                    //TODO: parse out the "wcs" -- this is WCS-specific
                    if ( !decodeMultipartStream( "wcs", part.get(), response.getParts() ) )
                    {
                        // error decoding an invalid multipart stream.
                        // should we do anything, or just leave the response empty?
                    }
                }
                else
                {
                    for (Headers::iterator itr = sp._headers.begin(); itr != sp._headers.end(); ++itr)
                    {
                        part->_headers[Strings::trim(itr->first)] = Strings::trim(itr->second);
                    }

                    // Write the headers to the metadata
                    response.getParts().push_back( part.get() );
                }
            }

            else
            {
                response.setMessage(curl_easy_strerror(res));

                if (res == CURLE_GOT_NOTHING)
                {
                    OE_TEST << LC << "CURLE_GOT_NOTHING for " << url << std::endl;
                }
            }

            response.setDuration(OE_STOP_TIMER(get_duration));

            if ( s_HTTP_DEBUG )
            {
                TimeStamp filetime = getCurlFileTime(_curl_handle);

                OE_NOTICE << LC
                    << "GET(" << response_code << ") " << response.getMimeType() << ": \""
                    << url << "\" (" << DateTime(filetime).asRFC1123() << ") t="
                    << std::setprecision(4) << response.getDuration() << "s" << std::endl;

                for(HTTPRequest::Parameters::const_iterator itr = request.getHeaders().begin();
                    itr != request.getHeaders().end(); 
                    ++itr)
                {
                    OE_NOTICE << LC << "    Header: " << itr->first << " = " << itr->second << std::endl;
                }

                {
                    std::lock_guard<std::mutex> lock(s_HTTP_DEBUG_mutex);
                    s_HTTP_DEBUG_request_count++;
                    s_HTTP_DEBUG_total_duration += response.getDuration();

                    if ( s_HTTP_DEBUG_request_count % 60 == 0 )
                    {
                        OE_NOTICE << LC << "Average duration = " << s_HTTP_DEBUG_total_duration/(double)s_HTTP_DEBUG_request_count
                            << std::endl;
                    }
                }

#if 0
                // time details - almost 100% of the time is spent in
                // STARTTRANSFER, which is the time until the first byte is received.
                double td[7];

                curl_easy_getinfo(_curl_handle, CURLINFO_TOTAL_TIME,         &td[0]);
                curl_easy_getinfo(_curl_handle, CURLINFO_NAMELOOKUP_TIME,    &td[1]);
                curl_easy_getinfo(_curl_handle, CURLINFO_CONNECT_TIME,       &td[2]);
                curl_easy_getinfo(_curl_handle, CURLINFO_APPCONNECT_TIME,    &td[3]);
                curl_easy_getinfo(_curl_handle, CURLINFO_PRETRANSFER_TIME,   &td[4]);
                curl_easy_getinfo(_curl_handle, CURLINFO_STARTTRANSFER_TIME, &td[5]);
                curl_easy_getinfo(_curl_handle, CURLINFO_REDIRECT_TIME,      &td[6]);

                for(int i=0; i<7; ++i)
                {
                    OE_NOTICE << LC
                        << std::setprecision(4)
                        << "TIMES: total=" <<td[0]
                        << ", lookup=" <<td[1]<<" ("<<(int)((td[1]/td[0])*100)<<"%)"
                        << ", connect=" <<td[2]<<" ("<<(int)((td[2]/td[0])*100)<<"%)"
                        << ", appconn=" <<td[3]<<" ("<<(int)((td[3]/td[0])*100)<<"%)"
                        << ", prexfer=" <<td[4]<<" ("<<(int)((td[4]/td[0])*100)<<"%)"
                        << ", startxfer=" <<td[5]<<" ("<<(int)((td[5]/td[0])*100)<<"%)"
                        << ", redir=" <<td[6]<<" ("<<(int)((td[6]/td[0])*100)<<"%)"
                        << std::endl;
                }
#endif
            }

            // Free the headers
            if (headers)
            {
                curl_slist_free_all(headers);
            }            

            return response;
        }
        
        void* getHandle() const
        {
            return _curl_handle;
        }

        void setUserAgent(const std::string& value)
        {
            curl_easy_setopt( _curl_handle, CURLOPT_USERAGENT, value.c_str() );
        }

        void setTimeout(long value)
        {
            curl_easy_setopt( _curl_handle, CURLOPT_TIMEOUT, value );
        }

        void setConnectTimeout(long value)
        {
            curl_easy_setopt( _curl_handle, CURLOPT_CONNECTTIMEOUT, value );
        }

        void readOptions(const osgDB::Options* options, std::string& proxy_host, std::string& proxy_port) const
        {
            // try to set proxy host/port by reading the CURL proxy options
            if ( options )
            {
                std::istringstream iss( options->getOptionString() );
                std::string opt;
                while( iss >> opt )
                {
                    int index = opt.find('=');
                    if( opt.substr( 0, index ) == "OSG_CURL_PROXY" )
                    {
                        proxy_host = opt.substr( index+1 );
                    }
                    else if ( opt.substr( 0, index ) == "OSG_CURL_PROXYPORT" )
                    {
                        proxy_port = opt.substr( index+1 );
                    }
                }
            }
        }

    private:
        void* _curl_handle;
        mutable std::string _previousPassword;
        mutable long _previousHttpAuthentication;
    };
}

HTTPClient::Implementation*
CURLHTTPImplementationFactory::create() const
{
    return new CURLImplementation();
}

#ifdef OSGEARTH_USE_WININET_FOR_HTTP
namespace
{
    std::string GetLastErrorAsString()
    {
        //Get the error message, if any.
        DWORD errorMessageID = ::GetLastError();
        if(errorMessageID == 0)
            return std::string("Error Code 0"); //No error message has been recorded

        LPSTR messageBuffer = 0L;
        size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);

        std::string message(messageBuffer, size);

        //Free the buffer.
        LocalFree(messageBuffer);

        message = Stringify() << "[Code " << errorMessageID << "] " << message;

        return message;
    }

    class WinInetImplementation : public HTTPClient::Implementation
    {
    public:
        WinInetImplementation() { }

        void initialize()
        {
            osg::ref_ptr< ConfigHandler > configHandler = HTTPClient::getConfigHandler();
            if (configHandler.valid()) {
                configHandler->onInitialize(NULL);
            }
        }

        ~WinInetImplementation()
        {
        }

        HTTPResponse doGet(const HTTPRequest&    request,
                           const osgDB::Options* options,
                           ProgressCallback*     progress) const
        {    
            OE_START_TIMER(http_get);

            std::string url = request.getURL();

            // Rewrite the url if the url rewriter is available
            osg::ref_ptr< URLRewriter > rewriter = HTTPClient::getURLRewriter();
            if ( rewriter.valid() )
            {
                std::string oldURL = url;
                url = rewriter->rewrite( oldURL );
                OE_TEST << LC << "Rewrote URL " << oldURL << " to " << url << std::endl;
            }

            HINTERNET hInternet = InternetOpen(
                _userAgent.c_str(),
                //"Mozilla/5.0 (Windows NT 6.1; WOW64; Trident/7.0; AS; rv:11.0) like Gecko",
                INTERNET_OPEN_TYPE_PRECONFIG,
                NULL,       // proxy
                NULL,       // proxy bypass
                0);         // flags

            if( !hInternet )
            {
                OE_WARN << LC << "InternetOpen failed: " << GetLastErrorAsString() << std::endl;
                return HTTPResponse(0);
            }

            // clears any session cookies..?
            // Commented this out, because is invalidates authorization caching:
            //InternetSetOption( 0, INTERNET_OPTION_END_BROWSER_SESSION, NULL, 0 );

            // parse the URL:
            URL_COMPONENTS urlcomp;
            ZeroMemory(&urlcomp, sizeof(urlcomp));
            urlcomp.dwStructSize = sizeof(urlcomp);
            urlcomp.dwHostNameLength = 1;
            urlcomp.dwUserNameLength = 1;
            urlcomp.dwPasswordLength = 1;
            urlcomp.dwUrlPathLength  = 1;

            if ( !InternetCrackUrl(url.c_str(), 0, 0L, &urlcomp) )
            {
                OE_WARN << LC << "InternetCrackUrl failed for " << url << ": " << GetLastErrorAsString() << std::endl;
                InternetCloseHandle( hInternet );
                return HTTPResponse(0);
            }

            WORD port =
                urlcomp.nPort != 0 ? urlcomp.nPort :
                urlcomp.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_DEFAULT_HTTPS_PORT :
                INTERNET_DEFAULT_HTTP_PORT;

            char hostName[128];
            char urlPath[256];
            memset(urlPath, 0, sizeof(urlPath));
            memset(hostName, 0, sizeof(hostName));
            strncpy(hostName, urlcomp.lpszHostName, urlcomp.dwHostNameLength);
            strncpy(urlPath, urlcomp.lpszUrlPath, urlcomp.dwUrlPathLength);

            OE_TEST
                << "\n"
                << "Host name = " << hostName << "\n"
                << "Url path = " << urlcomp.lpszUrlPath << "\n"
                << "Port = " << port << "\n";

            DWORD openFlags =
                //INTERNET_FLAG_PRAGMA_NOCACHE |
                INTERNET_FLAG_RELOAD |
                INTERNET_FLAG_NO_CACHE_WRITE |
                INTERNET_FLAG_KEEP_CONNECTION;

            if ( urlcomp.nScheme == INTERNET_SCHEME_HTTPS)
            {
                openFlags |= INTERNET_FLAG_SECURE;
            }

            // InternetOpenUrl is a lot less code, but we have to use the InternetConnnect +
            // HttpOpenRequest + HttpSendRequest approach in order to support system dialogs
            // for PKI certificates and username/password queries.

            HINTERNET hConnection = InternetConnect(
                hInternet,
                hostName,
                port,
                "", // username
                "", // password
                INTERNET_SERVICE_HTTP,
                0,  // flags
                INTERNET_NO_CALLBACK); // context

            if ( !hConnection )
            {
                OE_WARN << LC << "InternetConnect failed for " << url << ": " << GetLastErrorAsString() << std::endl;
                InternetCloseHandle( hInternet );
                return HTTPResponse(0);
            }

            // Enable automatic HTTP decompression of known types.
            BOOL enableDecompression = TRUE;
            InternetSetOption(hConnection, INTERNET_OPTION_HTTP_DECODING, &enableDecompression, sizeof(enableDecompression));

            HINTERNET hRequest = HttpOpenRequest(
                hConnection,            // handle from InternetConnect
                "GET",                  // verb
                urlPath,                // path
                NULL,                   // HTTP version (NULL = default)
                NULL,                   // Referrer
                NULL,                   // Accept types
                openFlags,              // flags
                INTERNET_NO_CALLBACK);  // context (user data)

            if ( !hRequest )
            {
                OE_WARN << LC << "HttpOpenRequest failed for " << url << ": " << GetLastErrorAsString() << std::endl;
                InternetCloseHandle( hConnection );
                InternetCloseHandle( hInternet );
                return HTTPResponse(0);
            }

            const char* headers = "Accept-Encoding: gzip, deflate";
            while( !HttpSendRequest(hRequest, headers, strlen(headers), NULL, 0) )
            {
                DWORD errorNum = GetLastError();

                // Request for client cert; open system dialog.
                if ( errorNum == ERROR_INTERNET_CLIENT_AUTH_CERT_NEEDED )
                {
                    OE_WARN << LC << "Server reports ERROR_INTERNET_CLIENT_AUTH_CERT_NEEDED!\n";

                    // Return ERROR_SUCCESS regardless of clicking on OK or Cancel
                    DWORD dialogResult = InternetErrorDlg(
                        GetDesktopWindow(),
                        hRequest,
                        ERROR_INTERNET_CLIENT_AUTH_CERT_NEEDED,
                        FLAGS_ERROR_UI_FILTER_FOR_ERRORS | FLAGS_ERROR_UI_FLAGS_GENERATE_DATA | FLAGS_ERROR_UI_FLAGS_CHANGE_OPTIONS,
                        NULL );

                    if ( dialogResult != ERROR_SUCCESS )
                    {
                        OE_WARN << LC << "InternetErrorDlg failed to produce client cert " << url << ": " << GetLastErrorAsString() << std::endl;
                        InternetCloseHandle( hRequest );
                        InternetCloseHandle( hConnection );
                        InternetCloseHandle( hInternet );
                        return HTTPResponse(0);
                    }
                }

                else
                {
                    OE_WARN << LC << "HttpSendRequest failed to open " << url << ": " << GetLastErrorAsString() << std::endl;
                    InternetCloseHandle( hRequest );
                    InternetCloseHandle( hConnection );
                    InternetCloseHandle( hInternet );
                    return HTTPResponse(0);
                }
            }

            int statusCode = 0;

            char  buffer[4096];
            DWORD bufferLen = 4096;
            DWORD index = 0;

            if ( HttpQueryInfo(hRequest, HTTP_QUERY_STATUS_CODE, buffer, &bufferLen, &index) )
            {
                statusCode = as<int>( std::string(buffer, bufferLen), 0 );
            }

            HTTPResponse response(statusCode);

            bufferLen = 4096, index = 0;
            if ( HttpQueryInfo(hRequest, HTTP_QUERY_CONTENT_TYPE, buffer, &bufferLen, &index) )
            {
                response.setMimeType(std::string(buffer, bufferLen));
            }

            bufferLen = 4096, index = 0;
            if ( HttpQueryInfo(hRequest, HTTP_QUERY_LAST_MODIFIED, buffer, &bufferLen, &index) )
            {
                DateTime dt(std::string(buffer, bufferLen));
                response.setLastModified(dt.asTimeStamp());
            }

            if ( statusCode == 200 )
            {
                osg::ref_ptr<HTTPResponse::Part> part = new HTTPResponse::Part();

                DWORD numBytesRead = 0;
                while( InternetReadFile(hRequest, buffer, 4096, &numBytesRead) && numBytesRead )
                {
                    part->_stream << std::string(buffer, numBytesRead);
                }

                response.getParts().push_back( part.get() );
            }

            InternetCloseHandle( hRequest );
            InternetCloseHandle( hConnection );
            InternetCloseHandle( hInternet );

            response.setDuration(OE_STOP_TIMER(http_get));

            return response;
        }

        void* getHandle() const
        {
            return NULL;
        }

        void setUserAgent(const std::string& value)
        {
            _userAgent = value;
        }

        void setTimeout(long value)
        {
            //TODO
        }

        void setConnectTimeout(long value)
        {
            //TODO
        }

        void readOptions(const osgDB::Options* options, std::string& proxy_host, std::string& proxy_port) const
        {
            // try to set proxy host/port by reading the CURL proxy options
            if ( options )
            {
                std::istringstream iss( options->getOptionString() );
                std::string opt;
                while( iss >> opt )
                {
                    int index = opt.find('=');
                    if( opt.substr( 0, index ) == "OSG_CURL_PROXY" )
                    {
                        proxy_host = opt.substr( index+1 );
                    }
                    else if ( opt.substr( 0, index ) == "OSG_CURL_PROXYPORT" )
                    {
                        proxy_port = opt.substr( index+1 );
                    }
                }
            }
        }

    private:
        std::string _userAgent;
    };
}
#endif

HTTPClient::Implementation*
WinInetHTTPImplementationFactory::create() const
{
#ifdef OSGEARTH_USE_WININET_FOR_HTTP
    return new WinInetImplementation();
#else
    return new CURLImplementation();
#endif
}

//........................................................................

#ifdef OSGEARTH_USE_WININET_FOR_HTTP
    HTTPClient::ImplementationFactory* HTTPClient::_implFactory = new WinInetHTTPImplementationFactory();
#else
    HTTPClient::ImplementationFactory* HTTPClient::_implFactory = new CURLHTTPImplementationFactory();
#endif

void
HTTPClient::setImplementationFactory(ImplementationFactory* factory)
{
    if (factory != NULL)
    {
        if (_implFactory != NULL)
            delete _implFactory;

        _implFactory = factory;
    }
}

HTTPClient&
HTTPClient::getClient()
{
    static thread_local HTTPClient s_clientPerThread;
    return s_clientPerThread;
}

HTTPClient::HTTPClient()
{
    //do no CURL calls here.
    if (_implFactory)
    {
        _impl = _implFactory->create();
    }
}

void
HTTPClient::initialize() const
{
    if ( !_initialized )
    {
        const_cast<HTTPClient*>(this)->initializeImpl();
    }
}

void
HTTPClient::initializeImpl()
{
    _previousHttpAuthentication = 0;

    //Get the user agent
    std::string userAgent = s_userAgent;
    const char* userAgentEnv = getenv("OSGEARTH_USERAGENT");
    if (userAgentEnv)
    {
        userAgent = std::string(userAgentEnv);
    }
    OE_TEST << LC << "HTTPClient setting userAgent=" << userAgent << std::endl;

    //Check for a response-code simulation (for testing)
    const char* simCode = getenv("OSGEARTH_SIMULATE_HTTP_RESPONSE_CODE");
    if ( simCode )
    {
        s_simResponseCode = osgEarth::as<long>(std::string(simCode), 404L);
        OE_WARN << LC << "Simulating a network error with Response Code = " << _simResponseCode << std::endl;
    }

    // Check to HTTP disabling (for testing)
    const char* disable = getenv("OSGEARTH_HTTP_DISABLE");
    if (disable)
    {
        s_simResponseCode = 500L; // SERVER ERROR
        OE_WARN << LC << "HTTP traffic disabled" << std::endl;
    }

    // Dumps out HTTP request/response info
    if ( ::getenv("OSGEARTH_HTTP_DEBUG") )
    {
        s_HTTP_DEBUG = true;
        OE_INFO << LC << "HTTP debugging enabled" << std::endl;
    }

    long timeout = s_timeout;
    const char* timeoutEnv = getenv("OSGEARTH_HTTP_TIMEOUT");
    if (timeoutEnv)
    {
        timeout = osgEarth::as<long>(std::string(timeoutEnv), 0);
    }
    OE_TEST << LC << "Setting timeout to " << timeout << std::endl;

    long connectTimeout = s_connectTimeout;
    const char* connectTimeoutEnv = getenv("OSGEARTH_HTTP_CONNECTTIMEOUT");
    if (connectTimeoutEnv)
    {
        connectTimeout = osgEarth::as<long>(std::string(connectTimeoutEnv), 0);
    }
    OE_TEST << LC << "Setting connect timeout to " << connectTimeout << std::endl;

    const char* retryDelayEnv = getenv("OSGEARTH_HTTP_RETRY_DELAY");
    if (retryDelayEnv)
    {
        s_retryDelay_s = osgEarth::as<double>(std::string(retryDelayEnv), 0.0);
    }
    OE_TEST << LC << "Setting retry delay to " << s_retryDelay_s << std::endl;

    _impl->initialize();

    _impl->setUserAgent(userAgent.c_str());
    _impl->setTimeout(timeout);
    _impl->setConnectTimeout(connectTimeout);

    _initialized = true;
}

HTTPClient::~HTTPClient()
{
    //nop
}

void
HTTPClient::setProxySettings( const optional<ProxySettings> & proxySettings )
{
    s_proxySettings = proxySettings;
}

const optional<ProxySettings> & 
HTTPClient::getProxySettings()
{
    return s_proxySettings;
}

const std::string& HTTPClient::getUserAgent()
{
    return s_userAgent;
}

void  HTTPClient::setUserAgent(const std::string& userAgent)
{
    s_userAgent = userAgent;
}

long HTTPClient::getTimeout()
{
    return s_timeout;
}

void HTTPClient::setTimeout( long timeout )
{
    s_timeout = timeout;
}

long HTTPClient::getConnectTimeout()
{
    return s_connectTimeout;
}

void HTTPClient::setConnectTimeout( long timeout )
{
    s_connectTimeout = timeout;
}

void HTTPClient::setRetryDelay(float value_s)
{
    s_retryDelay_s = value_s;
}

float HTTPClient::getRetryDelay() 
{
    return s_retryDelay_s;
}

URLRewriter* HTTPClient::getURLRewriter()
{
    return s_rewriter.get();
}

void HTTPClient::setURLRewriter( URLRewriter* rewriter )
{
    s_rewriter = rewriter;
}

ConfigHandler* HTTPClient::getConfigHandler()
{
    return s_curlConfigHandler.get();
}

void HTTPClient::setConfigHandler(ConfigHandler* handler)
{
    s_curlConfigHandler = handler;
}

void
HTTPClient::globalInit()
{
#ifndef OSGEARTH_USE_WININET_FOR_HTTP
    curl_global_init(CURL_GLOBAL_ALL);

#ifdef OE_CURL_SHARE
    CURL_SHARE = curl_share_init();
    curl_share_setopt(CURL_SHARE, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(CURL_SHARE, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(CURL_SHARE, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(CURL_SHARE, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(CURL_SHARE, CURLSHOPT_LOCKFUNC, share_lock_cb);
    curl_share_setopt(CURL_SHARE, CURLSHOPT_UNLOCKFUNC, share_unlock_cb);
#endif

#endif
}

void
HTTPClient::readOptions(const osgDB::Options* options, std::string& proxy_host, std::string& proxy_port) const
{
    // try to set proxy host/port by reading the CURL proxy options
    if ( options )
    {
        std::istringstream iss( options->getOptionString() );
        std::string opt;
        while( iss >> opt )
        {
            int index = opt.find('=');
            if( opt.substr( 0, index ) == "OSG_CURL_PROXY" )
            {
                proxy_host = opt.substr( index+1 );
            }
            else if ( opt.substr( 0, index ) == "OSG_CURL_PROXYPORT" )
            {
                proxy_port = opt.substr( index+1 );
            }
        }
    }
}

HTTPResponse
HTTPClient::get( const HTTPRequest&    request,
                 const osgDB::Options* options,
                 ProgressCallback*     progress)
{
    return getClient().doGet( request, options, progress );
}

HTTPResponse
HTTPClient::get( const std::string&    url,
                 const osgDB::Options* options,
                 ProgressCallback*     progress)
{
    return getClient().doGet( url, options, progress);
}

ReadResult
HTTPClient::readImage(const HTTPRequest&    request,
                      const osgDB::Options* options,
                      ProgressCallback*     progress)
{
    return getClient().doReadImage( request, options, progress );
}

ReadResult
HTTPClient::readNode(const HTTPRequest&    request,
                     const osgDB::Options* options,
                     ProgressCallback*     progress)
{
    return getClient().doReadNode( request, options, progress );
}

ReadResult
HTTPClient::readObject(const HTTPRequest&    request,
                       const osgDB::Options* options,
                       ProgressCallback*     progress)
{
    return getClient().doReadObject( request, options, progress );
}

ReadResult
HTTPClient::readString(const HTTPRequest&    request,
                       const osgDB::Options* options,
                       ProgressCallback*     progress)
{
    return getClient().doReadString( request, options, progress );
}

bool
HTTPClient::download(const std::string& uri,
                     const std::string& localPath)
{
    return getClient().doDownload( uri, localPath );
}

HTTPResponse
HTTPClient::doGet(const HTTPRequest&    request,
                  const osgDB::Options* options,
                  ProgressCallback*     progress) const
{
    OE_PROFILING_ZONE;
    OE_PROFILING_ZONE_TEXT(Stringify() << "url " << request.getURL());

#ifdef OSGEARTH_HAVE_SUPERLUMINALAPI
    PERFORMANCEAPI_INSTRUMENT_FUNCTION();
    PERFORMANCEAPI_INSTRUMENT_DATA("url", request.getURL().c_str());
#endif

    initialize();

    URI uri(request.getURL());

    // URL caching
    CacheBin* bin = nullptr;
    CacheSettings* cacheSettings = CacheSettings::get(options);
    osgEarth::optional<CachePolicy> cachePolicy;
    if (cacheSettings)
    {
        cachePolicy = cacheSettings->cachePolicy();
        if (cacheSettings->isCacheEnabled())
        {
            // Use the global bin instead of the defined cache bin so all URLs are cached to the same place
            bin = cacheSettings->getCache()->getOrCreateDefaultBin();
            //bin = cacheSettings->getCacheBin();
        }
    }

    bool expired = false;

    HTTPResponse response;

    bool gotFromCache = false;

    //Try to read result from the cache.
    if (bin)
    {
        ReadResult result = bin->readString(uri.cacheKey(), options);
        if (result.succeeded())
        {            
            gotFromCache = true;

            // If the cache-control header contains no-cache that means that it's ok to store the result in the cache, but it must be requested
            // from the server each time it is it requested.
            bool noCache = false;
            std::string cacheControl = result.metadata().value("cache-control");
            if (cacheControl.find("no-cache") != std::string::npos)
            {
                noCache = true;
            }

            expired = noCache || cachePolicy->isExpired(result.lastModifiedTime());
            result.setIsFromCache(true);            

            HTTPResponse cacheResponse(HTTPResponse::CATEGORY_SUCCESS);
            osg::ref_ptr<HTTPResponse::Part> part = new HTTPResponse::Part();
            part->_stream << result.getString();
            std::string contentType = result.metadata().value("content-type");
            cacheResponse.setMimeType(contentType);
            cacheResponse.getParts().push_back(part);
            cacheResponse.setHeadersFromConfig(result.metadata());
            cacheResponse.setFromCache(true);
            response = cacheResponse;
        }
    }

    if ((expired || !gotFromCache) && cachePolicy->usage() != CachePolicy::USAGE_CACHE_ONLY)
    {
        HTTPResponse remoteResponse = _impl->doGet(request, options, progress);

        if (remoteResponse.getCode() == ReadResult::RESULT_NOT_MODIFIED)
        {
            // Touch the cached item to update it's last modified timestamp so it doesn't expire again immediately.
            if (bin)
                bin->touch(uri.cacheKey());
        }
        else
        {
            response = remoteResponse;

            if (response.isOK())
            {
                if (bin != nullptr)
                {
                    osg::ref_ptr< StringObject> stringObject = new StringObject(response.getPartAsString(0));
                    bin->write(uri.cacheKey(), stringObject, response.getHeadersAsConfig(), options);
                }
            }
        }

        OE_PROFILING_ZONE_TEXT(Stringify() << "response_code " << response.getCode());
        if (response.isCanceled())
        {
            OE_PROFILING_ZONE_TEXT("cancelled");
        }        
    }
    return response;
}

bool
HTTPClient::doDownload(const std::string& url, const std::string& filename)
{
    initialize();

    // download the data
    HTTPResponse response = this->doGet( HTTPRequest(url) );

    if ( response.isOK() )
    {
        if (response.getNumParts() < 1)
            return false;

        unsigned int part_num = response.getNumParts() > 1? 1 : 0;
        std::istream& input_stream = response.getPartStream( part_num );

        std::ofstream fout;
        fout.open(filename.c_str(), std::ios::out | std::ios::binary);

        input_stream.seekg (0, std::ios::end);
        int length = input_stream.tellg();
        input_stream.seekg (0, std::ios::beg);

        char *buffer = new char[length];
        input_stream.read(buffer, length);
        fout.write(buffer, length);
        delete[] buffer;
        fout.close();
        return true;
    }
    else
    {
        OE_WARN << LC << "Error downloading file " << filename
            << " (" << response.getCode() << ")" << std::endl;
        return false;
    }
}

namespace
{
    osgDB::ReaderWriter*
    getReader( const std::string& url, const HTTPResponse& response )
    {
        osgDB::ReaderWriter* reader = 0L;
        std::string ext;

        if (response.getNumParts() > 0)
        {
            reader = ImageUtils::getReaderWriterForString(response.getPartAsString(0));
        }

        if (!reader)
        {
            // try extension first:
            std::string urlMinusQueryParams = removeQueryParams(url);
            ext = osgDB::getFileExtension(urlMinusQueryParams);
            if (!ext.empty())
            {
                reader = osgDB::Registry::instance()->getReaderWriterForExtension(ext);
            }
        }

        if ( !reader )
        {
            // try to look up a reader by mime-type:
            const std::string& mimeType = response.getMimeType();
            if ( !mimeType.empty() )
            {
                reader = osgDB::Registry::instance()->getReaderWriterForMimeType(mimeType);
            }
        }

        if ( !reader && s_HTTP_DEBUG )
        {
            OE_WARN << LC << "Cannot find an OSG plugin to read response data (ext="
                << ext << "; mime-type=" << response.getMimeType()
                << ")" << std::endl;

            if (Strings::ciEndsWith(response.getMimeType(), "xml") && response.getNumParts() > 0 )
            {
                OE_WARN << LC << "Content:\n" << response.getPartAsString(0) << "\n";
            }

            if (Strings::ciEndsWith(response.getMimeType(), "html") && response.getNumParts() > 0)
            {
                OE_WARN << LC << "Content:\n" << response.getPartAsString(0) << "\n";
            }
        }

        if (!reader)
        {
            OE_WARN << "Unhappy!" << std::endl;
        }

        return reader;
    }
}

ReadResult
HTTPClient::doReadImage(const HTTPRequest&    request,
                        const osgDB::Options* options,
                        ProgressCallback*     callback)
{
    initialize();

    ReadResult result;

    HTTPResponse response = this->doGet(request, options, callback);

    if (response.isOK())
    {
        osgDB::ReaderWriter* reader = getReader(request.getURL(), response);
        if (!reader)
        {
            result = ReadResult(ReadResult::RESULT_NO_READER);
            result.setErrorDetail("Content-Type=" + response.getMimeType());
        }

        else
        {
            osgDB::ReaderWriter::ReadResult rr;

            if (response.getNumParts() > 0)
                rr = reader->readImage(response.getPartStream(0), options);

            if ( rr.validImage() )
            {
                result = ReadResult(rr.takeImage());
            }
            else
            {
                if ( s_HTTP_DEBUG )
                {
                    OE_WARN << LC << reader->className()
                        << " failed to read image from " << request.getURL()
                        << "; message = " << rr.message()
                        <<  std::endl;
                }
                result = ReadResult(ReadResult::RESULT_READER_ERROR);
                result.setErrorDetail( rr.message() );
            }
        }

        // last-modified (file time)
        result.setLastModifiedTime( response._lastModified );

        // Time of query
        result.setDuration( response.getDuration() );
    }
    else
    {
        result = ReadResult(
            response.isCanceled() ? ReadResult::RESULT_CANCELED :
            response.getCode() == HTTPResponse::NOT_FOUND ? ReadResult::RESULT_NOT_FOUND :
            response.getCode() == HTTPResponse::NOT_MODIFIED ? ReadResult::RESULT_NOT_MODIFIED :
            response.getCode() == HTTPResponse::FORBIDDEN ? ReadResult::RESULT_UNAUTHORIZED :
            response.getCodeCategory() == HTTPResponse::CATEGORY_SERVER_ERROR ? ReadResult::RESULT_SERVER_ERROR :
            ReadResult::RESULT_UNKNOWN_ERROR);

        // for request errors, return an error result with the part data intact
        // so the user can parse it as needed. We only do this for readString.
        if (response.getNumParts() > 0u)
        {
            result.setErrorDetail(response.getPartAsString(0));

            if (s_HTTP_DEBUG)
            {
                OE_WARN << LC << "SERVER REPORTS: " << result.errorDetail() << std::endl;
            }
        }

        //If we have an error but it's recoverable, like a server error or timeout then set the callback to retry.
        if (HTTPClient::isRecoverable( result.code() ) )
        {
            if (callback)
            {
                callback->setRetryDelay(getRetryDelay());
                callback->cancel();

                if (response.getCode() == 503)
                {
                    callback->message() = "Server deferral";
                }

                if ( s_HTTP_DEBUG )
                {
                    if (response.isCanceled())
                    {
                        OE_NOTICE << LC << "Request was cancelled" << std::endl;
                    }
                    else
                    {
                        OE_NOTICE << LC << "Recoverable error in HTTPClient for " << request.getURL() << std::endl;
                    }
                }
            }
        }
    }

    // encode metadata
    setMetadata(result, request, response);

    result.setIsFromCache(response.getFromCache());

    // set the source name
    if ( result.getImage() )
        result.getImage()->setName( request.getURL() );

    return result;
}

ReadResult
HTTPClient::doReadNode(const HTTPRequest&    request,
                       const osgDB::Options* options,
                       ProgressCallback*     callback)
{
    initialize();

    ReadResult result;

    HTTPResponse response = this->doGet(request, options, callback);

    if (response.isOK())
    {
        osgDB::ReaderWriter* reader = getReader(request.getURL(), response);
        if (!reader)
        {
            result = ReadResult(ReadResult::RESULT_NO_READER);
        }

        else
        {
            osgDB::ReaderWriter::ReadResult rr;
            
            if (response.getNumParts() > 0)
                rr = reader->readNode(response.getPartStream(0), options);

            if ( rr.validNode() )
            {
                result = ReadResult(rr.takeNode());
            }
            else
            {
                if ( s_HTTP_DEBUG )
                {
                    OE_WARN << LC << reader->className()
                        << " failed to read node from " << request.getURL()
                        << "; message = " << rr.message()
                        <<  std::endl;
                }
                result = ReadResult(ReadResult::RESULT_READER_ERROR);
                result.setErrorDetail( rr.message() );
            }
        }

        // last-modified (file time)
        result.setLastModifiedTime( response._lastModified ); //getCurlFileTime(_curl_handle) );
    }
    else
    {
        result = ReadResult(
            response.isCanceled() ? ReadResult::RESULT_CANCELED :
            response.getCode() == HTTPResponse::NOT_FOUND ? ReadResult::RESULT_NOT_FOUND :
            response.getCode() == HTTPResponse::NOT_MODIFIED ? ReadResult::RESULT_NOT_MODIFIED :
            response.getCodeCategory() == HTTPResponse::CATEGORY_SERVER_ERROR ? ReadResult::RESULT_SERVER_ERROR :
            ReadResult::RESULT_UNKNOWN_ERROR);

        // for request errors, return an error result with the part data intact
        // so the user can parse it as needed. We only do this for readString.
        if (response.getNumParts() > 0u)
        {
            result.setErrorDetail(response.getPartAsString(0));

            if (s_HTTP_DEBUG)
            {
                OE_WARN << LC << "SERVER REPORTS: " << result.errorDetail() << std::endl;
            }

            if (s_HTTP_DEBUG)
            {
                OE_WARN << LC << "SERVER REPORTS: " << result.errorDetail() << std::endl;
            }
        }

        //If we have an error but it's recoverable, like a server error or timeout then set the callback to retry.
        if (HTTPClient::isRecoverable( result.code() ) )
        {
            if (callback)
            {
                callback->setRetryDelay(getRetryDelay());
                callback->cancel();

                if ( s_HTTP_DEBUG )
                {
                    if (response.isCanceled())
                    {
                        OE_NOTICE << LC << "Request was cancelled" << std::endl;
                    }
                    else
                    {
                        OE_NOTICE << LC << "Recoverable error in HTTPClient for " << request.getURL() << std::endl;
                    }
                }
            }
        }
    }


    // encode metadata
    setMetadata(result, request, response);

    result.setIsFromCache(response.getFromCache());

    return result;
}

ReadResult
HTTPClient::doReadObject(const HTTPRequest&    request,
                         const osgDB::Options* options,
                         ProgressCallback*     callback)
{
    initialize();

    ReadResult result;

    HTTPResponse response = this->doGet(request, options, callback);

    if (response.isOK())
    {
        osgDB::ReaderWriter* reader = getReader(request.getURL(), response);
        if (!reader)
        {
            result = ReadResult(ReadResult::RESULT_NO_READER);
            result.setErrorDetail(Stringify() << "Content-Type=" << response.getMimeType());
        }

        else
        {
            osgDB::ReaderWriter::ReadResult rr;
            
            if (response.getNumParts() > 0 )
                rr = reader->readObject(response.getPartStream(0), options);

            if ( rr.validObject() )
            {
                result = ReadResult(rr.takeObject());
            }
            else
            {
                if ( s_HTTP_DEBUG )
                {
                    OE_WARN << LC << reader->className()
                        << " failed to read object from " << request.getURL()
                        << "; message = " << rr.message()
                        <<  std::endl;
                }
                result = ReadResult(ReadResult::RESULT_READER_ERROR);
                result.setErrorDetail( rr.message() );
            }
        }

        // last-modified (file time)
        result.setLastModifiedTime( response._lastModified ); //getCurlFileTime(_curl_handle) );
    }
    else
    {
        result = ReadResult(
            response.isCanceled() ? ReadResult::RESULT_CANCELED :
            response.getCode() == HTTPResponse::NOT_FOUND ? ReadResult::RESULT_NOT_FOUND :
            response.getCode() == HTTPResponse::NOT_MODIFIED ? ReadResult::RESULT_NOT_MODIFIED :
            response.getCodeCategory() == HTTPResponse::CATEGORY_SERVER_ERROR ? ReadResult::RESULT_SERVER_ERROR :
            ReadResult::RESULT_UNKNOWN_ERROR);

        // for request errors, return an error result with the part data intact
        // so the user can parse it as needed. We only do this for readString.
        if (response.getNumParts() > 0u)
        {
            result.setErrorDetail(response.getPartAsString(0));

            if (s_HTTP_DEBUG)
            {
                OE_WARN << LC << "SERVER REPORTS: " << result.errorDetail() << std::endl;
            }

            if (s_HTTP_DEBUG)
            {
                OE_WARN << LC << "SERVER REPORTS: " << result.errorDetail() << std::endl;
            }
        }

        //If we have an error but it's recoverable, like a server error or timeout then set the callback to retry.
        if (HTTPClient::isRecoverable( result.code() ) )
        {
            if (callback)
            {
                callback->setRetryDelay(getRetryDelay());
                callback->cancel();

                if ( s_HTTP_DEBUG )
                {
                    if (response.isCanceled())
                    {
                        OE_NOTICE << LC << "Request was cancelled" << std::endl;
                    }
                    else
                    {
                        OE_NOTICE << LC << "Recoverable error in HTTPClient for " << request.getURL() << std::endl;
                    }
                }
            }
        }
    }

    // encode metadata
    setMetadata(result, request, response);

    return result;
}


ReadResult
HTTPClient::doReadString(const HTTPRequest&    request,
                         const osgDB::Options* options,
                         ProgressCallback*     callback )
{
    initialize();

    ReadResult result;

    HTTPResponse response = this->doGet( request, options, callback );
    if ( response.isOK() && response.getNumParts() > 0 )
    {
        result = ReadResult( new StringObject(response.getPartAsString(0)) );
    }
    else
    {
        result = ReadResult(
            response.isCanceled() ? ReadResult::RESULT_CANCELED :
            response.getCode() == HTTPResponse::NOT_FOUND ? ReadResult::RESULT_NOT_FOUND :
            response.getCode() == HTTPResponse::NOT_MODIFIED ? ReadResult::RESULT_NOT_MODIFIED :
            response.getCodeCategory() == HTTPResponse::CATEGORY_SERVER_ERROR ? ReadResult::RESULT_SERVER_ERROR :
            ReadResult::RESULT_UNKNOWN_ERROR);
        
        // for request errors, return an error result with the part data intact
        // so the user can parse it as needed. We only do this for readString.
        if (response.getNumParts() > 0u)
        {
            result.setErrorDetail(response.getPartAsString(0));

            if (s_HTTP_DEBUG)
            {
                OE_WARN << LC << "SERVER REPORTS: " << result.errorDetail() << std::endl;
            }
        }

        //If we have an error but it's recoverable, like a server error or timeout then set the callback to retry.
        if (HTTPClient::isRecoverable( result.code() ) )
        {
            if (callback)
            {
                callback->setRetryDelay(getRetryDelay());
                callback->cancel();

                if ( s_HTTP_DEBUG )
                {
                    if (response.isCanceled())
                    {
                        OE_NOTICE << LC << "HTTP request was cancelled" << std::endl;
                    }
                    else
                    {
                        OE_NOTICE << LC << "Recoverable error in HTTPClient for " << request.getURL() << std::endl;
                    }
                }
            }
        }
    }

    // encode metadata
    setMetadata(result, request, response);

    result.setIsFromCache(response.getFromCache());

    // last-modified (file time)
    result.setLastModifiedTime( response._lastModified );

    return result;
}
