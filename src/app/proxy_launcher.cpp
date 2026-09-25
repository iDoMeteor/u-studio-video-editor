#include "proxy_launcher.h"

#include "core/log.h"
#include "platform/process.h"

#include <gio/gio.h>

#include <cstdlib>

namespace ustudio::app {

namespace {

// Everything a running child's callbacks need; alive until it has exited,
// however long the queue keeps its Child.
struct Session
{
    GSubprocess *process = nullptr;
    GDataInputStream *out = nullptr;
    std::function<void(const std::string &)> onLine;
    std::function<void(int)> onExit;

    ~Session()
    {
        if (out)
            g_object_unref(out);
        if (process)
            g_object_unref(process);
    }
};
using Holder = std::shared_ptr<Session>;

void readNext(Holder session);

void exited(GObject *source, GAsyncResult *result, gpointer data)
{
    std::unique_ptr<Holder> holder(static_cast<Holder *>(data));
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, nullptr);
    const Session &session = **holder;
    const int status = g_subprocess_get_if_exited(session.process) ? g_subprocess_get_exit_status(session.process)
                                                                   : 128 + g_subprocess_get_term_sig(session.process);
    session.onExit(status);
}

void lineRead(GObject *source, GAsyncResult *result, gpointer data)
{
    std::unique_ptr<Holder> holder(static_cast<Holder *>(data));
    gsize length = 0;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), result, &length, nullptr);
    if (!line) { // end of output (or a read error): wait for the exit
        g_subprocess_wait_async((*holder)->process, nullptr, &exited, new Holder(*holder));
        return;
    }
    (*holder)->onLine(line);
    g_free(line);
    readNext(*holder);
}

void readNext(Holder session)
{
    GDataInputStream *out = session->out;
    g_data_input_stream_read_line_async(out, G_PRIORITY_DEFAULT, nullptr, &lineRead, new Holder(std::move(session)));
}

class GioChild : public ProxyQueue::Child
{
  public:
    explicit GioChild(Holder session) : m_session(std::move(session)) {}
    void cancel() override
    {
        const char *id = g_subprocess_get_identifier(m_session->process); // the pid, while it runs
        if (id)
            platform::requestTermination(std::strtoll(id, nullptr, 10));
    }

  private:
    Holder m_session;
};

} // namespace

std::unique_ptr<ProxyQueue::Child> GioLauncher::start(const std::vector<std::string> &argv,
                                                      std::function<void(const std::string &line)> onLine,
                                                      std::function<void(int status)> onExit)
{
    std::vector<const char *> args;
    for (const std::string &arg : argv)
        args.push_back(arg.c_str());
    args.push_back(nullptr);
    GError *error = nullptr;
    GSubprocess *process = g_subprocess_newv(args.data(), G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
    if (!process) {
        core::Log::warn(std::string("[proxy] couldn't start ") + argv.front() + ": " +
                        (error ? error->message : "unknown error"));
        if (error)
            g_error_free(error);
        onExit(-1);
        return nullptr;
    }
    auto session = std::make_shared<Session>();
    session->process = process;
    session->out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
    session->onLine = std::move(onLine);
    session->onExit = std::move(onExit);
    readNext(session);
    return std::make_unique<GioChild>(std::move(session));
}

} // namespace ustudio::app
