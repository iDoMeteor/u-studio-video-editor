#pragma once

// u-studio-share's client for the template sharing service (doc 21, ADR-020;
// T7). The only network code in the project: it lives in the helper
// (drop-ins/titles/share/), never in the editor or u-studio-titles, and is
// only called on an explicit user action.
//
// Synchronous: call it from a worker thread. One Client per thread (a
// SoupSession isn't for sharing between threads).

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

typedef struct _SoupSession SoupSession;

namespace ustudio::titles::share {

struct CatalogueEntry
{
    std::string id, title, author, licence, latestVersion;
    std::vector<std::string> tags;
    int64_t downloads = 0;
};

struct PackVersion
{
    std::string version, sha256;
    uint64_t size = 0;
};

struct PackDetails
{
    std::string id, title, description, author, licence;
    std::vector<std::string> tags, templates;
    std::vector<PackVersion> versions;
};

struct Tokens
{
    std::string access, refresh;
    int64_t expiresIn = 0;
};

enum class UploadState
{
    Pending,
    Validating,
    InReview,
    Published,
    Rejected,
};
struct UploadStatus
{
    UploadState state = UploadState::Pending;
    std::vector<std::string> reasons;
};

struct Publisher
{
    std::string displayName, slug;
};

class Client
{
  public:
    // `base`: the service's root, "https://…" (http only for a loopback
    // test server).
    explicit Client(std::string base);
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    void setAccessToken(std::string token);

    std::expected<std::vector<CatalogueEntry>, std::string> list(const std::string &query, const std::string &tag);
    std::expected<PackDetails, std::string> pack(const std::string &id);

    // Download: a signed URL from the service, the file into `directory`
    // (a .part, renamed when whole), then checked again here whatever the
    // service says: its size and SHA-256 against what the service
    // announced, and the whole of the pack validator (doc 20). The path,
    // or why not; a refused download leaves nothing.
    std::expected<std::string, std::string> download(const std::string &id, const std::string &version,
                                                     const std::string &directory);

    // Publish: validated here first (what others will accept), then a
    // presigned upload URL, the file PUT to it. The upload id, to poll.
    std::expected<std::string, std::string> publish(const std::string &packPath);
    std::expected<UploadStatus, std::string> uploadStatus(const std::string &uploadId);

    std::expected<Publisher, std::string> me();

    // OAuth 2.0 authorization code with PKCE (the system browser signs in).
    std::expected<Tokens, std::string> exchangeCode(const std::string &clientId, const std::string &code,
                                                    const std::string &verifier, const std::string &redirectUri);
    std::expected<Tokens, std::string> refresh(const std::string &clientId, const std::string &refreshToken);

  private:
    struct Response
    {
        unsigned status = 0;
        std::string body;
    };
    std::expected<Response, std::string> send(const char *method, const std::string &url, const std::string &body = {},
                                              const char *contentType = nullptr, bool auth = false);
    std::string m_base, m_token;
    SoupSession *m_session = nullptr;
};

// PKCE (RFC 7636): a random verifier and its S256 challenge.
struct Pkce
{
    std::string verifier, challenge;
};
Pkce makePkce();
// The sign-in page's address, for the system browser.
std::string authorizeUrl(const std::string &base, const std::string &clientId, const std::string &redirectUri,
                         const std::string &challenge, const std::string &state);

// The pack id in a URL path segment ("a/b" -> "a%2Fb").
std::string escapeSegment(const std::string &text);

} // namespace ustudio::titles::share
