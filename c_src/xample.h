
#ifndef __XAMPLE_H__

#ifdef __APPLE__
#include <machine/endian.h>

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htole16(x) (x)
#define htole32(x) (x)
#else
#define htole16(x) __DARWIN_OSSwapInt16((x))
#define htole32(x) __DARWIN_OSSwapInt32((x))
#endif

#else
#include <endian.h>
#endif


typedef uint16_t sample_t;

//
// Memory mapped structure
// +---------------+
// | current page  |
// +---------------+
// |  first page   |
// +---------------+
// |  last page    |
// +---------------+
// | sample rate   |
// +---------------+
// ...
// +===============+
// | page 1        |
// +===============+
// | page 2        |
// +===============+
// ...
//
// Eache page is divided into frames
// +========+========+=====+========+
// | frame1 | frame2 | ... | frameN |
// +========+========+=====+========+
//
// framesize <= page_size and is normally
// power of two (like a sub page)
//
typedef struct {
    unsigned long current_page;      // current page number
    unsigned long first_page;        // first page number
    unsigned long last_page;         // last  page number
    unsigned long page_size;         // used by close
    unsigned long samples_per_page;  // effective value

    unsigned long current_frame;     // current frame number
    unsigned long first_frame;       // first frame number
    unsigned long last_frame;        // last  frame number
    unsigned long frame_size;        // frame_size < page_size
    unsigned long samples_per_frame; // effective value
    unsigned long frames_per_page;   // >= 1

    unsigned long rate;             // sample rate 24.8 format
    unsigned long channels;         // number of channels (interleaved when > 1)
} xample_t;

#define UPPER_LIMIT_EXCEEDED                0x01
#define BELOW_LOWER_LIMIT                   0x02
#define CHANGED_BY_MORE_THAN_DELTA          0x04
#define CHANGED_BY_MORE_THAN_NEGATIVE_DELTA 0x08
#define CHANGED_BY_MORE_THAN_POSITIVE_DELTA 0x10
#define LIMIT_BITS 0x03
#define DELTA_BITS 0x1C

typedef struct {
    unsigned char mask;
    unsigned long upper_limit;
    unsigned long lower_limit;
    unsigned long delta;
    unsigned long negative_delta;
    unsigned long positive_delta;
} trigger_t;    

// create data stream 
extern xample_t* xample_create(char* name, size_t nsamples, size_t fdivpow2,
			       size_t nchannels,
			       double rate, mode_t mode, sample_t** data);
// open data stream for read
extern xample_t* xample_open(char* name, sample_t** data);

extern int xample_close(xample_t* xp);

#endif
