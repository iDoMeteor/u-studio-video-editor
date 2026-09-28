#include "client.h"

#include "core/media/utf8_path.h"
#include "package/archive.h"
#include "package/pack.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>

namespace ustudio::titles::share {

namespace fs = std::filesystem;

namespace {

struct ParserFree
{
    void operator()(JsonParser *p) const
    {
        g_object_unref(p);
    }
};

// A JSON object's root, or why not.
std::expected<std::unique_ptr<JsonParser, ParserFree>, std::string> parse(const std::string &body)
{
    std::unique_ptr<JsonParser, ParserFree> parser(json_parser_new());
    GError *error = nullptr;
    if (!json_parser_load_from_data(parser.get(), body.data(), static_cast<gssize>(body.size()), &error)) {
        const std::string why = error ? error->message : "not JSON";
        g_clear_error(&error);
        return std::unexpected("the service's answer isn't JSON (" + why + ")");
    }
    JsonNode *root = json_parser_get_root(parser.get());
    if (!root || json_node_get_node_type(root) != JSON_NODE_OBJECT)
        return std::unexpected("the service's answer isn't an object");
    return parser;
}

JsonObject *rootObject(JsonParser *parser)
{
    return json_node_get_object(json_parser_get_root(parser));
}

std::string str(JsonObject *o, const char *name)
{
    return json_object_get_string_member_with_default(o, name, "");
}

std::vector<std::string> strings(JsonObject *o, const char *name)
{
    std::vector<std::string> out;
    if (!json_object_has_member(o, name))
        return out;
    JsonArray *array = json_object_get_array_member(o, name);
    for (guint i = 0; array && i < json_array_get_length(array); ++i)
        out.emplace_back(json_array_get_string_element(array, i));
    return out;
}

// The service's error message, if its answer has one.
std::string errorOf(unsigned status, const std::string &body)
{
    if (auto parser = parse(body)) {
        const std::string message = str(rootObject(parser->get()), "error");
        if (!message.empty())
            return message + " (" + std::to_string(status) + ")";
    }
    return "the service answered " + std::to_string(status);
}

std::string base64url(const guchar *data, size_t size)
{
    gchar *b64 = g_base64_encode(data, size);
    std::string out = b64;
    g_free(b64);
    for (char &c : out)
        c = c == '+' ? '-' : (c == '/' ? '_' : c);
    while (!out.empty() && out.back() == '=')
        out.pop_back();
    return out;
}

std::string form(std::initializer_list<std::pair<const char *, std::string>> fields)
{
    std::string out;
    for (const auto &[name, value] : fields) {
        gchar *escaped = g_uri_escape_string(value.c_str(), nullptr, FALSE);
        out += (out.empty() ? "" : "&") + std::string(name) + "=" + escaped;
        g_free(escaped);
    }
    return out;
}

std::expected<Tokens, std::string> tokensFrom(const std::string &body)
{
    auto parser = parse(body);
    if (!parser)
        return std::unexpected(parser.error());
    JsonObject *o = rootObject(parser->get());
    Tokens t{str(o, "access_token"), str(o, "refresh_token"),
             json_object_get_int_member_with_default(o, "expires_in", 0)};
    if (t.access.empty())
        return std::unexpected("the sign-in answer has no access token");
    return t;
}

} // namespace

std::string escapeSegment(const std::string &text)
{
    gchar *escaped = g_uri_escape_string(text.c_str(), nullptr, FALSE);
    std::string out = escaped;
    g_free(escaped);
    return out;
}

Pkce makePkce()
{
    // A secret: the OS's random source (std::random_device reads it), not
    // GLib's g_random_*, which isn't cryptographic.
    std::random_device device;
    guchar random[48];
    for (guchar &b : random)
        b = static_cast<guchar>(device() & 0xff);
    Pkce p;
    p.verifier = base64url(random, sizeof random); // 64 characters, within RFC 7636's 43..128
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, reinterpret_cast<const guchar *>(p.verifier.data()), static_cast<gssize>(p.verifier.size()));
    guint8 digest[32];
    gsize length = sizeof digest;
    g_checksum_get_digest(sum, digest, &length);
    g_checksum_free(sum);
    p.challenge = base64url(digest, length);
    return p;
}

std::string authorizeUrl(const std::string &base, const std::string &clientId, const std::string &redirectUri,
                         const std::string &challenge, const std::string &state)
{
    return base + "/oauth2/authorize?" +
           form({{"response_type", "code"},
                 {"client_id", clientId},
                 {"redirect_uri", redirectUri},
                 {"code_challenge", challenge},
                 {"code_challenge_method", "S256"},
                 {"scope", "openid"},
                 {"state", state}});
}

Client::Client(std::string base) : m_base(std::move(base))
{
    while (!m_base.empty() && m_base.back() == '/')
        m_base.pop_back();
    m_session = soup_session_new();
    soup_session_set_user_agent(m_session, "u-studio-share");
    soup_session_set_timeout(m_session, 60);
}

Client::~Client()
{
    g_object_unref(m_session);
}

void Client::setAccessToken(std::string token)
{
    m_token = std::move(token);
}

