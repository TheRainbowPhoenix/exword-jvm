/* Regression test: scanning must not allocate 60 KiB+ and need not load
 * 96 KiB buffers for tiny Java class files. Directory rescans free names. */
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    if(argc != 2) return 2;
    if(sizeof(jvm_loader_t) > 12000) {
        fprintf(stderr, "class index regressed to %zu bytes\n",
                sizeof(jvm_loader_t));
        return 3;
    }
    jvm_loader_t *loader = calloc(1, sizeof *loader);
    if(!loader) return 4;
    for(int pass = 0; pass < 30; ++pass) {
        int count = jvm_loader_scan(loader, argv[1]);
        if(count != 2 || jvm_loader_find(loader, "BtuSmoke") < 0 ||
           jvm_loader_find(loader, "BtuObjects") < 0) {
            fprintf(stderr, "pass %d: %s, count=%d\n", pass,
                    loader->status, count);
            return 5;
        }
    }
    char path[JVM_PATH_MAX];
    snprintf(path, sizeof path, "%s/main.txt", argv[1]);
    uint8_t *data = NULL;
    size_t len = 0;
    int rc = jvm_read_file(path, &data, &len);
    if(rc || len != strlen("BtuSmoke\n") ||
       memcmp(data, "BtuSmoke\n", len)) {
        fprintf(stderr, "exact-sized main.txt read failed %d, %zu\n", rc, len);
        return 6;
    }
    free(data);
    jvm_loader_destroy(loader);
    free(loader);
    puts("Memory regression: compact index, repeated scans, tiny file read OK");
    return 0;
}
