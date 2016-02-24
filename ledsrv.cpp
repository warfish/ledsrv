#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>

#include <vector>
#include <list>
#include <exception>
#include <stdexcept>

#include <boost/algorithm/string.hpp>
#include <boost/scope_exit.hpp>

#include "ledsrv.h"

#if !defined(countof)
#   define countof(_a) (sizeof(_a) / sizeof(_a[0]))
#endif // countof

////////////////////////////////////////////////////////////////////////////////

//
// I/O utils
//

namespace {

/**
 * \brief   RAII helper for pipe descriptor
 */
class Fifo : boost::noncopyable
{
public:

    enum Type {
        kFifoRead = 0,
        kFifoWrite,
    };

    enum Flags {
        kFifoDefault = 0,
        kFifoDeleteOnClose, // Delete fifo on close
    };
    
    Fifo() : m_fd(-1), m_unlink(false) {
    }

    ~Fifo() {
        this->close();
    }

    /**
     * \brief   Create a fifo with name and type
     *          Will block until remote end is opened for appropriate access.
     *
     * \return  0 on success, negative value on error
     */
    int create(const std::string& name, Type type);

    /**
     * \brief   Open existing fifo
     *
     * \return  0 on success, negative value on error
     */
    int open(const std::string& name, Type type, Flags flags = kFifoDefault);

    /**
     * \brief   Read data from a fifo
     */
    ssize_t read(void* data, size_t bytes);

    /**
     * \brief   Write data to a fifo
     */
    ssize_t write(const void* data, size_t bytes);

    /**
     * \brief   Close fifo.
     */
    void close();

    bool is_open() const {
        return m_fd >= 0;
    }

    int fd() const {
        return m_fd;
    }

    const std::string& name() const {
        return m_name;
    }

private:

    int m_fd;
    std::string m_name;
    bool m_unlink;
};

int Fifo::create(const std::string& name, Fifo::Type type)
{
    // Opening the same fifo?
    if (name == m_name) {
        return 0;
    }

    int res = 0;
    res = ::access(name.c_str(), F_OK);
    if (res == 0) {
        // File exists, check if it's a pipe and remove it
        struct stat st;
        res = ::stat(name.c_str(), &st);
        if (res != 0) {
            perror("stat failed");
            return res;
        }

        if (S_IFIFO & st.st_mode) {
            res = ::unlink(name.c_str());
            if (res != 0) {
                perror("unlink failed");
                return res;
            }
        }
    }

    res = ::mkfifo(name.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (res != 0) {
        perror("mkfifo failed");
        return res;
    }

    res = this->open(name, type, kFifoDeleteOnClose);
    if (res != 0) {
        ::unlink(name.c_str());
    }

    return 0;
}

int Fifo::open(const std::string& name, Type type, Flags flags /* = kDefault */)
{
    int fd = ::open(name.c_str(), (type == kFifoRead ? O_RDONLY : O_WRONLY));
    if (fd < 0) {
        return fd;
    }

    this->close();

    m_fd = fd;
    m_name = name;
    m_unlink = (flags == kFifoDeleteOnClose);
    return 0;
}

void Fifo::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        if (m_unlink) {
            ::unlink(m_name.c_str());
        }

        m_fd = -1;
    }
}

ssize_t Fifo::read(void* data, size_t bytes)
{
    assert(data);
    return ::read(m_fd, data, bytes);
}

ssize_t Fifo::write(const void* data, size_t bytes)
{
    assert(data);
    return ::write(m_fd, data, bytes);
}

/**
 * \brief   RAII helper to hold client connection fifos
 */
class Connection : boost::noncopyable
{
public:

    Connection() {
    }

    ~Connection() {
        this->close();
    }

    /**
     * \brief   Init connection to specified client pid
     *          Will open in and out fifos and block until remote side completes its open.
     *
     * \return  0 on success, negative value on error
     */
    int open(pid_t pid);

    /**
     * \brief   Close connection
     */
    void close();

    Fifo& in() {
        return m_in;
    }

    Fifo& out() {
        return m_out;
    }

    // Shortcuts
    ssize_t read(void* data, size_t bytes) {
        return m_in.read(data, bytes);
    }

    ssize_t write(const void* data, size_t bytes) {
        return m_out.write(data, bytes);
    }

private:

    Fifo m_in;
    Fifo m_out;
};

int Connection::open(pid_t pid)
{
    int err = 0;
    char buf[PATH_MAX] = {0};
    
    snprintf(buf, sizeof(buf), LEDSRV_IN_FIFO, pid);
    err = m_in.open(std::string(buf), Fifo::kFifoRead);
    if (err < 0) {
        return err;
    }

    snprintf(buf, sizeof(buf), LEDSRV_OUT_FIFO, pid);
    err = m_out.open(std::string(buf), Fifo::kFifoWrite);
    if (err < 0) {
        return err;
    }

    return 0;
}   

void Connection::close()
{
    m_in.close();
    m_out.close();
}

} // anonymous namespace 

////////////////////////////////////////////////////////////////////////////////

//
// LED request handling
//

// Global LED state
static LedState gLedState = {
    .state = false,
    .color = LedColor::Red,
    .rate = 1,
};

bool operator == (const LedState& lhv, const LedState& rhv) 
{
    return (lhv.state == rhv.state) && (lhv.color == rhv.color) && (lhv.rate == rhv.rate);
}

bool operator != (const LedState& lhv, const LedState& rhv) 
{
    return !(lhv == rhv);
}

// Led view impl
std::unique_ptr<ILedView> gLedView;

// Describes supported command. 
struct LedRequestDesc
{
    const char* command;        // Command verb
    unsigned long nargs;        // Number of arguments this command accepts

