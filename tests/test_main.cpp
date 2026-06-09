#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Alryn/Core/Log.h>

int main(int argc, char** argv) {
    // Keep the engine quiet during tests; bump with --log on the cmdline if needed.
    alryn::Log::init(alryn::LogLevel::Warn);

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int result = context.run();
    return result;
}
