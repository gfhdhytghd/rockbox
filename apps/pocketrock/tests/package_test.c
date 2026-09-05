#include <stdio.h>
#include "package.h"

int main(int argc, char **argv)
{
    struct pocketrock_package package;
    if (argc != 2) {
        fprintf(stderr, "usage: package_test app.pocket\n");
        return 2;
    }
    if (pocketrock_package_open(argv[1], &package) < 0) {
        fprintf(stderr, "PocketRock package rejected\n");
        return 1;
    }
    if (package.bytecode.length == 0 || package.pak.length == 0 ||
        package.file_size == 0) {
        fprintf(stderr, "PocketRock package incomplete\n");
        return 1;
    }
    printf("PocketRock package OK: bytecode=%u pak=%u\n",
        package.bytecode.length, package.pak.length);
    return 0;
}
