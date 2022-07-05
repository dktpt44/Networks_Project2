#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"
#include "packet.h"

/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *sndpkt;
int expectedSeqno = 0;

int sockfd;                    /* socket */
int clientlen;                 /* byte size of client's address */
struct sockaddr_in clientaddr; /* client addr */

void sendAck() {
  sndpkt = make_packet(0);
  // sndpkt->hdr.seqno = recvpkt->hdr.seqno; // added
  sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
  // increment ackno by 1 for the final packet
  if (recvpkt->hdr.data_size == 0)
    sndpkt->hdr.ackno++;
  sndpkt->hdr.ctr_flags = ACK;
  if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)&clientaddr, clientlen) < 0) {
    error("ERROR in sendto");
  }
  printf("sending ack: %d\n", sndpkt->hdr.ackno);
}

int main(int argc, char **argv) {
  int portno; /* port to listen on */

  struct sockaddr_in serveraddr; /* server's addr */
  int optval;                    /* flag value for setsockopt */
  FILE *fp;
  char buffer[MSS_SIZE];
  struct timeval tp;

  /*
   * check command line arguments
   */
  if (argc != 3) {
    fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
    exit(1);
  }
  portno = atoi(argv[1]);

  fp = fopen(argv[2], "w");
  if (fp == NULL) {
    error(argv[2]);
  }

  /*
   * socket: create the parent socket
   */
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    error("ERROR opening socket");

  optval = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));

  /*
   * build the server's Internet address
   */
  bzero((char *)&serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons((unsigned short)portno);

  /*
   * bind: associate the parent socket with a port
   */
  if (bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
    error("ERROR on binding");

  /*
   * main loop: wait for a datagram, then echo it
   */
  VLOG(DEBUG, "epoch time, bytes received, sequence number");

  clientlen = sizeof(clientaddr);
  while (1) {
    /*
     * recvfrom: receive a UDP datagram from a client
     */
    // VLOG(DEBUG, "waiting from server \n");
    if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&clientaddr, (socklen_t *)&clientlen) < 0)
      error("ERROR in recvfrom");

    recvpkt = (tcp_packet *)buffer;
    assert(get_data_size(recvpkt) <= DATA_SIZE);

    // discarding out of sequence packets
    printf("excpt:%d, seqno:%d.\n", expectedSeqno, recvpkt->hdr.seqno);

    if (expectedSeqno == recvpkt->hdr.seqno) {
      if (recvpkt->hdr.data_size == 0) {
        // VLOG(INFO, "End Of File has been reached");
        fclose(fp);
        sendAck();  // sending acknowledgement for the EOF notifier packet
        break;
      }
      /*
       * sendto: ACK back to the client
       */
      gettimeofday(&tp, NULL);
      // note this is not printed if the file size is exactly a multiple for 1456 bytes
      if (recvpkt->hdr.data_size != 1456) {
        printf("This is final line.\n");
        printf("\n\n%s\n\n", recvpkt->data);
      }
      VLOG(DEBUG, "Writing: %lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, (int)(recvpkt->hdr.seqno / DATA_SIZE));

      fseek(fp, recvpkt->hdr.seqno, SEEK_SET);

      fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
      sendAck();
      expectedSeqno = sndpkt->hdr.ackno;
    }

    else {
      // discard the packet but still send the acknowledgement number
      // this is the case when acknowledgement number gets lost
      sendAck();
    }
  }

  return 0;
}

// run command: ./rdt_receiver 5001 recv.txt