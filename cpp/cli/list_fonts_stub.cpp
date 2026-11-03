#include <cstdio>
#include <string>

int printDWriteFonts(const std::string&, FILE* output) {
    if (output) std::fprintf(output, "font listing is available on Windows only\n");
    return 0;
}

int printGdiFonts(const std::string&, FILE* output) {
    if (output) std::fprintf(output, "font listing is available on Windows only\n");
    return 0;
}