std::expected<Client::Response, std::string> Client::send(const char *method, const std::string &url,
                                                          const std::string &body, const char *contentType, bool auth)
{
    SoupMessage *message = soup_message_new(method, url.c_str());
    if (!message)
        return std::unexpected("not a web address: " + url);
    // No redirects followed on API calls: an answer is from the service.
    soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    if (auth && !m_token.empty())
        soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization",
                                     ("Bearer " + m_token).c_str());
    if (contentType) {
        GBytes *bytes = g_bytes_new(body.data(), body.size());
        soup_message_set_request_body_from_bytes(message, contentType, bytes);
        g_bytes_unref(bytes);
    }
    GError *error = nullptr;
    GBytes *reply = soup_session_send_and_read(m_session, message, nullptr, &error);
    const unsigned status = soup_message_get_status(message);
    g_object_unref(message);
    if (!reply) {
        const std::string why = error ? error->message : "no answer";
        g_clear_error(&error);
        return std::unexpected("can't reach the service (" + why + ")");
    }
    gsize size = 0;
    const auto *data = static_cast<const char *>(g_bytes_get_data(reply, &size));
    Response response{status, std::string(data ? data : "", size)};
    g_bytes_unref(reply);
    return response;
}

std::expected<std::vector<CatalogueEntry>, std::string> Client::list(const std::string &query, const std::string &tag)
{
    auto r = send("GET", m_base + "/v1/packs?" + form({{"q", query}, {"tag", tag}}));
    if (!r)
        return std::unexpected(r.error());
    if (r->status != 200)
        return std::unexpected(errorOf(r->status, r->body));
    auto parser = parse(r->body);
    if (!parser)
        return std::unexpected(parser.error());
    std::vector<CatalogueEntry> out;
    JsonObject *root = rootObject(parser->get());
    if (!json_object_has_member(root, "packs"))
        return out;
    JsonArray *packs = json_object_get_array_member(root, "packs");
    for (guint i = 0; packs && i < json_array_get_length(packs); ++i) {
        JsonObject *p = json_array_get_object_element(packs, i);
        out.push_back({str(p, "id"), str(p, "title"), str(p, "author"), str(p, "licence"), str(p, "latestVersion"),
                       strings(p, "tags"), json_object_get_int_member_with_default(p, "downloads", 0)});
    }
    return out;
}

std::expected<PackDetails, std::string> Client::pack(const std::string &id)
{
    auto r = send("GET", m_base + "/v1/packs/" + escapeSegment(id));
    if (!r)
        return std::unexpected(r.error());
    if (r->status != 200)
        return std::unexpected(errorOf(r->status, r->body));
    auto parser = parse(r->body);
    if (!parser)
        return std::unexpected(parser.error());
    JsonObject *o = rootObject(parser->get());
    PackDetails d{str(o, "id"),      str(o, "title"),    str(o, "description"),   str(o, "author"),
                  str(o, "licence"), strings(o, "tags"), strings(o, "templates"), {}};
    if (json_object_has_member(o, "versions")) {
        JsonArray *versions = json_object_get_array_member(o, "versions");
        for (guint i = 0; versions && i < json_array_get_length(versions); ++i) {
            JsonObject *v = json_array_get_object_element(versions, i);
            d.versions.push_back({str(v, "version"), str(v, "sha256"),
                                  static_cast<uint64_t>(json_object_get_int_member_with_default(v, "size", 0))});
        }
    }
    return d;
}

std::expected<std::string, std::string> Client::download(const std::string &id, const std::string &version,
                                                         const std::string &directory)
{
    // POST, as doc 21 has it: a link a crawler follows never mints a URL.
    auto r =
        send("POST", m_base + "/v1/packs/" + escapeSegment(id) + "/versions/" + escapeSegment(version) + "/download",
             "{}", "application/json");
    if (!r)
        return std::unexpected(r.error());
    if (r->status != 200)
        return std::unexpected(errorOf(r->status, r->body));
    auto parser = parse(r->body);
    if (!parser)
        return std::unexpected(parser.error());
    JsonObject *o = rootObject(parser->get());
    const std::string url = str(o, "url"), sha256 = str(o, "sha256");
    const auto size = static_cast<uint64_t>(json_object_get_int_member_with_default(o, "size", 0));
    if (url.empty() || sha256.size() != 64)
        return std::unexpected("the service's download answer is incomplete");
    const pack::Limits limits;
    if (size == 0 || size > limits.maxTotalBytes)
        return std::unexpected("the pack is bigger than a pack may be");

    auto file = send("GET", url);
    if (!file)
        return std::unexpected(file.error());
    if (file->status != 200)
        return std::unexpected("the download failed (" + std::to_string(file->status) + ")");
    // Checked here, whatever the service says (ADR-020).
    if (file->body.size() != size || pack::sha256(file->body) != sha256)
        return std::unexpected("the download isn't the pack the service described (its size or SHA-256 differs)");

    std::error_code ec;
    fs::create_directories(core::pathFromUtf8(directory), ec);
    std::string name;
    for (char c : id + "-" + version)
        name += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
    const bool zip = file->body.starts_with("PK");
    const fs::path target = core::pathFromUtf8(directory) / core::pathFromUtf8(name + (zip ? ".zip" : ".tar.gz"));
    fs::path part = target;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(file->body.data(), static_cast<std::streamsize>(file->body.size()));
        if (!out) {
            fs::remove(part, ec);
            return std::unexpected("can't write " + core::utf8String(part));
        }
    }
    if (auto valid = pack::inspectPackage(core::utf8String(part)); !valid) {
        fs::remove(part, ec);
        return std::unexpected("the downloaded pack is refused: " + valid.error());
    }
    fs::rename(part, target, ec);
    if (ec) {
        fs::remove(part, ec);
        return std::unexpected("can't save the download (" + ec.message() + ")");
    }
    return core::utf8String(target);
}

