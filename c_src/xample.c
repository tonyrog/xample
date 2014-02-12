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

#define MAX_AMPLITUDE  20000.0
#define MIN_AMPLITUDE  1000.0
#define OFFSET      3000.0

sample_t read_sample(void)
{
    static double x = 0.0;
    static double ax = 0.0;
    static double ad = 0.01;
    double vf;

    vf = (MIN_AMPLITUDE + ax*(MAX_AMPLITUDE-MIN_AMPLITUDE))*sin(x*2*M_PI);
    vf += OFFSET;
    
    ax += ad;
    if (ax < 0.0) {
	ax = 0.0;
	ad = -ad;
    }
    else if (ax >= 1.0) {
	ax = 1.0;
	ad = -ad;
    }
    x += 0.01;
    if (x >= 1.0) x = 0.0;
    return (sample_t) (32767+(long)vf);
}

int main(int argc, char** argv)
{
    size_t max_samples;
    size_t samples_per_page;
    size_t samples_per_frame;
    unsigned long current_page;
    unsigned long first_page;
    unsigned long last_page;
    unsigned long current_frame;
    unsigned long first_frame;
    unsigned long last_frame;
    // unsigned long first_page_offset;
    unsigned long first_frame_offset;
    unsigned long frame_offset;
    unsigned long frames_per_page;
    unsigned long udelay;
    sample_t* sample_buffer;
    xample_t* xp;
    double rate;
    int i, f;

    max_samples = (size_t)(sample_freq*5);  // 5 seconds of samples
    udelay = ((unsigned long)(1000000*(1/sample_freq)));

    if (argc < 2) {
	printf("usage: xample <name>\n");
	exit(1);
    }
    
    // frame div pow = 2 => (1 << 2) == 4  (four frames per page)
    if ((xp = xample_create(argv[1], max_samples, 1, 2, sample_freq, 0666,
			    &sample_buffer)) == NULL) {
	fprintf(stderr, "unable to create shared memory\n");
	exit(1);
    }

    current_page = xp->current_page;
    last_page    = xp->last_page;
    first_page   = xp->first_page;
    samples_per_page = xp->samples_per_page;

    current_frame = xp->current_frame;
    last_frame    = xp->last_frame;
    first_frame   = xp->first_frame;
    samples_per_frame = xp->samples_per_frame;
    frames_per_page = xp->frames_per_page;

    rate         = (xp->rate >> 8) + (xp->rate & 0xff)/256.0;

    first_frame_offset = first_frame*samples_per_frame;
    // first_page_offset = first_page*samples_per_page;
    frame_offset = first_frame_offset;

    printf("page_size = %ld\n", xp->page_size);
    printf("frame_size = %ld\n", xp->frame_size);
    printf("rate = %f\n",       rate);
    printf("max_samples = %ld\n", max_samples);
    printf("samples_per_pages = %ld\n", samples_per_page);
    printf("udelay = %ld\n", udelay);
    printf("first_frame = %lu\n", first_frame);
    printf("last_frame = %lu\n",  last_frame);
    printf("current_frame = %lu\n", current_frame);

    // loop - sample data and save in shared memory
    i = 0;
    f = 0;

    while(1) {
	sample_t s = read_sample();
	sample_buffer[frame_offset + i] = s;
	i++;
	if (i >= samples_per_frame) {
	    f++;
	    if (f >= frames_per_page) {
		if (current_page >= last_page)
		    current_page = first_page;
		else
		    current_page++;
		f = 0;
		xp->current_page  = current_page;
	    }
	    i = 0;
	    if (current_frame >= last_frame) {
		current_frame = first_frame;
		frame_offset  = first_frame_offset;
	    }
	    else {
		current_frame++;
		frame_offset += samples_per_frame;
	    }
	    xp->current_frame = current_frame;
	}
	usleep(udelay);
    }
}
