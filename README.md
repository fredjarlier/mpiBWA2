10-19-2023

- Improve IO preallocation buffer.
- fix a memory leaks

tests:  
HG002 on CHM13 \
1 node 128 cores AMD EPYC 7763 => 5h => 3,13 kwh \
HG002 on HG19 \
1 node 128 cores  AMD EPYC 7763 => 3h15 = 1,99 kwh 


refence: \
chm13.0123, chm13.amb, chm13.ann, chm13.bwt.2bit.64, chm13.fasta, chm13.pac \





7-27-2023

Multinodes implementation of bwamem2.\
Tested on AMD EPYC 7763 64-Core Processor with 2 sockets per node.\
So far only the avx2 binary is selected on these nodes.\
The speed-up is almost x3 with the avx2 version.\
100% reproducibility with bwamem2 in alignment (only the @PG header line is modified).\
mpiBWA2 only work for fixed length and paired reads so far.

How to install:

git clone --recursive https://github.com/fredjarlier/mpiBWA2.git \
cd mpiBWA2 \
import mpi librairies or if you have module: \
. sourceme_intelmpi.sh\
make

The reference genome:

mpiBWA2 take the same reference as bwa-mem2.

See test_mpibwa2.sh for tests and arguments.

Some results:

Align 558051664 reads paired 100x100 with hg19, AVX2 (auto-selected), intelmpi

1 node with 128 threads\
$BWA2 mem -t 128 -Y -K 100000000 $REF $FASTQ1 $FASTQ2 > $SAM => 42 mn

1 node 8 mpi jobs with 16 threads per job\
$MPIBWA2 mem -t 16 -Y -K 100000000 -o $OUTPUT $REF $FASTQ1 $FASTQ2 => 27 mn

2 node 16 mpi jobs with 16 threads per job\
$MPIBWA2 mem -t 16 -Y -K 100000000 -o $OUTPUT $REF $FASTQ1 $FASTQ2 => 14 mn


WARNING: This product is not production ready. Use it at your own risk. 


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
