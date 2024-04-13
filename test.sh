./build/vzip compress examples/shakespeare.txt build/shakespeare.txt.vzip
./build/vzip decompress build/shakespeare.txt.vzip build/shakespeare.txt2
diff build/shakespeare.txt2 examples/shakespeare.txt
