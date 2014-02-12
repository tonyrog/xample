//
// Xample logger 
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "xample.h"

typedef struct _wav_file_t {
    FILE* f;
    char* name;
    size_t num_samples;       // number of samples written
    size_t num_channels;      // channels per sample (interleaved)
    size_t bytes_per_sample;  // bytes per sample 
    long   riff_offs;    // offset (=4) to set RIFF size
    long   data_offs;    // offset (=40) to set data chunk size
} wav_file_t;

size_t file_write_uint16(uint16_t value, wav_file_t* wf)
{
    value = htole16(value);
    return fwrite(&value, 1, sizeof(value), wf->f);
}

size_t file_write_uint32(uint32_t value, wav_file_t* wf)
{
    value = htole32(value);
    return fwrite(&value, 1, sizeof(value), wf->f);
}

size_t file_write_samples(sample_t* vec, size_t n, wav_file_t* wf)
{
    size_t r = 0;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    r = fwrite(vec, n, sizeof(sample_t), wf->f);
#else
    int i;
    for (i = 0; i < n; i++) {
	uint16_t value = htole16((uint16_t)vec[i]);
	r += fwrite(&value, 1, sizeof(value), wf->f);
    }
#endif
    wf->num_samples += n;
    return r;
}

int file_wav_init(wav_file_t* wf, xample_t* xp)
{
    uint32_t sample_rate;
    uint32_t num_channels;
    uint16_t bytes_per_sample;
    uint16_t byte_rate;
    uint32_t num_samples = 0;  // not known at this point (need patch)
    size_t   size;

    num_channels = xp->channels;
    bytes_per_sample = 2;
    sample_rate = (xp->rate >> 8);
    byte_rate = sample_rate*num_channels*bytes_per_sample;
    
    // size = 0 here, this will be patch in close! at two location
    size = bytes_per_sample*num_samples*num_channels;
    
    wf->num_channels = num_channels;
    wf->bytes_per_sample = 2;

    // write RIFF header
    fwrite("RIFF", 1, 4, wf->f);
    wf->riff_offs = ftell(wf->f);
    file_write_uint32(36 + size, wf);
    fwrite("WAVE", 1, 4, wf->f);
    // write fmt  subchunk 
    fwrite("fmt ", 1, 4, wf->f);
    file_write_uint32(16, wf);   // SubChunk1Size is 16
    file_write_uint16(1, wf);    // PCM is format 1
    file_write_uint16(num_channels, wf);
    file_write_uint32(sample_rate, wf);
    file_write_uint32(byte_rate, wf);
    // block align
    file_write_uint16(num_channels*bytes_per_sample, wf);
    file_write_uint16(8*bytes_per_sample, wf);  /* bits/sample */
    // write data subchunk
    fwrite("data", 1, 4, wf->f);
    wf->data_offs = ftell(wf->f);
    file_write_uint32(size, wf); 
    return 0;
}

wav_file_t* file_wav_open(char* name, xample_t* xp)
{
    wav_file_t* wf;
    FILE* f;

    if ((f=fopen(name, "w")) == NULL)
	return NULL;
    if ((wf = (wav_file_t*) calloc(1, sizeof(wav_file_t))) == NULL) {
	fclose(f);
	return NULL;
    }
    wf->f = f;
    wf->name = strdup(name);
    if (file_wav_init(wf, xp) < 0) {
	int err = errno;
	fclose(f);
	if (wf->name) free(wf->name);
	free(wf);
	errno = err;
	return NULL;
    }
    return wf;
}

void file_wav_close(wav_file_t* wf)
{
    uint32_t size;

    size = wf->bytes_per_sample * wf->num_channels * wf->num_samples;

    fseek(wf->f, wf->riff_offs, SEEK_SET);
    file_write_uint32(36 + size, wf);

    fseek(wf->f, wf->data_offs, SEEK_SET);
    file_write_uint32(size, wf);

    fclose(wf->f);
    if (wf->name) free(wf->name);
    free(wf);
}

