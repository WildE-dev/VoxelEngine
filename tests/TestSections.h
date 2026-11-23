#pragma once
// Running part of a long test, so ctest can run its parts side by side: each part is a
// named group of sections, and `SomeTest <group>` runs only that group (CMakeLists.txt
// registers one ctest test per group). With no argument, everything runs, as before.
#include <iostream>
#include <string>

class TestSections
{
public:
    TestSections(int argc, char** argv) : only(argc > 1 ? argv[1] : "") {}

    // Whether to run this group's sections.
    bool Run(const char* group) {
        if (only.empty()) return true;
        if (only != group) return false;
        matched = true;
        return true;
    }
    // Whether any of these groups runs (to skip setting up a server none of them uses).
    template <typename... Groups>
    bool Any(Groups... groups) {
        bool any = false;
        ((any = Run(groups) || any), ...);
        return any;
    }
    // False when a group was asked for that the test doesn't have (a typo would
    // otherwise run nothing and pass).
    bool Valid() const {
        if (only.empty() || matched) return true;
        std::cout << "  FAIL  there's no group called \"" << only << "\" in this test" << std::endl;
        return false;
    }

private:
    std::string only;
    bool matched = false;
};
