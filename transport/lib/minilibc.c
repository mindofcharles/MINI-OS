#include "minilibc.h"
#include "platform.h"
#include <stdarg.h>
#include <limits.h>

#define SYSCALL_CONST(name, value) enum { name = value };
#include "syscall.def"
#undef SYSCALL_CONST

void exit(int status) {
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_NR_EXIT), "b"(status)
        : "memory"
    );
}

int write(int fd, const char *buf, unsigned int count) {
    int ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_NR_WRITE), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return ret;
}

int read(int fd, char *buf, unsigned int count) {
    int ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_NR_READ), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return ret;
}

int getchar(void) {
    int key;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(key)
        : "a"(SYS_NR_GETKEY)
        : "memory"
    );
    return key;
}

unsigned int clock_monotonic_ms(void) {
    unsigned int milliseconds;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(milliseconds)
        : "a"(SYS_NR_CLOCK_MONOTONIC_MS)
        : "memory"
    );
    return milliseconds;
}

int kbd_poll_key(void) {
    int key;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(key)
        : "a"(SYS_NR_KBD_POLL_KEY)
        : "memory"
    );
    return key;
}

int get_random(void *buffer, unsigned int length) {
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_NR_GET_RANDOM), "b"(buffer), "c"(length)
        : "memory"
    );
    return result;
}


int putchar(int c) {
    char ch = (char)c;
    if (write(1, &ch, 1) != 1) return EOF;
    return (unsigned char)c;
}

void move_cursor(int row, int col) {
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_NR_MOVE_CURSOR), "b"(row), "c"(col)
        : "memory"
    );
}

void set_cursor(int row, int col) {
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_NR_SET_CURSOR), "b"(row), "c"(col)
        : "memory"
    );
}

void clear_screen(void) {
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_NR_CLEAR_SCREEN)
        : "memory"
    );
}

void save_screen(void) {
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_NR_SAVE_SCREEN)
        : "memory"
    );
}

void restore_screen(void) {
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_NR_RESTORE_SCREEN)
        : "memory"
    );
}

int get_cursor_position(void) {
    int position;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(position)
        : "a"(SYS_NR_GET_CURSOR)
        : "memory"
    );
    return position;
}



char *fgets(char *s, int size, void *stream) {
    int i = 0;
    char c;
    FILE *fp = (FILE *)stream;

    if (!s || size <= 1) return 0;

    if (stream == 0 || stream == stdin) {
        int count = read(0, s, (unsigned int)(size - 1));
        if (count < 0) return 0;
        s[count] = '\0';
        return s;
    }

    while (i < size - 1) {
        if (fread(&c, 1, 1, fp) != 1) {
            if (i == 0) return 0;
            break;
        }
        s[i++] = c;
        if (c == '\n') break;
    }

    s[i] = '\0';
    return s;
}


char *gets(char *buf) {
    return fgets(buf, 128, stdin);
}


size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    size_t i;
    while (*d) d++;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        *d++ = src[i];
    }
    *d = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    while (--n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    if ((char)c == '\0') return (char *)s;
    return 0;
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    size_t i;
    if (!*needle) return (char *)haystack;
    while (*haystack) {
        for (i = 0; needle[i] && haystack[i] == needle[i]; i++);
        if (!needle[i]) return (char *)haystack;
        haystack++;
    }
    return 0;
}

static char *strtok_saved = 0;
char *strtok(char *str, const char *delim) {
    char *token_start;
    if (!str) str = strtok_saved;
    if (!str) return 0;

    while (*str && strchr(delim, *str)) str++;
    if (!*str) {
        strtok_saved = 0;
        return 0;
    }

    token_start = str;
    while (*str && !strchr(delim, *str)) str++;

    if (*str) {
        *str = '\0';
        strtok_saved = str + 1;
    } else {
        strtok_saved = 0;
    }
    return token_start;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return 0;
}

int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return (c >= '0' && c <= '9'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isprint(int c) { return c >= 32 && c <= 126; }
int isgraph(int c) { return c > 32 && c <= 126; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }

int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }



