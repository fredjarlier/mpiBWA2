#!/bin/bash 
#MSUB -r MPIBWA2_1NODE_128CPU
#MSUB -N 4
#MSUB -n 32
#MSUB -c 16
#MSUB -T 6000
#MSUB --threads-per-core=1
#MSUB -m scratch
#MSUB -q milan
#MSUB -x         
#MSUB -o /ccc/scratch/cont007/fg0012/jarlierf/RESULTS/MPIBWA2/test_1N_mpi_bwa2_%I.o
#MSUB -e /ccc/scratch/cont007/fg0012/jarlierf/RESULTS/MPIBWA2/test_1N_mpi_bwa2_%I.e

#ml pu
#ml c++/inteloneapi/22.1.2
#ml flavor/buildcompiler/gcc/11
#ml gnu/11.1.0
#ml aocc-compiler/3.1.0
#ml mpi/openmpi/4.1.2
#ml mpi/intelmpi/22.1.2

#ml gnu/12.2.0
#ml mpi/openmpi/4.1.4

module load licsrv/intel
module load c/intel/20.0.4
module load c++/intel/20.0.4
module load intel/20.0.4
module load mpi/intelmpi/20.0.4

REF='/ccc/scratch/cont007/fg0012/jarlierf/REFERENCES/BWA2/hg19'
FASTQ1="/ccc/scratch/cont007/fg0012/jarlierf/FASTQ/DO15110N_R1.fastq"
FASTQ2="/ccc/scratch/cont007/fg0012/jarlierf/FASTQ/DO15110N_R2.fastq"
OUTPUT='/ccc/scratch/cont007/fg0012/jarlierf/RESULTS/MPIBWA2/DO15110N'

#to test on broadwell 
MPIBWA2=/ccc/cont007/home/fg0012/jarlierf/mpibwa2/mpibwa-mem2

export OMP_NUM_THREADS=16
#export I_MPI_LIBRARY_KIND="release_mt"
#export I_MPI_THREAD_RUNTIME="generic"
#export I_MPI_THREAD_SPLIT=1
#export I_MPI_THREAD_MAX=16
#export I_MPI_EXTRA_FILESYSTEM_FORCE="lustre"
 
ccc_mprun $MPIBWA2 mem -t 16 -Y -K 100000000 -o $OUTPUT $REF $FASTQ1 $FASTQ2 


