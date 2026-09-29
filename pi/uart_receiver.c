#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <inttypes.h>

#include "state.h"

/** Configure this **/
#define LOCAL_HOST  "172.26.74.45"   // IP of local interface
#define R_PORT 8765

#define REMOTE_HOST "172.26.2.109"
#define S_PORT 8001

#define UART_DEV  "/dev/ttyAMA3"    // UART3 (dtoverlay=uart3): TX=GPIO4, RX=GPIO5. Verify name with: ls /dev/ttyAMA*
#define UART_BAUD B115200           // must match the receiving side
#define ECHO_TO_STDOUT 1            // 1 = also print human-readable text to the local terminal
/** **/

#define TX_INTERVAL_MS 10           // UART packet rate to the STM32 (state messages), ~100 Hz
#define FORCE_INTERVAL_MS 10        // UDP force-feedback rate, when enabled -- independent of TX_INTERVAL_MS
#define STATE_SIZE sizeof(DIJOYSTATE2_t)

// Force feedback is off by default. If turned on, it goes to the wheel over
// UDP only -- it is never framed or sent over the UART link to the STM32.
// Override at build time with -DENABLE_FORCE_FEEDBACK=1 if needed.
#ifndef ENABLE_FORCE_FEEDBACK
#define ENABLE_FORCE_FEEDBACK 0
#endif

/* ---------------------------------------------------------------------
 * Binary UART protocol
 *
 * Force feedback never appears here -- it's UDP-only, straight to the
 * wheel (see ENABLE_FORCE_FEEDBACK / send_force). Only joystick state
 * goes to the STM32 over UART, framed as:
 *   [0]      SOF        0xA5  (frame sync byte)
 *   [1]      type        MSG_STATE
 *   [2]      len         sizeof(payload)
 *   [3..]    payload     packed struct, native byte order
 *   [last]   checksum    8-bit sum of type+len+payload bytes, mod 256
 *
 * The receiver can resync by scanning for SOF, then use `type` to know
 * which struct to memcpy the payload into -- no text parsing needed.
 * ------------------------------------------------------------------- */
#define UART_SOF 0xA5

/* ---------------------------------------------------------------------
 * Activity indicators. Raw register access via /dev/gpiomem, so no extra
 * library (libgpiod, wiringPi, etc.) is needed. Register layout is shared
 * between the BCM2835/2711 SoCs used on all Pi models including the Pi 4.
 * ------------------------------------------------------------------- */
#define GPIO_PIN_UART_TX 23  // toggles when a UART payload is sent to the STM32
#define GPIO_PIN_UDP_RX  24  // toggles when a UDP packet is received from the joystick proxy

#define GPFSEL_BASE   0            // word 0: GPFSEL0
#define GPSET0        7            // word 7: GPSET0
#define GPCLR0        10           // word 10: GPCLR0

static volatile uint32_t *gpio_map = NULL;
static uint32_t gpio_out_state = 0;   // bitmask of current output level per pin

// Maps the GPIO register page once. Call before gpio_config_output().
static int gpio_init(void) {
  int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
  if (fd < 0) return -1;

  gpio_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (gpio_map == MAP_FAILED) { gpio_map = NULL; return -1; }
  return 0;
}

// Configures one pin as an output. Safe to call once per pin after gpio_init().
static void gpio_config_output(int pin) {
  int reg   = GPFSEL_BASE + pin / 10;   // GPFSELn holds 10 pins, 3 bits each
  int shift = (pin % 10) * 3;
  gpio_map[reg] &= ~(0x7u << shift);    // clear the 3 function-select bits
  gpio_map[reg] |=  (0x1u << shift);    // 001 = output
}

// Flips one pin, independent of any other pin's state. The sender thread
// and the receive loop each toggle their own pin under uart_lock, so this
// doesn't need its own locking.
static void gpio_toggle(int pin) {
  if (!gpio_map) return;
  gpio_out_state ^= (1u << pin);
  int level = (gpio_out_state >> pin) & 1;
  gpio_map[level ? GPSET0 : GPCLR0] = (1u << pin);
}

typedef enum {
  MSG_STATE = 0x02,
} uart_msg_type_t;

typedef struct __attribute__((packed)) {
  uint32_t packet_ct;
  int32_t  wheel;     // state.lX
  int32_t  throttle;  // state.lY
  int32_t  brake;     // state.lRz
  uint8_t  button4;   // state.rgbButtons[4]
  uint8_t  button5;   // state.rgbButtons[5]
  uint8_t  button10;  // state.rgbButtons[10]
} state_payload_t;

static int uart_fd = -1;
static pthread_mutex_t uart_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

static int uart_open(const char *dev, speed_t baud) {
  int fd = open(dev, O_RDWR | O_NOCTTY);
  if (fd < 0) return -1;

  struct termios tty;
  if (tcgetattr(fd, &tty) < 0) { close(fd); return -1; }

  cfmakeraw(&tty);                       // 8 data bits, no parity, no output post-processing
  tty.c_cflag |= (CLOCAL | CREAD);       // ignore modem lines, enable receiver
  tty.c_cflag &= ~(CSTOPB | CRTSCTS);    // 1 stop bit, no hardware flow control
  tty.c_cc[VMIN]  = 0;
  tty.c_cc[VTIME] = 0;
  cfsetispeed(&tty, baud);
  cfsetospeed(&tty, baud);

  if (tcsetattr(fd, TCSANOW, &tty) < 0) { close(fd); return -1; }
  tcflush(fd, TCIOFLUSH);
  return fd;
}

