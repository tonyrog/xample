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

#define DEF_MAX_SAMPLES  (1024*1024) // 1M samples
#define DEF_MAX_TIME     60.0        // one minute of samples per file

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

// parse a trigger expression and store in t
// simple trigger expession,  all parts are optional
//    "u:<unsigned>:l:<unsigned>:d:<unsigned>:p:<unsigned>:n:<unsigned>"
// u trigger on above upper limit 
// l trigger on below lower limit 
// d trigger on delta
// p trigger on positive delta (raising)
// n trigger on negative delta (falling)
//
int parse_unsigned(char** pptr, unsigned long* val)
{
    unsigned long v = 0;
    int n = 0;
    char* ptr = *pptr;

    while((*ptr >= '0') && (*ptr <= '9')) {
	v = v*10 + (*ptr - '0');
	n++;
	ptr++;
    }
    *val  = v;
    *pptr = ptr;
    return n;
}

int parse_trigger(char* expr, trigger_t* t)
{
    int n;
    unsigned long* argp;
    unsigned char m;

    if (!expr || !t) return -1;

    memset(t, 0, sizeof(trigger_t));

again:
    switch(*expr) {
    case 'u':
	argp = &t->upper_limit; 
	m = UPPER_LIMIT_EXCEEDED; 
	break;
    case 'l':
	argp = &t->lower_limit; 
	m = BELOW_LOWER_LIMIT;
	break;
    case 'd':
	argp = &t->delta;
	m = CHANGED_BY_MORE_THAN_DELTA;
	break;
    case 'n':
	argp = &t->negative_delta;
	m = CHANGED_BY_MORE_THAN_NEGATIVE_DELTA;
	break;
    case 'p':
	argp = &t->positive_delta;
	m = CHANGED_BY_MORE_THAN_POSITIVE_DELTA;
	break;
    case '\0':
	return 0;
    default: 
	return -1;
    }
    if (expr[1] != ':') return -1;
    expr += 2;
    if ((n = parse_unsigned(&expr, argp)) == 0)
	return -1;
    if ((*expr != ':') && (*expr != '\0'))
	return -1;
    if (*expr == ':') expr++;
    t->mask |= m;
    goto again;
}

char* format_trigger(trigger_t* t)
{
    static char buffer[1024];
    char vbuffer[32];
    
    buffer[0] = '\0';
    if (t->mask & UPPER_LIMIT_EXCEEDED) {
	sprintf(vbuffer, "u:%lu:", t->upper_limit);
	strcat(buffer, vbuffer);
    }
    if (t->mask & BELOW_LOWER_LIMIT) {
	sprintf(vbuffer, "l:%lu:", t->lower_limit);
	strcat(buffer, vbuffer);
    }
    if (t->mask & CHANGED_BY_MORE_THAN_DELTA) {
	sprintf(vbuffer, "d:%lu:", t->delta);
	strcat(buffer, vbuffer);
    }
    if (t->mask & CHANGED_BY_MORE_THAN_NEGATIVE_DELTA) {
	sprintf(vbuffer, "n:%lu:", t->negative_delta);
	strcat(buffer, vbuffer);
    }
    if (t->mask & CHANGED_BY_MORE_THAN_POSITIVE_DELTA) {
	sprintf(vbuffer, "p:%lu:", t->positive_delta);
	strcat(buffer, vbuffer);
    }
    return buffer;
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

void usage(char* prog)
{
    printf("usage: %s [options] <shm-name>\n", prog);
    printf("  [-t <secs>]       max time in seconds per log file\n"
	   "  [-n <num>]        max number of samples per log file\n"
	   "  [-d <dir>]        log directory (default is current dir)\n"

	   "  [-s <trigger>]    start trigger\n"
	   "  [-e <trigger>]    end trigger\n"
	   "\n"
	   " trigger expression:\n"
	   "    [u:<num>] [l:<num>] [d:<num>] [p:<num] [n:<num>]\n"
           " u trigger on above upper limit\n"
	   " l trigger on below lower limit\n"
	   " d trigger on delta\n"
	   " p trigger on positive delta (raising)\n"
	   " n trigger on negative delta (falling)\n"
	   " example: "
	   " 'u:50000:l:100:d:10' = trigger when above 50000 or below 100 or\n"
	   "   value change (delta) is more than 10\n"
	);
    exit(1);    
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
    char*   dirname = ".";
    char   filename[FILENAME_MAX];
    int    fno = 0;        // 0..9
    int    opt;
    wav_file_t* wf;

    max_samples = DEF_MAX_SAMPLES;  // max 1M per file!
    max_time    = DEF_MAX_TIME;     // max 1 minutes
    
    // default: always trigger?  > 0 < 1
    memset(&cond1, 0, sizeof(cond1));
    memset(&cond2, 0, sizeof(cond2));
    cond1.mask = UPPER_LIMIT_EXCEEDED | BELOW_LOWER_LIMIT;
    cond1.upper_limit = 0;
    cond1.lower_limit = 1;

    while ((opt = getopt(argc, argv, "t:d:n:s:e:")) != -1) {
	switch(opt) {
	case 'd':  // set log directory
	    dirname = optarg;
	    break;
	case 't': // max time to log per trigger/file
	    max_time = atof(optarg);  
	    break;
	case 'n': // max number of sample to log per trigger/file
	    max_samples = atoi(optarg);
	    break;
	case 's':  // start trigger
	    if (parse_trigger(optarg, &cond1) < 0) {
		fprintf(stderr, "trigger expression error in %s\n", optarg);
		exit(1);
	    }
	    break;
	case 'e':  // end trigger
	    if (parse_trigger(optarg, &cond2) < 0) {
		fprintf(stderr, "trigger expression error in %s\n", optarg);
		exit(1);
	    }
	    break;
	default:
	    usage(argv[0]);
	}
    }

    if (optind >= argc)
	usage(argv[0]);

    if ((xp = xample_open(argv[optind], &sample_buffer)) == NULL) {
	fprintf(stderr, "unable to open shared memory %s\n", argv[optind]);
	exit(1);
    }

    
    current_page = xp->current_page;
    first_page   = xp->first_page;
    last_page    = xp->last_page;
    page_size    = xp->page_size;
    samples_per_page = xp->samples_per_page;
    rate         = (xp->rate >> 8) + (xp->rate & 0xff)/256.0;
    channels     = xp->channels;

    max_samples_t = rate * max_time;
    max_samples_t = page_align(max_samples_t, page_size);

    printf("max_time = %f\n", max_time);
    printf("max_samples = %zu\n", max_samples);
    printf("max_samples_t = %zu\n", max_samples_t);
    printf("start_cond = %s\n", format_trigger(&cond1));
    printf("end_cond = %s\n", format_trigger(&cond2));

    printf("page_size = %ld\n", xp->page_size);
    printf("sample_freq = %f\n", rate);
    printf("samples_per_pages = %zu\n", samples_per_page);
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

	    sprintf(filename, "%s/xam_%d.wav", dirname, fno);
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
		    printf("stop #sample = %zu\n", written);
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
