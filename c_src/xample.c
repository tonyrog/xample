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

#define MIN_CHUNK_SIZE 1
#define MAX_CHUNK_SIZE 256
#define DEF_CHUNK_SIZE 10

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
int read_n_samples_spi(int* selector, size_t nchan, uint16_t delay,
		       sample_t* samples, size_t n)
{
  uint8_t rx[3*256];
  uint8_t tx[3*256];
  struct spi_ioc_transfer tr[256];
  int i, j, k;

  if (n > 256)
      n = 256;

  for (i = 0, k=0, j=0; i < n; i++, j+=3) {
    tx[j+0] = 1;
    tx[j+1] = (2+selector[k++]) << 6;
    tx[j+2] = 0;
    if (k >= nchan) k = 0;
    tr[i].tx_buf = (unsigned long) &tx[j];
    tr[i].rx_buf = (unsigned long) &rx[j];
    tr[i].len = 3;
    tr[i].delay_usecs = delay;
    tr[i].speed_hz = SPI_SPEED;
    tr[i].bits_per_word = 8;
    tr[i].cs_change = 1;
  }
  if (ioctl(spi_fd, SPI_IOC_MESSAGE(n), &tr) < 0)
    return 0;
  for (i = 0, j=0; i < n; i++, j+=3) {
      sample_t v = (((rx[j+1] & 0xf)<<8) + rx[j+2])<<4;
      samples[i] = v;
  }
  return n;
}

#endif

#if defined(USE_HIDAPI)

#include "hidapi.h"
#define MAX_SERIAL 1024
static unsigned short hid_vendor  = 0;
static unsigned short hid_product = 0;
static char*          hid_serial = NULL;
static hid_device*    hid_dev = NULL;

int open_hid()
{
    char* ptr;
    int j = 0;
    wchar_t serial[MAX_SERIAL+1];
    wchar_t* serp = NULL;

    if ((ptr = hid_serial) != NULL) {
	while(*ptr && (j < MAX_SERIAL))
	    serial[j++] = *ptr++;
    }
    if (j > 0) {
	serial[j] = 0;
	serp = serial;
    }
    if ((hid_dev = hid_open(hid_vendor, hid_product, serp)) == NULL)
	return -1;
    return 0;
}

void close_hid(void)
{
    hid_close(hid_dev);
    hid_dev = NULL;
}

int read_sample_hid(int* selector, sample_t* samples, size_t n)
{
    uint8_t buffer[8];
    int r;
    if ((r = hid_read(hid_dev, buffer, 8)) == 8) {
	// uint16_t ts = buffer[0]+buffer[1]*256;
	int i, j;
	for (i = 0; i < n; i++) {
	    if ((j = selector[i]) < 4)
		samples[i] = (buffer[2+j] << 8);
	    else
		samples[i] = 0;
	}
	return 0;
    }
    return r;
}

int read_n_samples_hid(int* selector, size_t nchan, uint16_t delay,
		       sample_t* samples, size_t n)
{
    int k = 0;
    while(n > 0) {
	read_sample_hid(selector, samples, nchan);
	n -= nchan;
	samples += nchan;
	k += nchan;
	usleep(delay);  // fixme: pace this
    }
    return k;
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

int read_n_samples_sim(int* selector, size_t nchan,
		       uint16_t delay, sample_t* samples, size_t n)
{
  int i, k = 0;

  for (i = 0; i < n; i++) {
      samples[i] = read_sample_sim(selector[k++]);
      if (k >= nchan) {
	  k = 0;
	  usleep(delay);  // fixme: pace this
      }
  }
  return n;
}

void usage(char* prog)
{
    printf("usage: %s [options] <shm-name>\n", prog);
    printf("  [-t <secs>]         max buffer time in seconds\n"
	   "  [-f <freq-hz>]      sample frequency in hertz\n"
	   "  [-d <frame-div>]    page divider into frames\n"
	   "  [-k <chunk-size>]   chunk size 1..256 (10)\n"
	   "  [-c <channels>]     number of channels 1..8\n"
	   "  [-i <n>]            select spi interface 0 or 1\n"
	   "  [-s]                run simulated mode\n"
	   "  [-v <usb-vendor>]   hid mode usb vendor\n"
	   "  [-p <usb-product>]  hid mode usb product\n"
	   "  [-S <usb-serial>]   hid mode usb serial\n"
	   "  [-H <product-name>] hid mode product select\n"
	);
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
    int (*read_n_samples_fn)(int*, size_t, uint16_t, sample_t*, size_t) = NULL;
    struct timeval t0, t1;
    size_t chunk_size = DEF_CHUNK_SIZE;
    size_t nchannels = 1;
    int    selector[8];

    while ((opt = getopt(argc, argv, "sf:t:d:i:c:v:p:S:H:")) != -1) {
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
	    nchannels = atoi(optarg);
	    break;
	case 'k':
	  chunk_size = atoi(optarg);
	  if (chunk_size < MIN_CHUNK_SIZE) 
	    chunk_size = MIN_CHUNK_SIZE;
	  else if (chunk_size > MAX_CHUNK_SIZE)
	    chunk_size = MAX_CHUNK_SIZE;
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
#if defined(USE_HIDAPI)
	case 'v':
	    hid_vendor = atoi(optarg);
	    break;
	case 'p':
	    hid_product = atoi(optarg);
	    break;
	case 'S':
	    hid_serial = optarg;
	    break;
	case 'H':
	    if (strcmp(optarg, "usb-recorder") == 0) {
		hid_vendor = 0x10cf;
		hid_product = 0x8047;
		hid_serial  = "";
	    }
	    else {
		fprintf(stderr, "HID device named '%s' not found\n", 
			optarg);
		fprintf(stderr, "available: usb-recorder\n");
		exit(1);
	    }
	    break;
#endif
	default: /* '?' */
	    usage(argv[0]);
	}
    }

    if (optind >= argc)
	usage(argv[0]);

    // setup channel selector just a simple one-to-one map for now
    for (i = 0; i < nchannels; i++)
	selector[i] = i;

#if defined(USE_HIDAPI)
    if ((hid_vendor != 0) && (hid_product != 0)) {
	if (open_hid() < 0) {
	    perror("open hid");
	    exit(1);
	}
	read_n_samples_fn = read_n_samples_hid;
	printf("hid device %d:%d:%s is open\n", 
	       hid_vendor, hid_product, hid_serial?hid_serial:"");
    }
#endif

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
			    nchannels, sample_freq, 0666,
			    &sample_buffer)) == NULL) {
	fprintf(stderr, "unable to create shared memory %s\n", argv[optind]);
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
    printf("nchannels = %zu\n",  xp->channels);

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
      sample_t last_sample[8];
      uint16_t delay;

      if  (ns > remain)
	ns = remain;
      delay = (udelay > 65535) ? 65535 : udelay;
      read_n_samples_fn(selector, nchannels,
			delay, sample_buffer+frame_offset+i, ns);
      i += ns;

      memcpy(last_sample, &sample_buffer[frame_offset+i-1], 
	     sizeof(sample_t)*nchannels);

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
	  printf("last_sample = %u\n", last_sample[0]);
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
    }
}
