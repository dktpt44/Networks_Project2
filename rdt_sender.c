#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "packet.h"

#define STDIN_FD 0
#define RETRY 120      // millisecond
#define WINDOW_SIZE 5  // 5 for now, change to 10

int next_seqno = -1;
int send_base = -1;

/* Variables I added */

int expectedAck = -1;
tcp_packet *windowPacks[WINDOW_SIZE * 2];
int lastUnAckedPack = 0;  // packet waiting for acknowledgement
int nextPointer = -1;
int numUnackedPack = 0;  // ensure this number is less than WINDOW_SIZE

/*   */

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
FILE *fp;
/*
tcp_packet *sndpkt;
*/
tcp_packet *recvpkt;
sigset_t sigmask;

// function to increase next pointer
bool increasePointer() {
  if (numUnackedPack >= WINDOW_SIZE) {
    return false;
  }
  numUnackedPack++;
  nextPointer++;
  nextPointer %= WINDOW_SIZE;
  return true;
}

// function that resends unacked packets after a signal
void resend_packets(int sig) {
  if (sig == SIGALRM) {
    // Resend all packets range between
    // sendBase and nextSeqNum
    VLOG(INFO, "Timeout happend");
    VLOG(DEBUG, "Re-sending pack %d to %s", (int)send_base / (int)DATA_SIZE, inet_ntoa(serveraddr.sin_addr));
    if (sendto(sockfd, windowPacks[lastUnAckedPack], TCP_HDR_SIZE + get_data_size(windowPacks[lastUnAckedPack]), 0,
               (const struct sockaddr *)&serveraddr, serverlen) < 0) {
      error("sendto");
    }
  }
}

// function to start timer
void start_timer() {
  sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
  setitimer(ITIMER_REAL, &timer, NULL);
}

// function to stop timer
void stop_timer() {
  sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) {
  signal(SIGALRM, resend_packets);
  timer.it_interval.tv_sec = delay / 1000;  // sets an interval of the timer
  timer.it_interval.tv_usec = (delay % 1000) * 1000;
  timer.it_value.tv_sec = delay / 1000;  // sets an initial value
  timer.it_value.tv_usec = (delay % 1000) * 1000;

  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGALRM);
}

// function that initiates socket
void initiateSock(int argc, char **argv) {
  int portno;
  char *hostname;

  /* check command line arguments */
  if (argc != 4) {
    fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
    exit(0);
  }
  hostname = argv[1];
  portno = atoi(argv[2]);
  fp = fopen(argv[3], "r");
  if (fp == NULL) {
    error(argv[3]);
  }
  /* socket: create the socket */
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    error("ERROR opening socket");
  /* initialize server server details */
  bzero((char *)&serveraddr, sizeof(serveraddr));
  serverlen = sizeof(serveraddr);
  /* covert host into network byte order */
  if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
    fprintf(stderr, "ERROR, invalid host %s\n", hostname);
    exit(0);
  }
  /* build the server's Internet address */
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_port = htons(portno);
}

// main function
int main(int argc, char **argv) {
  initiateSock(argc, argv);
  int len = 0;
  char buffer[DATA_SIZE];
  assert(MSS_SIZE - TCP_HDR_SIZE > 0);

  // Stop and wait protocol
  init_timer(RETRY, resend_packets);
  next_seqno = 0;

  // outer loop to run forever
  while (1) {
    // loop to send multiple packets as long as the window size is not full
    while (increasePointer()) {
      len = fread(buffer, 1, DATA_SIZE, fp);

      // end of file = send empty packet
      if (len <= 0) {
        VLOG(INFO, "End Of File has been reached");
        nextPointer++;
        nextPointer %= WINDOW_SIZE;
        windowPacks[nextPointer] = make_packet(0);
        sendto(sockfd, windowPacks[nextPointer], TCP_HDR_SIZE, 0, (const struct sockaddr *)&serveraddr, serverlen);
        break;
      }

      // there is data to send = make packets
      send_base = next_seqno;
      next_seqno = send_base + len;
      if (expectedAck == -1)
        expectedAck = next_seqno;
      windowPacks[nextPointer] = make_packet(len);
      memcpy(windowPacks[nextPointer]->data, buffer, len);
      windowPacks[nextPointer]->hdr.seqno = send_base;
      VLOG(DEBUG, "Sending packet %d to %s", (int)send_base / (int)DATA_SIZE, inet_ntoa(serveraddr.sin_addr));
      if (sendto(sockfd, windowPacks[nextPointer], TCP_HDR_SIZE + get_data_size(windowPacks[nextPointer]), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0)
        error("sendto");
    }

    if (len <= 0)
      break;

    // Wait for ACK
    do {
      start_timer();
      // ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
      // struct sockaddr *src_addr, socklen_t *addrlen);

      do {
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen) < 0)
          error("recvfrom");

        recvpkt = (tcp_packet *)buffer;
        printf("%d, for:%d \n", get_data_size(recvpkt), (int)(recvpkt->hdr.ackno / DATA_SIZE) - 1);
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        free(windowPacks[lastUnAckedPack]);
        lastUnAckedPack++;
        lastUnAckedPack %= WINDOW_SIZE;
        numUnackedPack--;
      } while (recvpkt->hdr.ackno < expectedAck);  // ignore duplicate ACKs
      stop_timer();

      /*resend pack if don't recv ACK */
    } while (recvpkt->hdr.ackno != expectedAck);
    expectedAck = windowPacks[(lastUnAckedPack + 1) % WINDOW_SIZE]->hdr.seqno;
  }

  return 0;
}

// run command: ./rdt_sender $MAHIMAHI_BASE 5001 sendfile.txt