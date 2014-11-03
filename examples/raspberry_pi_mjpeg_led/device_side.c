/* Copyright (c) 2014 Cesanta Software Limited
 * All rights reserved
 *
 * This program polls given file, and if it is modified, it sends it
 * over the websocket connection to the specified server.
 */

/* This is the device endpoint of the Raspberry Pi camera/LED example
 * of the Fossa networking library.
 * It is a simple websocket client, sending jpeg frames obtained
 * from the RPi camera and receiving JSON commands through the same
 * WebSocket channel
 */

#include <unistd.h>
#include "fossa.h"

static int s_poll_interval_ms = 100;
static int s_still_period = 1000;
static int s_vertical_flip = 0;
static int s_width = 320;
static int s_height = 180;
static const char *s_mjpg_file = "/var/run/shm/cam.jpg";

static struct ns_connection *client;

/* Check if there is a new image available and
 * send it to the cloud endpoint if the send buffer is not too full.
 * The image is moved in a new file by the jpeg optimizer function;
 * this ensures that we will detect a new frame when raspistill writes
 * it's output file.
 */
static void send_mjpg_frame(struct ns_connection *nc, const char *file_path) {
  static int skipped_frames = 0;
  struct stat st;
  FILE *fp;

  /* Check file modification time. */
  if (stat(file_path, &st) == 0) {
    /* Skip the frame if there is too much unsent data. */
    if (nc->send_iobuf.len > 256) skipped_frames++;

    /* Read new mjpg frame into a buffer */
    fp = fopen(file_path, "rb");
    char buf[st.st_size];
    fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    /* Delete the file so we can detect when raspistill creates a new one.
     * mtime granularity is only 1s. */
    unlink(file_path);

    /* Send those buffer through the websocket connection */
    ns_send_websocket_frame(nc, WEBSOCKET_OP_BINARY, buf, sizeof(buf));
    printf("Sent mjpg frame, %lu bytes after skippping %d frames\n", (unsigned long) sizeof(buf), skipped_frames);
    skipped_frames = 0;
  }
}

/* Turn on or off the LED.
 * The LED in this example is an RGB led, so all the colors have to be set.
 */
static void set_led(int v) {
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "for i in 22 23 24; do"
           " echo %d >/sys/class/gpio/gpio$i/value; done", v);
  system(cmd);
}

/* Parse control JSON and perform command:
 * for now only LED on/off is supported. */
static void perform_control_command(const char* data, size_t len) {
  struct json_token toks[200], *onoff;
  parse_json(data, len, toks, sizeof(toks));
  onoff = find_json_token(toks, "onoff");
  set_led(strncmp("[\"on\"]", onoff->ptr, onoff->len) == 0);
}

/* Main event handler. Sends websocket frames and receives control commands */
static void ev_handler(struct ns_connection *nc, int ev, void *ev_data) {
  struct websocket_message *wm = (struct websocket_message *) ev_data;

  switch (ev) {
    case NS_CONNECT:
      printf("Reconnect: %s\n", * (int *) ev_data == 0 ? "ok" : "failed");
      if (* (int *) ev_data == 0) {
        /* tune the tcp send buffer size, so that we can skip frames
         * when the connection is congested. This helps maintaining a
         * reasonable latency. */
        int sndbuf_size = 512;
        if(setsockopt(nc->sock, SOL_SOCKET, SO_SNDBUF,
                      (void *) &sndbuf_size, sizeof(int)) == -1) {
          perror("failed to tune TCP send buffer size\n");
        }

        ns_send_websocket_handshake(nc, "/stream", NULL);
      }
      break;
    case NS_CLOSE:
      printf("Connection %p closed\n", nc);
      client = NULL;
      break;
    case NS_POLL:
      send_mjpg_frame(nc, s_mjpg_file);
      break;
    case NS_WEBSOCKET_FRAME:
      printf("Got control command: [%.*s]\n", (int) wm->size, wm->data);
      perform_control_command((const char*)wm->data, wm->size);
      break;
  }
}

/* This thread regenerates s_mjpg_file every s_poll_interval_ms milliseconds.
 * It is Raspberry PI specific, change this function on other systems. */
static void *generate_mjpg_data_thread_func(void *param) {
  char cmd[400];
  (void) param;

  snprintf(cmd, sizeof(cmd), "raspistill -w %d -h %d -n -q 100 -tl %d "
                "-t 999999999 -v %s -o %s >/dev/null 2>&1", s_width, s_height,
                s_still_period, s_vertical_flip ? "-vf" : "", s_mjpg_file);

  for(;;) {
    int ret = system(cmd);
    if (WIFSIGNALED(ret)) exit(1);
    sleep(1);
  }
  return NULL;
}

void usage(char *argv[]) {
  fprintf(stderr, "Usage: %s <server_address>\n", argv[0]);
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
  struct ns_mgr mgr;

  if (argc < 1) {
    usage(argv);
  }
  char *addr = argv[1];

  /* Start separate thread that generates MJPG data */
  ns_start_thread(generate_mjpg_data_thread_func, NULL);

  printf("Streaming [%s] to [%s]\n", s_mjpg_file, addr);

  ns_mgr_init(&mgr, NULL);

  for(;;) {
    ns_mgr_poll(&mgr, s_poll_interval_ms);
    
    /* Reconnect if disconnected */
    if (!client) {
      sleep(1); /* limit reconnections frequency */
      printf("Reconnecting to %s...\n", addr);
      
      client = ns_connect(&mgr, addr, ev_handler);
      if (client) ns_set_protocol_http_websocket(client);
    }
  }
}