int puts(const char *str) {
    int len;
    if (!str) return EOF;
    len = (int)strlen(str);
    if (write(1, str, (unsigned int)len) != len) return EOF;
    if (write(1, "\n", 1) != 1) return EOF;
    return len + 1;
}

typedef int (*format_emit_fn)(void *context, char value);

struct format_buffer {
    char *destination;
    size_t capacity;
    size_t position;
};

static int emit_console(void *context, char value) {
    (void)context;
    return write(1, &value, 1) == 1 ? 0 : -1;
}

static int emit_buffer(void *context, char value) {
    struct format_buffer *buffer = (struct format_buffer *)context;
    if (buffer->capacity > 0 && buffer->position < buffer->capacity - 1) {
        buffer->destination[buffer->position] = value;
    }
    buffer->position++;
    return 0;
}

static int format_emit_char(format_emit_fn emit, void *context, char value,
                            int *count) {
    if (*count == INT_MAX || emit(context, value) < 0) return -1;
    (*count)++;
    return 0;
}

static int format_emit_text(format_emit_fn emit, void *context,
                            const char *text, int *count) {
    while (*text) {
        if (format_emit_char(emit, context, *text++, count) < 0) return -1;
    }
    return 0;
}

static int format_emit_unsigned(format_emit_fn emit, void *context,
                                unsigned long value, unsigned int base,
                                int *count) {
    char digits[32];
    int used = 0;
    const char *alphabet = "0123456789abcdef";

    do {
        digits[used++] = alphabet[value % base];
        value /= base;
    } while (value != 0);

    while (used > 0) {
        if (format_emit_char(emit, context, digits[--used], count) < 0) {
            return -1;
        }
    }
    return 0;
}

static unsigned long signed_magnitude(long value) {
    if (value < 0) return 0UL - (unsigned long)value;
    return (unsigned long)value;
}

static int format_core(format_emit_fn emit, void *context, const char *fmt,
                       va_list args) {
    int count = 0;

    if (!fmt) return -1;
    while (*fmt) {
        int long_argument = 0;
        char conversion;

        if (*fmt != '%') {
            if (format_emit_char(emit, context, *fmt++, &count) < 0) return -1;
            continue;
        }

        fmt++;
        if (*fmt == '\0') {
            if (format_emit_char(emit, context, '%', &count) < 0) return -1;
            break;
        }
        if (*fmt == 'l') {
            long_argument = 1;
            fmt++;
        }
        conversion = *fmt++;

        if (conversion == 'c') {
            if (format_emit_char(emit, context,
                                 (char)va_arg(args, int), &count) < 0) {
                return -1;
            }
        } else if (conversion == 's') {
            const char *text = va_arg(args, const char *);
            if (!text) text = "(null)";
            if (format_emit_text(emit, context, text, &count) < 0) return -1;
        } else if (conversion == 'd' || conversion == 'i') {
            long value = long_argument ? va_arg(args, long)
                                       : (long)va_arg(args, int);
            if (value < 0 &&
                format_emit_char(emit, context, '-', &count) < 0) {
                return -1;
            }
            if (format_emit_unsigned(emit, context, signed_magnitude(value),
                                     10, &count) < 0) {
                return -1;
            }
        } else if (conversion == 'u') {
            unsigned long value = long_argument
                                      ? va_arg(args, unsigned long)
                                      : (unsigned long)va_arg(args, unsigned int);
            if (format_emit_unsigned(emit, context, value, 10, &count) < 0) {
                return -1;
            }
        } else if (conversion == 'x') {
            unsigned long value = long_argument
                                      ? va_arg(args, unsigned long)
                                      : (unsigned long)va_arg(args, unsigned int);
            if (format_emit_unsigned(emit, context, value, 16, &count) < 0) {
                return -1;
            }
        } else if (conversion == 'p') {
            unsigned long value = (unsigned long)va_arg(args, void *);
            if (format_emit_unsigned(emit, context, value, 16, &count) < 0) {
                return -1;
            }
        } else if (conversion == '%') {
            if (format_emit_char(emit, context, '%', &count) < 0) return -1;
        } else {
            if (format_emit_char(emit, context, '%', &count) < 0) return -1;
            if (long_argument &&
                format_emit_char(emit, context, 'l', &count) < 0) {
                return -1;
            }
            if (format_emit_char(emit, context, conversion, &count) < 0) {
                return -1;
            }
        }
    }
    return count;
}

