#include <time.h>
#include "net_include.h"

enum TYPE
{
    TYPE_START,
    TYPE_DATA,
    TYPE_FEEDBACK,
    TYPE_POLL,
    TYPE_FINALIZE
};

int main()
{

    int mcast_addr;
    unsigned char ttl_val;

    struct sockaddr_in sendAddr;
    int sendingSocket;
    char send_buf[80];

    mcast_addr = 225 << 24 | 1 << 16 | 3 << 8 | 50; /* (225.1.3.50) */


    sendingSocket = socket(AF_INET, SOCK_DGRAM, 0); /* Socket for sending */

    if (sendingSocket < 0)
    {
        perror("Mcast: socket");
        exit(1);
    }


    ttl_val = 1;
    if (setsockopt(sendingSocket, IPPROTO_IP, IP_MULTICAST_TTL, (void *)&ttl_val,
                   sizeof(ttl_val)) < 0)
    {
        printf("Mcast: problem in setsockopt of multicast ttl %d - ignore in WinNT or Win95\n", ttl_val);
    }

    sendAddr.sin_family = AF_INET;
    sendAddr.sin_addr.s_addr = htonl(mcast_addr); /* mcast address */
    sendAddr.sin_port = htons(PORT);

    send_buf[0] = TYPE_START;
    printf("sending START to all...\n");
    sendto( sendingSocket, send_buf, 80, 0,
        (struct sockaddr *)&sendAddr, sizeof(sendAddr) );
    printf("done! Exiting...\n");
    return 0;
}
