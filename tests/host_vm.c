/* Host acceptance runner. It uses precisely the same C parser/interpreter
 * as EXWord, but without its hardware BTU (a host machine has no J_JP).
 * A second round of checks must run on a physical EXWord in BTU mode. */
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>

static void output(void *unused, const char *p, size_t n)
{
    (void)unused;
    fwrite(p, 1, n, stdout);
}
int main(int argc, char **argv)
{
    if(argc < 3) {
        fprintf(stderr, "usage: %s class-directory MainClass\n", argv[0]);
        return 2;
    }
    jvm_loader_t *loader = calloc(1, sizeof *loader);
    jvm_vm_t *vm = calloc(1, sizeof *vm);
    if(!loader || !vm) {
        fprintf(stderr, "alloc failed\n");
        return 2;
    }
    int classes = jvm_loader_scan(loader, argv[1]);
    if(classes <= 0) {
        fprintf(stderr, "%s\n", loader->status);
        return 3;
    }
    jvm_vm_init(vm, loader, NULL, output, NULL);
    int rc = jvm_vm_run(vm, argv[2]);
    fflush(stdout);
    fprintf(stderr, "%s: %s, %u classes, %u C steps\n",
            argv[2], rc ? vm->error : "OK",
            vm->stats.classes_loaded, vm->stats.sw_steps);
    jvm_vm_destroy(vm);
    free(vm); free(loader);
    return rc ? 1 : 0;
}
