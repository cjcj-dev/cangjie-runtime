// Test-side stderr capture only; all records are emitted by the linked product SO.
#ifndef GC_UNIT_GCLOG_CAPTURE_HPP
#define GC_UNIT_GCLOG_CAPTURE_HPP
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include "gc_unittest.hpp"
class GcLogCapture {
    FILE* file;
    int saved;
public:
    GcLogCapture() : file(tmpfile()), saved(dup(STDERR_FILENO))
    {
        GC_EXPECT_TRUE(file != nullptr && saved >= 0);
        std::fflush(stderr);
        GC_EXPECT_TRUE(dup2(fileno(file), STDERR_FILENO) >= 0);
    }
    std::string Finish()
    {
        std::fflush(stderr);
        GC_EXPECT_TRUE(dup2(saved, STDERR_FILENO) >= 0);
        close(saved);
        saved = -1;
        rewind(file);
        std::string result;
        char buffer[4096];
        while (size_t n = fread(buffer, 1, sizeof(buffer), file)) result.append(buffer, n);
        fclose(file);
        file = nullptr;
        // Keep the exact product input for the Python consumer and red-arm evidence.
        std::fwrite(result.data(), 1, result.size(), stderr);
        return result;
    }
    ~GcLogCapture()
    {
        if (saved >= 0) { dup2(saved, STDERR_FILENO); close(saved); }
        if (file != nullptr) fclose(file);
    }
};
#endif