size_t page_align(size_t v, int page_size)
{
    return ((v + page_size - 1) / page_size)*page_size;
}

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
    int page_size;
    unsigned long current_page;
    unsigned long first_page;
    unsigned long last_page;
    unsigned long channels;
    sample_t v0 = 0;
    unsigned char m0 = 0;
    double rate;
    sample_t* sample_buffer;
    xample_t* xp;
    int start = 0;
    int stop = 0;
    trigger_t cond1;
    trigger_t cond2;
    size_t max_samples;
    size_t max_samples_t;
    double max_time;
    char filename[10];   // "xam_i.wav"
    int  fno = 0;        // 0..9
    wav_file_t* wf;

    if (argc < 2) {
	printf("usage: xample_rd <name>\n");
	exit(1);
    }

    if ((xp = xample_open(argv[1], &sample_buffer)) == NULL) {
	fprintf(stderr, "unable to open %s\n", argv[1]);
	exit(1);
    }

    // setup start trigger condition
    cond1.mask = UPPER_LIMIT_EXCEEDED;
    cond1.lower_limit = 0;
    cond1.upper_limit = 100;
    cond1.positive_delta = 1;
    cond1.negative_delta = 1;
    cond1.delta = 1;

    // setup stop trigger condition
    cond2.mask = BELOW_LOWER_LIMIT;
    cond2.lower_limit = 10;
    cond2.upper_limit = 65535;
    cond2.positive_delta = 1;
    cond2.negative_delta = 1;
    cond2.delta = 1;
    
    max_samples = 1024*512;  // max 1M per file!
    max_time    = 60.0*1;    // max 1 minutes

    current_page = xp->current_page;
    first_page   = xp->first_page;
    last_page    = xp->last_page;
    page_size    = xp->page_size;
    samples_per_page = xp->samples_per_page;
    rate         = (xp->rate >> 8) + (xp->rate & 0xff)/256.0;
    channels     = xp->channels;

    max_samples_t = rate * max_time;  // sample covering 5 minutes
    max_samples_t = page_align(max_samples_t, page_size);

    printf("max_time = %f\n", max_time);
    printf("max_samples = %ld\n", max_samples);
    printf("max_samples_t = %ld\n", max_samples_t);

    printf("page_size = %ld\n", xp->page_size);
    printf("sample_freq = %f\n", rate);
    printf("samples_per_pages = %ld\n", samples_per_page);
    printf("channels = %lu\n",     channels);
    printf("first_page = %lu\n",   first_page);
    printf("last_page = %lu\n",    last_page);
    printf("current_page = %lu\n", current_page);

    wf = NULL;
    start = 0;
    m0    = 0;

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
	while(!start && (i < samples_per_page)) {
	    sample_t v = sample_buffer[page_offset+i];
	    unsigned char m = eval_trigger(v, v0, &cond1);
	    if (m && ((m & DELTA_BITS) || ((m & ~m0) & LIMIT_BITS))) {
		printf("start %x[%x] %lu:%d (v=%u, v'=%u)\n", 
		       m, m0, page, i, v, v0);
		v0 = v;
		m0 = m;
		start = 1;
	    }
	    i++;
	}

	if (start) {
	    m0 = 0;
	    size_t written = 0;

	    sprintf(filename, "xam_%d.wav", fno);
	    printf("open %s\n", filename);
	    if ((wf = file_wav_open(filename, xp)) == NULL) {
		fprintf(stderr, "unable to open file %s [%s]\n", filename,
			strerror(errno));
	    }

	    stop = 0;

	    do {
		while(!stop && (i < samples_per_page)) {
		    sample_t v = sample_buffer[page_offset+i];
		    unsigned char m = eval_trigger(v, v0, &cond2);
		    if (m && ((m & DELTA_BITS) || ((m & ~m0) & LIMIT_BITS))) {
			printf("stop %x[%x] %lu:%d (v=%u, v'=%u)\n", 
			       m, m0, page, i, v, v0);
			v0 = v;
			m0 = m;
			stop = 1;
		    }

		    i++;
		}
		written += samples_per_page;
		if (wf) {
		    file_write_samples(sample_buffer+page_offset,
				       samples_per_page, wf);
		}
		// stop if we have had enough
		if ((written >= max_samples) || (written >= max_samples_t)) {
		    printf("stop #sample = %ld\n", written);
		    stop = 1;
		}
		if (!stop) {
		    // 10 times / sec
		    while(current_page == xp->current_page) {
			usleep(100000);
		    }
		    page = current_page;
		    page_offset = page * samples_per_page;
		    current_page = xp->current_page;
		    i = 0;
		}
	    } while(!stop);

	    if (wf) {
		file_wav_close(wf);
		fno++;
		if (fno >= 10) fno = 0;
	    }
	    start = 0;
	}
    }
}
