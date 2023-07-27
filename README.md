7-27-2023

Multinodes implementation of bwamem2.\
Tested on AMD EPYC 7763 64-Core Processor with 2 sockets per node.\
So far only the avx2 binary is selected on these nodes.\
The speed-up is almost x3 with the avx version.\
\
Not tested on EPY 9XXX yet.\
See test_mpibwa2.sh for tests and arguments.\
The option -f for fixmate doesn't work yet.\
mpiBWA2 only work for fixed length and paired reads so far.\



# mpiBWA2

First test of mpiBWA2

This version is a prototype it works only for paired reads and not trimmed. 
Ii is not multinode ready due to the shared reference not implemented yet.


Tested on Intel Broadwell 
with mpicxx --version
icpc (ICC) 17.0.6 20171215

How it works

1) install Bwa-mem2-2.1 from Source_code_including_submodules.tar.gz
2) then replace the Makefile with the Makefile from mpiBWA2
3) add the file main_parallel_version.cpp in the folder src/
4) type make, it will build all targets 
5) build reference with BWA-MEM2
6) then use it with mpiBWA2

example command lines:

mpirun $MPIBWA2 mem -t 16 -o $SAM $REF $FASTQ1 $FASTQ2

further dev and results will follow...
