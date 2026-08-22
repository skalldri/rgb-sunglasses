#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasm3.h"
#include "wasm3_security_modules.h"

static const void* fill_host(IM3Runtime runtime, IM3ImportContext context, uint64_t* stack,
                             void* memory) {
    (void)runtime;
    (void)context;
    (void)stack;
    (void)memory;
    return m3Err_none;
}

static int expect_parse_rejected(const uint8_t* bytes, size_t size) {
    IM3Environment environment = m3_NewEnvironment();
    IM3Module module = NULL;
    M3Result result;

    if (environment == NULL) {
        fprintf(stderr, "environment allocation failed\n");
        return 1;
    }

    result = m3_ParseModule(environment, &module, bytes, (uint32_t)size);
    if (module != NULL) {
        m3_FreeModule(module);
    }
    m3_FreeEnvironment(environment);

    if (result == m3Err_none) {
        fprintf(stderr, "malformed data segment was accepted\n");
        return 1;
    }
    return 0;
}

static int expect_compile_rejected(const uint8_t* bytes, size_t size) {
    IM3Environment environment = m3_NewEnvironment();
    IM3Runtime runtime = NULL;
    IM3Module module = NULL;
    M3Result result;
    bool loaded = false;
    int failed = 1;

    if (environment == NULL) {
        fprintf(stderr, "environment allocation failed\n");
        return 1;
    }
    runtime = m3_NewRuntime(environment, 1024, NULL);
    if (runtime == NULL) {
        fprintf(stderr, "runtime allocation failed\n");
        goto done;
    }
    result = m3_ParseModule(environment, &module, bytes, (uint32_t)size);
    if (result != m3Err_none || module == NULL) {
        fprintf(stderr, "branch-table module did not reach compilation: %s\n", result);
        goto done;
    }
    result = m3_LoadModule(runtime, module);
    if (result != m3Err_none) {
        fprintf(stderr, "branch-table module did not load: %s\n", result);
        goto done;
    }
    loaded = true; /* runtime owns it after a successful load */
    result = m3_CompileModule(module);
    if (result == m3Err_none) {
        fprintf(stderr, "overflowing br_table target count was accepted\n");
        goto done;
    }
    failed = 0;

done:
    if (module != NULL && !loaded) {
        m3_FreeModule(module);
    }
    if (runtime != NULL) {
        m3_FreeRuntime(runtime);
    }
    m3_FreeEnvironment(environment);
    return failed;
}

static int expect_recursive_call_traps(const uint8_t* bytes, size_t size, const char* export_name,
                                       bool link_fill) {
    IM3Environment environment = m3_NewEnvironment();
    IM3Runtime runtime = NULL;
    IM3Module module = NULL;
    IM3Function function = NULL;
    M3Result result;
    bool loaded = false;
    int failed = 1;

    if (environment == NULL) {
        fprintf(stderr, "environment allocation failed\n");
        return 1;
    }
    runtime = m3_NewRuntime(environment, 1024, NULL);
    if (runtime == NULL) {
        fprintf(stderr, "runtime allocation failed\n");
        goto done;
    }
    result = m3_ParseModule(environment, &module, bytes, (uint32_t)size);
    if (result != m3Err_none || module == NULL) {
        fprintf(stderr, "recursive module parse failed: %s\n", result);
        goto done;
    }
    result = m3_LoadModule(runtime, module);
    if (result != m3Err_none) {
        fprintf(stderr, "recursive module load failed: %s\n", result);
        goto done;
    }
    loaded = true;
    if (link_fill) {
        result = m3_LinkRawFunction(module, "rgbx_mvp", "fill", "v(i)", fill_host);
        if (result != m3Err_none) {
            fprintf(stderr, "recursive module link failed: %s\n", result);
            goto done;
        }
    }
    result = m3_CompileModule(module);
    if (result != m3Err_none) {
        fprintf(stderr, "recursive module compile failed: %s\n", result);
        goto done;
    }
    result = m3_FindFunction(&function, runtime, export_name);
    if (result != m3Err_none) {
        fprintf(stderr, "recursive function lookup failed: %s\n", result);
        goto done;
    }
    result = m3_CallV(function, 0u);
    if (result != m3Err_trapStackOverflow) {
        fprintf(stderr, "recursive call returned %s instead of stack overflow\n", result);
        goto done;
    }
    failed = 0;

done:
    if (module != NULL && !loaded) {
        m3_FreeModule(module);
    }
    if (runtime != NULL) {
        m3_FreeRuntime(runtime);
    }
    m3_FreeEnvironment(environment);
    return failed;
}

int main(void) {
    if (expect_parse_rejected(kWasm3DataSegmentOverflowModule,
                              sizeof(kWasm3DataSegmentOverflowModule)) != 0 ||
        expect_compile_rejected(kWasm3BranchTableOverflowModule,
                                sizeof(kWasm3BranchTableOverflowModule)) != 0 ||
        expect_recursive_call_traps(kWasm3Issue562Module, sizeof(kWasm3Issue562Module), "main",
                                    false) != 0) {
        return 1;
    }

    puts("Wasm3 sanitizer regressions passed");
    return 0;
}
