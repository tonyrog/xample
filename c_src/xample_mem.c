//
//  open / create shared memory segment 
//
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "xample.h"

xample_t* xample_create(char* name, size_t nsamples, size_t fdivpow2,
			size_t nchannels,
			double rate, mode_t mode, sample_t** data)
{
    size_t page_size;
    size_t frame_size;
    size_t buffer_size;
    size_t real_size;
    void* ptr;
    xample_t* xp;
    int fd;

    if ((page_size = sysconf(_SC_PAGE_SIZE)) == 0) {
	fprintf(stderr, "error: sysconf(_SC_PAGE_SIZE) return 0\n");
	return NULL;
    }
    buffer_size = nsamples*nchannels*sizeof(sample_t);
    real_size = (((buffer_size+page_size-1)/page_size)+1)*page_size;
    frame_size = page_size / (1 << fdivpow2);

    // start with trying unlink the segment (delete old one)
    
    if (shm_unlink(name) < 0) {
	perror("shm_unlink"); // normally ok if exited nice?
    }
    if ((fd=shm_open(name, O_CREAT | O_RDWR, mode)) < 0) {
	perror("shm_open");
	return NULL;
    }
    if (ftruncate(fd, real_size) < 0) {
	perror("ftruncate");
	close(fd);
	return NULL;
    }
    ptr = mmap(NULL, real_size, PROT_READ | PROT_WRITE, MAP_SHARED, 
	       fd, (off_t) 0);
    close(fd);
    if (ptr == MAP_FAILED) {
	perror("mmap");
	return NULL;
    }
    xp = (xample_t*) ptr;
    xp->current_page = 0;
    xp->first_page   = 0;
    xp->last_page    = (real_size/page_size)-2;
    xp->page_size    = page_size;
    xp->samples_per_page = page_size / sizeof(sample_t);

    xp->frames_per_page = (1 << fdivpow2);
    xp->current_frame = 0;
    xp->first_frame   = 0;
    xp->last_frame    = ((xp->last_page-xp->first_page)+1)*
	xp->frames_per_page-1;
    xp->frame_size    = frame_size;
    xp->samples_per_frame = frame_size / sizeof(sample_t);


    xp->rate         = (unsigned long) (rate*256);
    xp->channels     = nchannels;
    *data = (sample_t*) (ptr + page_size);
    return xp;
}

xample_t* xample_open(char* name, sample_t** data)
{
    size_t page_size;
    size_t buffer_size;
    void* ptr;
    int fd;

    if ((page_size = sysconf(_SC_PAGE_SIZE)) == 0) {
	fprintf(stderr, "error: sysconf(_SC_PAGE_SIZE) return 0\n");
	return NULL;
    }

    if ((fd=shm_open(name, O_RDONLY, 0)) < 0) {
	perror("shm_open");
	return NULL;
    }

    ptr = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, (off_t) 0);
    if (ptr == MAP_FAILED) {
	perror("mmap");
	close(fd);
	return NULL;
    }

    // calculate size and remap
    buffer_size = (((xample_t*)ptr)->last_page + 2)*page_size;

    if (munmap(ptr, page_size) < 0) {
	perror("munmap");
	close(fd);
	return NULL;
    }

    ptr = mmap(NULL, buffer_size, PROT_READ, MAP_SHARED, fd, (off_t) 0);
    close(fd);
    if (ptr == MAP_FAILED) {
	perror("mmap");
	return NULL;
    }
    *data = (sample_t*) (ptr + page_size);
    return (xample_t*) ptr;
}

int xample_close(xample_t* xp)
{
    if (xp != NULL) {
	size_t len = (xp->last_page + 2)*xp->page_size;
	return munmap((void*) xp, len);
    }
    return 0;
}