static void terminate_format_buffer(struct format_buffer *buffer) {
    size_t terminator;
    if (buffer->capacity == 0) return;
    terminator = buffer->position;
    if (terminator >= buffer->capacity) terminator = buffer->capacity - 1;
    buffer->destination[terminator] = '\0';
}

int printf(const char *fmt, ...) {
    va_list args;
    int result;
    va_start(args, fmt);
    result = format_core(emit_console, 0, fmt, args);
    va_end(args);
    return result;
}

int vsprintf(char *str, const char *fmt, va_list args) {
    struct format_buffer buffer;
    int result;
    if (!str) return -1;
    buffer.destination = str;
    buffer.capacity = (size_t)-1;
    buffer.position = 0;
    result = format_core(emit_buffer, &buffer, fmt, args);
    terminate_format_buffer(&buffer);
    return result;
}

int sprintf(char *str, const char *fmt, ...) {
    va_list args;
    int result;
    va_start(args, fmt);
    result = vsprintf(str, fmt, args);
    va_end(args);
    return result;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list args) {
    struct format_buffer buffer;
    int result;
    if (size > 0 && !str) return -1;
    buffer.destination = str;
    buffer.capacity = size;
    buffer.position = 0;
    result = format_core(emit_buffer, &buffer, fmt, args);
    terminate_format_buffer(&buffer);
    return result;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list args;
    int result;
    va_start(args, fmt);
    result = vsnprintf(str, size, fmt, args);
    va_end(args);
    return result;
}

/* ========================================================================= */
/* Phase 2: Heap Allocator & Standard Utilities                              */
/* ========================================================================= */

static void *sbrk(int increment) {
    void *old_brk;
    void *actual_brk;
    unsigned long old_address;
    unsigned long new_address;
    unsigned long magnitude;

    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(old_brk)
        : "a"(SYS_NR_BRK), "b"(0)
        : "memory"
    );

    if (increment == 0) return old_brk;

    old_address = (unsigned long)old_brk;
    if (increment > 0) {
        if (old_address > ULONG_MAX - (unsigned int)increment) {
            return (void *)-1;
        }
        new_address = old_address + (unsigned int)increment;
    } else {
        magnitude = 0UL - (unsigned long)increment;
        if (old_address < magnitude) return (void *)-1;
        new_address = old_address - magnitude;
    }

    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(actual_brk)
        : "a"(SYS_NR_BRK), "b"((void *)new_address)
        : "memory"
    );

    if ((unsigned long)actual_brk != new_address) return (void *)-1;
    return old_brk;
}

typedef long Align;

union header {
    struct {
        union header *ptr;
        unsigned int size;
    } s;
    Align x;
};

typedef union header Header;

static Header base;
static Header *freep = 0;

void free(void *ap) {
    Header *bp, *p;

    if (!ap) return;
    bp = (Header *)ap - 1;

    for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr) {
        if (p >= p->s.ptr && (bp > p || bp < p->s.ptr)) {
            break;
        }
    }

    if (bp + bp->s.size == p->s.ptr) {
        bp->s.size += p->s.ptr->s.size;
        bp->s.ptr = p->s.ptr->s.ptr;
    } else {
        bp->s.ptr = p->s.ptr;
    }

    if (p + p->s.size == bp) {
        p->s.size += bp->s.size;
        p->s.ptr = bp->s.ptr;
    } else {
        p->s.ptr = bp;
    }
    freep = p;
}

static Header *morecore(unsigned int nu) {
    char *cp;
    Header *up;

    if (nu < 1024) nu = 1024;
    if (nu > (unsigned int)INT_MAX / sizeof(Header)) return 0;
    cp = (char *)sbrk(nu * sizeof(Header));
    if (cp == (char *)-1 || !cp) return 0;

    up = (Header *)cp;
    up->s.size = nu;
    free((void *)(up + 1));
    return freep;
}