    /**
     * \brief   Request handler. 
     *          Normally i'd put function pointers here, but let's have some fun with lambdas.
     *
     * \argv    Command name at index 0 followed by any additional arguments
     * \output  If request generates any output, store it here
     * \led     Explicit led state to operate on
     *
     * \return  True if command was successful, false if anything went wrong.
     */
    std::function<bool(const std::vector<std::string>& argv, std::string& output, LedState& led)> handler;
};

// We know all supported requests at compile time so here's a static list of commands we support 
static const LedRequestDesc gRequests[] = 
{
    {   
        "set-led-state", 1, 
        [](const std::vector<std::string>& argv, std::string& output, LedState& led)
        {  
            assert(argv.size() == 2);
            const std::string& arg = argv[1];

            if (boost::iequals(arg, "on")) {
                led.state = true;
                return true;
            } else if (boost::iequals(arg, "off")) {
                led.state = false;
                return true;
            } else {
                return false;
            }
        } 
    },
    
    {   
        "get-led-state", 0, 
        [](const std::vector<std::string>& argv, std::string& output, LedState& led)
        { 
            led.state ? output.assign("on") : output.assign("off"); 
            return true;
        } 
    },

    {   
        "set-led-color", 1, 
        [](const std::vector<std::string>& argv, std::string& output, LedState& led)
        {
            assert(argv.size() == 2);
            const std::string& arg = argv[1];

            if (boost::iequals(arg, "red")) {
                led.color = LedColor::Red;
                return true;
            } else if (boost::iequals(arg, "blue")) {
                led.color = LedColor::Blue;
                return true;
            } else if (boost::iequals(arg, "green")) {
                led.color = LedColor::Green;
                return true;
            } else {
                return false;          
            }
        }
    },

    {   
        "get-led-color", 0, 
        [](const std::vector<std::string>& argv, std::string& output, LedState& led)
        {   
            switch(led.color) {
            case LedColor::Red:     output.assign("red"); break;
            case LedColor::Blue:    output.assign("blue"); break;
            case LedColor::Green:   output.assign("green"); break; 
            default:                assert(0);
            };

            return true;
        }
    },

    {   
        "set-led-rate", 1, 
        [](const std::vector<std::string>& argv, std::string& output, LedState& led)
        { 
            assert(argv.size() == 2);
            
            int arg = std::stoi(argv[1]);
            if (arg < 1 || arg > 5) {
                return false;
            }

            led.rate = arg;
            return true;
        }
    },

    {   
        "get-led-rate", 0, 
        [](const std::vector<std::string>& argv, std::string& output, LedState& led)
        { 
            output = std::to_string(led.rate);
            return true;
        }
    },

    // Add new command handler here
};

// Parse and dispatch received request
static bool DispatchRequest(const std::string& req, std::string& respose)
{
    std::vector<std::string> argv;

    // Deconstruct request into command and args, separated by whitespace
    // At least 1 command word should be there
    boost::split(argv, req, boost::is_space());
    if (argv.size() < 1) {
        return false;
    }

    size_t nargs = argv.size() - 1;

    // Find request with this command and number of args
    for (size_t i = 0; i < countof(gRequests); ++i) 
    {
        const LedRequestDesc* r = &gRequests[i];
        if ((0 == argv[0].compare(r->command)) && (nargs == r->nargs)) 
        {
            LedState led = gLedState;
            bool res = r->handler(argv, respose, led);
            if (res && (led != gLedState)) {
                gLedView->Update(led); // Update view only when state has changed
                gLedState = led;
            }

            return res;
        }
    }
     
    return false;
}

// Read pending '\n'-separated requests from fifo
// TODO: not sure if it is possible for fifo to accumulate several requests before we are unblocked from read.
static bool ReadRequests(Fifo& fifo, std::vector<std::string>& req)
{
    // PIPE_BUF read should be atomic
    char buf[PIPE_BUF] = {0};
    if (fifo.read(buf, PIPE_BUF) < 0) {
        return false;
    }

    std::string input(buf, strnlen(buf, sizeof(buf)));
    boost::split(req, input, boost::is_any_of("\n"), boost::algorithm::token_compress_on);

    // split will return an empty trailing request, remove it
    req.pop_back();
    return true;
}

// Process newly connected clients
// Error are ignored but we should probably handle a lot of things, like remote fifo close, etc.
static void ProcessClient(Connection& conn)
{
    std::vector<std::string> req;
    if (!ReadRequests(conn.in(), req)) {
        return;
    }

    for (auto i : req) {
        std::string response;
        if (DispatchRequest(i, response)) {
            std::string output = LEDSRV_STATUS_OK;
            if (response.length() > 0) {
                output.append(" ");
                output.append(response);
            }

            output.append("\n");
            conn.write(output.c_str(), output.length()); 
        } else {
            const char* failed = LEDSRV_STATUS_FAILED "\n";
            conn.write(failed, strlen(failed));
        }
    }
}

static void inthandler(int s)
{
    unlink(LEDSRV_FIFO_NAME);
}

int main(void)
{
    int err = 0;

    gLedView = CreateLedView();
    if (!gLedView) {
        return EXIT_FAILURE;
    }

    gLedView->Update(gLedState);
    
    signal(SIGINT, inthandler);

    Fifo connFifo;
    err = connFifo.create(LEDSRV_FIFO_NAME, Fifo::kFifoRead);
    if (err != 0) {
        return EXIT_FAILURE;
    }

    // Wait for incoming PIDs on connection fifo separated by new line chars
    std::vector<std::string> req;
    while (ReadRequests(connFifo, req)) {
        for (auto i : req) {
            int pid = std::stoi(i);

            Connection conn;
            err = conn.open(pid);
            if (err) {
                return EXIT_FAILURE;
            }

            ProcessClient(conn);
        }
    }

    return EXIT_SUCCESS;
}
