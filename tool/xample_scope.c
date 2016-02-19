#include <unistd.h>
#include "epx.h"
#include "../c_src/xample.h"

typedef struct {
    epx_backend_t* be;
    epx_pixmap_t*  px;
    epx_window_t*  wn;
    epx_gc_t*      gc;
    epx_pixmap_t* grid;
    int need_redraw;
} state_t;

#define GRID_N    8
#define GRID_M    10
#define GRID_S    8
#define GRID_PX   (5*GRID_S)

#define GRID_WIDTH  (GRID_M*GRID_PX+1)
#define GRID_HEIGHT (GRID_N*GRID_PX+1)

//#define WINDOW_WIDTH   640 // (GRID_M*GRID_PX+1+GRID_LEFT+GRID_RIGHT)
//#define WINDOW_HEIGHT  480 // (GRID_N*GRID_PX+1+GRID_TOP+GRID_BOTTOM)
#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600

#define BACKGROUND_WIDTH  WINDOW_WIDTH
#define BACKGROUND_HEIGHT WINDOW_HEIGHT

#define GRID_TOP    ((WINDOW_HEIGHT-GRID_HEIGHT)/2)
#define GRID_LEFT   ((WINDOW_WIDTH-GRID_WIDTH)/2)
#define GRID_BOTTOM 10
#define GRID_RIGHT  10


void update_window(state_t* sp)
{
    epx_window_draw_begin(sp->wn);
    epx_backend_pixmap_draw(sp->be, sp->px, sp->wn, 0, 0, 0, 0, 
			    BACKGROUND_WIDTH, BACKGROUND_HEIGHT);
    epx_window_draw_end(sp->wn, 0);
    epx_window_swap(sp->wn);
    sp->need_redraw = 0;
}

epx_pixmap_t* create_grid(epx_gc_t* gc, epx_format_t pixel_format)
{
    epx_pixmap_t* p;
    int x, y;

    p = epx_pixmap_create(GRID_WIDTH, GRID_HEIGHT, pixel_format);

    // create grid
    epx_pixmap_fill(p, epx_pixel_rgb(123, 247, 173));
    // draw grid 10x8 (28x28) + 4 per central line
    epx_gc_set_foreground_color(gc, epx_pixel_black);
    
    for (x = 0; x <= GRID_N; x++)
	epx_pixmap_draw_line(p, gc, 0, x*GRID_PX, GRID_WIDTH-1, x*GRID_PX);

    for (y = 0; y <= GRID_M; y++)
	epx_pixmap_draw_line(p, gc, y*GRID_PX, 0, y*GRID_PX, GRID_HEIGHT-1);
    
    y = (GRID_N/2)*GRID_PX;
    for (x = 0; x < GRID_WIDTH; x += GRID_S)
	epx_pixmap_draw_line(p, gc, x, y-2, x,  y+2);

    x = (GRID_M/2)*GRID_PX;
    for (y = 0; y < GRID_HEIGHT; y += GRID_S)
	epx_pixmap_draw_line(p, gc, x-2, y, x+2, y);
    return p;
}

int init(state_t* sp)
{
    epx_init(EPX_SIMD_AUTO);

    if ((sp->be = epx_backend_create(getenv("EPX_BACKEND"), NULL)) == NULL) {
	fprintf(stderr, "xample_scope: no epx backend found\n");
	return -1;
    }

    sp->wn = epx_window_create(50, 50, WINDOW_WIDTH, WINDOW_HEIGHT);
    epx_backend_window_attach(sp->be, sp->wn);

    sp->px = epx_pixmap_create(WINDOW_WIDTH, WINDOW_HEIGHT, EPX_FORMAT_ARGB);
    epx_backend_pixmap_attach(sp->be, sp->px);
    epx_pixmap_fill(sp->px, epx_pixel_floralWhite);
    sp->gc = epx_gc_copy(&epx_default_gc);
    sp->grid = create_grid(sp->gc, sp->px->pixel_format);
    
    epx_backend_event_attach(sp->be);

    epx_window_set_event_mask(sp->wn, EPX_EVENT_BUTTON_PRESS | 
			      EPX_EVENT_BUTTON_RELEASE |
			      EPX_EVENT_CLOSE);
    return 0;
}

void final(state_t* sp)
{
    epx_pixmap_detach(sp->px);
    epx_window_detach(sp->wn);
    epx_pixmap_destroy(sp->px);
    epx_pixmap_destroy(sp->grid);
    epx_window_destroy(sp->wn);
    epx_backend_destroy(sp->be);
}


void update_data(state_t* sp)
{
    epx_pixmap_copy_area(sp->grid, sp->px, 
			 0, 0, GRID_LEFT, GRID_TOP,
			 GRID_WIDTH, GRID_HEIGHT, 0);
}

void draw_samples(state_t* sp, xample_t* xp, unsigned long frame,
		  sample_t* sample_buffer)
{
    unsigned long samples_per_frame = xp->samples_per_frame;
    int frame_offset = frame * samples_per_frame;
    int x = GRID_LEFT;
    int i = 0;

    // start with redraw a clean grid
    epx_pixmap_copy_area(sp->grid, sp->px, 
			 0, 0, GRID_LEFT, GRID_TOP,
			 GRID_WIDTH, GRID_HEIGHT, 0);
    
    // just draw one frame for now
    while((i < samples_per_frame) && (i < GRID_WIDTH)) {
	int k;
	for (k = 0; k < xp->channels; k++) {
	    int ys = (65535-sample_buffer[frame_offset+i]);
	    int y;
	    y = GRID_TOP + ((ys*GRID_HEIGHT) >> 16);
	    epx_pixmap_put_pixel(sp->px, x, y, 0, epx_pixel_black);
	    i++;
	}
	x++;
    }

    update_window(sp);
    
}


int main(int argc, char** argv)
{
    state_t s;
    xample_t* xp;
    sample_t* sample_buffer;
    unsigned long frame;
    
    memset(&s, 0, sizeof(s));

    if (argc < 2) {
	fprintf(stderr, "usage: xample_scope <shm-name>\n");
	exit(1);
    }

    if ((xp = xample_open(argv[1], &sample_buffer)) == NULL) {
	fprintf(stderr, "xample_scope: unable to open shm %s\n", argv[1]);
	exit(1);
    }

    init(&s);

    update_data(&s);

    update_window(&s);

    frame = xp->current_frame;

    while(1) {
	epx_event_t e;

	if (frame != xp->current_frame) {
	    unsigned next_frame = xp->current_frame;
	    // draw data in current_page
	    draw_samples(&s, xp, frame, sample_buffer);
	    frame = next_frame;
	}

	if (epx_backend_event_read(s.be, &e) > 0) {
	    if ((e.type == EPX_EVENT_BUTTON_PRESS) &&
		(e.pointer.button == 1)) {
		printf("press 1\n");
	    }
	    else if ((e.type == EPX_EVENT_BUTTON_RELEASE) &&
		     (e.pointer.button == 1)) {
		printf("release 1\n");
	    }
	    else if (e.type == EPX_EVENT_CLOSE) {
		xample_close(xp);
		final(&s);
		exit(0);
	    }
	}
	usleep(50000);
    }
}
