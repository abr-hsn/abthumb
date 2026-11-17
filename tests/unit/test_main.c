#include "abt_test.h"

int abt_test_failures = 0;
int abt_test_checks = 0;

void suite_core(void);
void suite_eps(void);
void suite_psd(void);

unsigned char* abt_test_read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* b = (unsigned char*)malloc(n > 0 ? (size_t)n : 1);
    if (b && n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (size) *size = (size_t)(n > 0 ? n : 0);
    return b;
}

const char* abt_test_corpus(void) {
    static char buf[1024];
    const char* e = getenv("ABT_CORPUS");
    if (e && *e) return e;
    snprintf(buf, sizeof(buf), "%s/tests/corpus/generated", ABT_SOURCE_DIR);
    return buf;
}

const char* abt_test_path(const char* rel) {
    static char bufs[4][1024];
    static int i = 0;
    i = (i + 1) & 3;
    snprintf(bufs[i], sizeof(bufs[i]), "%s/%s", abt_test_corpus(), rel);
    return bufs[i];
}

static void run(const char* name, void (*fn)(void)) {
    int before = abt_test_failures;
    printf("[%s]\n", name);
    fn();
    printf("  %s\n", abt_test_failures == before ? "ok" : "FAILED");
}

int main(void) {
    run("core", suite_core);
    run("eps", suite_eps);
    run("psd", suite_psd);
    printf("%d checks, %d failures\n", abt_test_checks, abt_test_failures);
    return abt_test_failures ? 1 : 0;
}
