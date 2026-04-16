/* config.h for Quanta PCRE2 integration */

#define HAVE_ASSERT_H 1
#define HAVE_MEMMOVE 1
#define HAVE_STRERROR 1
#define HAVE_WINDOWS_H 1

#define HAVE_BUILTIN_ASSUME 1
#define HAVE_BUILTIN_MUL_OVERFLOW 1
#define HAVE_BUILTIN_UNREACHABLE 1

#define SUPPORT_PCRE2_8 1
#define SUPPORT_UNICODE 1
#define SUPPORT_JIT 1

#define PCRE2_EXPORT
#define LINK_SIZE               2
#define HEAP_LIMIT              20000000
#define MATCH_LIMIT             10000000
#define MATCH_LIMIT_DEPTH       10000000
#define MAX_VARLOOKBEHIND       255
#define NEWLINE_DEFAULT         5
#define PARENS_NEST_LIMIT       250
#define PCRE2GREP_BUFSIZE       20480
#define PCRE2GREP_MAX_BUFSIZE   1048576
#define MAX_NAME_SIZE           128
#define MAX_NAME_COUNT          10000