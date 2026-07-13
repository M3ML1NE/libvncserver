
/*
 * a simple MulticastVNC example server that is set up to compare
 * unicast and multicast VNC.
 *
 * some code taken from the camera example.
 *
 * Two test patterns are available, toggled live by the connected viewer
 * with the 't' key:
 *
 *   - full-frame (default): a static gradient plus a moving scanline, with
 *     the _whole_ framebuffer marked modified every frame. Good for comparing
 *     raw unicast vs. multicast bandwidth, but it re-sends everything every
 *     frame, so it both overdrives a thin link and hides any repair defect.
 *
 *   - static-box: the same static gradient plus a small bouncing box,
 *     with only the box's old and new positions marked modified. Because the
 *     background never changes, a partial update that is lost and _not_
 *     repaired stays visible as a persistent artifact - which makes the
 *     multicast NACK/repair path directly observable.
 */

#include <rfb/rfb.h>
#include <rfb/keysym.h>
#include "radon.h"

#define WIDTH  640
#define HEIGHT 480
#define BYTESPERPIXEL 4

/* 15 frames per second (if we can) */
#define FPS 15
#define PICTURE_TIMEOUT (1.0/FPS)

/* static-box pattern parameters */
#define BOX_SIZE     40


typedef enum { PATTERN_FULLFRAME, PATTERN_STATICBOX } patternMode;

/* toggled live by the viewer via the 't' hotkey; default preserves the
   original full-frame behaviour */
static patternMode g_pattern = PATTERN_FULLFRAME;
/* set on a mode switch to force one full repaint so every client re-syncs */
static rfbBool g_repaint = FALSE;
/* pristine copy of the static background, used to erase the moving box */
static unsigned char *g_bgBuffer = NULL;



/*
 * throttle camera updates
*/
int UpdateIntervalOver() 
{
    static struct timeval now={0,0}, then={0,0};
    double elapsed, dnow, dthen;

    gettimeofday(&now,NULL);

    dnow  = now.tv_sec  + (now.tv_usec /1000000.0);
    dthen = then.tv_sec + (then.tv_usec/1000000.0);
    elapsed = dnow - dthen;

    if (elapsed > PICTURE_TIMEOUT)
      memcpy((char *)&then, (char *)&now, sizeof(struct timeval));
    return elapsed > PICTURE_TIMEOUT;
}



/*
 * draw server-side frame rate and number of connected clients, both to
 * stderr and as an on-screen overlay in the framebuffer. Used by both
 * patterns so the log format stays consistent for analysis.
 */
void ShowStats(rfbScreenInfoPtr rfbScreen)
{
  static uint32_t fcount, fps, fps_avg, fps_sum, fps_nr_samples;
  static time_t last_sec = 0;
  struct timeval now;
  int clients=0, mc_clients=0;
  rfbClientIteratorPtr it;
  rfbClientPtr cl;
  char stats_string[256];
  int fx1, fy1, fx2, fy2, top, bot, j;
  unsigned char* buffer = (unsigned char*)rfbScreen->frameBuffer;

  gettimeofday(&now, NULL);
  ++fcount;
  if(now.tv_sec != last_sec) /* one-second tick */
    {
      fps = fcount;
      fcount = 0;
      ++fps_nr_samples;
      fps_sum += fps;
      fps_avg = fps_sum/fps_nr_samples;
      last_sec = now.tv_sec;
    }

  it = rfbGetClientIterator(rfbScreen);
  while((cl=rfbClientIteratorNext(it)) != NULL)
    {
      if(cl->useMulticastVNC)
	++mc_clients;
      else
	++clients;
    }
  rfbReleaseClientIterator(it);

  snprintf(stats_string, 256,
	   "%s - Srv FPS: %03d now, %03d avg - Clients: %d unicast, %d multicast",
	   g_pattern == PATTERN_FULLFRAME ? "full-frame" : "static-box",
	   fps, fps_avg, clients, mc_clients);

  fprintf(stderr, "%s\r", stats_string);

  /* on-screen overlay at (10,100); compute the affected text band */
  rfbWholeFontBBox(&radonFont, &fx1, &fy1, &fx2, &fy2);
  top = 100 + fy1 - 1;  if(top < 0) top = 0;
  bot = 100 + fy2 + 1;  if(bot > HEIGHT) bot = HEIGHT;

  /* in static-box mode, restore the background band first so the previous
     frame's text (e.g. old FPS digits) doesn't smear */
  if(g_pattern == PATTERN_STATICBOX)
    for(j=top; j<bot; ++j)
      memcpy(&buffer[(j*WIDTH)*BYTESPERPIXEL],
	     &g_bgBuffer[(j*WIDTH)*BYTESPERPIXEL],
	     WIDTH*BYTESPERPIXEL);

  rfbDrawString(rfbScreen, &radonFont, 10, 100, stats_string, 0xffffff);
  rfbMarkRectAsModified(rfbScreen, 0, top, WIDTH, bot);
}



