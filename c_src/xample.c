/*
 *  Task that samples adc values in shared memory segment
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <sys/time.h>
#include <sys/mman.h>

#include "xample.h"

#if defined(__linux__)
#define MCP3202
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SPI_SPEED 2500000 // SPI frequency clock

static char* spi_dev = "/dev/spidev0.0";
static int spi_fd = -1;

int open_spi(void)
{
    uint8_t  mode = SPI_MODE_0;
    uint32_t speed = SPI_SPEED;

    if ((spi_fd= open(spi_dev, O_RDWR)) < 0) {
	perror("open spi");
	return -1;
    }
    if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
	fprintf(stderr, "SPI_IOC_WR_MAX_SPEED_HZ failed %s\n",
		strerror(errno));
	goto error;
    }
    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0) {
	fprintf(stderr, "failed to set SPI_IOC_WR_MODE: %s\n",
		strerror(errno));
	goto error;
    }
    return 0;
error:
    close(spi_fd);
    spi_fd = -1;
    return -1;
}

void close_spi(void)
{
    if (spi_fd >= 0)
	close(spi_fd);
    spi_fd = -1;
}

sample_t read_sample_spi(int channel) 
{
    sample_t v;
    char  rx[3];
    uint8_t tx[] = {1,(2+channel)<<6,0,};

    struct spi_ioc_transfer tr = {
	.tx_buf = (unsigned long)tx,
	.rx_buf = (unsigned long)rx,
	.len = ARRAY_SIZE(tx),
	.delay_usecs = 0,
	.speed_hz = SPI_SPEED,
	.bits_per_word = 8,
    };
    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0)
      return 0;
    v = ((rx[1]&0x0F) << 8) + rx[2]; // 4+8=12 bits = 4096
    // based on 3.3V supply
    return v << 4;  // scale to 16 bit
}

//  read n (interleaved) samples
int read_n_samples_spi(int chan0, int chan1, uint16_t delay,
		       sample_t* samples, size_t n)
{
  uint8_t rx[3*256];
  uint8_t tx[3*256];
  struct spi_ioc_transfer tr[256];
  int i, j;

  if (n > 256) 
    n = 256;

  for (i = 0, j=0; i < n; i++, j+=3) {
    tx[j+0] = 1;
    tx[j+1] = (i & 1) ? ((2+chan1) << 6) : ((2+chan0)<<6);
    tx[j+2] = 0;

    tr[i].tx_buf = (unsigned long) &tx[j];
    tr[i].rx_buf = (unsigned long) &rx[j];
    tr[i].len = 3;
    tr[i].len = 3;
    tr[i].delay_usecs = delay;
    tr[i].speed_hz = SPI_SPEED;
    tr[i].bits_per_word = 8;
    tr[i].cs_change = 1;
  }
  if (ioctl(spi_fd, SPI_IOC_MESSAGE(n), &tr) < 0)
    return 0;
  for (i = 0, j=0; i < n; i++,j+=3)
    samples[i] = (((rx[j+1] & 0xf)<<8) + rx[j+2])<<4;
  return n;
}

#endif

#define MAX_AMPLITUDE  20000.0
#define MIN_AMPLITUDE  1000.0
#define OFFSET      3000.0

sample_t read_sample_sim(int channel)
{
    static double x = 0.0;
    static double ax = 0.0;
    static double ad = 0.01;
    double xi;
    double vf;

    switch(channel) {
    case 0: xi = 0.0*M_PI/4.0; break;
    case 1: xi = 1.0*M_PI/4.0; break;
    case 2: xi = 2.0*M_PI/4.0; break;
    case 3: xi = 3.0*M_PI/4.0; break;
    default: xi = 0.0; break;
    }

    x += channel*M_PI;
    vf = (MIN_AMPLITUDE + ax*(MAX_AMPLITUDE-MIN_AMPLITUDE))*sin(xi+x*2*M_PI);
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

int read_n_samples_sim(int chan0, int chan1, uint16_t delay,
		       sample_t* samples, size_t n)
{
  int i;

  for (i = 0; i < n; i++) {
    samples[i] = read_sample_sim((i & 1) ? chan1 : chan0);
    usleep(delay);  // fixme: pace this
  }
  return n;
}

void usage(char* prog)
{
    printf("usage: %s [options] <shm-name>\n", prog);
    printf("  [-t <secs>]       max buffer time in seconds\n"
	   "  [-f <freq-hz>]    sample frequency in hertz\n"
	   "  [-d <frame-div>]  page divider into frames\n");
    exit(1);
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
    size_t frames_per_page;
    unsigned long udelay;
    unsigned long nsamples = 0;
    long sample_time = 5;
    sample_t* sample_buffer;
    xample_t* xp;
    double rate;
    double sample_freq = 1000.0;  // default = 1K HZ
    int i, f, opt;
    size_t fdivpow2 = 2;    // 0 => 2^0 = 1 => frame_size = page_size 
    // sample_t (*read_sample_fn)(int channel) = NULL;
    int (*read_n_samples_fn)(int, int, uint16_t, sample_t*, size_t) = NULL;
    struct timeval t0, t1;
    int chunk_size = 10;  // read 10 sample per round

    while ((opt = getopt(argc, argv, "sf:t:d:i:c:")) != -1) {
	switch(opt) {
	case 'f':
	    sample_freq = atof(optarg);  // sample frequency
	    break;
	case 't':
	    sample_time = atoi(optarg);  // buffer time in seconds
	    break;
	case 'd':
	    fdivpow2 = atoi(optarg);
	    break;
	case 'c':
	  chunk_size = atoi(optarg);
	  if (chunk_size <= 0) 
	    chunk_size = 1;
	  else if (chunk_size > 256)
	    chunk_size = 256;
	  break;
	case 'i':
#if defined(__linux__) && defined(MCP3202)
	  if (atoi(optarg) == 1)
	    spi_dev = "/dev/spidev0.1";
	  else
	    spi_dev = "/dev/spidev0.0";
	  break;
#endif
	case 's':
	  read_n_samples_fn = read_n_samples_sim;
	  break;
	default: /* '?' */
	    usage(argv[0]);
	}
    }

    if (optind >= argc)
	usage(argv[0]);

