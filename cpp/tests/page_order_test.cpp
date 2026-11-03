#include <cstdlib>
#include <iostream>

#include "options.h"

namespace {

void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

}

int main() {
    using hardcopy::outputPageIndex0;
    expect(outputPageIndex0(0, 5, false) == 0, "forward keeps first page");
    expect(outputPageIndex0(4, 5, false) == 4, "forward keeps last page");
    expect(outputPageIndex0(0, 5, true) == 4, "reverse sends first page last");
    expect(outputPageIndex0(4, 5, true) == 0, "reverse sends last page first");
    expect(outputPageIndex0(2, 5, true) == 2, "reverse keeps the middle of odd counts");
    expect(outputPageIndex0(0, 1, true) == 0, "single page is unchanged");
    expect(outputPageIndex0(outputPageIndex0(3, 8, true), 8, true) == 3,
           "reverse is an involution");
    std::cout << "page order regression passed\n";
    return 0;
}