/*
 * full-frame pattern: coloured gradient plus a moving black line, redrawn
 * over the whole framebuffer every frame, just like the camera example.
 */
void UpdateFullFrame(rfbScreenInfoPtr rfbScreen)
{
  int line=0;
  int i,j;
  struct timeval now;
  unsigned char* buffer = (unsigned char*)rfbScreen->frameBuffer;

  /*
   * simulate grabbing data from a device by updating the entire framebuffer
   */
  for(j=0;j<HEIGHT;++j) {
    for(i=0;i<WIDTH;++i) {
      buffer[(j*WIDTH+i)*BYTESPERPIXEL+0]=(i+j)*128/(WIDTH+HEIGHT); /* red */
      buffer[(j*WIDTH+i)*BYTESPERPIXEL+1]=i*128/WIDTH; /* green */
      buffer[(j*WIDTH+i)*BYTESPERPIXEL+2]=j*256/HEIGHT; /* blue */
    }
    buffer[j*WIDTH*BYTESPERPIXEL+0]=0xff;
    buffer[j*WIDTH*BYTESPERPIXEL+1]=0xff;
    buffer[j*WIDTH*BYTESPERPIXEL+2]=0xff;
  }

  /*
   * simulate the passage of time
   *
   * draw a simple black line that moves down the screen. The faster the
   * client, the more updates it will get, the smoother it will look!
   */
  gettimeofday(&now,NULL);
  line = now.tv_usec / (1000000/HEIGHT);
  if(line>=HEIGHT)
    line=HEIGHT-1;

  memset(&buffer[(WIDTH * BYTESPERPIXEL) * line], 0, (WIDTH * BYTESPERPIXEL));
}



/*
 * draw the static gradient background into the framebuffer and keep a pristine
 * copy in g_bgBuffer for erasing the moving box. The background never changes,
 * so a partial update that is lost and not repaired keeps showing the red box
 * (a bright ghost/trail against the gradient) until repaired.
 */
void DrawStaticBackground(rfbScreenInfoPtr rfbScreen)
{
  int i,j;
  unsigned char* buffer = (unsigned char*)rfbScreen->frameBuffer;

  for(j=0;j<HEIGHT;++j)
    for(i=0;i<WIDTH;++i) {
      buffer[(j*WIDTH+i)*BYTESPERPIXEL+0]=(i+j)*128/(WIDTH+HEIGHT); /* red */
      buffer[(j*WIDTH+i)*BYTESPERPIXEL+1]=i*128/WIDTH; /* green */
      buffer[(j*WIDTH+i)*BYTESPERPIXEL+2]=j*256/HEIGHT; /* blue */
    }

  memcpy(g_bgBuffer, buffer, WIDTH*HEIGHT*BYTESPERPIXEL);
}



/*
 * static-box pattern: erase the box from its old position (restoring the
 * background), move it, redraw it, and mark only the two affected rects as
 * modified.
 */
void UpdateStaticBox(rfbScreenInfoPtr rfbScreen)
{
  static int x=100, y=100, dx=4, dy=3;
  int oldx=x, oldy=y;
  int i,j;
  unsigned char* buffer = (unsigned char*)rfbScreen->frameBuffer;

  /* erase old box: copy the pristine background back */
  for(j=oldy; j<oldy+BOX_SIZE; ++j)
    memcpy(&buffer[(j*WIDTH+oldx)*BYTESPERPIXEL],
	   &g_bgBuffer[(j*WIDTH+oldx)*BYTESPERPIXEL],
	   BOX_SIZE*BYTESPERPIXEL);

  /* advance and bounce off the edges */
  x+=dx; y+=dy;
  if(x<0)             { x=0;             dx=-dx; }
  if(x+BOX_SIZE>WIDTH){ x=WIDTH-BOX_SIZE; dx=-dx; }
  if(y<0)              { y=0;              dy=-dy; }
  if(y+BOX_SIZE>HEIGHT){ y=HEIGHT-BOX_SIZE; dy=-dy; }

  /* draw the box (solid red) at its new position */
  for(j=y; j<y+BOX_SIZE; ++j)
    for(i=x; i<x+BOX_SIZE; ++i) {
      buffer[(j*WIDTH+i)*BYTESPERPIXEL+0]=0xff; /* red */
      buffer[(j*WIDTH+i)*BYTESPERPIXEL+1]=0x00; /* green */
      buffer[(j*WIDTH+i)*BYTESPERPIXEL+2]=0x00; /* blue */
    }

  /* mark only the changed sub-regions modified */
  rfbMarkRectAsModified(rfbScreen, oldx, oldy, oldx+BOX_SIZE, oldy+BOX_SIZE);
  rfbMarkRectAsModified(rfbScreen, x,    y,    x+BOX_SIZE,    y+BOX_SIZE);
}