static void uart_write_bytes(const uint8_t *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(uart_fd, buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      perror("uart write");
      break;
    }
    off += (size_t) w;
  }
}

// Builds and sends one framed binary message over UART: SOF, type, len,
// payload, checksum. Toggles GPIO_PIN_UART_TX every time a payload
// actually goes out on the wire to the STM32.
static void uart_send_frame(uart_msg_type_t type, const void *payload, uint8_t len) {
  uint8_t frame[3 + 255 + 1];
  uint8_t sum = 0;

  frame[0] = UART_SOF;
  frame[1] = (uint8_t) type;
  frame[2] = len;
  memcpy(frame + 3, payload, len);

  for (uint8_t i = 1; i < 3 + len; i++) sum = (uint8_t) (sum + frame[i]);
  frame[3 + len] = sum;

  pthread_mutex_lock(&uart_lock);
  uart_write_bytes(frame, 3 + len + 1);
  gpio_toggle(GPIO_PIN_UART_TX);
  pthread_mutex_unlock(&uart_lock);
}

// Optional human-readable echo to the local terminal only (never goes on the wire).
static void stdout_echo(const char *fmt, ...) {
#if ECHO_TO_STDOUT
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
#else
  (void) fmt;
#endif
}

#if ENABLE_FORCE_FEEDBACK
// Force feedback to the wheel: UDP only, straight to REMOTE_HOST. This
// never touches the UART link to the STM32, and has no GPIO indicator --
// GPIO23/24 are reserved for the UART-TX and UDP-RX links respectively.
void *send_force(void *arg) {
  (void) arg;
  int8_t force = 0;
  int sockfd;
  struct sockaddr_in servaddr = { 0 };

  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(S_PORT);
  servaddr.sin_addr.s_addr = inet_addr(REMOTE_HOST);

  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("failed to create socket");
    exit(EXIT_FAILURE);
  }

  while (1) {
    usleep(FORCE_INTERVAL_MS * 1000);

    stdout_echo("Send force %d\n", force);

    force += 10;
    sendto(sockfd, (char*) &force, 1, MSG_CONFIRM,
           (struct sockaddr *) &servaddr, sizeof(servaddr));
  }
  return NULL;
}
#endif

int main() {
  int sockfd;
  struct sockaddr_in servaddr = { 0 };

  uart_fd = uart_open(UART_DEV, UART_BAUD);
  if (uart_fd < 0) {
    perror("failed to open " UART_DEV);
    exit(EXIT_FAILURE);
  }

  if (gpio_init() < 0) {
    perror("failed to open /dev/gpiomem");
    exit(EXIT_FAILURE);
  }
  gpio_config_output(GPIO_PIN_UART_TX);
  gpio_config_output(GPIO_PIN_UDP_RX);

  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("failed to create socket");
    exit(EXIT_FAILURE);
  }

  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(R_PORT);
  servaddr.sin_addr.s_addr = inet_addr(LOCAL_HOST);

  if (bind(sockfd, (const struct sockaddr *) &servaddr,
           sizeof(servaddr)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }

  DIJOYSTATE2_t state;
  char recvbuf[sizeof(DIJOYSTATE2_t) + 4];
  uint64_t last_tx_ms = 0;
#if ENABLE_FORCE_FEEDBACK
  pthread_t send_tid;
  pthread_create(&send_tid, NULL, send_force, NULL);
#endif

  while (1) {
    struct sockaddr_in cliaddr;
    socklen_t len = sizeof(cliaddr);
    // packet = 4-byte counter + full joystick state
    ssize_t n = recvfrom(sockfd, recvbuf, sizeof(recvbuf), 0,
                         (struct sockaddr *) &cliaddr, &len);
    if (n < (ssize_t) sizeof(recvbuf)) continue;   // short/failed read
    gpio_toggle(GPIO_PIN_UDP_RX);                  // a full state packet arrived

    uint32_t packet_ct;
    memcpy(&packet_ct, recvbuf, 4);
    memcpy(&state, recvbuf + 4, sizeof(state));    // always keep the latest state

    // Rate-limit the UART link to the STM32 to TX_INTERVAL_MS, independent
    // of how fast UDP packets actually arrive -- this is what sets the
    // Pi<->STM32 packet rate, not the incoming joystick update rate.
    uint64_t now = now_ms();
    if (now - last_tx_ms < TX_INTERVAL_MS) continue;
    last_tx_ms = now;

    state_payload_t payload = {
      .packet_ct = packet_ct,
      .wheel     = state.lX,
      .throttle  = state.lY,
      .brake     = state.lRz,
      .button4   = state.rgbButtons[4],
      .button5   = state.rgbButtons[5],
      .button10  = state.rgbButtons[10],
    };
    uart_send_frame(MSG_STATE, &payload, sizeof(payload));

    stdout_echo("Receive state (Pkt: %8X) :  Wheel: %d | Throttle: %d | Brake: %d | Button4: %d | Button5: %d | Button10: %d\n",
                packet_ct, state.lX, state.lY, state.lRz,
                state.rgbButtons[4], state.rgbButtons[5], state.rgbButtons[10]);
  }

  return 0;
}
