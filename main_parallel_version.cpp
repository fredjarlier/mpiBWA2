/*
This file is part of mpiBWA

NGS aligner inspired by BWA

The project was developped by Frederic Jarlier from Institut Curie and Nicolas Joly from Institut Pasteur

Copyright (C) 2016-2017  Institut Curie / Institut Pasteur

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <sys/mman.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h> /* For PRIu64 */
#include <libgen.h>
#include <limits.h>   /* For PATH_MAX */
#include <math.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "bwa.h"
#include "bwamem.h"
#include "utils.h"
#include "macro.h"
#include "bandedSWA.h"
#include "profiling.h"
#include "FMI_search.h"
#include "fastmap.h"
#include "bwamem.h"

#ifdef USE_MALLOC_WRAPPERS
#  include "malloc_wrap.h"
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "2.0pre1"
#endif

//due to mpi_read_at limit buffer 1gb
#define DEFAULT_INBUF_SIZE (1024*1024*1024)
//#define DEFAULT_INBUF_SIZE 1024

/* We require 64bit offsets */
#ifndef MPI_OFFSET
#define MPI_OFFSET MPI_LONG_LONG
#endif

#ifdef TIMIMG
#define xfprintf fprintf
#else
#define xfprintf(...) /**/
#endif


#define STRIPING_FACTOR "6"
//#define STRIPING_UNIT "4194304"   // 4 MB
//#define STRIPING_UNIT "268435456"   // 256 MB
//#define STRIPING_UNIT "536870912"   // 500 MB
#define STRIPING_UNIT "1073741824"  // 1GB
//#define STRIPING_UNIT "1610612736"  // 1.5GB
//#define STRIPING_UNIT "2147483648"  // 2GB
//#define STRIPING_UNIT "2684354560"  // 2.5GB
//#define STRIPING_UNIT "3221225472"  // 3GB
//#define STRIPING_UNIT "3758096384"  // 3.5GB


#define NB_PROC  "16" //numer of threads for writing
#define CB_NODES "2" //numer of server for writing
#define CB_BLOCK_SIZE  "268435456" /* 256 MBytes - should match FS block size */
#define CB_BUFFER_SIZE  "3758096384" /* multiple of the block size by the number of proc*/
#define DATA_SIEVING_READ "enable"
#define min(a,b) (a>=b?b:a)

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "2.0pre1"
#endif


// ----------------------------------
uint64_t proc_freq, tprof[LIM_R][LIM_C], prof[LIM_R];
int nthreads;
int num_ranks = 1, myrank = 0;
int64_t reference_seq_len;
// ----------------------------------



// --------------
// global vars
FMI_search *fmi;
extern uint64_t proc_freq, tprof[LIM_R][LIM_C];
extern unsigned char nst_nt4_table[256];
extern int num_ranks, myrank;
extern int nthreads;
uint8_t *ref_string;
int readLen, affy[256];
int64_t nreads, memSize;

static void update_a(mem_opt_t *opt, const mem_opt_t *opt0)
{
	if (opt0->a) { // matching score is changed
		if (!opt0->b) opt->b *= opt->a;
		if (!opt0->T) opt->T *= opt->a;
		if (!opt0->o_del) opt->o_del *= opt->a;
		if (!opt0->e_del) opt->e_del *= opt->a;
		if (!opt0->o_ins) opt->o_ins *= opt->a;
		if (!opt0->e_ins) opt->e_ins *= opt->a;
		if (!opt0->zdrop) opt->zdrop *= opt->a;
		if (!opt0->pen_clip5) opt->pen_clip5 *= opt->a;
		if (!opt0->pen_clip3) opt->pen_clip3 *= opt->a;
		if (!opt0->pen_unpaired) opt->pen_unpaired *= opt->a;
	}
}


void memoryAlloc(ktp_aux_t *aux, worker_t &w, int32_t nreads, int32_t nthreads)
{
    mem_opt_t *opt = aux->opt;
    int32_t memSize = nreads;
    int32_t readLen = READ_LEN;

    
    w.regs = NULL; w.chain_ar = NULL; w.seedBuf = NULL;

    w.regs = (mem_alnreg_v *) calloc(memSize, sizeof(mem_alnreg_v));
    w.chain_ar = (mem_chain_v*) malloc (memSize * sizeof(mem_chain_v));
    w.seedBuf = (mem_seed_t *) calloc(sizeof(mem_seed_t),  memSize * AVG_SEEDS_PER_READ);

    assert(w.seedBuf  != NULL);
    assert(w.regs     != NULL);
    assert(w.chain_ar != NULL);

    w.seedBufSize = BATCH_SIZE * AVG_SEEDS_PER_READ;

    
    int64_t allocMem = memSize * sizeof(mem_alnreg_v) +
    memSize * sizeof(mem_chain_v) +
    sizeof(mem_seed_t) * memSize * AVG_SEEDS_PER_READ;
    fprintf(stderr, "------------------------------------------\n");
    fprintf(stderr, "1. Memory pre-allocation for Chaining: %0.4lf MB\n", allocMem/1e6);
    
     int64_t wsize = BATCH_SIZE * SEEDS_PER_READ;
    for(int l=0; l<nthreads; l++)
    {
        w.mmc.seqBufLeftRef[l*CACHE_LINE]  = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_REF * sizeof(int8_t) + MAX_LINE_LEN, 64);
        w.mmc.seqBufLeftQer[l*CACHE_LINE]  = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_QER * sizeof(int8_t) + MAX_LINE_LEN, 64);
        w.mmc.seqBufRightRef[l*CACHE_LINE] = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_REF * sizeof(int8_t) + MAX_LINE_LEN, 64);
        w.mmc.seqBufRightQer[l*CACHE_LINE] = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_QER * sizeof(int8_t) + MAX_LINE_LEN, 64);

        w.mmc.wsize_buf_ref[l*CACHE_LINE] = wsize * MAX_SEQ_LEN_REF;
        w.mmc.wsize_buf_qer[l*CACHE_LINE] = wsize * MAX_SEQ_LEN_QER;

        assert(w.mmc.seqBufLeftRef[l*CACHE_LINE]  != NULL);
        assert(w.mmc.seqBufLeftQer[l*CACHE_LINE]  != NULL);
        assert(w.mmc.seqBufRightRef[l*CACHE_LINE] != NULL);
        assert(w.mmc.seqBufRightQer[l*CACHE_LINE] != NULL);
    }

    for(int l=0; l<nthreads; l++) {
        w.mmc.seqPairArrayAux[l]      = (SeqPair *) malloc((wsize + MAX_LINE_LEN)* sizeof(SeqPair));
        w.mmc.seqPairArrayLeft128[l]  = (SeqPair *) malloc((wsize + MAX_LINE_LEN)* sizeof(SeqPair));
        w.mmc.seqPairArrayRight128[l] = (SeqPair *) malloc((wsize + MAX_LINE_LEN)* sizeof(SeqPair));
        w.mmc.wsize[l] = wsize;

        assert(w.mmc.seqPairArrayAux[l] != NULL);
        assert(w.mmc.seqPairArrayLeft128[l] != NULL);
        assert(w.mmc.seqPairArrayRight128[l] != NULL);
    }


    allocMem = (wsize * MAX_SEQ_LEN_REF * sizeof(int8_t) + MAX_LINE_LEN) * opt->n_threads * 2+
        (wsize * MAX_SEQ_LEN_QER * sizeof(int8_t) + MAX_LINE_LEN) * opt->n_threads  * 2 +
        wsize * sizeof(SeqPair) * opt->n_threads * 3;
    fprintf(stderr, "2. Memory pre-allocation for BSW: %0.4lf MB\n", allocMem/1e6);
    for (int l=0; l<nthreads; l++)
    {
        w.mmc.wsize_mem[l]     = BATCH_MUL * BATCH_SIZE *               readLen;
        w.mmc.matchArray[l]    = (SMEM *) _mm_malloc(w.mmc.wsize_mem[l] * sizeof(SMEM), 64);
        w.mmc.min_intv_ar[l]   = (int32_t *) malloc(w.mmc.wsize_mem[l] * sizeof(int32_t));
        w.mmc.query_pos_ar[l]  = (int16_t *) malloc(w.mmc.wsize_mem[l] * sizeof(int16_t));
        w.mmc.enc_qdb[l]       = (uint8_t *) malloc(w.mmc.wsize_mem[l] * sizeof(uint8_t));
        w.mmc.rid[l]           = (int32_t *) malloc(w.mmc.wsize_mem[l] * sizeof(int32_t));
        w.mmc.lim[l]           = (int32_t *) _mm_malloc((BATCH_SIZE + 32) * sizeof(int32_t), 64); // candidate not for reallocation, deferred for next round of changes.
    }

    allocMem = nthreads * BATCH_MUL * BATCH_SIZE * readLen * sizeof(SMEM) +
    nthreads * BATCH_MUL * BATCH_SIZE * readLen *sizeof(int32_t) +
    nthreads * BATCH_MUL * BATCH_SIZE * readLen *sizeof(int16_t) +
    nthreads * BATCH_MUL * BATCH_SIZE * readLen *sizeof(int32_t) +
    nthreads * (BATCH_SIZE + 32) * sizeof(int32_t);
    fprintf(stderr, "3. Memory pre-allocation for BWT: %0.4lf MB\n", allocMem/1e6);
    fprintf(stderr, "------------------------------------------\n");
}

void init_goff(size_t *goff, MPI_File mpi_filed, size_t fsize,int numproc,int rank){


	char * current_line = NULL;
	MPI_Status status;
	int i = 0;
	int j = 0;
	// TODO problem here when we have only one job!
	size_t lsize = fsize/numproc;
	goff[0]=0;
	for(i=1;i<numproc;i++){goff[i]=lsize*i;}
	goff[numproc]=fsize;
	for(i=1;i<numproc;i++){
		current_line =(char*)calloc(2000,sizeof(char));
		//we read 2K caracters
		MPI_File_read_at(mpi_filed, (MPI_Offset)goff[i], current_line, 2000, MPI_CHAR, &status);
		j=0;
		while (j < 2000 ){
			//we check the \n after and before
			j++;
			if (current_line[j] == '+' && current_line[j+1] == '\n' && current_line[j-1] == '\n') {j++;break;}}
		//then we look for return after quality
		while( j < 2000){j++;if (current_line[j] == '\n') {break;}}
		goff[i]+=(j+1); free(current_line);}

	return;
}

///Function used to find the starting reading offset in a given file according to the number of processes used.
//Parameters (ordered):	pointer on the vector that will save these offsets
//			the size of the given file
//			the studied file
//			the number or processes used
//			the rank of the actual process
//quick workflow : 	find the size to read per process
//			define the starting offset arbitrarily
//			browse a sample, from that offset, find the next beginning of a new read
//			update the starting offset in the vector
void find_process_starting_offset(size_t *goff, size_t size, char* file_to_read, int proc_num, int rank_num)
{
	///MPI related resources
	int res; //used to assert success of MPI communications
	MPI_File mpi_fd; // file descriptor used to open and read from the right file
	MPI_Status status;
	res = MPI_File_open(MPI_COMM_WORLD, file_to_read,  MPI_MODE_RDONLY , MPI_INFO_NULL, &mpi_fd); //open the wanted file
	assert(res==MPI_SUCCESS);
	
	///other resources
	off_t tmp_sz = 1024; //size of the sample from the file, big enough to contain a full read
    char *buffer_r0 = malloc( tmp_sz + 1); //buffer used to save the sample
	buffer_r0[tmp_sz] = '\0'; 
	size_t lsize = size/proc_num; //proportion of the file 1 process should read
	int i; //used as an iterator
    char *p, *q, *e; //pointers on the buffer ot find the start of a read
	
	///define the arbitrary offsets
	goff[0]=0;
    for(i = 1 ; i < proc_num; i++){goff[i] = lsize*i;}
    goff[proc_num] = size;
    res = MPI_File_read_at(mpi_fd, (MPI_Offset)goff[rank_num], buffer_r0, tmp_sz, MPI_CHAR, &status); //read the wanted part of the file nd save it into the buffer
    assert(res == MPI_SUCCESS);
	p = buffer_r0;
    e = buffer_r0 + tmp_sz;

	//browse the buffer to find the beginning of the next read
        while (p < e) {
                if (*p != '@') { p++; continue; }
                if (p != buffer_r0 && *(p-1) != '\n') { p++; continue; }
                q = p + 1;
                while (q < e && *q != '\n') q++; q++;
                while (q < e && *q != '\n') q++; q++;
                if (q < e && *q == '+') break;
                p++;
        }
        assert(*p == '@');

        //we update begining offsets with the found value
        goff[rank_num] += p - buffer_r0;        

        ///free the resources no longer needed
        free(buffer_r0);
        res = MPI_File_close(&mpi_fd);
        assert(res == MPI_SUCCESS);

}

