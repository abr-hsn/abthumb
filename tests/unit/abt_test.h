/* Minimal test harness. Each test file defines one suite function, listed in test_main.c. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int abt_test_failures;
extern int abt_test_checks;

#define ABT_CHECK(cond)                                                                 \
    do {                                                                                \
        ++abt_test_checks;                                                              \
        if (!(cond)) {                                                                  \
            ++abt_test_failures;                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                               \
    } while (0)

#define ABT_CHECK_MSG(cond, ...)                                                        \
    do {                                                                                \
        ++abt_test_checks;                                                              \
        if (!(cond)) {                                                                  \
            ++abt_test_failures;                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);            \
            fprintf(stderr, __VA_ARGS__);                                               \
            fputc('\n', stderr);                                                        \
        }                                                                               \
    } while (0)

/* Reads a whole file (malloc'd), NULL if missing. */
unsigned char* abt_test_read_file(const char* path, size_t* size);
/* Corpus root: env ABT_CORPUS or <source>/tests/corpus/generated. Returns static buffer. */
const char* abt_test_corpus(void);
/* Builds "<corpus>/<rel>" into a static rotating buffer. */
const char* abt_test_path(const char* rel);
