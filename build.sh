set -x

if [ "$1" = "release" ]
then
    FLAGS="-O2"
else
    FLAGS="-g -fsanitize=undefined"
fi

mkdir -p build
clang $FLAGS code/main.c -o build/vzip