///Function used to find the size and starting offset of each read in a given file
//Parameters (ordered):	the starting offset of the running process
//			the size of the file the process has to read
//			the given file
//			a pointer on the number of reads in the studied interval
//			a pointer on the number of reads in the file
//			a doubly differenciated pointer on the array used to store the offsets
//			a doubly differenciated pointer on the array used to store the sizes
//			the number of processes used
//			the rank of the running process
//quick worflow : 	read from the starting offset 1Go at a time
//			find the last read in that buffer and stop at its end
//			find the number of lines in contains and use it to realloc the size and offset vector
//			then browse the buffer to find the size and starting offset of each read
//			display stats about the time it took to do all the previous actions
void find_reads_size_and_offsets(size_t offset_in_file,
								size_t siz2read,
								char *file_to_read,
								size_t *p_local_num_reads,
								size_t *p_total_num_reads,
								size_t **local_read_offsets,
								int **local_read_size,
								size_t **local_read_bytes,
								int proc_num, int rank_num){
	
	///MPI related resources
	MPI_File  mpi_fd; //file handle for the given file
	int res; 
	res = MPI_File_open(MPI_COMM_WORLD, file_to_read, MPI_MODE_RDONLY, MPI_INFO_NULL, &mpi_fd);
	assert(res==MPI_SUCCESS);
	
	///Non MPI related resources
	char *buffer_r;
	char *b, *r, *t, *e; //used as iterators
	size_t offset_end_buff;
	size_t pos_in_vect = 0;
	size_t lines = 0;
	size_t total_parsing = 0;
	size_t g=0;

	MPI_Datatype arraytype;
	MPI_Datatype arraytype0;	
	MPI_Datatype arraytype_r1;
	MPI_Datatype arraytype_r2;
	//init p_total_num_reads
	*p_total_num_reads = 0;
	//you can only read a certain size of file at a time iot reduce the processor load
	size_t read_buffer_sz = 0;
	if ( siz2read < DEFAULT_INBUF_SIZE ) read_buffer_sz = siz2read;
	else read_buffer_sz = DEFAULT_INBUF_SIZE;
	
	while (1){

		buffer_r = malloc(read_buffer_sz + 1);
		assert( buffer_r != NULL );
		buffer_r[read_buffer_sz] = '0';
		
		// FOR TEST //
		// MPI_Type_contiguous(read_buffer_sz, MPI_CHAR, &arraytype0);
		// MPI_Type_commit(&arraytype0);
		// MPI_File_set_view(mpi_fd_in2, (MPI_Offset)off_in_file, MPI_CHAR, MPI_CHAR, "native", MPI_INFO_NULL ) ; 
		// res = MPI_File_read(mpi_fd_in2, b , 1, arraytype0, &status);
		res = MPI_File_read_at(mpi_fd, (MPI_Offset)offset_in_file, buffer_r, read_buffer_sz, MPI_CHAR, MPI_STATUS_IGNORE);
		assert(res == MPI_SUCCESS);
		assert(*buffer_r == '@');	

		//we search the last read 
		b = buffer_r;
		r = b + read_buffer_sz;

		if ( read_buffer_sz == DEFAULT_INBUF_SIZE){
			//go to last previous read
			while (r-- != b){if (*r == '\n' && *(r+1) == '+') {r--; break;}}					
			while (r-- != b){if (*r == '\n') break;}
			while (r-- != b){if (*r == '@') break;}
			r--; //stop at \n
			offset_end_buff = (r - b);
		}
		else
			offset_end_buff = (r - b);

		//we count the number of lines
		t = buffer_r ;
		e = buffer_r + offset_end_buff;
		lines = 0;		
		while (t++ < e){if (*t == '\n') lines++;}

		//assert( lines%4 == 0);

		*p_local_num_reads =  (lines/4);
		*p_total_num_reads += *p_local_num_reads;
		//realloc the vectors according to the new number of reads you have
		//they are doubly differenciated because the realloc can change the vector pointer
		*local_read_size  	= (int *)realloc(*local_read_size, sizeof(int) * (*p_total_num_reads));
		*local_read_offsets	= (size_t *)realloc(*local_read_offsets, sizeof(size_t) * (*p_total_num_reads));
		*local_read_bytes	= (size_t *)realloc(*local_read_bytes, sizeof(size_t) * (*p_total_num_reads));

		assert( *local_read_offsets != NULL);
		assert( *local_read_size 	!= NULL);
		assert( *local_read_bytes 	!= NULL);
		
		//for the first first read
		t = buffer_r;
		
		int size=0;
		size_t lines2 = 0;
		size_t lines3 = 0;

		t = buffer_r;
		//we move to the next line because we test *(t-1)
		int last_size;
		int done = 0;
		size_t start_read_offset = 0;

		g = offset_in_file;
		//here we browse the buffer and each time we reach a new read we save:
		//- the size of the previous one
		//- the offset of the next one
		//the last_size resource is used in case we're treating the last read

		int count = 0;

		while (t < e){

			if (lines3 == lines) break;
			assert( *t == '@');

			//we get the read start offset
			start_read_offset = g;
			while (*t != '\n'){ t++; g++;} //the name
			t++;g++; lines3++;
			while (*t != '\n'){ size++; t++; g++;} //the read
			t++;g++; lines3++;//skip \n
			while (*t != '\n'){ t++; g++;} //the +
			t++;g++; lines3++;
			while (*t != '\n'){ t++; g++;} //the qual
			lines3++;

			(*local_read_offsets)[pos_in_vect]	= start_read_offset;
			(*local_read_bytes)[pos_in_vect]	= (g - start_read_offset) + 1;
			assert((*local_read_bytes)[pos_in_vect] != 0);
			(*local_read_size)[pos_in_vect] 	= size;
			assert((*local_read_size)[pos_in_vect] != 0);

			size = 0;
			pos_in_vect++;
			t++;g++; //we go to the next read

		}

		assert( lines == lines3 );
		total_parsing 	+= offset_end_buff; 
		//check if we finished reading the size given to the process or if we have to continue
		if (total_parsing == siz2read) {free(buffer_r); break;}
		if ((siz2read - total_parsing) < DEFAULT_INBUF_SIZE)
			read_buffer_sz = siz2read - total_parsing;			
		else read_buffer_sz = DEFAULT_INBUF_SIZE;
					
		offset_in_file  += offset_end_buff + 1;
		free(buffer_r);				
	}

	assert(total_parsing == siz2read);
	MPI_File_close(&mpi_fd);	

}

