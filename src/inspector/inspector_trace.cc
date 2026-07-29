#include "inspector/inspector_trace.h"

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "uv.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>

#ifdef __POSIX__
#include <signal.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <io.h>
#endif

namespace node {
namespace inspector {
namespace inspector_trace {
namespace {

constexpr size_t kDefaultLimit = 512;
constexpr size_t kMaxLimit = 4096;
constexpr size_t kMaxEventLength = 40;
constexpr size_t kMaxMethodLength = 80;
constexpr size_t kMaxReasonLength = 80;
constexpr size_t kMaxProtocolScanLength = 16 * 1024;
constexpr size_t kMaxDirectoryLength = 1024;
constexpr size_t kMaxPathLength = kMaxDirectoryLength + 128;

struct TraceEntry {
  std::atomic<uint32_t> sequence;
  uint64_t timestamp;
  int pid;
  int session_id;
  size_t bytes;
  int64_t value;
  int protocol_id;
  char event[kMaxEventLength];
  char method[kMaxMethodLength];
  char reason[kMaxReasonLength];
};

struct TraceState {
  std::atomic<uint32_t> next_sequence{0};
  std::atomic<uint32_t> dump_counter{0};
  std::atomic<bool> dumping{false};
  bool enabled = false;
  bool dump_on_signal = false;
  bool dump_on_exit = false;
  bool dump_on_wait = false;
  int pid = 0;
  size_t limit = kDefaultLimit;
  char directory[kMaxDirectoryLength] = { 0 };
  TraceEntry entries[kMaxLimit] = {};
#ifdef __POSIX__
  struct sigaction previous_sigterm = {};
  struct sigaction previous_sigabrt = {};
#endif
};

struct PendingEntry {
  TraceEntry* entry;
  uint32_t sequence;
};

TraceState trace_state;
std::once_flag initialize_once;

int CurrentPid() {
  return static_cast<int>(uv_os_getpid());
}

void CopyCString(char* dest, size_t dest_len, const char* source) {
  if (dest_len == 0) return;
  if (source == nullptr) {
    dest[0] = '\0';
    return;
  }
  size_t i = 0;
  for (; i + 1 < dest_len && source[i] != '\0'; i++) {
    dest[i] = source[i];
  }
  dest[i] = '\0';
}

template <typename Char>
bool MatchesAt(const Char* data,
               size_t length,
               size_t offset,
               const char* token) {
  for (size_t i = 0; token[i] != '\0'; i++) {
    if (offset + i >= length ||
        static_cast<unsigned char>(data[offset + i]) !=
            static_cast<unsigned char>(token[i])) {
      return false;
    }
  }
  return true;
}

template <typename Char>
bool ExtractJsonString(const Char* data,
                       size_t length,
                       const char* token,
                       char* out,
                       size_t out_len) {
  if (out_len == 0) return false;
  out[0] = '\0';
  for (size_t i = 0; i < length; i++) {
    if (!MatchesAt(data, length, i, token)) continue;
    i += std::strlen(token);
    size_t out_index = 0;
    for (; i < length; i++) {
      const uint32_t ch = static_cast<uint32_t>(data[i]);
      if (ch == '"') break;
      if (out_index + 1 < out_len) {
        out[out_index++] = ch < 0x80 ? static_cast<char>(ch) : '?';
      }
      if (ch == '\\' && i + 1 < length) {
        i++;
        const uint32_t escaped = static_cast<uint32_t>(data[i]);
        if (out_index + 1 < out_len) {
          out[out_index++] =
              escaped < 0x80 ? static_cast<char>(escaped) : '?';
        }
      }
    }
    out[out_index] = '\0';
    return true;
  }
  return false;
}

template <typename Char>
int ExtractJsonInt(const Char* data, size_t length, const char* token) {
  for (size_t i = 0; i < length; i++) {
    if (!MatchesAt(data, length, i, token)) continue;
    i += std::strlen(token);
    while (i < length && data[i] == ' ') i++;
    int value = 0;
    bool has_digit = false;
    for (; i < length; i++) {
      const uint32_t ch = static_cast<uint32_t>(data[i]);
      if (ch < '0' || ch > '9') break;
      has_digit = true;
      value = value * 10 + static_cast<int>(ch - '0');
    }
    return has_digit ? value : -1;
  }
  return -1;
}

template <typename Char>
void SummarizeProtocolMessage(const Char* data,
                              size_t length,
                              TraceEntry* entry) {
  length = std::min(length, kMaxProtocolScanLength);
  entry->protocol_id = ExtractJsonInt(data, length, "\"id\":");
  ExtractJsonString(data,
                    length,
                    "\"method\":\"",
                    entry->method,
                    sizeof(entry->method));
  ExtractJsonString(data,
                    length,
                    "\"reason\":\"",
                    entry->reason,
                    sizeof(entry->reason));
}

void WriteAll(int fd, const char* data, size_t length) {
#ifdef __POSIX__
  while (length > 0) {
    ssize_t written = write(fd, data, length);
    if (written < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (written == 0) return;
    data += written;
    length -= static_cast<size_t>(written);
  }
#elif defined(_WIN32)
  while (length > 0) {
    const unsigned int chunk =
        static_cast<unsigned int>(std::min<size_t>(length, 64 * 1024));
    int written = _write(fd, data, chunk);
    if (written < 0) return;
    if (written == 0) return;
    data += written;
    length -= static_cast<size_t>(written);
  }
#else
  return;
#endif
}

void WriteLiteral(int fd, const char* data) {
  WriteAll(fd, data, std::strlen(data));
}

void WriteUnsigned(int fd, uint64_t value) {
  char buffer[32];
  size_t index = sizeof(buffer);
  buffer[--index] = '\0';
  do {
    buffer[--index] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value > 0);
  WriteLiteral(fd, &buffer[index]);
}

void WriteSigned(int fd, int64_t value) {
  if (value < 0) {
    WriteLiteral(fd, "-");
    const uint64_t absolute =
        static_cast<uint64_t>(-(value + 1)) + 1;
    WriteUnsigned(fd, absolute);
    return;
  }
  WriteUnsigned(fd, static_cast<uint64_t>(value));
}

void WriteJsonString(int fd, const char* value) {
  WriteLiteral(fd, "\"");
  for (size_t i = 0; value[i] != '\0'; i++) {
    switch (value[i]) {
      case '\\':
        WriteLiteral(fd, "\\\\");
        break;
      case '"':
        WriteLiteral(fd, "\\\"");
        break;
      case '\n':
        WriteLiteral(fd, "\\n");
        break;
      case '\r':
        WriteLiteral(fd, "\\r");
        break;
      case '\t':
        WriteLiteral(fd, "\\t");
        break;
      default:
        WriteAll(fd, &value[i], 1);
        break;
    }
  }
  WriteLiteral(fd, "\"");
}

size_t AppendCString(char* out, size_t offset, size_t max, const char* value) {
  while (offset + 1 < max && value != nullptr && *value != '\0') {
    out[offset++] = *value++;
  }
  out[offset] = '\0';
  return offset;
}

size_t AppendUnsigned(char* out, size_t offset, size_t max, uint64_t value) {
  char buffer[32];
  size_t index = sizeof(buffer);
  buffer[--index] = '\0';
  do {
    buffer[--index] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value > 0);
  return AppendCString(out, offset, max, &buffer[index]);
}

void BuildDumpPath(char* out,
                   size_t out_len,
                   const char* reason,
                   int signo,
                   uint32_t dump_id) {
  size_t offset = 0;
  offset = AppendCString(out, offset, out_len, trace_state.directory);
  if (offset > 0 && offset + 1 < out_len &&
      out[offset - 1] != '/' && out[offset - 1] != '\\') {
#ifdef _WIN32
    out[offset++] = '\\';
#else
    out[offset++] = '/';
#endif
    out[offset] = '\0';
  }
  offset = AppendCString(out, offset, out_len, "native-inspect-trace-");
  offset = AppendUnsigned(out, offset, out_len, trace_state.pid);
  offset = AppendCString(out, offset, out_len, "-");
  offset = AppendCString(out, offset, out_len, reason);
  if (signo != 0) {
    offset = AppendCString(out, offset, out_len, "-sig");
    offset = AppendUnsigned(out, offset, out_len, static_cast<uint64_t>(signo));
  }
  offset = AppendCString(out, offset, out_len, "-");
  offset = AppendUnsigned(out, offset, out_len, dump_id);
  AppendCString(out, offset, out_len, ".jsonl");
}

int OpenDumpFile(const char* path) {
#ifdef __POSIX__
  return open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
#elif defined(_WIN32)
  return _open(path, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  return -1;
#endif
}

void CloseDumpFile(int fd) {
#ifdef __POSIX__
  close(fd);
#elif defined(_WIN32)
  _close(fd);
#endif
}

void DumpUnlocked(const char* reason, int signo) {
  if (!trace_state.enabled || trace_state.directory[0] == '\0') return;
  const uint32_t written =
      trace_state.next_sequence.load(std::memory_order_acquire);
  if (written == 0) return;

  char path[kMaxPathLength];
  const uint32_t dump_id =
      trace_state.dump_counter.fetch_add(1, std::memory_order_relaxed) + 1;
  BuildDumpPath(path, sizeof(path), reason, signo, dump_id);
  int fd = OpenDumpFile(path);
  if (fd < 0) return;

  WriteLiteral(fd, "{\"reason\":");
  WriteJsonString(fd, reason);
  WriteLiteral(fd, ",\"signo\":");
  WriteSigned(fd, signo);
  WriteLiteral(fd, ",\"pid\":");
  WriteUnsigned(fd, trace_state.pid);
  WriteLiteral(fd, ",\"events\":");
  WriteUnsigned(fd, written);
  WriteLiteral(fd, "}\n");

  const uint32_t first =
      written > trace_state.limit ? written - trace_state.limit + 1 : 1;
  for (uint32_t sequence = first; sequence <= written; sequence++) {
    const TraceEntry& entry =
        trace_state.entries[(sequence - 1) % trace_state.limit];
    const uint32_t entry_sequence =
        entry.sequence.load(std::memory_order_acquire);
    if (entry_sequence != sequence) continue;
    WriteLiteral(fd, "{\"seq\":");
    WriteUnsigned(fd, entry_sequence);
    WriteLiteral(fd, ",\"t\":");
    WriteUnsigned(fd, entry.timestamp);
    WriteLiteral(fd, ",\"pid\":");
    WriteSigned(fd, entry.pid);
    WriteLiteral(fd, ",\"event\":");
    WriteJsonString(fd, entry.event);
    WriteLiteral(fd, ",\"session\":");
    WriteSigned(fd, entry.session_id);
    WriteLiteral(fd, ",\"bytes\":");
    WriteUnsigned(fd, entry.bytes);
    WriteLiteral(fd, ",\"value\":");
    WriteSigned(fd, entry.value);
    WriteLiteral(fd, ",\"id\":");
    WriteSigned(fd, entry.protocol_id);
    if (entry.method[0] != '\0') {
      WriteLiteral(fd, ",\"method\":");
      WriteJsonString(fd, entry.method);
    }
    if (entry.reason[0] != '\0') {
      WriteLiteral(fd, ",\"reasonText\":");
      WriteJsonString(fd, entry.reason);
    }
    WriteLiteral(fd, "}\n");
  }
  CloseDumpFile(fd);
}

void DumpAtExit() {
  Dump("exit");
}

#ifdef __POSIX__
struct sigaction* PreviousActionForSignal(int signo) {
  switch (signo) {
    case SIGTERM:
      return &trace_state.previous_sigterm;
    case SIGABRT:
      return &trace_state.previous_sigabrt;
    default:
      return nullptr;
  }
}

void SignalDumpHandler(int signo, siginfo_t* info, void* ucontext) {
  (void) info;
  (void) ucontext;
  if (!trace_state.dumping.exchange(true, std::memory_order_acq_rel)) {
    DumpUnlocked("signal", signo);
  }

  struct sigaction* previous = PreviousActionForSignal(signo);
  if (previous != nullptr) {
    sigaction(signo, previous, nullptr);
  } else {
    struct sigaction fallback;
    std::memset(&fallback, 0, sizeof(fallback));
    fallback.sa_handler = SIG_DFL;
    sigaction(signo, &fallback, nullptr);
  }
  raise(signo);
}

void InstallSignalHandler(int signo, struct sigaction* previous) {
  struct sigaction handler;
  std::memset(&handler, 0, sizeof(handler));
  handler.sa_sigaction = SignalDumpHandler;
  handler.sa_flags = SA_SIGINFO;
  sigfillset(&handler.sa_mask);
  sigaction(signo, &handler, previous);
}
#endif  // __POSIX__

void Initialize() {
  const char* enabled = std::getenv("NODE_INSPECT_TRACE");
  trace_state.enabled = enabled != nullptr && std::strcmp(enabled, "1") == 0;
  if (!trace_state.enabled) return;
  trace_state.pid = CurrentPid();

  const char* limit = std::getenv("NODE_INSPECT_TRACE_LIMIT");
  if (limit != nullptr && limit[0] != '\0') {
    const long parsed = std::strtol(limit, nullptr, 10);
    if (parsed > 0) {
      trace_state.limit =
          std::min<size_t>(static_cast<size_t>(parsed), kMaxLimit);
    }
  }

  const char* dir = std::getenv("NODE_INSPECT_DEBUG_LOG_DIR");
  if (dir != nullptr && dir[0] != '\0') {
    CopyCString(trace_state.directory, sizeof(trace_state.directory), dir);
  }

  const char* dump_on_exit =
      std::getenv("NODE_INSPECT_TRACE_DUMP_ON_EXIT");
  trace_state.dump_on_exit =
      dump_on_exit != nullptr && std::strcmp(dump_on_exit, "1") == 0;
  if (trace_state.dump_on_exit) {
    std::atexit(DumpAtExit);
  }

  const char* dump_on_wait =
      std::getenv("NODE_INSPECT_TRACE_DUMP_ON_WAIT");
  trace_state.dump_on_wait =
      dump_on_wait != nullptr && std::strcmp(dump_on_wait, "1") == 0;

  const char* dump_on_signal =
      std::getenv("NODE_INSPECT_NATIVE_TRACE_DUMP_ON_SIGNAL");
  trace_state.dump_on_signal =
      dump_on_signal != nullptr && std::strcmp(dump_on_signal, "1") == 0;
#ifdef __POSIX__
  if (trace_state.dump_on_signal) {
    InstallSignalHandler(SIGTERM, &trace_state.previous_sigterm);
    InstallSignalHandler(SIGABRT, &trace_state.previous_sigabrt);
  }
#endif
}

PendingEntry NewEntry(const char* event, int session_id, size_t bytes) {
  std::call_once(initialize_once, Initialize);
  if (!trace_state.enabled) return { nullptr, 0 };
  if (trace_state.dumping.load(std::memory_order_acquire)) {
    return { nullptr, 0 };
  }

  const uint32_t sequence =
      trace_state.next_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  TraceEntry* entry = &trace_state.entries[(sequence - 1) % trace_state.limit];
  entry->sequence.store(0, std::memory_order_relaxed);
  entry->timestamp = uv_hrtime();
  entry->pid = trace_state.pid;
  entry->session_id = session_id;
  entry->bytes = bytes;
  entry->value = 0;
  entry->protocol_id = -1;
  CopyCString(entry->event, sizeof(entry->event), event);
  entry->method[0] = '\0';
  entry->reason[0] = '\0';
  return { entry, sequence };
}

void CommitEntry(PendingEntry pending) {
  if (pending.entry == nullptr) return;
  pending.entry->sequence.store(pending.sequence, std::memory_order_release);
}

}  // namespace

void Record(const char* event, int session_id, size_t bytes) {
  CommitEntry(NewEntry(event, session_id, bytes));
}

void RecordState(const char* event, int session_id, int64_t value) {
  PendingEntry pending = NewEntry(event, session_id, 0);
  if (pending.entry == nullptr) return;
  pending.entry->value = value;
  CommitEntry(pending);
}

void RecordMessage(const char* event,
                   int session_id,
                   std::string_view message) {
  PendingEntry pending = NewEntry(event, session_id, message.size());
  if (pending.entry == nullptr) return;
  SummarizeProtocolMessage(message.data(), message.size(), pending.entry);
  CommitEntry(pending);
}

void RecordMessage(const char* event,
                   int session_id,
                   const v8_inspector::StringView& message) {
  PendingEntry pending = NewEntry(event, session_id, message.length());
  if (pending.entry == nullptr) return;
  if (message.is8Bit()) {
    SummarizeProtocolMessage(
        message.characters8(), message.length(), pending.entry);
  } else {
    SummarizeProtocolMessage(
        message.characters16(), message.length(), pending.entry);
  }
  CommitEntry(pending);
}

void Dump(const char* reason, int signo) {
  std::call_once(initialize_once, Initialize);
  if (!trace_state.enabled) return;
  if (trace_state.dumping.exchange(true, std::memory_order_acq_rel)) return;
  DumpUnlocked(reason, signo);
  trace_state.dumping.store(false, std::memory_order_release);
}

void DumpOnWait(const char* reason) {
  std::call_once(initialize_once, Initialize);
  if (!trace_state.enabled || !trace_state.dump_on_wait) return;
  Dump(reason);
}

}  // namespace inspector_trace
}  // namespace inspector
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
