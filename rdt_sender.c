#include <arpa/inet.h>
#include <assert.h>
#include <math.h>
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
#define RETRY 1000      // millisecond
#define WINDOW_SIZE 10  // 5 for now, change to 10
#define ARRAY_SIZE WINDOW_SIZE * 2

int next_seqno = -1;
int send_base = -1;

/* Variables I added */

int expectedAck = -1;
tcp_packet *windowPacks[ARRAY_SIZE];
tcp_packet *eofPacket;
int lastUnAckedPack = 0;  // packet waiting for acknowledgement
int nextPointer = -1;
int numUnackedPack = 0;  // ensure this number is less than WINDOW_SIZE

/*   */

int sockfd, serverlen, retrial = 0;
struct sockaddr_in serveraddr;
struct itimerval timer;
FILE *fp;
tcp_packet *recvpkt;
sigset_t sigmask;

// function to increase next pointer
bool increasePointer() {
  if (numUnackedPack >= WINDOW_SIZE) {
    return false;
  }
  numUnackedPack++;
  nextPointer++;
  nextPointer %= ARRAY_SIZE;
  return true;
}

// function that resends unacked packets after a signal
void resend_packets(int sig) {
  // send all n packets in the array
  if (sig == SIGALRM) {
    // Resend all packets range between
    // sendBase and nextSeqNum
    VLOG(INFO, "Timeout.");
    // printf("lastUnacked: %d, numUnacked:%d")
    int upperLim = lastUnAckedPack + numUnackedPack;
    for (int i = lastUnAckedPack; i < upperLim; i++) {
      int packNum = i % WINDOW_SIZE;
      VLOG(DEBUG, "Resending pack %d", (int)windowPacks[packNum]->hdr.seqno / (int)DATA_SIZE);
      if (sendto(sockfd, windowPacks[packNum], TCP_HDR_SIZE + get_data_size(windowPacks[packNum]), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0) {
        error("sendto");
      }
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
  int len = 1;  // dummy value does not matter
  char buffer[DATA_SIZE];
  assert(MSS_SIZE - TCP_HDR_SIZE > 0);

  // Stop and wait protocol
  init_timer(RETRY, resend_packets);
  next_seqno = 0;

  // outer loop to run forever
  while (1) {
    // loop to send multiple packets as long as the window size is not full
    // int finalPck = nextPointer;

    while (len > 0 && increasePointer()) {  // if len < 0, no need to make more packets
      len = fread(buffer, 1, DATA_SIZE, fp);
      // end of file = send empty packet
      if (len <= 0) {
        VLOG(INFO, "End Of File has been reached");
        // windowPacks[nextPointer] = make_packet(0);
        eofPacket = make_packet(0);

        // sequence number for the final EOF packet is the size of file
        // windowPacks[nextPointer]->hdr.seqno = windowPacks[finalPck]->hdr.seqno + windowPacks[finalPck]->hdr.data_size;
        numUnackedPack--;

        if (nextPointer == 0)
          nextPointer = ARRAY_SIZE - 1;
        else
          nextPointer--;

        eofPacket->hdr.seqno = windowPacks[nextPointer]->hdr.seqno + windowPacks[nextPointer]->hdr.data_size;
        // nextPointer--;
        // VLOG(INFO, "Sending packet last to notify EOF\n");

        // sendto(sockfd, windowPacks[nextPointer], TCP_HDR_SIZE, 0, (const struct sockaddr *)&serveraddr, serverlen);
        break;
      }

      // Else: there is data to send = make packets
      // finalPck = nextPointer;
      send_base = next_seqno;
      next_seqno += len;
      if (expectedAck == -1)
        expectedAck = next_seqno;
      windowPacks[nextPointer] = make_packet(len);
      memcpy(windowPacks[nextPointer]->data, buffer, len);
      windowPacks[nextPointer]->hdr.seqno = send_base;
      VLOG(DEBUG, "Sending packet %d, seqno:(%d) to %s", (int)send_base / (int)DATA_SIZE, send_base, inet_ntoa(serveraddr.sin_addr));

      if (sendto(sockfd, windowPacks[nextPointer], TCP_HDR_SIZE + get_data_size(windowPacks[nextPointer]), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0)
        error("sendto");
    }

    // Wait for ACK
    start_timer();
    do {
      // ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
      // struct sockaddr *src_addr, socklen_t *addrlen);
      // printf("Exac1: %d\n", expectedAck);

      do {
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen) < 0)
          error("recvfrom");

        recvpkt = (tcp_packet *)buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
      } while (recvpkt->hdr.ackno < expectedAck);  // ignore duplicate ACKs

      /*resend pack if don't recv ACK */

    } while (recvpkt->hdr.ackno != expectedAck);

    printf("%d, Acked: %d, pkt:%d \n", get_data_size(recvpkt), recvpkt->hdr.ackno, (int)(((float)recvpkt->hdr.ackno / (float)DATA_SIZE) - 0.01f));

    bzero(windowPacks[lastUnAckedPack]->data, sizeof(windowPacks[lastUnAckedPack]->data));
    free(windowPacks[lastUnAckedPack]);
    lastUnAckedPack++;
    lastUnAckedPack %= ARRAY_SIZE;
    numUnackedPack--;
    // end of transfer // no need to ack the EOF packet
    if (len <= 0 && numUnackedPack == 0) {
      // send eof packet
      printf("Sending EOF packet. \n");
      for (int indx1 = 0; indx1 <= 20; indx1++)
        sendto(sockfd, eofPacket, TCP_HDR_SIZE, 0, (const struct sockaddr *)&serveraddr, serverlen);
      break;
    }
    expectedAck = windowPacks[lastUnAckedPack]->hdr.seqno + windowPacks[lastUnAckedPack]->hdr.data_size;
    // we are asuming the expected ack for the final empty packet is seqnumber + 1
    if (windowPacks[lastUnAckedPack]->hdr.data_size == 0)
      expectedAck++;
    stop_timer();

    // printf("Exack: %d\n", expectedAck);
  }

  return 0;
}

// run command: ./rdt_sender $MAHIMAHI_BASE 5001 sendfile.txt