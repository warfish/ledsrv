#pragma once

#include <boost/noncopyable.hpp>
#include <memory>

#define LEDSRV_FIFO_NAME            "/tmp/ledsrv"
#define LEDSRV_IN_FIFO              "/tmp/ledsrv.in.%d"
#define LEDSRV_OUT_FIFO             "/tmp/ledsrv.out.%d"
#define LEDSRV_STATUS_OK            "OK"
#define LEDSRV_STATUS_FAILED        "FAILED"

/**
 * \brief   Possible LED colors
 */
enum class LedColor 
{
    Red = 0,
    Green,
    Blue,
};

/**
 * \brief   Led state description
 */
struct LedState 
{
    bool state;         // On/Off
    LedColor color;     // Current color
    unsigned rate;      // Blink rate in HZ [0..5]
};

/**
 * \brief   Led view interface. 
 *          Abstracts led display.
 */
class ILedView : boost::noncopyable 
{
public:

    /**
     * \brief   Update display based on new led state
     */
    virtual void Update(const LedState& state) = 0;
    virtual ~ILedView() {};
};

/**
 * \brief   Create whatever led view we want to create.
 *          Implemented once by current view we link with since we only support a single view for now.
 */  
extern std::unique_ptr<ILedView> CreateLedView(void);
