#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>

#include <vector>
#include <list>
#include <exception>
#include <stdexcept>

#include <boost/algorithm/string.hpp>
#include <boost/scope_exit.hpp>

#include "ledsrv.h"

////////////////////////////////////////////////////////////////////////////////

#define LEDSRV_FIFO_NAME            "/tmp/ledsrv"
#define LEDCLI_FIFO_NAME            "/tmp/ledcli"
#define LEDSRV_STATUS_OK            "OK"
#define LEDSRV_STATUS_FAILED        "FAILED"

#if !defined(countof)
#   define countof(_a) (sizeof(_a) / sizeof(_a[0]))
#endif // countof

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
    printf("%s\n", argv[0].c_str());

    // Find request with this command and number of args
    for (size_t i = 0; i < countof(gRequests); ++i) 
    {
        const LedRequestDesc* r = &gRequests[i];
        if ((0 == argv[0].compare(r->command)) && (nargs == r->nargs)) 
        {
            LedState led = gLedState;
            bool res = r->handler(argv, respose, led);
            if (res && (led != gLedState)) {
                gLedView->Update(led);
                gLedState = led;
            }

            return res;
        }
    }
     
    return false;
}

int main(void)
{
    int err = 0;
    int fd = -1;
    int fd_cli = -1;

    BOOST_SCOPE_EXIT(&fd, &fd_cli) {
        close(fd);
        close(fd_cli);
        unlink(LEDSRV_FIFO_NAME);
    } BOOST_SCOPE_EXIT_END

    err = mkfifo(LEDSRV_FIFO_NAME, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
    if (err != 0) {
        perror("mkfifo");
        return EXIT_FAILURE;
    }

    err = mkfifo(LEDCLI_FIFO_NAME, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
    if (err != 0) {
        perror("mkfifo");
        return EXIT_FAILURE;
    }

    fd = open(LEDSRV_FIFO_NAME, O_RDWR);
    if (fd < 0) {
        perror("open");
        return EXIT_FAILURE;
    }

/*
    fd_cli = open(LEDCLI_FIFO_NAME, O_WRONLY);
    if (fd_cli < 0) {
        perror("open");
        return EXIT_FAILURE;
    }
*/
    printf("%d, %d\n", fd, fd_cli);

    gLedView = CreateLedView();
    if (!gLedView) {
        fprintf(stderr, "Could not create led view\n");
        return EXIT_FAILURE;
    }

    gLedView->Update(gLedState);

    // Writes of PIPE_BUF size are guaranteed to be atomic as long as they are done in a batch
    // PIPE_BUF itself is guaranteed to be at least 512 bytes.

    std::string accum; // Accumulated input since last read in case command is fragmented
    while(1) 
    {
        char buf[PIPE_BUF + 1] = {0}; // +1 to guarantee NULL terminator
        ssize_t nbytes = 0;

        nbytes = read(fd, buf, PIPE_BUF);
        if (nbytes < 0) {
            perror("read");
            return EXIT_FAILURE;
        }

        printf("Read: %s", buf);

        // Construct new buffer and scan for requests
        std::string str = accum + buf;
        size_t pos = 0;
        while(1) {
            size_t end = str.find('\n', pos);
            if (end == std::string::npos) {
                // We've reached the end of the buffer without seeing full request
                // Store remaining data in accumulator buffer and continue
                accum = str.substr(pos);
                break;
            }

            std::string response;
            if (DispatchRequest(str.substr(pos, end - pos), response)) {
                // Try and do a single write
                std::string output = LEDSRV_STATUS_OK;
                if (response.length() > 0) {
                    output.append(" ");
                    output.append(response);
                }
                output.append("\n");

                printf("%s", output.c_str());
                nbytes = write(fd, output.c_str(), output.length()); 
            } else {
                const char* failed = LEDSRV_STATUS_FAILED "\n";
                printf("%s", failed);
                nbytes = write(fd, failed, strlen(failed));
            }

            if (nbytes < 0) {
                perror("write");
                return EXIT_FAILURE;
            }

            fsync(fd_cli);
            pos = end + 1;
        }
    }

    return err;
}

