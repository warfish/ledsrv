#include "ledsrv.h"

#include <iostream>

/**
 * \brief   Simplest possible LED view dumps state to stdout
 */
class LedViewStdout : public ILedView
{
public:

    void Update(const LedState& state) override
    {
        std::cout << "{ "
                  << (state.state ? "on" : "off") 
                  << ", "
                  << (state.color == LedColor::Red ? "red" : (state.color == LedColor::Blue ? "blue" : "green")) 
                  << ", "
                  << state.rate 
                  << "} "
                  << std::endl;
    }
};

std::unique_ptr<ILedView> CreateLedView(void)
{
    return std::unique_ptr<ILedView>(new LedViewStdout);
}
