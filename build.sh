mkdir -p build
clang -g -fsanitize=undefined code/main.c -o build/vzip
