7-27-2023

Multinode implementation of bwamem2.
Almost 3 times faster than mpiBWA on multinode.
Tested on AMD EPYC 7763 64-Core Processor with 2 sockets per node.
So far only the avx2 binary is selected but avx2 is almost 3 times faster than
mpiBWA. Maybe it works on new EPY 9XXX I don't know.

See test_mpibwa2.sh for tests and arguments.
The option -f for fixmate doesn't work yet.

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
