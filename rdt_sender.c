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

#define WINDOW_SIZE 10
#define STDIN_FD 0
#define RETRY 120  // millisecond

int next_seqno = 0;
int send_base = 0;
int window_size = WINDOW_SIZE;  //
int unacketPackets = 0;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;

int nextSpace = 0;
int lastAckedPointer = 0;

tcp_packet *windowPckst[WINDOW_SIZE * 2];

void resend_packets(int sig) {
  if (sig == SIGALRM) {
    // Resend all packets range between
    // sendBase and nextSeqNum
    VLOG(INFO, "Timout happend. Resending...");

    if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0) {
      error("sendto");
    }
  }
}

void start_timer() {
  printf("++ Timer started.\n");
  sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
  setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer() {
  printf("-- Timer ended.\n");
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

int main(int argc, char **argv) {
  int portno, len;
  int next_seqno;
  char *hostname;
  char buffer[DATA_SIZE];
  FILE *fp;

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

  assert(MSS_SIZE - TCP_HDR_SIZE > 0);

  // Stop and wait protocol

  init_timer(RETRY, resend_packets);
  next_seqno = 0;
  while (1) {
    // make packets
    send_base = next_seqno;
    // lastAckedPointer
    // nextSpace
    int numOfPcksTomake = lastAckedPointer > nextSpace ? lastAckedPointer - nextSpace : nextSpace - lastAckedPointer;
    numOfPcksTomake = numOfPcksTomake == 0 ? 5 : numOfPcksTomake;

    for (int i = 0; i < numOfPcksTomake; i++) {
      len = fread(buffer, 1, DATA_SIZE, fp);

      if (len <= 0) {
        VLOG(INFO, "End Of File has been reached");
        sndpkt = make_packet(0);
        printf("Send done. Packnum: %d\n", send_base);
        sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (const struct sockaddr *)&serveraddr, serverlen);

        break;
      }

      next_seqno += len;
      sndpkt = make_packet(len);
      memcpy(sndpkt->data, buffer, len);

      sndpkt->hdr.seqno = send_base;
      // everytime a packet is made, store in the array to avoid remaking of packets when lost
      windowPckst[nextSpace] = sndpkt;

      if (nextSpace == window_size) {
        nextSpace++;
        nextSpace %= window_size;
        break;
      }
      nextSpace++;
    }

    // Wait for ACK
    do {
      /*
       * If the sendto is called for the first time, the system will
       * will assign a random port number so that server can send its
       * response to the src port.
       */

      // send packets
      int j = 0;
      int packIndex = nextSpace;
      do {
        VLOG(DEBUG, "Sending packet %d to %s", send_base, inet_ntoa(serveraddr.sin_addr));
        if (sendto(sockfd, windowPckst[], TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0) {
          error("sendto");
        }
        unacketPackets++;
        // start timer after sending the first packet
        if (j == 0) {
          start_timer();
        }
        j++;
      } while (unacketPackets < window_size);

      // ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
      // struct sockaddr *src_addr, socklen_t *addrlen);

      // listening for acknowledgements
      do {
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen) < 0) {
          error("recvfrom");
        }
        recvpkt = (tcp_packet *)buffer;
        if (recvpkt->hdr.ackno == send_base + DATA_SIZE) {
          send_base = recvpkt->hdr.ackno;
          lastAckedPointer++;
          lastAckedPointer %= window_size;
        }

        printf("AckPcktSize:%d \n", get_data_size(recvpkt));
        assert(get_data_size(recvpkt) <= DATA_SIZE);

      } while (recvpkt->hdr.ackno < next_seqno);  // ignore duplicate ACKsf

      /*resend pack if don't recv ACK */
    } while (recvpkt->hdr.ackno != next_seqno);
    stop_timer();

    free(sndpkt);
  }

  return 0;
}
