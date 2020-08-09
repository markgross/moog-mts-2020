/*
 * This program pretends to be the MTS keyboard. Its job is to send out a
 * single OSC-formatted UDP packet, 500 times/sec. Each such OSC/UDP packet
 * has 88 keys x 6 params/key encoded into it. These packets are intended to
 * arrive at the cvt (conversion/calibration) program I wrote, which applies
 * a normalization/calibration to the X and Y parameters for each key.
 * Eventually there will probably also be calibration for the Z, F, & A.
 *
 * Let the program invocation accept a command line input for the #packets/sec,
 * with a no-arg default of 500/sec. (I'd rather debug 1 pkt/sec than 500!)
 * This program is pretending to be the MTS keyboard, so I expect the "all 88
 * keys all the time" to stick, even though it will cause 9x the UDP traffic.
 * The cvt program downstream will figure out which keys are active and only
 * send those on to the SuperCollider running on Windows 10.
 *
 * RPC 5/6/2020
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <time.h>

#define PORTNO 57120
#define MAX(i,j) (((i) > (j)) ? (i) : (j))
#define MIN(i,j) (((i) < (j)) ? (i) : (j))
#define MAXLINE 1024
#define NUMKEYS 88
#define PARAMS_PER_KEY 6
#define BLOBSIZE NUMKEYS * PARAMS_PER_KEY
#define FIRSTBLOBVALIN 62
#define DEFAULT_HEADERSIZE 20

int  init_osc_pkt(void);
void mod_osc_pkt(int, int, int);
void read_calib_file();
void dump_buff(void);
void delay(int);
char pktfile[30] = {};
void find_next_pkt();
void read_pkt_file(void);
void skip_bytes(int);

// outbuff[0 - 15] don't get overwritten in code below. Rest does.
char outbuff[DEFAULT_HEADERSIZE + BLOBSIZE] =
    {
        '/', 'm', 't', 's',   0,   0,   0,   0,
        ',', 'i', 'b',   0,   0,   0,   0,  24, // 24 is arb value for 'i'
          0,   0,   2,  10,   1,   1,   1,   1, // 88x6 = 528 (0x210)
          1,   1,   2,   2,   2,   2,   2,   2  // 
    };

typedef struct {
    int xmax;
    int xmin;
    int ymax;
    int ymin;
} Ctable;

Ctable Ct[NUMKEYS+1]; // we ignore the [0] entry...piano keys numbered 1:88

void handle_switches(int, char**);
typedef struct { int x, y; } xy;
xy gen_xy(int);

void error(char *msg) {
    perror(msg);
    exit(0);
}

//globals to be set in handle_switches()
float pkts_per_sec;
bool  pkt_file_in = false;
char  hostname[20] = "127.0.0.1";
bool  debug=false;
FILE * pf;  // packet in file descriptor
FILE * fp;
int   HeaderSize = 20;


int main(int argc, char *argv[])
{
    int k,x,y,z,f,a,keydown,i;
    xy rawxy;
    int sockfd, n, len, serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;

    handle_switches(argc, argv);

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        perror("ERROR opening socket");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host as %s\n", hostname);
        exit(0);
    }
    if(debug) printf("Will send OSC packets to %s.\n", hostname);

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
	  (char *)&serveraddr.sin_addr.s_addr, server->h_length);

    serveraddr.sin_port = htons(PORTNO);
    serverlen = sizeof(serveraddr);

    // if pktfile is to be source, let's see if we can open it
    if (pkt_file_in) {
       pf = fopen(pktfile, "r");
       if (pf == NULL) {
          printf("Error, cannot open %s as packet file input. \n", pktfile);
          exit(1);
       }
    }

    len = HeaderSize + BLOBSIZE;  // 20 + 88*6) = 548, in other words
    init_osc_pkt();
    read_calib_file();

    if (pkt_file_in) {
	while (1) {     // do every packet in the file

	   // pull in entire packet, all 88 keys * 6 params/key
	   read_pkt_file();

	   // outbuff now has next packet loaded. Send it to dest.
           //if(debug)dump_buff();
           n = sendto(sockfd, outbuff, len, 0, (struct sockaddr *) &serveraddr, serverlen);
           if (n < 0)
              fprintf(stderr,"ERROR in sendto, returned %d.\n", n);

           delay((int) (1000.0/pkts_per_sec));  // scanrate is #scans/sec
                                                // delay() arg is milliseconds
        }

    } else {

        while (1) {
           for (i=1; i<=NUMKEYS; i++) {
              rawxy = gen_xy(i);       // gen random x, y within cal table bounds
              if (debug) printf("DEBUG: key %2d, xraw=%02x, yraw=%02x\n",
                                                  i, rawxy.x, rawxy.y);
              mod_osc_pkt(i, rawxy.x, rawxy.y);
           }

           if(debug)dump_buff();
           n = sendto(sockfd, outbuff, len, 0, (struct sockaddr *) &serveraddr, serverlen);
           if (n < 0)
              fprintf(stderr,"ERROR in sendto, returned %d.\n", n);

           delay((int) (1000.0/pkts_per_sec));  // scanrate is #scans/sec
                                                // delay() arg is milliseconds
        } // end while
    } // end else
}; // end main


// read packet from packet file in, put in outbuff
void read_pkt_file() {
   int b, i, j;

   find_next_pkt();	// skip over file stream to next data
   skip_bytes(42);
   for (b=HeaderSize; b<BLOBSIZE+HeaderSize; ) {
      for (i=0; i<8; i++) {
         if (fscanf(pf, "%x", &j) == EOF) break;
         if(debug)printf("%02x %d", j, b);
	 if(debug && i==7) printf("\n");
         outbuff[b] = j;
	 b++;
         if (b >= BLOBSIZE+HeaderSize) break;
      }
   }
   if(debug)printf("\n\n");
}

// skip stuff in the pkt input file that isn't blob data
void find_next_pkt() {
   int i, sink;
   char str[MAXLINE];

   // fgets returns NULL on end-of-file
   if (fgets(str, MAXLINE, pf) == NULL) exit(0);
   while (strstr(str, "static const unsigned char")==NULL ||
          strstr(str, "[590]")==NULL) {
      if (fgets(str, MAXLINE, pf) == NULL) exit(0);
   }
   return;
}

// skip incoming bytes of header; we're making our own
void skip_bytes(int n) {
   int m, sink;

   for (m=0; m<n; m++) {
      if (fscanf(pf, "%x", &sink) == EOF) break;
         if(debug)printf("skip: %02x ", sink);
   }
   if(debug)printf("\n\n");
}


// using the key value passed in, look up calibration bounds for x and y
// then generate a random value for each that is within those bounds
xy gen_xy(int k) {
   int xraw, xmin, xmax, yraw, ymin, ymax;
   xy xyraw;

   // FIXME: arbitrarily setting mins/maxs; will look up in table once this works
   xmin = Ct[k].xmin;
   xmax = Ct[k].xmax;
   ymin = Ct[k].ymin;
   ymax = Ct[k].ymax;

   do {
       xyraw.x = rand() & 0xff;
   } while (xyraw.x<xmin || xyraw.x>xmax);

   do {
       xyraw.y = rand() & 0xff;
   } while (xyraw.y<ymin || xyraw.y>ymax);

   return (xyraw);
}

/************* New OSC Message using blobs *************/
void mod_osc_pkt(int k, int xraw, int yraw)
{
    int i, zraw, fraw, araw, key_byte_num;
    //printf("DEBUG: mod_osc_pkt called k=%d, xraw=0x%02x, yraw=0x%02x\n", k, xraw, yraw);
    //FIXME: for debug, setting Z, F, A to key num
    zraw = fraw = araw = k;

    i = key_byte_num = HeaderSize + (k-1)*PARAMS_PER_KEY; // OSC pkt is header | blob

    // per email of 5/25/2020 from GS, order is X,Y,Z,A,F
    outbuff[i]   = k;
    outbuff[i+1] = xraw;
    outbuff[i+2] = yraw;
    outbuff[i+3] = zraw;
    outbuff[i+4] = araw;
    outbuff[i+5] = fraw;

// key_active stuff...add this in later FIXME
/*           // key number range is 1-88. So use bit[6] for "key active"
           // not using MSB to keep SC from thinking neg numbers
           if (key_active==k) {
              printf("DEBUG: key_active=%2d, k=%2d\n",key_active, k);
              buff_str[i] = buff_str[i] | 0x80;
           }
           k++;
        }
*/


} // End mod_osc