/*
 * dispatch to the active pattern, handling a pending mode switch first.
 */
void UpdateFramebuffer(rfbScreenInfoPtr rfbScreen)
{
  if(g_pattern == PATTERN_FULLFRAME) {
    /* full-frame repaints itself every frame, so a mode switch needs nothing */
    UpdateFullFrame(rfbScreen);
    rfbMarkRectAsModified(rfbScreen, 0, 0, WIDTH, HEIGHT);
  }

  if(g_pattern == PATTERN_STATICBOX) {
    if(g_repaint) {
      /* on switching to static-box, (re)draw the whole background and mark it
	 once so all clients re-sync */
      DrawStaticBackground(rfbScreen);
      rfbMarkRectAsModified(rfbScreen, 0, 0, WIDTH, HEIGHT);
      g_repaint = FALSE;
    }
    UpdateStaticBox(rfbScreen);
  }

  ShowStats(rfbScreen);
}



/*
 * viewer keyboard handler: 't' toggles the test pattern.
 */
void HandleKey(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
  if(down && key == XK_t) {
    g_pattern = (g_pattern == PATTERN_FULLFRAME) ? PATTERN_STATICBOX : PATTERN_FULLFRAME;
    g_repaint = TRUE;
    rfbLog("MulticastVNC example: switched to %s pattern\n",
	   g_pattern == PATTERN_FULLFRAME ? "full-frame" : "static-box");
  }
}




int main(int argc,char** argv)
{                                                                
  rfbScreenInfoPtr server;
  if(!(server = rfbGetScreen(&argc,argv,WIDTH,HEIGHT,8,3,BYTESPERPIXEL)))
    {
      rfbErr("Could not get server.\n");
      return EXIT_FAILURE;
    }
  server->frameBuffer=(char*)malloc(WIDTH*HEIGHT*BYTESPERPIXEL);
  g_bgBuffer=(unsigned char*)malloc(WIDTH*HEIGHT*BYTESPERPIXEL);

  server->desktopName = "MulticastVNC example";

  /* toggle the test pattern with the 't' key from the viewer */
  server->kbdAddEvent = HandleKey;

  /* enable MulticastVNC */
  server->multicastVNC = TRUE;
  /* and make sure unicast and multicast VNC are comparable */
  server->multicastDeferUpdateTime = server->deferUpdateTime = 10;
  server->maxRectsPerUpdate = WIDTH*HEIGHT;

  /* 
     If we said TRUE above, we can supply the address for the multicast group,
     port, TTL and a time interval in miliseconds by which to defer updates.
     Otherwise, libvncserver will use its defaults.
  */
  /*
  server->multicastAddr = "ff00::e000:2a8a"; 
  server->multicastPort = 5901;
  server->multicastTTL = 32;
  server->multicastDeferUpdateTime = 50;
  server->multicastMaxSendRateFixed= 1048576;
  server->multicastPacketSize = 15000;
  */


  /* Initialize the server */
  rfbInitServer(server);           

  rfbLog("Doing %dx%d @%d FPS, sending %dkB/s (raw)\n",
	 WIDTH, HEIGHT, FPS, (WIDTH*HEIGHT*BYTESPERPIXEL*FPS)/1024);
  rfbLog("Press 't' in the viewer to toggle full-frame / static-box pattern.\n");

  /* Loop, updating framebuffer and processing clients */
  while(rfbIsActive(server)) 
    {
      if(UpdateIntervalOver()) {
          UpdateFramebuffer(server);
      }
      rfbProcessEvents(server, server->deferUpdateTime*1000);
    }

  free(g_bgBuffer);
  return EXIT_SUCCESS;
}
