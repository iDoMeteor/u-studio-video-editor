#pragma once

// Signing in to the sharing service from u-studio-share (doc 21: OAuth 2.0
// authorization code with PKCE, through the system browser). A loopback
// listener on 127.0.0.1 receives the browser's redirect; the code is only
// taken with the state this sign-in sent. The refresh token goes to the
// user's keyring (libsecret), never to a file.

#include "share/client.h"

#include <libsoup/soup.h>

#include <functional>
#include <memory>
#include <string>

namespace ustudio::titles::share {

constexpr const char *kClientId = "u-studio-share";

class LoopbackSignIn
{
  public:
    // Listens on 127.0.0.1 on a free port (main thread; the result comes
    // back on the main loop).
    LoopbackSignIn();
    ~LoopbackSignIn();
    LoopbackSignIn(const LoopbackSignIn &) = delete;
    LoopbackSignIn &operator=(const LoopbackSignIn &) = delete;

    bool listening() const
    {
        return !m_redirect.empty();
    }
    // "http://127.0.0.1:<port>/callback"
    const std::string &redirectUri() const
    {
        return m_redirect;
    }
    // The sign-in page to open in the browser; `done` gets the code (or
    // "" and an error) once the browser comes back.
    std::string start(const std::string &base, std::function<void(std::string code, std::string error)> done);
    const Pkce &pkce() const
    {
        return m_pkce;
    }

  private:
    void received(const std::string &code, const std::string &state, const std::string &error);
    SoupServer *m_server = nullptr;
    std::string m_redirect, m_state;
    Pkce m_pkce;
    std::function<void(std::string, std::string)> m_done;

    // --- libsoup trampolines -------------------------------------------------
    static void onCallback(SoupServer *, SoupServerMessage *message, const char *, GHashTable *query, gpointer self);
};

// The keyring (libsecret): the refresh token for one service. Empty when
// there's none, or no keyring.
std::string loadRefreshToken(const std::string &base);
void storeRefreshToken(const std::string &base, const std::string &token);
void forgetRefreshToken(const std::string &base);

} // namespace ustudio::titles::share