void *malloc(size_t nbytes) {
    Header *p, *prevp;
    unsigned int nunits;

    if (nbytes == 0) return 0;
    if (nbytes > (size_t)UINT_MAX - (sizeof(Header) - 1)) return 0;
    nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
    if (nunits == 0) return 0;

    if ((prevp = freep) == 0) {
        base.s.ptr = freep = prevp = &base;
        base.s.size = 0;
    }

    for (p = prevp->s.ptr; ; prevp = p, p = p->s.ptr) {
        if (p->s.size >= nunits) {
            if (p->s.size == nunits) {
                prevp->s.ptr = p->s.ptr;
            } else {
                p->s.size -= nunits;
                p += p->s.size;
                p->s.size = nunits;
            }
            freep = prevp;
            return (void *)(p + 1);
        }
        if (p == freep) {
            if ((p = morecore(nunits)) == 0) {
                return 0;
            }
        }
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t total;
    if (size != 0 && nmemb > (size_t)UINT_MAX / size) return 0;
    total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    Header *bp;
    size_t old_size;
    void *new_ptr;

    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return 0;
    }

    bp = (Header *)ptr - 1;
    old_size = (bp->s.size - 1) * sizeof(Header);
    if (size <= old_size) return ptr;

    new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size);
        free(ptr);
    }
    return new_ptr;
}

int atoi(const char *nptr) {
    int res = 0, sign = 1;
    while (isspace((unsigned char)*nptr)) nptr++;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') { nptr++; }
    while (isdigit((unsigned char)*nptr)) {
        res = res * 10 + (*nptr - '0');
        nptr++;
    }
    return res * sign;
}

long strtol(const char *nptr, char **endptr, int base) {
    long res = 0;
    int sign = 1;
    const char *p = nptr;

    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }

    if (base == 0) {
        if (*p == '0') {
            if (p[1] == 'x' || p[1] == 'X') { base = 16; p += 2; }
            else { base = 8; p++; }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    }

    while (*p) {
        int v;
        if (isdigit((unsigned char)*p)) v = *p - '0';
        else if (*p >= 'a' && *p <= 'z') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') v = *p - 'A' + 10;
        else break;
        if (v >= base) break;
        res = res * base + v;
        p++;
    }
    if (endptr) *endptr = (char *)p;
    return res * sign;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtol(nptr, endptr, base);
}

int abs(int j) {
    if (j >= 0) return j;
    return (int)(0U - (unsigned int)j);
}

long labs(long j) {
    if (j >= 0) return j;
    return (long)(0UL - (unsigned long)j);
}

