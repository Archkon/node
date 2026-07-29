#ifndef SRC_INSPECTOR_INSPECTOR_TRACE_H_
#define SRC_INSPECTOR_INSPECTOR_TRACE_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "v8-inspector.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace node {
namespace inspector {
namespace inspector_trace {

void Record(const char* event, int session_id, size_t bytes = 0);
void RecordState(const char* event, int session_id, int64_t value);
void RecordMessage(const char* event,
                   int session_id,
                   std::string_view message);
void RecordMessage(const char* event,
                   int session_id,
                   const v8_inspector::StringView& message);
void Dump(const char* reason, int signo = 0);
void DumpOnWait(const char* reason);

}  // namespace inspector_trace
}  // namespace inspector
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_INSPECTOR_INSPECTOR_TRACE_H_