///Function used to evaluate the number, size, number of reads and starting offset of the chunks
//Parameters (ordered):	pointer on the vector that has the starting offset of each chunk for f1 
//			pointer on the vector that has the starting offset of each chunk for f2 
//			pointer on the vector that has the size of each chunk for f1 (in bytes)
//			pointer on the vector that has the size of each chunk for f2 (in bytes)
//			pointer on the vector that has the number of reads in each chunk for f1
//			pointer on the vector that has the number of reads in each chunk for f2
//			pointer on the vector that has the size of each read for f1
//			pointer on the vector that has the size of each read for f2
//			pointer on the vector that has the starting offset of each read for f1
//			pointer on the vector that has the starting offset of each read for f2
//			rank of the running process
//			number of processes used
//			number of reads in the size given to the running process
//			total number of reads in the file
//			maximum size of a chunk
//			number of chunks needed by the file
//quick workfllow : 	every process waits for the previous one to end him info except rank 0
//			rank 0 browses his reads and gets the chunk info
//			when he hits maxsiz, he send the current info to the next process
//			next processes does the same until every read is taken into account
//			then a last chunk is filled with the remaining bases
//			display some stats about the time it took to do all this
void find_chunks_info(	size_t *begin_offset_chunk,
			size_t *begin_offset_chunk_2,
			size_t *chunk_size,
			size_t *chunk_size_2,
			size_t *reads_in_chunk,
			size_t *reads_in_chunk_2,
			int *local_read_size,
			int *local_read_size_2,
			size_t *local_read_bytes,
			size_t *local_read_bytes_2,
			size_t *local_read_offsets,
			size_t *local_read_offsets_2,
			int rank_num,
			int proc_num,
			size_t local_num_reads,
			size_t local_num_reads_2,
			size_t grand_total_num_reads,
			size_t grand_total_num_reads_2,
			off_t maxsiz,
			size_t *chunk_count,
			char *file_r1,
			char *file_r2){
	
	//non MPI related resources

	char *buffer_r1 = malloc(100);
	char *buffer_r2 = malloc(100);
	char *p1, *q1, *p2, *q2;
	char *name1, *name2;

	int count 				= 0;
	int res, i, bef, aft;
	int read_nb 				= 0; // have to use a buffer value instead of the iterator directly in case there are more reads handled by the process than there are in a chunk.
	int nb_reads_to_send			= 0;
	int nb_reads_to_recv			= 0;
	int reads_type_to_send			= 0; 	// use to know if we shall send forward (0) or backward (1)
	int reads_type_to_recv			= 0; 	// use to know if we shall recieve forward (0) or backward (1)
	int *sizes_to_send			= NULL;
	int *sizes_to_recv			= NULL;
	int *p_sizes  				= NULL; //reserved pointer
	int *p_sizes_2  			= NULL; //reserved pointer
	
	size_t *p_bytes  			= NULL; //reserved pointer
	size_t *p_bytes_2  			= NULL; //reserved pointer
	size_t u1 						= 0;
	size_t local_read_min;
	size_t *bytes_to_send			= NULL;
	size_t *bytes_to_recv			= NULL;
	size_t *p_offset 			= NULL; //reserved pointer
	size_t *p_offset_2 			= NULL; //reserved pointer
	size_t reads_recieved			= 0;
	size_t reads_recieved_2			= 0;
	size_t *offsets_to_send			= NULL;
	size_t *offsets_to_recv			= NULL;
	size_t counter_bases  	 		= 0;
	size_t bytes_in_chunk 	 		= 0, bytes_in_chunk_2 		= 0;
	size_t index_in_chunk 			= 0;
	size_t begin_offset 			= 0, begin_offset_2		= 0;
	size_t bases_previous_chunk     	= 0;
	size_t offset_previous_chunk 		= 0, offset_previous_chunk_2 	= 0;
	size_t size_previous_chunk 	 	= 0, size_previous_chunk_2 	= 0;
	size_t x 				= 0,y = 0; 	// iterators of the number of reads in each file
	size_t read1,read2			= 0;
	size_t size_chunk 			= 100;
	size_t tmp_cnt				= 0;

	MPI_File fh_r1;
	MPI_File fh_r2;	
	MPI_Status status;
	
	//init char buffer
	buffer_r1[99] = '0';
	buffer_r2[99] = '0';

	bef = MPI_Wtime();

	if (rank_num > 0){
		// we wait the previous rank to send the final size chunk
		// and offset of the chunk
		MPI_Recv(&bases_previous_chunk,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				0,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		MPI_Recv(&offset_previous_chunk,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				1,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		MPI_Recv(&offset_previous_chunk_2,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				2,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		MPI_Recv(&size_previous_chunk,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				3,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		MPI_Recv(&size_previous_chunk_2,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				4,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		//we send the type or reads we are going to send
		MPI_Recv(&reads_type_to_recv,
				1,
				MPI_INT,
				rank_num - 1,
				5,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		//we send the size of the vector for the recieving part
		MPI_Recv(&nb_reads_to_recv,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				6,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		if ( reads_type_to_recv == 0){
			//we recieve for forward reads part
			reads_recieved 		= nb_reads_to_recv;
			reads_recieved_2 	= 0;
		}
		else{
			//we recieve for backward reads part
			reads_recieved 		= 0;
			reads_recieved_2 	= nb_reads_to_recv;
		}

		//here we malloc the size
		sizes_to_recv 		= realloc(sizes_to_recv, nb_reads_to_recv * sizeof(int));
		bytes_to_recv 		= realloc(bytes_to_recv, nb_reads_to_recv * sizeof(size_t));
		offsets_to_recv 	= realloc(offsets_to_recv, nb_reads_to_recv * sizeof(size_t));

		assert(sizes_to_recv != NULL);
		assert(bytes_to_recv != NULL);
		assert(offsets_to_recv != NULL);

		//we send the vector of offsets to the next rank
		MPI_Recv(offsets_to_recv,
				nb_reads_to_recv,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				7,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		//we send the vector of sizes to the next rank
		MPI_Recv(sizes_to_recv,
				nb_reads_to_recv,
				MPI_INT,
				rank_num - 1,
				8,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		MPI_Recv(bytes_to_recv,
				nb_reads_to_recv,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				9,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		// we send read1 and read2
		// number of reads already parsed
		MPI_Recv(&read1,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				10,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		MPI_Recv(&read2,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				11,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

	}

	if (rank_num == 0){
		
		// here you find which file buffer of the rank 0 holds the least reads
		local_read_min = min(local_num_reads, local_num_reads_2);

		if(local_num_reads_2 == local_read_min) {
			nb_reads_to_send 	= (local_num_reads - local_read_min);
			reads_type_to_send 	= 0;
		}
		else {
			nb_reads_to_send 	= (local_num_reads_2 - local_read_min);
			reads_type_to_send	= 1;
		}
		for (u1 = 0; u1 < local_read_min; u1++){

			if (u1 >= 1){
				assert(*(local_read_offsets + x)   != *(local_read_offsets + x - 1));
				assert(*(local_read_offsets_2 + y) != *(local_read_offsets_2 + y - 1));
			}

			// we for offsets multiple of maxsize
			if (counter_bases == 0){
				begin_offset 	= *(local_read_offsets + x);
				begin_offset_2 	= *(local_read_offsets_2 + y);
			}

			assert( (*(local_read_size + x)) != 0 );
			assert( (*(local_read_bytes + x)) != 0 );

			counter_bases 	+= *(local_read_size + x);
			bytes_in_chunk  += *(local_read_bytes + x);
			x++;read1++;

			assert( (*(local_read_size_2 + y)) != 0 );
			assert( (*(local_read_bytes_2 + y)) != 0 );

			counter_bases	 += *(local_read_size_2 + y);
			bytes_in_chunk_2 += *(local_read_bytes_2 + y);
			y++;read2++;

			if ( counter_bases > maxsiz){

				assert( read1 == read2 );

				//the current chunk is full
				//then we have the number of bases wanted
				begin_offset_chunk[index_in_chunk] 		= begin_offset;
				begin_offset_chunk_2[index_in_chunk] 		= begin_offset_2;
				chunk_size[index_in_chunk] 	   			= bytes_in_chunk;
				chunk_size_2[index_in_chunk] 	   		= bytes_in_chunk_2;
				reads_in_chunk[index_in_chunk]	   		= read1;
				reads_in_chunk_2[index_in_chunk]   		= read2;
				*chunk_count 			   += 1;
				index_in_chunk 			   += 1;
				//we reset counter of bases
				counter_bases  		= 0;
				bytes_in_chunk		= 0;
				bytes_in_chunk_2	= 0;
				read_nb				= 0;
				read1				= 0;
				read2				= 0;
			}
		} //end for loop on local_read_min
	}//end if rank_num == 0
	else{

		//receive the info about the current chunk being filled
		counter_bases 			= bases_previous_chunk;
		bytes_in_chunk 			= size_previous_chunk;
		bytes_in_chunk_2 		= size_previous_chunk_2;
		begin_offset			= offset_previous_chunk;
		begin_offset_2			= offset_previous_chunk_2;
		///receive the nb of reads sent
		//malloc a vector to its size
		//fill it with the sent data
		//redo the calculus of the nb of reads to treat and to send
		//once you get that new number, do the rest of the treatments
		//once you're finished check that everything went smoothly and send the right nb ofreads to the next rank.
		x=0; y=0;
		size_t counter_tmp = 0;
		///here you find which file buffer of the rank 0 that holds the last reads

		//we test if for the last rank the reads are equal
		if (rank_num == (proc_num - 1))
			assert((local_num_reads + reads_recieved) == (local_num_reads_2 + reads_recieved_2));

		local_read_min = min( local_num_reads + reads_recieved, local_num_reads_2 + reads_recieved_2);

		if ( (local_num_reads_2 + reads_recieved_2) == local_read_min ) {
			nb_reads_to_send 	= (local_num_reads + reads_recieved) - local_read_min;
			reads_type_to_send 	= 0; //we send forward
		}
		else {
			nb_reads_to_send 	= (local_num_reads_2 + reads_recieved_2) - local_read_min;
			reads_type_to_send	= 1; //we send backward
		}

		// here we initialyze p_offset and p_sizes
		// according to recieve part
		if ( reads_type_to_recv == 0){
			// we recieve from forward reads part
			p_offset		= offsets_to_recv;
			p_sizes		= sizes_to_recv;
			p_bytes		= bytes_to_recv;

			p_offset_2	= local_read_offsets_2;
			p_sizes_2	= local_read_size_2;
			p_bytes_2	= local_read_bytes_2;
		}
		else{
			// we recieve from backward reads part
			p_offset	= local_read_offsets;
			p_sizes		= local_read_size;
			p_bytes		= local_read_bytes;

			p_offset_2	= offsets_to_recv;
			p_sizes_2	= sizes_to_recv;
			p_bytes_2	= bytes_to_recv;
		}

		for (u1 = 0; u1 < local_read_min; u1++){

			if (u1 >= 1){
				assert(*(p_offset + x) != *(p_offset + x - 1));
				assert(*(p_offset_2 + y) != *(p_offset_2 + y - 1));
			}
			// when we reached the number of reads recieved
			// we switch of vector and pass to the local vector
			if (counter_tmp == nb_reads_to_recv){

				if ( reads_type_to_recv == 0){
					p_offset	= local_read_offsets;
					p_sizes		= local_read_size;
					p_bytes		= local_read_bytes;
					x = 0; //only pos in forward buff are reset
				}
				else{
					p_offset_2	= local_read_offsets_2;
					p_sizes_2	= local_read_size_2;
					p_bytes_2	= local_read_bytes_2;
					y = 0; //only pos in backward buff are reset
				}
			} //end condition switch



			//we for offsets multiple of maxsize
			if (counter_bases == 0){
				begin_offset 	= *(p_offset + x);
				begin_offset_2 	= *(p_offset_2 + y);
			}

			counter_bases 	+= *(p_sizes + x);
			bytes_in_chunk  += *(p_bytes + x);
			x++; read1++;

			counter_bases	 += *(p_sizes_2 + y);
			bytes_in_chunk_2 += *(p_bytes_2 + y);
			y++; read2++;
			counter_tmp++;

			if ( counter_bases > maxsiz){

				assert( read1 == read2 );

				//the current chunk is full
				//then we have the number of bases wanted
				begin_offset_chunk[index_in_chunk] 		= begin_offset;
				begin_offset_chunk_2[index_in_chunk] 		= begin_offset_2;
				chunk_size[index_in_chunk] 	   			= bytes_in_chunk;
				chunk_size_2[index_in_chunk] 	   		= bytes_in_chunk_2;

				reads_in_chunk[index_in_chunk]	   		= read1;
				reads_in_chunk_2[index_in_chunk]   		= read2;
				*chunk_count		+= 1;
				index_in_chunk 		+= 1;
				//we reset counter of bases
				counter_bases  		= 0;
				bytes_in_chunk 		= 0;
				bytes_in_chunk_2	= 0;
				read_nb				= 0;
				read1				= 0;
				read2				= 0;
			}
		} 	// end for loop

		//free(offsets_to_recv);
		//free(sizes_to_recv);
	} // end else
	

	if ( rank_num == (proc_num - 1) ){

		// we complete the last chunk
		// with the last reads
		begin_offset_chunk[index_in_chunk] 		= begin_offset;
		begin_offset_chunk_2[index_in_chunk]	= begin_offset_2;
		chunk_size[index_in_chunk] 				= bytes_in_chunk;
		chunk_size_2[index_in_chunk]			= bytes_in_chunk_2;

		// the total num reads in the file is : grand_total_num_reads
		// grand total - sum from 0 to index in chunk -1 = num reads in that chunk
		int i_chunks	 	= 0;
		size_t sum_reads 	= 0;
		size_t sum_reads_2	= 0;

		for(i_chunks = 0; i_chunks < index_in_chunk; i_chunks++){
			sum_reads 	+= reads_in_chunk[i_chunks];
			sum_reads_2 += reads_in_chunk_2[i_chunks];
		}

		reads_in_chunk[index_in_chunk]		= read1;
		reads_in_chunk_2[index_in_chunk]	= read2;

		index_in_chunk 	+=1;
		*chunk_count 	+=1;

	}
	if (rank_num < (proc_num -1)){

		//we send to rank + 1
		MPI_Send(&counter_bases,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				0,
				MPI_COMM_WORLD);

		MPI_Send(&begin_offset,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				1,
				MPI_COMM_WORLD);

		MPI_Send(&begin_offset_2,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				2,
				MPI_COMM_WORLD);

		MPI_Send(&bytes_in_chunk,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				3,
				MPI_COMM_WORLD);

		MPI_Send(&bytes_in_chunk_2,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				4,
				MPI_COMM_WORLD);

		//we send the type or reads we are going to send
		MPI_Send(&reads_type_to_send,
				1,
				MPI_INT,
				rank_num + 1,
				5,
				MPI_COMM_WORLD);

		// we send the size of the vector for the recieving part
		MPI_Send(&nb_reads_to_send,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				6,
				MPI_COMM_WORLD);

		if ( reads_type_to_send == 0){
			offsets_to_send	= local_read_offsets + local_read_min - reads_recieved;
			sizes_to_send 	= local_read_size + local_read_min - reads_recieved;
			bytes_to_send	= local_read_bytes + local_read_min - reads_recieved;
		}
		else {
			offsets_to_send	= local_read_offsets_2 + local_read_min - reads_recieved_2;
			sizes_to_send 	= local_read_size_2 + local_read_min - reads_recieved_2;
			bytes_to_send	= local_read_bytes_2 + local_read_min - reads_recieved_2;
		}
		// we send the vector of offsets to the next rank

		MPI_Send(offsets_to_send,
				nb_reads_to_send,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				7,
				MPI_COMM_WORLD);

		//we send the vector of sizes to the next rank
		MPI_Send(sizes_to_send,
				nb_reads_to_send,
				MPI_INT,
				rank_num + 1,
				8,
				MPI_COMM_WORLD);

		MPI_Send(bytes_to_send,
				nb_reads_to_send,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				9,
				MPI_COMM_WORLD);

		// we send read1 and read2
		// number of reads already parsed
		MPI_Send(&read1,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				10,
				MPI_COMM_WORLD);

		MPI_Send(&read2,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				11,
				MPI_COMM_WORLD);

	}


	free(sizes_to_recv);
	free(bytes_to_recv);
	free(offsets_to_recv);

}


void find_chunks_info_single_end(	
						size_t *begin_offset_chunk,
						size_t *chunk_size,
						size_t *reads_in_chunk,
						int *local_read_size,
						size_t *local_read_bytes,
						size_t *local_read_offsets,
						int rank_num,
						int proc_num,
						size_t local_num_reads,
						size_t grand_total_num_reads,
						off_t maxsiz,
						size_t *chunk_count,
						char *file_r1
						){
	
	//non MPI related resources
	char *buffer_r1 = malloc(100);
	char *p1, *q1, *name1;	
	int i,bef,aft;
	int read_nb=0;   					// have to use a buffer value instead of the iterator directly in case there are more reads handled by the process than there are in a chunk.
	int nb_reads_to_send			= 0;
	int nb_reads_to_recv			= 0;
	int reads_type_to_send			= 0; 	// use to know if we shall send forward (0) or backward (1)
	int reads_type_to_recv			= 0; 	// use to know if we shall recieve forward (0) or backward (1)
	int *sizes_to_send				= NULL;
	int *sizes_to_recv				= NULL;
	int *p_sizes  					= NULL; //reserved pointer
	int count 						= 0;
	int res 						= 0;
	size_t *p_bytes  				= NULL; //reserved pointer
	size_t u1						= 0;
	size_t local_read_min			= 0;
	size_t *bytes_to_send			= NULL;
	size_t *bytes_to_recv			= NULL;
	size_t *p_offset 				= NULL; //reserved pointer
	size_t reads_recieved			= 0;
	size_t *offsets_to_send			= NULL;
	size_t *offsets_to_recv			= NULL;
	size_t counter_bases  	 		= 0;
	size_t bytes_in_chunk 	 		= 0;
	size_t index_in_chunk 			= 0;
	size_t begin_offset 			= 0;
	size_t bases_previous_chunk 	= 0;
	size_t offset_previous_chunk 	= 0;
	size_t size_previous_chunk 	 	= 0;
	size_t x=0,y=0; 					// iterators of the number of reads in each file
	size_t read1=0,read2=0;
	size_t size_chunk = 100;
	size_t tmp_cnt=0;

	MPI_File fh_r1;
	MPI_Status status;
	
	buffer_r1[99] = '0';
	
	bef = MPI_Wtime();

	if (rank_num > 0){
		// we wait the previous rank to send the final size chunk
		// and offset of the chunk
		MPI_Recv(&bases_previous_chunk,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				0,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		MPI_Recv(&offset_previous_chunk,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				1,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		MPI_Recv(&size_previous_chunk,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				2,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		//we send the type or reads we are going to send
		MPI_Recv(&reads_type_to_recv,
				1,
				MPI_INT,
				rank_num - 1,
				3,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		//we send the size of the vector for the recieving part
		MPI_Recv(&nb_reads_to_recv,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				4,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		//we recieve for forward reads part
		reads_recieved 		= nb_reads_to_recv;
		
		//here we malloc the size
		sizes_to_recv 		= realloc(sizes_to_recv, nb_reads_to_recv * sizeof(int));
		bytes_to_recv 		= realloc(bytes_to_recv, nb_reads_to_recv * sizeof(size_t));
		offsets_to_recv 	= realloc(offsets_to_recv, nb_reads_to_recv * sizeof(size_t));

		assert(sizes_to_recv != NULL);
		assert(bytes_to_recv != NULL);
		assert(offsets_to_recv != NULL);

		//we send the vector of offsets to the next rank
		MPI_Recv(offsets_to_recv,
				nb_reads_to_recv,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				5,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		//we send the vector of sizes to the next rank
		MPI_Recv(sizes_to_recv,
				nb_reads_to_recv,
				MPI_INT,
				rank_num - 1,
				6,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		MPI_Recv(bytes_to_recv,
				nb_reads_to_recv,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				7,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

		// we send read1 and read2
		// number of reads already parsed
		MPI_Recv(&read1,
				1,
				MPI_LONG_LONG_INT,
				rank_num - 1,
				8,
				MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);

	}

	if (rank_num == 0){
		
		// here you find which file buffer of the rank 0 holds the least reads
		local_read_min = local_num_reads;

		// we are only in forward
		reads_type_to_send 	= 0;
		
		for (u1 = 0; u1 < local_read_min; u1++){

			if (u1 >= 1){
				assert(*(local_read_offsets + x)   != *(local_read_offsets + x - 1));
			}

			// we for offsets multiple of maxsize
			if (counter_bases == 0){
				begin_offset 	= *(local_read_offsets + x);
			}

			assert( (*(local_read_size + x)) != 0 );
			assert( (*(local_read_bytes + x)) != 0 );

			counter_bases 	+= *(local_read_size + x);
			bytes_in_chunk  += *(local_read_bytes + x);
			x++;read1++;
			
			if ( counter_bases > maxsiz){

				//the current chunk is full
				//then we have the number of bases wanted
				begin_offset_chunk[index_in_chunk] 		= begin_offset;
				chunk_size[index_in_chunk] 	   			= bytes_in_chunk;
				reads_in_chunk[index_in_chunk]	   		= read1;
				*chunk_count 			   += 1;
				index_in_chunk 			   += 1;
				//we reset counter of bases
				counter_bases  		= 0;
				bytes_in_chunk		= 0;
				read_nb				= 0;
				read1				= 0;
				read2				= 0;
			}
		} //end for loop on local_read_min
	}//end if rank_num == 0
	else{

		//receive the info about the current chunk being filled
		counter_bases 		= bases_previous_chunk;
		bytes_in_chunk 		= size_previous_chunk;
		begin_offset		= offset_previous_chunk;
		
		///receive the nb of reads sent
		//malloc a vector to its size
		//fill it with the sent data
		//redo the calculus of the nb of reads to treat and to send
		//once you get that new number, do the rest of the treatments
		//once you're finished check that everything went smoothly and send the right nb ofreads to the next rank.
		x=0; y=0;
		size_t counter_tmp = 0;
		///here you find which file buffer of the rank 0 that holds the last reads

		local_read_min = local_num_reads + reads_recieved;

		
		nb_reads_to_send 	= (local_num_reads + reads_recieved) - local_read_min;
		reads_type_to_send 	= 0; //we send forward
		
		// here we initialyze p_offset and p_sizes
		// according to recieve part
		// we recieve from forward reads part
		p_offset	= offsets_to_recv;
		p_sizes		= sizes_to_recv;
		p_bytes		= bytes_to_recv;
		
		for (u1 = 0; u1 < local_read_min; u1++){

			if (u1 >= 1){
				assert(*(p_offset + x) != *(p_offset + x - 1));
			}
			
			// when we reached the number of reads recieved
			// we switch of vector and pass to the local vector
			if (counter_tmp == nb_reads_to_recv){

					p_offset	= local_read_offsets;
					p_sizes		= local_read_size;
					p_bytes		= local_read_bytes;
					x 			= 0; //only pos in forward buff are reset
			} //end condition switch

			//we for offsets multiple of maxsize
			if (counter_bases == 0){
				begin_offset 	= *(p_offset + x);
			}

			counter_bases 	+= *(p_sizes + x);
			bytes_in_chunk  += *(p_bytes + x);
			x++; read1++;

			counter_tmp++;

			if ( counter_bases > maxsiz){

				//the current chunk is full
				//then we have the number of bases wanted
				begin_offset_chunk[index_in_chunk] 		= begin_offset;
				chunk_size[index_in_chunk] 	   			= bytes_in_chunk;
				
				reads_in_chunk[index_in_chunk]	   		= read1;
				*chunk_count		+= 1;
				index_in_chunk 		+= 1;
				//we reset counter of bases
				counter_bases  		= 0;
				bytes_in_chunk 		= 0;
				read_nb				= 0;
				read1				= 0;
				
			}
		} 	// end for loop

		//free(offsets_to_recv);
		//free(sizes_to_recv);
	} // end else
	

	if ( rank_num == (proc_num - 1) ){

		// we complete the last chunk
		// with the last reads
		begin_offset_chunk[index_in_chunk] 		= begin_offset;
		chunk_size[index_in_chunk] 				= bytes_in_chunk;
		
		// the total num reads in the file is : grand_total_num_reads
		// grand total - sum from 0 to index in chunk -1 = num reads in that chunk
		int i_chunks	 	= 0;
		size_t sum_reads 	= 0;
		
		for(i_chunks = 0; i_chunks < index_in_chunk; i_chunks++){
			sum_reads 	+= reads_in_chunk[i_chunks];
		}

		reads_in_chunk[index_in_chunk]		= read1;
		
		index_in_chunk 	+=1;
		*chunk_count 	+=1;

	}
	if (rank_num < (proc_num -1)){

		//we send to rank + 1
		MPI_Send(&counter_bases,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				0,
				MPI_COMM_WORLD);

		MPI_Send(&begin_offset,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				1,
				MPI_COMM_WORLD);

		MPI_Send(&bytes_in_chunk,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				2,
				MPI_COMM_WORLD);

		//we send the type or reads we are going to send
		MPI_Send(&reads_type_to_send,
				1,
				MPI_INT,
				rank_num + 1,
				3,
				MPI_COMM_WORLD);

		// we send the size of the vector for the recieving part
		MPI_Send(&nb_reads_to_send,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				4,
				MPI_COMM_WORLD);

		if ( reads_type_to_send == 0){
			offsets_to_send	= local_read_offsets + local_read_min - reads_recieved;
			sizes_to_send 	= local_read_size + local_read_min - reads_recieved;
			bytes_to_send	= local_read_bytes + local_read_min - reads_recieved;
		}
		// we send the vector of offsets to the next rank

		MPI_Send(offsets_to_send,
				nb_reads_to_send,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				5,
				MPI_COMM_WORLD);

		//we send the vector of sizes to the next rank
		MPI_Send(sizes_to_send,
				nb_reads_to_send,
				MPI_INT,
				rank_num + 1,
				6,
				MPI_COMM_WORLD);

		MPI_Send(bytes_to_send,
				nb_reads_to_send,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				7,
				MPI_COMM_WORLD);

		// we send read1 and read2
		// number of reads already parsed
		MPI_Send(&read1,
				1,
				MPI_LONG_LONG_INT,
				rank_num + 1,
				8,
				MPI_COMM_WORLD);
	}

	free(sizes_to_recv);
	free(bytes_to_recv);
	free(offsets_to_recv);
}

// Function used to create the header of the .sam result file
// Parameters (ordered):	result file
//			bwa parameter
//			count of top level elements in the file
//			header string
//			rg = read group string
void create_sam_header(char *file_out, bwaidx_t *indix, int *count, char *hdr_line, char *rg_line, int rank_num)
{

	//MPI resources
	MPI_File fh_out;
	MPI_Status status;

	//non MPI related resources
	int bef,aft;
	int res;
	
	//the rank 0 takes care of that task
	if (rank_num == 0) {
		int s, len;
		char *buff;

		res = MPI_File_delete(file_out, MPI_INFO_NULL);
		assert(res == MPI_SUCCESS || res == MPI_ERR_NO_SUCH_FILE || res == MPI_ERR_IO);
		res = MPI_File_open(MPI_COMM_SELF, file_out, MPI_MODE_CREATE|MPI_MODE_WRONLY, MPI_INFO_NULL, &fh_out);
		assert(res == MPI_SUCCESS);
		/* Add reference sequence lines */
		for (s = 0; s < (*indix).bns->n_seqs; ++s) {
		len = asprintf(&buff, "@SQ\tSN:%s\tLN:%d\n", (*indix).bns->anns[s].name, (*indix).bns->anns[s].len);
		res = MPI_File_write(fh_out, buff, len, MPI_CHAR, &status);
		assert(res == MPI_SUCCESS);
		res = MPI_Get_count(&status, MPI_CHAR, count);
		assert(res == MPI_SUCCESS);
		assert(*count == len);
		free(buff);
		}
		/* Add header lines */
		if (hdr_line != NULL) {
		len = asprintf(&buff, "%s\n", hdr_line);
		res = MPI_File_write(fh_out, buff, len, MPI_CHAR, &status);
		assert(res == MPI_SUCCESS);
		res = MPI_Get_count(&status, MPI_CHAR, count);
		assert(res == MPI_SUCCESS);
		assert(*count == len);
		free(buff);
		}
		/* Add read group line */
		if (rg_line != NULL) {
		len = asprintf(&buff, "%s\n", rg_line);
		res = MPI_File_write(fh_out, buff, len, MPI_CHAR, &status);
		assert(res == MPI_SUCCESS);
		res = MPI_Get_count(&status, MPI_CHAR, count);
		assert(res == MPI_SUCCESS);
		assert(*count == len);
		free(buff);
		}
		res = MPI_File_close(&fh_out);
		assert(res == MPI_SUCCESS);
	}
	bef = MPI_Wtime();
	res = MPI_Barrier(MPI_COMM_WORLD);
	assert(res == MPI_SUCCESS);
	aft = MPI_Wtime();
	xfprintf(stderr, "%s: synched processes (%.02f)\n", __func__, aft - bef);
}




int main(int argc, char *argv[]) {
	

	const char *mode = NULL;
	char *progname = basename(argv[0]);
	char *file_r1 = NULL, *file_r2 = NULL;
	char *buffer_r1, *buffer_r2;
	char *file_out = NULL;
	char *buffer_out;
	char *file_ref = NULL;
	char *rg_line = NULL, *hdr_line = NULL;
	char file_map[PATH_MAX], file_tmp[PATH_MAX];
	char *p, *q, *s, *e;
	uint8_t *a, *addr, *addr_map;
	int fd_in1, fd_in2;
	int proc_num, rank_num, rank_shr;
	int res, count;
	int files, nargs;
	int no_mt_io, i, c, copy_comment = 0;
	int ignore_alt = 0;
	double bef, aft;
	size_t localsize;
	size_t n = 0;
	off_t locoff, locsiz, *alloff, *curoff, maxsiz, totsiz, filsiz;
	struct stat stat_r1, stat_r2;

	MPI_Aint size_shr;
	MPI_Comm comm_shr;
	MPI_File fh_r1, fh_r2, fh_out, fh_tmp;
	MPI_Offset *coff, m, size_map, size_tot;
	MPI_Status status;
	MPI_Win win_shr;

	mem_opt_t *opt, opt0;
	mem_pestat_t pes[4], *pes0 = NULL;
	bwaidx_t indix;
	bseq1_t *seqs;

	ktp_aux_t		 aux;
    worker_t         w;
	bool			 is_o	   = 0;
	int64_t			 nread_lim = 0;

	// uint64_t tim = __rdtsc();
	memset(&aux, 0, sizeof(ktp_aux_t));
	memset(pes, 0, 4 * sizeof(mem_pestat_t));
	for (i = 0; i < 4; ++i) pes[i].failed = 1;
	
	// opterr = 0;
	aux.fp = stdout;
	aux.opt = opt = mem_opt_init();
	memset(&opt0, 0, sizeof(mem_opt_t));


	if (strcmp(argv[1], "mem") == 0)
	{
		uint64_t tim = __rdtsc();
		kstring_t pg = {0,0,0};
		extern char *bwa_pg;

		fprintf(stderr, "-----------------------------\n");
#if __AVX512BW__
		fprintf(stderr, "Executing in AVX512 mode!!\n");
#endif
#if ((!__AVX512BW__) & (__AVX2__))
		fprintf(stderr, "Executing in AVX2 mode!!\n");
#endif
#if ((!__AVX512BW__) && (!__AVX2__) && (__SSE2__))
		fprintf(stderr, "Executing in SSE4.1 mode!!\n");
#endif
#if ((!__AVX512BW__) && (!__AVX2__) && (!__SSE2__))
		fprintf(stderr, "Executing in Scalar mode!!\n");
#endif
		fprintf(stderr, "-----------------------------\n");

		ksprintf(&pg, "@PG\tID:bwa\tPN:bwa\tVN:%s\tCL:%s", PACKAGE_VERSION, argv[0]);
		for (int i = 1; i < argc; ++i) ksprintf(&pg, " %s", argv[i]);
		ksprintf(&pg, "\n");
		bwa_pg = pg.s;
	}	

	/*
	 * DEBUG
	 *
	 */
	fprintf(stderr, "DEBUG 0 \n");

	/*
	if (argc < 2) {
		fprintf(stderr, "Program: MPI version of BWA MEM\n\n"
			"Version: v%s\n\n"
			"Contact 1: Frederic Jarlier (frederic.jarlier@curie.fr)\n\n"
			"usage : mpirun -n TOTAL_PROC %s mem -t 8 -o RESULTS REFERENCE FASTQ_R1 FASTQ_R2\n\n"
			"Requirements : After the creation of reference file with BWA you need to create a referenced\n"
			"	   map file genome with pidx like this  pidx ref.fasta generate a ref.fasta.map.\n"
			"	   The .map file is a copy of memory mapped reference used for shared memory purpose.\n",
			VERSION, progname);
		return 1; }
	*/

	/* Validate provided command (first argument) */
	if (strcmp(argv[1], "mem") != 0) {
		fprintf(stderr, "%s: unsupported %s command\n", progname, argv[1]);
		return 1; }

	/*
	 * DEBUG
	 *
	 */
	fprintf(stderr, "DEBUG 1 \n");


		
	/* Parse input arguments */
	while ((c = getopt(argc, argv, "1paMCSPVYjk:c:v:s:r:t:R:A:B:O:E:U:w:L:d:T:Q:D:m:I:N:W:x:G:h:y:K:X:H:o:q:")) >= 0)
	{
		if (c == 'k') opt->min_seed_len = atoi(optarg), opt0.min_seed_len = 1;
		else if (c == '1') no_mt_io = 1;
		else if (c == 'x') mode = optarg;
		else if (c == 'w') opt->w = atoi(optarg), opt0.w = 1;
		else if (c == 'A') opt->a = atoi(optarg), opt0.a = 1, assert(opt->a >= INT_MIN && opt->a <= INT_MAX);
		else if (c == 'B') opt->b = atoi(optarg), opt0.b = 1, assert(opt->b >= INT_MIN && opt->b <= INT_MAX);
		else if (c == 'T') opt->T = atoi(optarg), opt0.T = 1, assert(opt->T >= INT_MIN && opt->T <= INT_MAX);
		else if (c == 'U')
			opt->pen_unpaired = atoi(optarg), opt0.pen_unpaired = 1, assert(opt->pen_unpaired >= INT_MIN && opt->pen_unpaired <= INT_MAX);
		else if (c == 't')
			opt->n_threads = atoi(optarg), opt->n_threads = opt->n_threads > 1? opt->n_threads : 1, assert(opt->n_threads >= INT_MIN && opt->n_threads <= INT_MAX);
		else if (c == 'o')
		{
			/*is_o = 1;
			aux.fp = fopen(optarg, "w");
			if (aux.fp == NULL) {
				fprintf(stderr, "Error: can't open %s input file\n", optarg);
				exit(0);
			}
			/*fclose(aux.fp);*/
			/*MPI_File_open(MPI_COMM_WORLD, optarg, MPI_MODE_CREATE|MPI_MODE_WRONLY, MPI_INFO_NULL, &aux.mfp);*/
			file_out = optarg;		
			//aux.totEl = 0;
		}
		else if (c == 'q') {
			nread_lim = atoi(optarg);
		}
		else if (c == 'P') opt->flag |= MEM_F_NOPAIRING;
		else if (c == 'a') opt->flag |= MEM_F_ALL;
		else if (c == 'p') opt->flag |= MEM_F_PE | MEM_F_SMARTPE;
		else if (c == 'M') opt->flag |= MEM_F_NO_MULTI;
		else if (c == 'S') opt->flag |= MEM_F_NO_RESCUE;
		else if (c == 'Y') opt->flag |= MEM_F_SOFTCLIP;
		else if (c == 'V') opt->flag |= MEM_F_REF_HDR;
		else if (c == 'c') opt->max_occ = atoi(optarg), opt0.max_occ = 1;
		else if (c == 'd') opt->zdrop = atoi(optarg), opt0.zdrop = 1;
		else if (c == 'v') bwa_verbose = atoi(optarg);
		else if (c == 'j') ignore_alt = 1;
		else if (c == 'r')
			opt->split_factor = atof(optarg), opt0.split_factor = 1.;
		else if (c == 'D') opt->drop_ratio = atof(optarg), opt0.drop_ratio = 1.;
		else if (c == 'm') opt->max_matesw = atoi(optarg), opt0.max_matesw = 1;
		else if (c == 's') opt->split_width = atoi(optarg), opt0.split_width = 1;
		else if (c == 'G')
			opt->max_chain_gap = atoi(optarg), opt0.max_chain_gap = 1;
		else if (c == 'N')
			opt->max_chain_extend = atoi(optarg), opt0.max_chain_extend = 1;
		else if (c == 'W')
			opt->min_chain_weight = atoi(optarg), opt0.min_chain_weight = 1;
		else if (c == 'y')
			opt->max_mem_intv = atol(optarg), opt0.max_mem_intv = 1;
		else if (c == 'C') aux.copy_comment = 1;
		//else if (c == 'K') fixed_chunk_size = atoi(optarg);
		else if (c == 'X') opt->mask_level = atof(optarg);
		else if (c == 'h')
		{
			opt0.max_XA_hits = opt0.max_XA_hits_alt = 1;
			opt->max_XA_hits = opt->max_XA_hits_alt = strtol(optarg, &p, 10);
			if (*p != 0 && ispunct(*p) && isdigit(p[1]))
				opt->max_XA_hits_alt = strtol(p+1, &p, 10);
		}
		else if (c == 'Q')
		{
			opt0.mapQ_coef_len = 1;
			opt->mapQ_coef_len = atoi(optarg);
			opt->mapQ_coef_fac = opt->mapQ_coef_len > 0? log(opt->mapQ_coef_len) : 0;
		}
		else if (c == 'O')
		{
			opt0.o_del = opt0.o_ins = 1;
			opt->o_del = opt->o_ins = strtol(optarg, &p, 10);
			if (*p != 0 && ispunct(*p) && isdigit(p[1]))
				opt->o_ins = strtol(p+1, &p, 10);
		}
		else if (c == 'E')
		{
			opt0.e_del = opt0.e_ins = 1;
			opt->e_del = opt->e_ins = strtol(optarg, &p, 10);
			if (*p != 0 && ispunct(*p) && isdigit(p[1]))
				opt->e_ins = strtol(p+1, &p, 10);
		}
		else if (c == 'L')
		{
			opt0.pen_clip5 = opt0.pen_clip3 = 1;
			opt->pen_clip5 = opt->pen_clip3 = strtol(optarg, &p, 10);
			if (*p != 0 && ispunct(*p) && isdigit(p[1]))
				opt->pen_clip3 = strtol(p+1, &p, 10);
		}
		else if (c == 'R')
		{
		if ((rg_line = bwa_set_rg(optarg)) == 0) {
			free(opt);
			if (is_o)
				fclose(aux.fp);
				return 1;
            }
		}
		else if (c == 'H')
		{
			if (optarg[0] != '@')
			{
				FILE *fp;
				if ((fp = fopen(optarg, "r")) != 0)
				{
					char *buf;
					buf = (char *) calloc(1, 0x10000);
					while (fgets(buf, 0xffff, fp))
					{
						i = strlen(buf);
						assert(buf[i-1] == '\n');
						buf[i-1] = 0;
						hdr_line = bwa_insert_header(buf, hdr_line);
					}
					free(buf);
					fclose(fp);
				}
			} else hdr_line = bwa_insert_header(optarg, hdr_line);
		}
		else if (c == 'I')
		{
			aux.pes0 = pes;
			pes[1].failed = 0;
			pes[1].avg = strtod(optarg, &p);
			pes[1].std = pes[1].avg * .1;
			if (*p != 0 && ispunct(*p) && isdigit(p[1]))
				pes[1].std = strtod(p+1, &p);
			pes[1].high = (int)(pes[1].avg + 4. * pes[1].std + .499);
			pes[1].low  = (int)(pes[1].avg - 4. * pes[1].std + .499);
			if (pes[1].low < 1) pes[1].low = 1;
			if (*p != 0 && ispunct(*p) && isdigit(p[1]))
				pes[1].high = (int)(strtod(p+1, &p) + .499);
			if (*p != 0 && ispunct(*p) && isdigit(p[1]))
				pes[1].low  = (int)(strtod(p+1, &p) + .499);
		}
		else {
			free(opt);
			if (is_o)
				fclose(aux.fp);
			return 1;
		}
	}
	
	/*
	 * DEBUG
	 *
	 */
	fprintf(stderr, "DEBUG 2 \n");

	/* Check output file name */
	if (rg_line)
	{
		hdr_line = bwa_insert_header(rg_line, hdr_line);
		free(rg_line);
	}

	if (opt->n_threads < 1) opt->n_threads = 1;
	/*
	 *
	 *  PROBLEME here 
		
	if (optind + 2 != argc && optind + 3 != argc) {
		//usage(opt);
		free(opt);
		if (is_o) 
			fclose(aux.fp);
		return 1;
	}
	*/


	/*
	 * DEBUG
	 *
	 */
	fprintf(stderr, "DEBUG 3 \n");

	/* Further input parsing */
	if (mode)
	{
		fprintf(stderr, "WARNING: bwa-mem2 doesn't work well with long reads or contigs; please use minimap2 instead.\n");
		if (strcmp(mode, "intractg") == 0)
		{
			if (!opt0.o_del) opt->o_del = 16;
			if (!opt0.o_ins) opt->o_ins = 16;
			if (!opt0.b) opt->b = 9;
			if (!opt0.pen_clip5) opt->pen_clip5 = 5;
			if (!opt0.pen_clip3) opt->pen_clip3 = 5;
		}
		else if (strcmp(mode, "pacbio") == 0 || strcmp(mode, "pbref") == 0 || strcmp(mode, "ont2d") == 0)
		{
			if (!opt0.o_del) opt->o_del = 1;
			if (!opt0.e_del) opt->e_del = 1;
			if (!opt0.o_ins) opt->o_ins = 1;
			if (!opt0.e_ins) opt->e_ins = 1;
			if (!opt0.b) opt->b = 1;
			if (opt0.split_factor == 0.) opt->split_factor = 10.;
			if (strcmp(mode, "ont2d") == 0)
			{
				if (!opt0.min_chain_weight) opt->min_chain_weight = 20;
				if (!opt0.min_seed_len) opt->min_seed_len = 14;
				if (!opt0.pen_clip5) opt->pen_clip5 = 0;
				if (!opt0.pen_clip3) opt->pen_clip3 = 0;
			}
			else
			{
				if (!opt0.min_chain_weight) opt->min_chain_weight = 40;
				if (!opt0.min_seed_len) opt->min_seed_len = 17;
				if (!opt0.pen_clip5) opt->pen_clip5 = 0;
				if (!opt0.pen_clip3) opt->pen_clip3 = 0;
			}
		}
		else
		{
			fprintf(stderr, "[E::%s] unknown read type '%s'\n", __func__, mode);
			free(opt);
			if (is_o)
				fclose(aux.fp);
			return 1;
        }
	} else update_a(opt, &opt0);
	

	bwa_fill_scmat(opt->a, opt->b, opt->mat);


	files = 0;
	file_ref = argv[optind+1+0];
	fprintf(stderr, "reference = %s \n", file_ref);

	
	file_r1 = argv[optind+1+1]; files += 1;
	file_r2 = argv[optind+1+2]; files += 1;
	opt->flag |= MEM_F_PE;
	
	
	
	/* Derived file names */
	sprintf(file_map, "%s.map", file_ref);

	/* start up MPI */
	//res = MPI_Init(&argc, &argv);
	int provided = 0;
	res = MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
	assert(res == MPI_SUCCESS);
	res = MPI_Comm_size(MPI_COMM_WORLD, &proc_num);
	assert(res == MPI_SUCCESS);
	res = MPI_Comm_rank(MPI_COMM_WORLD, &rank_num);
	assert(res == MPI_SUCCESS);

	 // some internal structures
	char *p1, *q1, *e1, *p2, *q2, *e2;
	int line_number, line_number2;	
	int64_t bases;
	double local_time_spend_mapping = 0;
	double before_local_mapping = 0;
	double after_local_mapping	= 0;
	double total_time_local_mapping  = 0;
	double total_time_mapping = 0;
	double total_time_reading_seq = 0;
	double grand_total_time_reading_seq = 0;
	double total_time_parsing = 0;
	double grand_total_time_parsing = 0;
	double total_time_writing = 0;
	double grand_total_time_writing = 0;
	size_t reads_r1, reads_r2, reads;	
	size_t offset_chunk;
	size_t offset_chunk_2;
	size_t size_chunk;
	size_t size_chunk_2;
	size_t total_local_reads_aligned= 0;
	size_t total_reads_check 		= 0;
	size_t local_num_reads = 0;
	size_t total_num_reads = 0;
	size_t grand_total_num_reads = 0;
	size_t *begin_offset_chunk 	= NULL;
	size_t *chunk_size 		        = NULL;
	size_t *reads_in_chunk 		= NULL;

	//MPI_Info finfo;
	//MPI_Info_create(&finfo);
	/*
	 * In this part you shall adjust the striping factor and unit according
	 * to the underlying filesystem.
	 * Harmless for other file system.
	 *
	 */
	//MPI_Info_set(finfo,"striping_factor", STRIPING_FACTOR);
	//MPI_Info_set(finfo,"striping_unit", STRIPING_UNIT); //2G striping
	//MPI_Info_set(finfo,"ind_rd_buffer_size", STRIPING_UNIT); //2gb buffer
	//MPI_Info_set(finfo,"romio_ds_read",DATA_SIEVING_READ);

	/*
	 * for collective reading and writing
	 * should be adapted too and tested according to the file system
	 * Harmless for other file system.
	 */
	//MPI_Info_set(finfo,"nb_proc", NB_PROC);
	//MPI_Info_set(finfo,"cb_nodes", CB_NODES);
	//MPI_Info_set(finfo,"cb_block_size", CB_BLOCK_SIZE);
	//MPI_Info_set(finfo,"cb_buffer_size", CB_BUFFER_SIZE);
	//MPI_Info_set(finfo, "collective_buffering", "true");
	//MPI_Info_set(finfo,"romio_cb_write","enable");
	//MPI_Info_set(finfo,"romio_cb_read","enable");


	/* Check that output file (-o) is not null ... */
	if (file_out == NULL) {
		fprintf(stderr, "missing mandatory output file (-o)\n");
		res = MPI_Finalize();
		assert(res == MPI_SUCCESS);
		exit(2);
	}

	/* Check R1 & R2 file sizes */
	if (file_r1 != NULL && stat(file_r1, &stat_r1) == -1) {
		fprintf(stderr, "%s: %s\n", file_r1, strerror(errno));
		res = MPI_Finalize();
		assert(res == MPI_SUCCESS);
		exit(2);
	}

	if (file_r2 != NULL && stat(file_r2, &stat_r2) == -1) {
		fprintf(stderr, "%s: %s\n", file_r2, strerror(errno));
		res = MPI_Finalize();
		assert(res == MPI_SUCCESS);
		exit(2);
	}

	opt->chunk_size = 10000000;

	/*
	 * DEBUG
	 *
	 */
	fprintf(stderr, "DEBUG 4 file_r1 = %s :: file_r2 = %s \n", file_r1, file_r2 );

	if ( (file_r1 != NULL && file_r2 != NULL  && (stat_r1.st_size == stat_r2.st_size)))  {
	
		/*
	 	 * 
	     * We are in case we are paired and not trimmed
	     *
	 	 */

		/* Work around build warning in non timing case */
		aft = 0; aft++;
		bef = 0; bef++;
		/*
		 * Rank 0 estimate the size of a read
		 *
		 */
		size_t slen, blen;
		off_t tmp_sz = 1024;
		if (rank_num == 0){
			// 512 Mo are
			int fd_tmp = open(file_r1, O_RDONLY, 0666);
			char *buffer = malloc(tmp_sz  + 1);
			buffer[tmp_sz] = '\0';
			size_t read_out = read(fd_tmp, buffer, tmp_sz);
			assert(read_out);
			assert(strlen(buffer) == tmp_sz);
			assert( *buffer == '@');
			/* Estimate sequence size (bases and bytes) */
			/* With this estimation we compute approximately the chuncks size and offset*/

			s = buffer;
			e = buffer + tmp_sz;
			p = q = s;
			while (q < e && *q != '\n') q++; p = ++q;
			while (q < e && *q != '\n') q++; blen = q - p; p = ++q;
			while (q < e && *q != '\n') q++; p = ++q;
			while (q < e && *q != '\n') q++; slen = q - s + 1;

			/* Split local buffer in chunks of 100000000 bases */
			free(buffer);
		}

		//Rank O broadcast the size of a read
	 	res = MPI_Bcast(&blen, 1, MPI_LONG_LONG_INT, 0, MPI_COMM_WORLD);
		assert(res == MPI_SUCCESS);

		/*
		 * Split sequence files in chunks
		 */
		MPI_File mpi_fd_in1, mpi_fd_in2;
		bef = MPI_Wtime();

		res = MPI_File_open(MPI_COMM_WORLD, file_r1,  MPI_MODE_RDONLY , MPI_INFO_NULL, &mpi_fd_in1);
		assert(res == MPI_SUCCESS);
		
		res = MPI_File_open(MPI_COMM_WORLD, file_r1,  MPI_MODE_RDONLY , MPI_INFO_NULL, &mpi_fd_in2);
		assert(res == MPI_SUCCESS);
		
		/*
		 * first we parse the buffer and see
		 * how many reads we have
		 */

		size_t read_number=0;
		assert(fd_in1 != -1);
		size_t *goff = NULL; //global offset contain the start offset in the fastq
		goff = malloc((proc_num + 1) * sizeof(size_t));
		char *current_line = NULL;

		int i = 0;
		int j = 0;
		// TODO problem here when we have only one job!
		size_t lsize = stat_r1.st_size / proc_num;
		goff[0]=0;
		for(i = 1 ; i < proc_num; i++){goff[i] = lsize*i;}
		goff[proc_num] = stat_r1.st_size;
		char *buffer_r0 = malloc( tmp_sz + 1);
		buffer_r0[tmp_sz] = '\0';

		res = MPI_File_read_at(mpi_fd_in1, (MPI_Offset)goff[rank_num], buffer_r0, tmp_sz, MPI_CHAR, &status);
		assert(res == MPI_SUCCESS);	
		p = buffer_r0;
		e = buffer_r0 + tmp_sz;

		while (p < e) {
			if (*p != '@') { p++; continue; }
			if (p != buffer_r0 && *(p-1) != '\n') { p++; continue; }
			q = p + 1;
			while (q < e && *q != '\n') q++; q++;
			while (q < e && *q != '\n') q++; q++;
			if (q < e && *q == '+') break;
			p++;
		}
		assert(*p == '@');
		//we update begining offsets of windows in order to start at the begining of a read
		goff[rank_num] += p - buffer_r0;	
		free(buffer_r0);
		res = MPI_File_close(&mpi_fd_in1);
		assert(res == MPI_SUCCESS);
		//now we exchange the goff buffer between all proc
		size_t goff_inter = goff[rank_num]; //avoid memcpy overlap
		//rank 0 gather the vector
		res = MPI_Allgather(&goff_inter, 1, MPI_LONG_LONG_INT, goff , 1, MPI_LONG_LONG_INT, MPI_COMM_WORLD);
		assert(res == MPI_SUCCESS);

		//we compute the new size according to the shift
		//We calculate the size to read for each process
		int ind = rank_num;
		size_t siz2read = goff[ind+1]-goff[ind];
		MPI_Barrier(MPI_COMM_WORLD);
		j=0;
		size_t off_in_file = goff[ind]; //Current offset in file sam
	
		size_t total_siz_read = 0;
		size_t read_buffer_sz = 0;
		size_t offset_read = 0;
	
		size_t offset_buffer_r1 = 0;
		size_t offset_tmp  = 0;
		size_t count1;

		MPI_Datatype arraytype;
		MPI_Datatype arraytype0;	
		MPI_Datatype arraytype_r1;
		MPI_Datatype arraytype_r2;

		if ( siz2read < DEFAULT_INBUF_SIZE ) read_buffer_sz = siz2read;
		else read_buffer_sz = DEFAULT_INBUF_SIZE;

		size_t *local_read_offsets = calloc(1 , sizeof(size_t));
		int *local_read_size    = calloc(1, sizeof(int));
		assert( local_read_offsets != NULL);
		assert( local_read_size != NULL);


		bef = MPI_Wtime();
		char *b, *r, *t;
		size_t offset_end_buff;
		size_t pos_in_vect_offset = 0;
		size_t lines = 0;
		size_t total_parsing = 0;
		size_t new_offset = 0;
		size_t new_off_in_file =0;
		size_t g=0;
		size_t w=0;
		size_t total_read = 0;
		size_t total_lines = 0;
		while (1){

			buffer_r1 = malloc(read_buffer_sz + 1);
			assert( buffer_r1 != NULL );
			buffer_r1[read_buffer_sz] = 0;		
		
			// FOR TEST //
			// MPI_Type_contiguous(read_buffer_sz, MPI_CHAR, &arraytype0);
			// MPI_Type_commit(&arraytype0);
			// MPI_File_set_view(mpi_fd_in2, (MPI_Offset)off_in_file, MPI_CHAR, MPI_CHAR, "native", MPI_INFO_NULL ) ; 
			// res = MPI_File_read(mpi_fd_in2, b , 1, arraytype0, &status);

			res = MPI_File_read_at(mpi_fd_in2, (MPI_Offset)off_in_file, buffer_r1, read_buffer_sz, MPI_CHAR, MPI_STATUS_IGNORE);
			assert(res == MPI_SUCCESS);
			assert(*buffer_r1 == '@');	
			//we search the last read 
			b = buffer_r1;
			r  = b + read_buffer_sz;

			if ( read_buffer_sz == DEFAULT_INBUF_SIZE){			
				//go to last read			
				while (r-- != b){if (*r == '\n' && *(r+1) == '+') {r--; break;}}					
				while (r-- != b){if (*r == '\n') break;}
				while (r-- != b){if (*r == '@') break;			}			
				r--; //stop at \n
				offset_end_buff = (r - b);
			}
			else
				offset_end_buff = (r - b);
		
			//we count the number of lines
			t = b ;
			e = buffer_r1 + offset_end_buff;
			m = 0;
			lines = 0;		
			while (t++ < e){if (*t == '\n') lines++;}

			local_num_reads =  lines/4;
			total_lines += lines;		
			total_num_reads += local_num_reads;
			
			local_read_size  	= (int *)realloc(local_read_size, sizeof(int) * (total_num_reads + 1));
			local_read_offsets	= (size_t *)realloc(local_read_offsets, sizeof(size_t) * (total_num_reads + 1));

			assert( local_read_offsets != NULL);
			assert( local_read_size != NULL);
		
			local_read_size[total_num_reads] = 0;
			local_read_offsets[total_num_reads] = 0;
		
			//for the first first read
			local_read_offsets[pos_in_vect_offset] = off_in_file;
			local_read_size[pos_in_vect_offset]    = blen;				
			pos_in_vect_offset++;
			g = 0;
			t = buffer_r1;
			size_t w = 0;
			//we move to the next line because we test *(t-1)
			lines = 0;
		
			while (t < e ){
				if (*t == '\n') lines++;
				if (lines == 4) {
					local_read_size[pos_in_vect_offset] = blen;
					local_read_offsets[pos_in_vect_offset] = off_in_file + g + 1; //+1 for \n caracters
					pos_in_vect_offset++;				
					w++; lines = 0;//t+=blen;g+=blen; 
				}			
				t++;g++;
			}
	
			assert(g == offset_end_buff);
			total_parsing 	+= offset_end_buff; 				
		
			if (total_parsing == siz2read) {free(buffer_r1);break;}

			if ((siz2read - total_parsing) < DEFAULT_INBUF_SIZE)
				read_buffer_sz = siz2read - total_parsing;			
			else read_buffer_sz = DEFAULT_INBUF_SIZE;
					
			off_in_file  += offset_end_buff + 1;
	
			free(buffer_r1);				
		}

		fprintf(stderr, "rank %d : in wait\n", rank_num);
		MPI_Barrier(MPI_COMM_WORLD);
		aft = MPI_Wtime();
		fprintf(stderr, "%s: rank %d num reads parsed: %zu ::: time spend reading and parsing entire buffer = (%.02f) \n", __func__, rank_num, total_num_reads, aft - bef);
		lines = 0;	
		if (goff) free(goff);
	
		MPI_File_close(&mpi_fd_in2);			
		res = MPI_Reduce(&total_num_reads, &grand_total_num_reads, 1,MPI_LONG_LONG_INT, MPI_SUM, 0,MPI_COMM_WORLD);
		assert ( res == MPI_SUCCESS);	
		res = MPI_Bcast(&grand_total_num_reads, 1, MPI_LONG_LONG_INT, 0, MPI_COMM_WORLD);
		assert ( res == MPI_SUCCESS);	

		size_t num_reads_by_proc[proc_num];
		res = MPI_Allgather(&total_num_reads, 1, MPI_LONG_LONG_INT, num_reads_by_proc, 1, MPI_LONG_LONG_INT, MPI_COMM_WORLD);
		//we dispatch the previous result
	
		if ( rank_num == 0 || rank_num ==1 )
			fprintf(stderr, "rank %d ::: total_num_reads = %zu \n", rank_num, grand_total_num_reads);

		/*
		 * Now each rank compute buffers offset start and end.
		 */

		local_num_reads = total_num_reads;
		//now we estimate the number of chunk per rank
		size_t chunck_num = (local_num_reads * blen) / (( opt->chunk_size * opt->n_threads) / 2);
		chunck_num += 2; //the last chunk hold the remain bases
        fprintf(stderr, "rank %d ::: chunck num = %zu \n", rank_num, chunck_num);

		size_t h=0;
		for ( h = 0; h < total_num_reads; h++){
			assert( local_read_size[h] == blen );
			assert( local_read_offsets[h] >= 0 );
		}

		/*
		 * We compute number of chunks for forward reads
		 * and send it to
		 */
		bef = MPI_Wtime();

		size_t bases_previous_chunk      = 0;
		size_t offset_previous_chunk 	 = 0;
		size_t size_previous_chunk 	 = 0;

		if (rank_num > 0){

				//we wait the previous rank to send the final size chunk
				//and offset of the chunk
				MPI_Recv(&bases_previous_chunk,
						1,
						MPI_LONG_LONG_INT,
						rank_num - 1,
						0,
						MPI_COMM_WORLD,
						MPI_STATUS_IGNORE);

				MPI_Recv(&offset_previous_chunk,
						1,
						MPI_LONG_LONG_INT,
						rank_num - 1,
						0,
						MPI_COMM_WORLD,
						MPI_STATUS_IGNORE);

				MPI_Recv(&size_previous_chunk,
						1,
						MPI_LONG_LONG_INT,
						rank_num - 1,
						0,
						MPI_COMM_WORLD,
						MPI_STATUS_IGNORE);
			}

		/*
		 * We update information from previous rank
		 */

		// we allocate vector for chunks offset
		begin_offset_chunk 	= calloc(chunck_num, sizeof(size_t));
		chunk_size 		    = calloc(chunck_num, sizeof(size_t));
		reads_in_chunk 		= calloc(chunck_num, sizeof(size_t));

		assert( begin_offset_chunk != NULL );
		assert( chunk_size != NULL );
		assert( reads_in_chunk != NULL );
	
		size_t u1=0;
	
		size_t counter_bases  	 = 0;
		size_t bytes_in_chunk 	 = 0;

		maxsiz = ( opt->chunk_size * opt->n_threads) / 2; //half the normal number of bases for bwa mem
		size_t index_in_chunk = 0;

		size_t begin_offset = 0;
		size_t chunk_count = 0;

		if (rank_num == 0){

			for (u1 = 0; u1 < local_num_reads; u1++){
				//we for offsets multiple of maxsize

				if (counter_bases == 0)  begin_offset = local_read_offsets[u1];

				counter_bases 	+= local_read_size[u1];
				bytes_in_chunk  += local_read_offsets[u1+1] - local_read_offsets[u1];

				if ( counter_bases > maxsiz){
					//then we have the number of bases wanted
					begin_offset_chunk[index_in_chunk] = begin_offset;
					chunk_size[index_in_chunk] 	   = bytes_in_chunk;
					reads_in_chunk[index_in_chunk]	   = counter_bases / blen; 
					chunk_count 			   +=1;
					index_in_chunk 			   +=1;
					//we reset counter of bases
					counter_bases  			   = 0;
					bytes_in_chunk 			   = 0;
				}
			}
		}
		else{

			counter_bases 			= bases_previous_chunk;
			bytes_in_chunk 			= size_previous_chunk;
			begin_offset				= offset_previous_chunk;

			for (u1 = 0; u1 < local_num_reads; u1++){
				//we for offsets multiple of maxsize

				if (counter_bases == 0) begin_offset = local_read_offsets[u1];

				counter_bases 	+= local_read_size[u1];
				bytes_in_chunk    += local_read_offsets[u1+1] - local_read_offsets[u1];

				if ( counter_bases > maxsiz){
					//then we have the number of bases wanted
					begin_offset_chunk[index_in_chunk] 	= begin_offset;
					chunk_size[index_in_chunk] 	   		= bytes_in_chunk;
					reads_in_chunk[index_in_chunk]	   	= counter_bases / blen; 
					chunk_count 			   +=1;
					index_in_chunk 			   +=1;
					//we reset counter of bases
					counter_bases  			   = 0;
					bytes_in_chunk 			   = 0;
				}
			}
		}
		if (rank_num == (proc_num - 1)){

			// we complete the last chunk
			// with the last reads

			begin_offset_chunk[index_in_chunk] 	= begin_offset;
			chunk_size[index_in_chunk] 		= bytes_in_chunk;
			reads_in_chunk[index_in_chunk]		= counter_bases / blen;

			index_in_chunk 	+=1;
			chunk_count 	+=1;

		}
		if (rank_num < (proc_num -1)){

			//we send to rank + 1
			MPI_Send(&counter_bases,
					1,
					MPI_LONG_LONG_INT,
					rank_num + 1,
					0,
					MPI_COMM_WORLD);

			MPI_Send(&begin_offset,
					1,
					MPI_LONG_LONG_INT,
					rank_num + 1,
					0,
					MPI_COMM_WORLD);

			MPI_Send(&bytes_in_chunk,
					1,
					MPI_LONG_LONG_INT,
					rank_num + 1,
					0,
					MPI_COMM_WORLD);
		}

		aft = MPI_Wtime();
		fprintf(stderr, "%s: rank %d time spend evaluating chunks = (%.02f) \n", __func__, rank_num, aft - bef);

		free(local_read_offsets);
		free(local_read_size);

         /*
             Create SAM header
 *           TODO: Add line for BWA version
 *       */

        uint64_t tim = __rdtsc();
        fprintf(stderr, "* Ref file: %s\n", file_ref);
        aux.fmi = new FMI_search(file_ref);
        aux.fmi->load_index();
        tprof[FMI][0] += __rdtsc() - tim;


         if (rank_num == 0) {
            int s, len;
            char *buff;

            res = MPI_File_open(MPI_COMM_SELF, file_out, MPI_MODE_CREATE|MPI_MODE_WRONLY, MPI_INFO_NULL, &fh_out);
            assert(res == MPI_SUCCESS);
            for (s = 0; s < aux.fmi->idx->bns->n_seqs; ++s) {
            len = asprintf(&buff, "@SQ\tSN:%s\tLN:%d\n", aux.fmi->idx->bns->anns[s].name, aux.fmi->idx->bns->anns[s].len);
            res = MPI_File_write(fh_out, buff, len, MPI_CHAR, &status);
            assert(res == MPI_SUCCESS);
            res = MPI_Get_count(&status, MPI_CHAR, &count);
            assert(res == MPI_SUCCESS);
            assert(count == len);
            free(buff);
            }
            if (hdr_line != NULL) {
            len = asprintf(&buff, "%s\n", hdr_line);
            res = MPI_File_write(fh_out, buff, len, MPI_CHAR, &status);
            assert(res == MPI_SUCCESS);
            res = MPI_Get_count(&status, MPI_CHAR, &count);
            assert(res == MPI_SUCCESS);
            assert(count == len);
            free(buff);
            }
            if (rg_line != NULL) {
            len = asprintf(&buff, "%s\n", rg_line);
            res = MPI_File_write(fh_out, buff, len, MPI_CHAR, &status);
            assert(res == MPI_SUCCESS);
            res = MPI_Get_count(&status, MPI_CHAR, &count);
            assert(res == MPI_SUCCESS);
            assert(count == len);
            free(buff);
            }
            res = MPI_File_close(&fh_out);
            assert(res == MPI_SUCCESS);
        }

        bef = MPI_Wtime();
        res = MPI_Barrier(MPI_COMM_WORLD);
        assert(res == MPI_SUCCESS);
        aft = MPI_Wtime();

        fprintf(stderr, "%s: sam header (%.02f)\n", __func__, aft - bef);
    
        res = MPI_File_open(MPI_COMM_WORLD, file_out, MPI_MODE_WRONLY|MPI_MODE_APPEND, MPI_INFO_NULL, &fh_out);
        assert(res == MPI_SUCCESS);

        if (file_r1 != NULL) {
            res = MPI_File_open(MPI_COMM_WORLD, file_r1, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh_r1);
            assert(res == MPI_SUCCESS);
        }
        if (file_r2 != NULL) {
            res = MPI_File_open(MPI_COMM_WORLD, file_r2, MPI_MODE_RDONLY, MPI_INFO_NULL, &fh_r2);
            assert(res == MPI_SUCCESS);
        }


		/*
			bwa_mem2idx(size_map, addr_map, &indix);
		
			if (ignore_alt)
				for (c = 0; c < indix.bns->n_seqs; ++c)
				indix.bns->anns[c].is_alt = 0;
		*/
		


		/* 
		 * Load bwt2/FMI index 
		 *
		 */
	
		//uint64_t tim = __rdtsc();
   
        /* Matrix for SWA */
        bwa_fill_scmat(opt->a, opt->b, opt->mat);

 
        //fprintf(stderr, "* Ref file: %s\n", file_ref);          
        //aux.fmi = new FMI_search(file_ref);
        //aux.fmi->load_index();
        //tprof[FMI][0] += __rdtsc() - tim;
    
        // reading ref string from the file
        tim = __rdtsc();
        fprintf(stderr, "* Reading reference genome..\n");


	    //ref_string = addr_map_ref;
	
		char binary_seq_file[PATH_MAX];
        strcpy_s(binary_seq_file, PATH_MAX, file_ref);
        strcat_s(binary_seq_file, PATH_MAX, ".0123");
        sprintf(binary_seq_file, "%s.0123", file_ref);
        
        fprintf(stderr, "* Binary seq file = %s\n", binary_seq_file);
        FILE *fr = fopen(binary_seq_file, "r");
        if (fr == NULL) {
            fprintf(stderr, "Error: can't open %s input file\n", binary_seq_file);
            exit(EXIT_FAILURE);
        }
        int64_t rlen = 0;
        fseek(fr, 0, SEEK_END); 
        rlen = ftell(fr);
        ref_string = (uint8_t*) _mm_malloc(rlen, 64);
        aux.ref_string = ref_string;
        rewind(fr);
                                
        /* Reading ref. sequence */
        err_fread_noeof(ref_string, 1, rlen, fr);
                                        
        uint64_t timer  = __rdtsc();
        tprof[REF_IO][0] += timer - tim;
                               
        fclose(fr);
        fprintf(stderr, "* Reference genome size: %ld bp\n", rlen);
        fprintf(stderr, "* Done reading reference genome !!\n\n");

        if (ignore_alt)
        for (i = 0; i < fmi->idx->bns->n_seqs; ++i)
            fmi->idx->bns->anns[i].is_alt = 0;


		// here we loop until there's nothing to read
		// in the offset and size file
	
		before_local_mapping = MPI_Wtime();

		/*
		 *	In this part we initiate the worker threads  
		 *
		 */
        aux.ks2                       = 0;
        aux.ks                        = 0;
        aux.task_size = opt->chunk_size * opt->n_threads;
		fprintf(stderr, "\naux.task_size : %d\n", aux.task_size);
		worker_t	 w2;
		mem_opt_t	*opt			  = aux.opt;
		nthreads = opt->n_threads; // global variable for profiling!
        w2.nthreads = opt->n_threads;
        readLen = blen;
		//here we initialize pthreads 
		fprintf(stderr, "\nThreads used (compute): %d\n", nthreads);
        aux.actual_chunk_size = chunk_size[0];
        fprintf(stderr, "\naux2->actual_chunk_size: %d\n", aux.actual_chunk_size);
		nreads = aux.actual_chunk_size/ (readLen) + 10;
		fprintf(stderr, "Info: projected #read in a task: %ld\n", (long)nreads);
	
		/* All memory allocation */
		memoryAlloc(&aux, w2, nreads , nthreads);
        w2.ref_string = aux.ref_string;
        w2.fmi = aux.fmi;
        w2.nreads  = nreads;
		ktp_data_t *ret = (ktp_data_t *) calloc(1, sizeof(ktp_data_t));


       // here we loop until there's nothing to read
       // in the offset and size file
       //   before_local_mapping = MPI_Wtime();
       
		//we loop the chunck_count
		for (u1 = 0; u1 < chunk_count; u1++){

			offset_chunk = begin_offset_chunk[u1];
			size_chunk   = chunk_size[u1];
			assert(size_chunk != 0);
			/*
			 * Read sequence datas ...
			 *
			 */
			bef = MPI_Wtime();

			buffer_r1 = malloc(size_chunk+1);
			assert(buffer_r1 != NULL);
			buffer_r1[size_chunk] = 0;
		
			// FOR TEST //
			//MPI_Type_contiguous(size_chunk, MPI_CHAR, &arraytype_r1);
			//MPI_Type_commit(&arraytype_r1);
			//MPI_File_set_view(fh_r2, (MPI_Offset)offset_chunk, MPI_CHAR, MPI_CHAR, "native", finfo ) ; 
			//res = MPI_File_read(fh_r2, buffer_r1, 1, arraytype_r1, &status);

			res = MPI_File_read_at(fh_r1, offset_chunk, buffer_r1, size_chunk, MPI_CHAR, &status);		
			assert(res == MPI_SUCCESS);
			res = MPI_Get_count(&status, MPI_CHAR, &count);
			assert(res == MPI_SUCCESS);
			assert(count == (int)size_chunk && *buffer_r1 == '@');

			buffer_r2 = malloc(size_chunk+1);
			assert(buffer_r2 != NULL);
			buffer_r2[size_chunk]=0;

			// FOR TEST //
			//MPI_Type_contiguous(size_chunk, MPI_CHAR, &arraytype_r2);
			//MPI_Type_commit(&arraytype_r2);
			//MPI_File_set_view(fh_r1, (MPI_Offset)offset_chunk, MPI_CHAR, MPI_CHAR, "native", finfo ) ; 
			//res = MPI_File_read(fh_r1, buffer_r2, 1, arraytype_r2, &status);		
			res = MPI_File_read_at(fh_r2, offset_chunk, buffer_r2, size_chunk, MPI_CHAR, &status);		
			assert(res == MPI_SUCCESS);
			res = MPI_Get_count(&status, MPI_CHAR, &count);
			assert(res == MPI_SUCCESS);
			assert(count == (int) size_chunk && *buffer_r2 == '@');

			aft = MPI_Wtime();
			fprintf(stderr, "%s: read sequences (%.02f)\n", __func__, aft - bef);
			total_time_reading_seq += (aft - bef);

			bef = MPI_Wtime();
			reads_r1 = reads_in_chunk[u1];
			
			if (file_r2 != NULL) reads_r2 = reads_in_chunk[u1];

			reads = reads_r1 + reads_r2; bases = 0;
			total_local_reads_aligned += reads/2;
			assert(reads <= INT_MAX);
			fprintf(stderr, "%s: num_rank = %d :: number of reads = %zu \n", __func__, rank_num, reads);

			/* Parse sequences ... */
			seqs = malloc(reads * sizeof(*seqs));
			assert(seqs != NULL);

			//case one we are paired
			if (file_r1 != NULL && file_r2 !=NULL) {
				p1 = q1 = buffer_r1; e1 = buffer_r1 + size_chunk; line_number = 0;
				p2 = q2 = buffer_r2; e2 = buffer_r2 + size_chunk; 
				
				while (q1 < e1) {
					if (*q1 != '\n') { q1++; q2++; continue; }
					/* We have a full line ... process it */
					*q1 = '\0'; 
					*q2 = '\0';
					n = files * (line_number / 4);
				
					switch (line_number % 4) {
					case 0: /* Line1: Name and Comment */
						assert(*p1 == '@');
						seqs[n].name   = p1 + 1;
						seqs[n+1].name = p2 + 1;
					
						while (*p1 && !isspace((unsigned char)*p1)) {p1++;p2++;}
						if (*(p1-2) == '/' && isdigit((unsigned char)*(p1-1))) {*(p1-2) = '\0'; *(p2-2) = '\0';}
						if (*p1) {*p1++ = '\0'; *p2++ = '\0';}
						seqs[n].comment = (copy_comment != 0) ? p1 : NULL;
						seqs[n].sam = NULL;
						seqs[n+1].comment = (copy_comment != 0) ? p2 : NULL;
						seqs[n+1].sam = NULL;					
						break;
					case 1: /* Line2: Sequence */
						seqs[n].seq = p1;
						seqs[n].l_seq = q1 - p1;
						seqs[n+1].seq = p2;
						seqs[n+1].l_seq = q2 - p2;

						bases += seqs[n].l_seq;
						bases += seqs[n+1].l_seq;
						break;
					case 2: /* Line3: Ignored */
						assert(*p1 == '+');
						break;
					case 3: /* Line4: Quality */
						seqs[n].qual = p1;
						seqs[n+1].qual = p2;
						break; }
					p1 = ++q1; 
					p2 = ++q2; 
					line_number++; 
				}
				//fprintf(stderr, "rank %d ::: Parse %d lines \n",rank_num, line_number );
			}
		
			aft = MPI_Wtime();
			fprintf(stderr, "%s: parsed sequences (%.02f)\n", __func__, aft - bef);
			total_time_parsing += (aft - bef);
			fprintf(stderr, "rank %d ::: [M::%s] read %zu sequences (%ld bp)...\n",rank_num , __func__, reads, (long)bases);
			/* Datas computation ... */
			bef = MPI_Wtime();
			//fprintf(stderr, "rank %d ::::  Call memseqs with count sequences %zu \n", rank_num, reads);

			//mem_process_seqs(opt, indix.bwt, indix.bns, indix.pac, 0, (int)reads, seqs, pes0);

            ret->seqs   = seqs;
            ret->n_seqs     = reads;				

            int64_t size = 0;
            for (int i = 0; i < ret->n_seqs; ++i) size += ret->seqs[i].l_seq;

            fprintf(stderr, "\t[0000][ M::%s] read %d sequences (%ld bp)...\n",
                    __func__, ret->n_seqs, (long)size);
	

			fprintf(stderr, "[RANK %d] 1: Calling process()\n", rank_num);

			//fprintf(stderr, "Reallocating initial memory allocations!!\n");
			if (w2.regs) free(w2.regs); 
			if (w2.chain_ar) free(w2.chain_ar); 
			if (w2.seedBuf) free(w2.seedBuf);
			
			nreads = ret->n_seqs;
			w2.regs = (mem_alnreg_v *) calloc(nreads, sizeof(mem_alnreg_v));
			w2.chain_ar = (mem_chain_v*) malloc (nreads * sizeof(mem_chain_v));
			w2.seedBuf = (mem_seed_t *) calloc(sizeof(mem_seed_t),nreads * AVG_SEEDS_PER_READ);
			
            /* new call */
            w2.nreads=ret->n_seqs;
            mem_process_seqs(opt,
                             aux.n_processed,
                             ret->n_seqs,
                             ret->seqs,
                             aux.pes0,
                             w2);



            aux.n_processed += ret->n_seqs;            

			aft = MPI_Wtime();
			fprintf(stderr, "%s: computed mappings (%.02f)\n", __func__, aft - bef);

			//MPI_Barrier(MPI_COMM_WORLD);
			/* Write results ... */
			bef = MPI_Wtime();
			localsize = 0;
			for (n = 0; n < reads; n++) {
				/* Reuse .l_seq to store SAM line length to avoid multiple strlen() calls */
				seqs[n].l_seq = strlen(seqs[n].sam);
				localsize += seqs[n].l_seq; 
			}
			assert(localsize <= INT_MAX);
			buffer_out = malloc(localsize);
			assert(buffer_out != NULL);
			p = buffer_out;
			for (n = 0; n < reads; n++) {
				memmove(p, seqs[n].sam, seqs[n].l_seq);
				p += seqs[n].l_seq;
				free(seqs[n].sam); 
			}
			free(seqs);
			res = MPI_File_write_shared(fh_out, buffer_out, localsize, MPI_CHAR, &status);
			assert(res == MPI_SUCCESS);
			res = MPI_Get_count(&status, MPI_CHAR, &count);
			assert(res == MPI_SUCCESS);
			assert(count == (int)localsize);
			free(buffer_out);
			aft = MPI_Wtime();
			fprintf(stderr, "%s: wrote results (%.02f)\n", __func__, aft - bef);
			total_time_writing += (aft - bef);
			free(buffer_r1);
			free(buffer_r2);
		} //end for (u1 = 0; u1 < chunk_count; u1++){
	} //end if (file_r2 != NULL && stat_r1.st_size == stat_r2.st_size)
	
	

	/*
	*
	*   Print some statistics
	*
	*/


	after_local_mapping	 = MPI_Wtime();
	total_time_local_mapping = after_local_mapping - before_local_mapping;

	res = MPI_Allreduce(&total_time_local_mapping, &total_time_mapping, 1,MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
	assert ( res == MPI_SUCCESS);

	res = MPI_Allreduce(&total_local_reads_aligned, &total_reads_check, 1,MPI_LONG_LONG_INT, MPI_SUM, MPI_COMM_WORLD);
	assert ( res == MPI_SUCCESS);

	res = MPI_Allreduce(&total_time_reading_seq, &grand_total_time_reading_seq, 1,MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
	assert ( res == MPI_SUCCESS);

	res = MPI_Allreduce(&total_time_parsing, &grand_total_time_parsing, 1,MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
	assert ( res == MPI_SUCCESS);

	res = MPI_Allreduce(&total_time_writing, &grand_total_time_writing, 1,MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
	assert ( res == MPI_SUCCESS);
	
	fprintf(stderr, "rank %d :::: total_time_reading_seq = %.02f seconds \n", rank_num, grand_total_time_reading_seq);
	fprintf(stderr, "rank %d :::: total_time_writing = %.02f seconds \n", rank_num, grand_total_time_writing);
	fprintf(stderr, "rank %d :::: total_time_parsing = %.02f  seconds \n", rank_num, grand_total_time_parsing);
	fprintf(stderr, "rank %d :::: total_time_local_mapping = %.02f seconds \n", rank_num, total_time_local_mapping);
	fprintf(stderr, "rank %d :::: total_time_mapping = %.02f seconds \n", rank_num, total_time_mapping);
	fprintf(stderr, "rank %d :::: total_reads_check for all ranks= %zu \n", rank_num, total_reads_check);
	fprintf(stderr, "rank %d :::: total_local_reads_aligned = %zu \n", rank_num, total_local_reads_aligned);
	

	fprintf(stderr, "rank %d :::: finish mappings for reads \n", rank_num);
	if (begin_offset_chunk != NULL) free(begin_offset_chunk);
	if (chunk_size != NULL) free(chunk_size);
	if (reads_in_chunk != NULL) free(reads_in_chunk);

	if (opt != NULL) free(opt);

	if (file_r2 != NULL) {
		res = MPI_File_close(&fh_r2);
		assert(res == MPI_SUCCESS);
	}
	if (file_r1 != NULL) {
		res = MPI_File_close(&fh_r1);
		assert(res == MPI_SUCCESS);
	}

	//res = MPI_Win_free(&win_shr);
	//assert(res == MPI_SUCCESS);

	res = MPI_File_close(&fh_out);
	assert(res == MPI_SUCCESS);

	bef = MPI_Wtime();
	res = MPI_Finalize();
	assert(res == MPI_SUCCESS);
	aft = MPI_Wtime();
	fprintf(stderr, "%s: finalized synched processes (%.02f)\n", __func__, aft - bef);

	return 0;
}

