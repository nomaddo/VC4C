/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef REGRESSIONTEST_H
#define REGRESSIONTEST_H

#include "cpptest.h"
#include "Compiler.h"

class RegressionTest : public Test::Suite
{
public:
    RegressionTest(const vc4c::Frontend frontend, bool onlyRegressions = false, bool onlyFast = false);
    RegressionTest(const vc4c::Frontend frontend, std::string substring);
    ~RegressionTest() override;
    
    void testRegression(std::string clFile, std::string options, vc4c::Frontend frontend);
    
    void testPending(std::string clFile, std::string options, vc4c::Frontend frontend);
    void testSlowPending(std::string clFile, std::string options, vc4c::Frontend frontend);

    void printProfilingInfo();
};

using Entry = std::tuple<uint8_t, uint8_t, std::string, std::string>;

extern std::vector<std::vector<Entry>> kernelList;

#endif /* REGRESSIONTEST_H */