static unsigned long next_rand = 1;
int rand(void) {
    next_rand = next_rand * 1103515245 + 12345;
    return (unsigned int)(next_rand / 65536) % 32768;
}
void srand(unsigned int seed) {
    next_rand = seed;
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    char *b = (char *)base;
    size_t i, j, byte_index;

    if (!base || !compar || nmemb < 2 || size == 0) return;
    if (nmemb > (size_t)UINT_MAX / size) return;
    for (i = 0; i < nmemb - 1; i++) {
        for (j = 0; j < nmemb - 1 - i; j++) {
            if (compar(b + j * size, b + (j + 1) * size) > 0) {
                char *left = b + j * size;
                char *right = left + size;
                for (byte_index = 0; byte_index < size; byte_index++) {
                    char temporary = left[byte_index];
                    left[byte_index] = right[byte_index];
                    right[byte_index] = temporary;
                }
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    size_t l = 0, r = nmemb;
    while (l < r) {
        size_t m = l + (r - l) / 2;
        const void *p = (const char *)base + m * size;
        int cmp = compar(key, p);
        if (cmp == 0) return (void *)p;
        if (cmp < 0) r = m;
        else l = m + 1;
    }
    return 0;
}

/* ========================================================================= */
/* Phase 4: File Stream I/O Implementations                                  */
/* ========================================================================= */

struct FILE {
    int fd;
    int flags;
    int eof;
    int error;
};

static int parse_fopen_mode(const char *mode, int *flags_out) {
    int flags;
    int plus_seen = 0;
    int binary_seen = 0;
    const char *cursor;

    if (!mode || !mode[0]) return -1;
    if (mode[0] == 'r') {
        flags = SYS_OPEN_READ;
    } else if (mode[0] == 'w') {
        flags = SYS_OPEN_WRITE | SYS_OPEN_CREATE | SYS_OPEN_TRUNCATE;
    } else if (mode[0] == 'a') {
        flags = SYS_OPEN_WRITE | SYS_OPEN_CREATE | SYS_OPEN_APPEND;
    } else {
        return -1;
    }

    for (cursor = mode + 1; *cursor; cursor++) {
        if (*cursor == '+' && !plus_seen) {
            flags |= SYS_OPEN_READ | SYS_OPEN_WRITE;
            plus_seen = 1;
        } else if (*cursor == 'b' && !binary_seen) {
            binary_seen = 1;
        } else {
            return -1;
        }
    }
    *flags_out = flags;
    return 0;
}

FILE *fopen(const char *filename, const char *mode) {
    int flags;
    int fd;
    FILE *f;

    if (!filename || parse_fopen_mode(mode, &flags) < 0) return NULL;

    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(fd)
        : "a"(SYS_NR_OPEN), "b"(filename), "c"(flags)
        : "memory"
    );

    if (fd < 0) return NULL;

    f = (FILE *)malloc(sizeof(FILE));
    if (!f) {
        __asm__ __volatile__ (
            "int $0x80"
            :
            : "a"(SYS_NR_CLOSE), "b"(fd)
            : "memory"
        );
        return NULL;
    }

    f->fd = fd;
    f->flags = flags;
    f->eof = 0;
    f->error = 0;

    return f;
}

int fclose(FILE *stream) {
    int ret;
    if (!stream) return EOF;

    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_NR_CLOSE), "b"(stream->fd)
        : "memory"
    );

    free(stream);
    return ret;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total_bytes;
    int bytes_read;

    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    if (nmemb > (size_t)UINT_MAX / size) {
        if (stream) stream->error = 1;
        return 0;
    }
    total_bytes = size * nmemb;
    if (total_bytes > (size_t)INT_MAX) {
        stream->error = 1;
        return 0;
    }

    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(bytes_read)
        : "a"(SYS_NR_READ_FILE), "b"(stream->fd), "c"(ptr), "d"(total_bytes)
        : "memory"
    );

    if (bytes_read < 0) {
        stream->error = 1;
        return 0;
    }
    if (bytes_read == 0) {
        stream->eof = 1;
        return 0;
    }
    if ((size_t)bytes_read < total_bytes) stream->eof = 1;

    return (size_t)(bytes_read / size);
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total_bytes;
    int bytes_written;

    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    if (nmemb > (size_t)UINT_MAX / size) {
        if (stream) stream->error = 1;
        return 0;
    }
    total_bytes = size * nmemb;
    if (total_bytes > (size_t)INT_MAX) {
        stream->error = 1;
        return 0;
    }

    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(bytes_written)
        : "a"(SYS_NR_WRITE_FILE), "b"(stream->fd), "c"(ptr), "d"(total_bytes)
        : "memory"
    );

    if (bytes_written < 0) {
        stream->error = 1;
        return 0;
    }
    if ((size_t)bytes_written < total_bytes) stream->error = 1;

    return (size_t)(bytes_written / size);
}

int fseek(FILE *stream, long offset, int whence) {
    int ret;
    if (!stream) return -1;

    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_NR_LSEEK), "b"(stream->fd), "c"(offset), "d"(whence)
        : "memory"
    );

    if (ret >= 0) {
        stream->eof = 0;
    } else {
        stream->error = 1;
    }
    return (ret >= 0) ? 0 : -1;
}

long ftell(FILE *stream) {
    long pos;
    if (!stream) return -1;

    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(pos)
        : "a"(SYS_NR_LSEEK), "b"(stream->fd), "c"(0), "d"(SEEK_CUR)
        : "memory"
    );

    if (pos < 0) stream->error = 1;
    return pos;
}

void rewind(FILE *stream) {
    fseek(stream, 0, SEEK_SET);
}

int fflush(FILE *stream) {
    /* MINI-OS streams are unbuffered; there is no userspace buffer to flush. */
    (void)stream;
    return 0;
}

int feof(FILE *stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream) {
    return stream ? stream->error : 0;
}