int init_osc_pkt() {
        int blobsize;

        // gotta map blob size into 4 consecutive chars in blob (OSC rule)
	// if -n switch (no SC header) these 4 writes will get overwritten
	// by key data.
        blobsize = BLOBSIZE;
        //printf("DEBUG: blobsize: %d\n", blobsize);
        outbuff[19] = (char) ( blobsize & 0xff);
        outbuff[18] = (char) ((blobsize & 0xff00)>>8);
        outbuff[17] = (char) ((blobsize & 0xff0000)>>16);
        outbuff[16] = (char) ((blobsize & 0xff000000)>>24);

        //dump_buff(buff_str);

	return (sizeof(outbuff));  // return size of entire packet for send()
}

void dump_buff() {
   int i, j;

   if (HeaderSize != 0) {
      for (i=0;  i<8;  i++) { printf("%02x ", (int) outbuff[i]); }; printf("\n");
      for (i=8;  i<16; i++) { printf("%02x ", (int) outbuff[i]); }; printf("\n");
      for (i=16; i<20; i++) { printf("%02x ", (int) outbuff[i]); }; printf("\n");
   };

   for (i=HeaderSize; i<(HeaderSize + BLOBSIZE); i++) {
      printf("%02x ", (int) outbuff[i]);
   }
}