#if defined(__linux__) && defined(MCP3202)
    if (read_n_samples_fn == NULL) {
      open_spi();
      if (spi_fd >= 0) {
	read_n_samples_fn = read_n_samples_spi;
	printf("spi device %s is open\n", spi_dev);
      }
    }
#endif
    max_samples = (size_t)(sample_freq*sample_time);
    udelay = ((unsigned long)(1000000*(1/sample_freq)));
    
    // frame div pow = 2 => (1 << 2) == 4  (four frames per page)
    if ((xp = xample_create(argv[optind], max_samples, fdivpow2, 
			    1, sample_freq, 0666,
			    &sample_buffer)) == NULL) {
	fprintf(stderr, "unable to create shared memory\n");
	exit(1);
    }

    rate = (xp->rate >> 8) + (xp->rate & 0xff)/256.0;

    // general
    printf("rate = %f\n", rate);
    printf("udelay = %lu\n", udelay);
    printf("max_samples = %zu\n", max_samples);
    printf("chunk_size = %zu\n",  chunk_size);
   
    // page info
    current_page = xp->current_page;
    last_page    = xp->last_page;
    first_page   = xp->first_page;
    samples_per_page = xp->samples_per_page;

    printf("page_size = %ld\n", xp->page_size);
    printf("first_page = %lu\n", first_page);
    printf("last_page = %lu\n",  last_page);
    printf("current_page = %lu\n", current_page);
    printf("samples_per_page = %zu\n", samples_per_page);
    
    // frame info
    current_frame = xp->current_frame;
    last_frame    = xp->last_frame;
    first_frame   = xp->first_frame;
    samples_per_frame = xp->samples_per_frame;
    frames_per_page = xp->frames_per_page;

    printf("frame_size = %ld\n", xp->frame_size);
    printf("first_frame = %lu\n", first_frame);
    printf("last_frame = %lu\n",  last_frame);
    printf("current_frame = %lu\n", current_frame);
    printf("samples_per_frame = %zu\n", samples_per_frame);
    printf("frames_per_page = %zu\n", frames_per_page);

    first_frame_offset = first_frame*samples_per_frame;
    // first_page_offset = first_page*samples_per_page;
    frame_offset = first_frame_offset;

    // loop - sample data and save in shared memory
    i = 0;
    f = 0;

    gettimeofday(&t0, NULL);

    while(1) {
      int ns = chunk_size;
      int remain = samples_per_frame - i;
      sample_t last_sample;
      uint16_t delay;

      if  (ns > remain)
	ns = remain;
      delay = (udelay > 65535) ? 65535 : udelay;
      read_n_samples_fn(0, 0, delay, sample_buffer+frame_offset+i, ns);
      i += ns;
      last_sample = sample_buffer[frame_offset+i-1];

      if (i >= samples_per_frame) {
	nsamples += samples_per_frame;
	f++;
	if (f >= frames_per_page) {
	  long td;
	  if (current_page >= last_page)
	    current_page = first_page;
	  else
	    current_page++;
	  f = 0;
	  xp->current_page  = current_page;
	  
	  gettimeofday(&t1, NULL);
	  
	  td = (t1.tv_sec-t0.tv_sec)*1000000+(t1.tv_usec-t0.tv_usec);
	  printf("Hz = %f\n", ((double)nsamples/(double) td)*1000000.0);
	  printf("last_sample = %u\n", last_sample);
	  nsamples = 0;
	  t0 = t1;
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
      // usleep(udelay);
    }
}
