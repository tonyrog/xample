//
// Xample logger 
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "xample.h"

static inline int eval_trigger(sample_t v, sample_t v0, trigger_t* t)
{
    unsigned char m = t->mask;
    unsigned char r = 0;

    if (m & LIMIT_BITS) {
	if ((m & UPPER_LIMIT_EXCEEDED)  && (v > t->upper_limit))
	    r |= UPPER_LIMIT_EXCEEDED;
	if ((m & BELOW_LOWER_LIMIT) && (v <= t->lower_limit))
	    r |= BELOW_LOWER_LIMIT;
    }
    if (m & DELTA_BITS) {
	if (v > v0) {
	    unsigned long d = v - v0;
	    if ((m & CHANGED_BY_MORE_THAN_DELTA) && (d > t->delta))
		r |= CHANGED_BY_MORE_THAN_DELTA;
	    if ((m & CHANGED_BY_MORE_THAN_POSITIVE_DELTA) 
		&& (d > t->positive_delta))
		r |= CHANGED_BY_MORE_THAN_POSITIVE_DELTA;
	}
	else if (v < v0) {
	    unsigned long d = v0 - v;
	    if ((m & CHANGED_BY_MORE_THAN_DELTA) && (d > t->delta))
		r |= CHANGED_BY_MORE_THAN_DELTA;
	    if ((m & CHANGED_BY_MORE_THAN_NEGATIVE_DELTA)
		&& (d > t->negative_delta))
		r |= CHANGED_BY_MORE_THAN_NEGATIVE_DELTA;
	}
    }
    return r;
}


int main(int argc, char** argv)
{
    size_t samples_per_page;
    unsigned long current_page;
    unsigned long first_page;
    unsigned long last_page;
    unsigned long channels;
    sample_t v0 = 0;
    unsigned char m0 = 0;
    double rate;
    sample_t* sample_buffer;
    xample_t* xp;
    trigger_t cond1;
    // trigger_t cond2;
    
    if (argc < 2) {
	printf("usage: xample_rd <name>\n");
	exit(1);
    }

    if ((xp = xample_open(argv[1], &sample_buffer)) == NULL) {
	fprintf(stderr, "unable to open %s\n", argv[1]);
	exit(1);
    }

    // setup trigger condition
    cond1.mask = UPPER_LIMIT_EXCEEDED | BELOW_LOWER_LIMIT;
    cond1.lower_limit = 11;
    cond1.upper_limit = 2000;
    cond1.positive_delta = 10;
    cond1.negative_delta = 10;
    cond1.delta = 10;

    current_page = xp->current_page;
    first_page   = xp->first_page;
    last_page    = xp->last_page;
    samples_per_page = xp->samples_per_page;
    rate         = (xp->rate >> 8) + (xp->rate & 0xff)/256.0;
    channels     = xp->channels;

    printf("page_size = %ld\n", xp->page_size);
    printf("sample_freq = %f\n", rate);
    printf("samples_per_pages = %ld\n", samples_per_page);
    printf("channels = %lu\n",     channels);
    printf("first_page = %lu\n",   first_page);
    printf("last_page = %lu\n",    last_page);
    printf("current_page = %lu\n", current_page);

    while(1) {
	int page_offset, i;
	unsigned long page;

	// 10 times / sec
	while(current_page == xp->current_page) {
	    usleep(100000);
	}
	page = current_page;
	page_offset = page * samples_per_page;
	current_page = xp->current_page;

	i = 0;
	while(i < samples_per_page) {
	    sample_t v = sample_buffer[page_offset+i];
	    unsigned char m = eval_trigger(v, v0, &cond1);
	    if (m && ((m & DELTA_BITS) || ((m & ~m0) & LIMIT_BITS))) {
		printf("trigger %x[%x] %lu:%d (v=%u, v'=%u)\n", 
		       m, m0, page, i, v, v0);
		v0 = v;
	    }
	    m0 = m;
	    i++;
	}
    }
}