//
// for getopts() doc see man7.org/linux/man-pages/man3/getopt.3.html
//
void handle_switches( int argc, char *argv[]) {
   int opt;
   float scanrate;  // outgoing 88-key pkts/sec

   scanrate = 1.0;    // default 1 pkt/sec for debug
   debug = false;
   pkt_file_in = false;

   while((opt = getopt(argc, argv, "hnp:dr:s:")) != -1) // ':' means that flag expects arg
   {
      switch(opt)
      {
	case 'h':
		printf("usage: fakemts [-h][-d][-r rate][-s SC IP address]\n");
		printf("-n no Supercollider header, key blob only. SC default.\n");
		printf("-p pktfile.ext as input; randoms are default if no -p.\n");
		printf("-r defaults to 1 scan/sec; max r is 500.\n");
		printf("-s defaults to 127.0.0.1. Windows SC is 192.168.1.4.\n");
		printf("-d turns debug mode on.\n");
		exit(EXIT_FAILURE);
		break;
	case 'n':
		HeaderSize = 0;
		break;
	case 'p':		// use packet file from GS
		strcpy(pktfile, optarg);
		pkt_file_in = true;
		break;
	case 'd':
		printf("Debug mode requested.\n");
		debug = true;
		break;
        case 'r':
                scanrate = atof(optarg);
		if (scanrate > 500) {
			fprintf(stderr, "Error: upper limit of scanrate is 500.\n");
			exit(EXIT_FAILURE);
                };
		break;
	case 's':
		strcpy(hostname, optarg);
		break;
	default:
		fprintf(stderr, "Error: -h for help.\n");
		exit(EXIT_FAILURE);
      }
   }
   for(; optind < argc; optind++) {
	printf("extra arguments: %s\n", argv[optind]);
   }

   pkts_per_sec = scanrate;
}

void read_calib_file() {
    FILE * fp;
    int i, key;
    int x_raw_nw, x_raw_ne, x_raw_sw, x_raw_se;
    int y_raw_nw, y_raw_ne, y_raw_sw, y_raw_se;
    char str[80];

    fp = fopen("MTSKeyboard.dat", "r");
    if (fp==NULL) {
       perror("ERROR: could not open ./MTSKeyboard.dat for reading.\n");
    }

    // MTSKeyboard file format:
    // timestamp
    // key number
    // X (back left)     X (back right)
    // Y (back left)     Y (back right)
    // X (front left)    X (front right)
    // Y (front left)    Y (front right)
    fscanf(fp, "%[^\n]", str);  // skip the timestamp for now
    for (i=1; i<=NUMKEYS; i++) {
       fscanf(fp, "%d", &key);

       fscanf(fp, "%x %x", &x_raw_nw, &x_raw_ne);
       fscanf(fp, "%x %x", &y_raw_nw, &y_raw_ne);
       fscanf(fp, "%x %x", &x_raw_sw, &x_raw_se);
       fscanf(fp, "%x %x", &y_raw_sw, &y_raw_se);

       Ct[key].xmax = MAX(x_raw_ne, x_raw_se);
       Ct[key].xmin = MIN(x_raw_nw, x_raw_sw);
       Ct[key].ymax = MAX(y_raw_nw, y_raw_ne);
       Ct[key].ymin = MIN(y_raw_sw, y_raw_se);
    }
}


// c-for-dummies.com/blog/?p=69
void delay(int ms) {
   long pause;
   clock_t now, then;

   pause = ms * (CLOCKS_PER_SEC/1000);
   now = then = clock();
   while ( (now - then) < pause )
        now = clock();

}
