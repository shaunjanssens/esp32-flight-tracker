#include "log.h"

#include <stdarg.h>

namespace app {
namespace {

constexpr size_t kBufferSize = 4096;

char g_buffer[kBufferSize];
size_t g_head = 0;          // next write position
bool g_wrapped = false;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void append(const char *text, size_t length)
{
    portENTER_CRITICAL(&g_mux);
    for (size_t i = 0; i < length; i++) {
        g_buffer[g_head] = text[i];
        g_head = (g_head + 1) % kBufferSize;
        if (g_head == 0) {
            g_wrapped = true;
        }
    }
    portEXIT_CRITICAL(&g_mux);
}

} // namespace

void logf(const char *format, ...)
{
    char line[192];
    const int prefix = snprintf(line, sizeof(line), "[%7lu] ", (unsigned long)millis());

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(line + prefix, sizeof(line) - prefix - 2, format, args);
    va_end(args);

    size_t length = prefix + (written > 0 ? (size_t)written : 0);
    if (length > sizeof(line) - 2) {
        length = sizeof(line) - 2;
    }
    line[length++] = '\n';
    line[length] = '\0';

    Serial.print(line);
    append(line, length);
}

String logDump()
{
    String out;
    portENTER_CRITICAL(&g_mux);
    const size_t start = g_wrapped ? g_head : 0;
    const size_t count = g_wrapped ? kBufferSize : g_head;
    portEXIT_CRITICAL(&g_mux);

    out.reserve(count + 1);
    for (size_t i = 0; i < count; i++) {
        out += g_buffer[(start + i) % kBufferSize];
    }
    return out;
}

} // namespace app