std::expected<std::string, std::string> Client::publish(const std::string &packPath)
{
    auto valid = pack::inspectPackage(packPath);
    if (!valid)
        return std::unexpected("the pack wouldn't be accepted: " + valid.error());
    std::ifstream in(core::pathFromUtf8(packPath), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > 25u << 20)
        return std::unexpected("the pack is over the service's 25 MB");

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, valid->manifest.id.c_str());
    json_builder_set_member_name(builder, "version");
    json_builder_add_string_value(builder, valid->manifest.version.c_str());
    json_builder_end_object(builder);
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *json = json_generator_to_data(generator, nullptr);
    const std::string request = json;
    g_free(json);
    json_node_unref(root);
    g_object_unref(generator);
    g_object_unref(builder);

    auto r = send("POST", m_base + "/v1/uploads", request, "application/json", true);
    if (!r)
        return std::unexpected(r.error());
    if (r->status == 401)
        return std::unexpected("sign in first");
    if (r->status != 200)
        return std::unexpected(errorOf(r->status, r->body));
    auto parser = parse(r->body);
    if (!parser)
        return std::unexpected(parser.error());
    JsonObject *o = rootObject(parser->get());
    const std::string uploadId = str(o, "uploadId"), url = str(o, "url");
    if (uploadId.empty() || url.empty())
        return std::unexpected("the service's upload answer is incomplete");
    auto put = send("PUT", url, bytes, "application/octet-stream");
    if (!put)
        return std::unexpected(put.error());
    if (put->status != 200)
        return std::unexpected("the upload failed (" + std::to_string(put->status) + ")");
    return uploadId;
}

std::expected<UploadStatus, std::string> Client::uploadStatus(const std::string &uploadId)
{
    auto r = send("GET", m_base + "/v1/uploads/" + escapeSegment(uploadId), {}, nullptr, true);
    if (!r)
        return std::unexpected(r.error());
    if (r->status != 200)
        return std::unexpected(errorOf(r->status, r->body));
    auto parser = parse(r->body);
    if (!parser)
        return std::unexpected(parser.error());
    JsonObject *o = rootObject(parser->get());
    const std::string status = str(o, "status");
    UploadStatus s;
    s.reasons = strings(o, "reasons");
    if (status == "published")
        s.state = UploadState::Published;
    else if (status == "rejected")
        s.state = UploadState::Rejected;
    else if (status == "in-review")
        s.state = UploadState::InReview;
    else if (status == "validating")
        s.state = UploadState::Validating;
    return s;
}

std::expected<Publisher, std::string> Client::me()
{
    auto r = send("GET", m_base + "/v1/me", {}, nullptr, true);
    if (!r)
        return std::unexpected(r.error());
    if (r->status != 200)
        return std::unexpected(errorOf(r->status, r->body));
    auto parser = parse(r->body);
    if (!parser)
        return std::unexpected(parser.error());
    JsonObject *o = rootObject(parser->get());
    return Publisher{str(o, "displayName"), str(o, "slug")};
}

std::expected<Tokens, std::string> Client::exchangeCode(const std::string &clientId, const std::string &code,
                                                        const std::string &verifier, const std::string &redirectUri)
{
    auto r = send("POST", m_base + "/oauth2/token",
                  form({{"grant_type", "authorization_code"},
                        {"client_id", clientId},
                        {"code", code},
                        {"code_verifier", verifier},
                        {"redirect_uri", redirectUri}}),
                  "application/x-www-form-urlencoded");
    if (!r)
        return std::unexpected(r.error());
    if (r->status != 200)
        return std::unexpected("sign-in failed: " + errorOf(r->status, r->body));
    return tokensFrom(r->body);
}

std::expected<Tokens, std::string> Client::refresh(const std::string &clientId, const std::string &refreshToken)
{
    auto r = send("POST", m_base + "/oauth2/token",
                  form({{"grant_type", "refresh_token"}, {"client_id", clientId}, {"refresh_token", refreshToken}}),
                  "application/x-www-form-urlencoded");
    if (!r)
        return std::unexpected(r.error());
    if (r->status != 200)
        return std::unexpected("sign-in expired: " + errorOf(r->status, r->body));
    return tokensFrom(r->body);
}

} // namespace ustudio::titles::share
