/*
 *  Task that samples adc values in shared memory segment
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>

#include "xample.h"

double sample_freq = 10000.0;

#define AMPLITUDE   5000.0
#define OFFSET      3000

sample_t read_sample(void)
{
    static double x = 0.0;
    long v = 32767 + OFFSET + AMPLITUDE*sin(x*2*M_PI);
    x += 1/50.0;
    if (x >= 1.0) x = 0.0;
    return (sample_t) v;
}

int main(int argc, char** argv)
{
    size_t max_samples;
    size_t samples_per_page;
    unsigned long current_page;
    unsigned long first_page;
    unsigned long last_page;
    unsigned long first_page_offset;
    unsigned long page_offset;
    unsigned long udelay;
    sample_t* sample_buffer;
    xample_t* xp;
    double rate;
    int i;

    max_samples = (size_t)(sample_freq*5);  // 5 seconds of samples
    udelay = ((unsigned long)(1000000*(1/sample_freq)));

    if (argc < 2) {
	printf("usage: xample <name>\n");
	exit(1);
    }
    
    if ((xp = xample_create(argv[1], max_samples, 1, sample_freq, 0666,
			    &sample_buffer)) == NULL) {
	fprintf(stderr, "unable to create shared memory\n");
	exit(1);
    }

    current_page = xp->current_page;
    last_page    = xp->last_page;
    first_page   = xp->first_page;
    samples_per_page = xp->samples_per_page;
    rate         = (xp->rate >> 8) + (xp->rate & 0xff)/256.0;

    first_page_offset = first_page*samples_per_page;
    page_offset = first_page_offset;

    printf("page_size = %ld\n", xp->page_size);
    printf("rate = %f\n",       rate);
    printf("max_samples = %ld\n", max_samples);
    printf("samples_per_pages = %ld\n", samples_per_page);
    printf("udelay = %ld\n", udelay);
    printf("first_page = %lu\n", first_page);
    printf("last_page = %lu\n",  last_page);
    printf("current_page = %lu\n", current_page);

    // loop - sample data and save in mmap,
    // signal on unix sockets when trigger occure
    i = 0;
    while(1) {
	sample_t s = read_sample();
	sample_buffer[page_offset + i] = s;
	i++;
	if (i >= samples_per_page) {
	    i = 0;
	    if (current_page >= last_page) {
		current_page = first_page;
		page_offset  = first_page_offset;
	    }
	    else {
		current_page++;
		page_offset += samples_per_page;
	    }
	    xp->current_page = current_page;
	}
	usleep(udelay);
    }
}
